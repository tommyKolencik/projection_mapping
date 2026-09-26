#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <sys/ioctl.h>
#include <termios.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
#ifndef FRONTEND_DEFAULT
#define FRONTEND_DEFAULT "frontend"
#endif

// All application and network callbacks run on one io_context thread.
class State {
public:
    bool demo = false, connected = false, blackout = false;
    int scene = 0;
    uint64_t received = 0, invalid = 0;
    json device = nullptr;
    std::string last_event = "waiting";
    std::optional<uint32_t> last_sequence;
    Clock::time_point last_seen{}, started = Clock::now();
    std::function<void()> changed = [] {};

    json snapshot() const {
        const bool fresh = !device.is_null() && Clock::now() - last_seen < 5s;
        return {{"type", "state"}, {"mode", demo ? "demo" : "hardware"},
            {"serial_connected", connected}, {"device_online", fresh && (demo || connected)},
            {"scene_index", scene}, {"blackout", blackout}, {"last_event", last_event},
            {"received_messages", received}, {"invalid_messages", invalid}, {"device", device},
            {"sample_age_ms", device.is_null() ? json(nullptr) : json(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-last_seen).count())}};
    }
    void action(const std::string& name) {
        if (name == "next_scene") scene = (scene + 1) % 3;
        else if (name == "toggle_blackout") blackout = !blackout;
        else return;
        last_event = name;
        changed();
    }
    bool ingest(const std::string& line) {
        const json j = json::parse(line, nullptr, false);
        static const std::set<std::string> events = {"ready", "telemetry", "short_press", "long_press", "button_release", "status", "unknown_command", "command_too_long"};
        const auto uint32field = [&](const char* key) {
            return j.contains(key) && j[key].is_number_unsigned() && j[key].get<uint64_t>() <= UINT32_MAX;
        };
        if (!j.is_object() || !j.contains("schema_version") || j["schema_version"] != 1 ||
            !j.contains("event") || !j["event"].is_string() || !events.count(j["event"].get<std::string>()) ||
            !uint32field("seq") || !uint32field("uptime_ms") ||
            !j.contains("button_pressed") || !j["button_pressed"].is_boolean() ||
            !j.contains("button_available") || !j["button_available"].is_boolean() ||
            !j.contains("temperature_available") || !j["temperature_available"].is_boolean() ||
            !j.contains("temperature_c") || !(j["temperature_c"].is_null() || j["temperature_c"].is_number())) {
            ++invalid; return false;
        }
        if (j["temperature_c"].is_number()) {
            const double t = j["temperature_c"].get<double>();
            if (!std::isfinite(t) || t < -55 || t > 150) { ++invalid; return false; }
        }
        if (j["temperature_available"].get<bool>() && j["temperature_c"].is_null()) { ++invalid; return false; }
        const auto seq = j["seq"].get<uint32_t>();
        // Repeated identical packets must not toggle blackout twice.
        if (last_sequence && *last_sequence == seq && !device.is_null() && device["uptime_ms"] == j["uptime_ms"]) return false;
        last_sequence = seq;
        device = j; last_seen = Clock::now(); ++received;
        last_event = j["event"].get<std::string>();
        if (j["button_available"].get<bool>()) {
            if (last_event == "short_press") scene = (scene + 1) % 3;
            else if (last_event == "long_press") blackout = !blackout;
        }
        changed(); return true;
    }
};

class SerialReader : public std::enable_shared_from_this<SerialReader> {
    net::serial_port port_;
    net::steady_timer retry_;
    State& state_;
    std::string path_, line_;
    std::array<char, 512> bytes_{};
    bool dropping_ = false;
    void reconnect() {
        beast::error_code ignored;
        port_.close(ignored);
        state_.connected = false;
        state_.last_seen = {};
        state_.changed();
        line_.clear(); dropping_ = false;
        retry_.expires_after(2s);
        retry_.async_wait([self=shared_from_this()](beast::error_code ec) { if (!ec) self->start(); });
    }
    void read() {
        port_.async_read_some(net::buffer(bytes_), [self=shared_from_this()](beast::error_code ec, size_t n) {
            if (ec) { std::cerr << "Serial disconnected: " << ec.message() << '\n'; self->reconnect(); return; }
            for (size_t i=0; i<n; ++i) {
                const char c = self->bytes_[i];
                if (c == '\n') {
                    if (!self->dropping_ && !self->line_.empty()) self->state_.ingest(self->line_);
                    self->line_.clear(); self->dropping_ = false;
                } else if (c != '\r' && !self->dropping_) {
                    if (self->line_.size() < 4096) self->line_ += c;
                    else { self->dropping_ = true; ++self->state_.invalid; self->line_.clear(); }
                }
            }
            self->read();
        });
    }
public:
    SerialReader(net::io_context& io, State& state, std::string path) : port_(io), retry_(io), state_(state), path_(std::move(path)) {}
    void start() {
        beast::error_code ec;
        port_.open(path_, ec);
        if (ec) { std::cerr << "Waiting for " << path_ << ": " << ec.message() << '\n'; reconnect(); return; }
        const int fd = port_.native_handle();
        // Match the working CLI monitor's DTR behavior and prevent another reader.
        if (::ioctl(fd, TIOCEXCL) != 0) { std::cerr << "Cannot reserve serial port\n"; reconnect(); return; }
        termios settings{};
        if (::tcgetattr(fd, &settings) != 0) { reconnect(); return; }
        ::cfmakeraw(&settings);
        ::cfsetispeed(&settings, B115200); ::cfsetospeed(&settings, B115200);
        settings.c_cflag |= CLOCAL | CREAD;
        settings.c_cflag &= ~(HUPCL | CRTSCTS);
        if (::tcsetattr(fd, TCSANOW, &settings) != 0) { reconnect(); return; }
        int dtr = TIOCM_DTR;
        ::ioctl(fd, TIOCMBIS, &dtr); // Some test pseudo-terminals don't implement DTR.
        state_.connected = true;
        state_.last_sequence.reset();
        state_.changed();
        std::cout << "Serial open: " << path_ << '\n';
        read();
    }
};

class WebSocket;
class Hub {
public:
    State& state;
    std::vector<std::weak_ptr<WebSocket>> clients;
    explicit Hub(State& s) : state(s) {}
    void broadcast();
};

class WebSocket : public std::enable_shared_from_this<WebSocket> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer input_;
    std::deque<std::string> output_;
    Hub& hub_;
    bool stopped_ = false;
    void stop() { stopped_ = true; beast::error_code ec; beast::get_lowest_layer(ws_).socket().close(ec); }
    void read() {
        ws_.async_read(input_, [self=shared_from_this()](beast::error_code ec, size_t) {
            if (ec) { self->stop(); return; }
            const auto message = json::parse(beast::buffers_to_string(self->input_.data()), nullptr, false);
            self->input_.consume(self->input_.size());
            if (message.is_object() && message.contains("action") && message["action"].is_string())
                self->hub_.state.action(message["action"].get<std::string>());
            self->read();
        });
    }
    void write() {
        ws_.text(true);
        ws_.async_write(net::buffer(output_.front()), [self=shared_from_this()](beast::error_code ec, size_t) {
            if (ec) { self->stop(); return; }
            self->output_.pop_front();
            if (!self->stopped_ && !self->output_.empty()) self->write();
        });
    }
public:
    WebSocket(beast::tcp_stream stream, Hub& hub) : ws_(std::move(stream)), hub_(hub) {}
    void start(http::request<http::string_body> request) {
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.read_message_max(1024);
        auto owned_request = std::make_shared<http::request<http::string_body>>(std::move(request));
        ws_.async_accept(*owned_request, [self=shared_from_this(), owned_request](beast::error_code ec) {
            if (ec) return;
            self->hub_.clients.push_back(self);
            self->send(self->hub_.state.snapshot().dump());
            self->read();
        });
    }
    void send(std::string text) {
        if (stopped_) return;
        // Disconnect a slow browser instead of accumulating unbounded data.
        if (output_.size() >= 32) { stop(); return; }
        const bool busy = !output_.empty();
        output_.push_back(std::move(text));
        if (!busy) write();
    }
};
void Hub::broadcast() {
    clients.erase(std::remove_if(clients.begin(), clients.end(), [](auto& c) { return c.expired(); }), clients.end());
    const auto text = state.snapshot().dump();
    for (auto& weak : clients) if (auto client = weak.lock()) client->send(text);
}

class HttpSession : public std::enable_shared_from_this<HttpSession> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request_parser<http::string_body> parser_;
    Hub& hub_;
    std::filesystem::path root_;
    void handle() {
        auto request = parser_.release();
        const std::string target(request.target());
        if (target == "/ws" && websocket::is_upgrade(request)) {
            // Accept same-origin browsers and clients without an Origin header.
            const auto origin = request[http::field::origin];
            const std::string expected = "http://" + std::string(request[http::field::host]);
            if (!origin.empty() && origin != expected) { respond(http::status::forbidden, "text/plain", "Origin rejected"); return; }
            std::make_shared<WebSocket>(std::move(stream_), hub_)->start(std::move(request));
            return;
        }
        if (request.method() != http::verb::get) { respond(http::status::method_not_allowed, "text/plain", "GET required"); return; }
        if (target == "/health") { respond(http::status::ok, "application/json", json({{"status","ok"},{"service","projection_mapping"}}).dump()); return; }
        if (target == "/api/state") { respond(http::status::ok, "application/json", hub_.state.snapshot().dump()); return; }
        std::string file, mime;
        if (target == "/" || target == "/index.html") { file = "index.html"; mime = "text/html; charset=utf-8"; }
        else if (target == "/styles.css") { file = "styles.css"; mime = "text/css"; }
        else if (target == "/app.js") { file = "app.js"; mime = "text/javascript"; }
        else { respond(http::status::not_found, "text/plain", "Not found"); return; }
        std::ifstream input(root_ / file, std::ios::binary);
        if (!input) { respond(http::status::not_found, "text/plain", "Frontend file missing"); return; }
        respond(http::status::ok, mime, std::string(std::istreambuf_iterator<char>(input), {}));
    }
    void respond(http::status status, const std::string& mime, std::string body) {
        auto response = std::make_shared<http::response<http::string_body>>(status, 11);
        response->set(http::field::content_type, mime);
        response->set(http::field::cache_control, "no-store");
        response->set("X-Content-Type-Options", "nosniff");
        response->set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; object-src 'none'; frame-ancestors 'none'");
        response->keep_alive(false); response->body() = std::move(body); response->prepare_payload();
        http::async_write(stream_, *response, [self=shared_from_this(), response](beast::error_code, size_t) {
            beast::error_code ec; self->stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
        });
    }
public:
    HttpSession(tcp::socket socket, Hub& hub, std::filesystem::path root) : stream_(std::move(socket)), hub_(hub), root_(std::move(root)) {}
    void start() {
        parser_.body_limit(1024); parser_.header_limit(8192);
        stream_.expires_after(10s);
        http::async_read(stream_, buffer_, parser_, [self=shared_from_this()](beast::error_code ec, size_t) { if (!ec) self->handle(); });
    }
};

class Listener : public std::enable_shared_from_this<Listener> {
    tcp::acceptor acceptor_;
    Hub& hub_;
    std::filesystem::path root_;
public:
    Listener(net::io_context& io, Hub& hub, const tcp::endpoint& ep, std::filesystem::path root) : acceptor_(io), hub_(hub), root_(std::move(root)) {
        acceptor_.open(ep.protocol()); acceptor_.set_option(net::socket_base::reuse_address(true)); acceptor_.bind(ep); acceptor_.listen();
    }
    void start() {
        acceptor_.async_accept([self=shared_from_this()](beast::error_code ec, tcp::socket socket) {
            if (!ec) std::make_shared<HttpSession>(std::move(socket), self->hub_, self->root_)->start();
            if (ec != net::error::operation_aborted) self->start();
        });
    }
};

int main(int argc, char** argv) {
    try {
        std::string serial = "/dev/ttyACM0", bind = "127.0.0.1", root = FRONTEND_DEFAULT;
        unsigned port = 8080; bool demo = false;
        for (int i=1; i<argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--demo") { demo = true; continue; }
            if (arg == "--help") { std::cout << "projection_backend [--serial PATH | --demo] [--bind IP] [--port PORT] [--frontend DIR]\n"; return 0; }
            if (i+1 >= argc) throw std::runtime_error("Missing value: " + arg);
            const std::string value = argv[++i];
            if (arg == "--serial") serial = value;
            else if (arg == "--bind") bind = value;
            else if (arg == "--frontend") root = value;
            else if (arg == "--port") {
                size_t n;
                const auto parsed = std::stoul(value, &n);
                if (n != value.size() || parsed == 0 || parsed > 65535) throw std::runtime_error("Invalid port");
                port = static_cast<unsigned>(parsed);
            }
            else throw std::runtime_error("Unknown option: " + arg);
        }
        if (!std::filesystem::is_regular_file(std::filesystem::path(root)/"index.html")) throw std::runtime_error("Frontend directory missing; use --frontend PATH");
        net::io_context io;
        State state; state.demo = demo;
        Hub hub(state); state.changed = [&hub] { hub.broadcast(); };
        auto listener = std::make_shared<Listener>(io, hub, tcp::endpoint(net::ip::make_address(bind), static_cast<unsigned short>(port)), root);
        listener->start();
        std::shared_ptr<SerialReader> reader;
        if (!demo) { reader = std::make_shared<SerialReader>(io, state, serial); reader->start(); }
        net::steady_timer tick(io);
        uint32_t demoSeq = 0;
        std::function<void()> heartbeat;
        heartbeat = [&] {
            tick.expires_after(1s);
            tick.async_wait([&](beast::error_code ec) {
                if (ec) return;
                if (demo) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-state.started).count();
                    state.ingest(json({{"schema_version",1},{"event","telemetry"},{"seq",demoSeq++},{"uptime_ms",uint32_t(ms)},
                        {"temperature_c",22.0+3.0*std::sin(ms/15000.0)},{"temperature_available",true},{"button_available",false},{"button_pressed",false}}).dump());
                }
                hub.broadcast(); heartbeat();
            });
        };
        heartbeat();
        net::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](beast::error_code, int) { io.stop(); });
        std::cout << "projection_mapping: http://" << bind << ':' << port << " (" << (demo ? "DEMO" : "hardware") << ")\n";
        io.run();
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
}
