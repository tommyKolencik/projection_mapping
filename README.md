# Projection Mapping

Built using old headless Dell running Ubuntu, an Arduino Nano ESP32, and a browser on a laptop.

## Components

| Location | Component | Responsibility |
| --- | --- | --- |
| Nano ESP32 | `firmware/sensor_node/sensor_node.ino` | Temperature reads, debounced short/long button gestures, newline-delimited JSON |
| Ubuntu  | `backend/src/main.cpp` | C++17 serial ingestion, validation, shared scene state, HTTP and WebSockets |
| Browser | `frontend/` | HTML/CSS dashboard and JavaScript|

The C++ backend owns projection selection and blackout state. Browsers render that shared state; reconnecting a browser doesn't replay button actions. Visual drawing currently runs in JavaScript. Future perspective-transform calculations are intended to live in C++.

## Build on the Dell

Install the build tools once:

```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-system-dev nlohmann-json3-dev
```

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

The dependencies are supplied by Ubuntu packages; the build does not download source dependencies.

## Run with the Nano

Close `arduino-cli monitor` first. Only the backend should own the serial port.

```bash
arduino-cli board list
./build/projection_backend --serial /dev/ttyACM0
```

Use the detected port if it differs. For a persistent USB identity, inspect `ls -l /dev/serial/by-id/` and pass the matching full path to `--serial`. The backend retries the configured path every two seconds; it does not guess which newly numbered USB port belongs to your board.

The service listens at `127.0.0.1:8080` by default. In a **separate MacBook terminal**, open an SSH tunnel:

```bash
ssh -N -L 8080:127.0.0.1:8080 sage
```

Then open **http://localhost:8080** in the MacBook browser. The tunnel normally prints nothing; leave it running. If VS Code has already forwarded port 8080, use that forwarded URL instead of opening a second tunnel. Alternatively choose another Mac port, e.g. `-L 8081:127.0.0.1:8080`, and browse to `http://localhost:8081`.

The backend currently runs in the foreground: closing its terminal or stopping the process may stop the service. Automatic startup with systemd is a later milestone. Scene and blackout state reset when the backend restarts.

## Controls

| Input | Behavior |
| --- | --- |
| Short press and release | Cycle Aurora → Constellation → Contours |
| Hold the physical button for two seconds | Toggle blackout once; release does not advance the scene |
| Next scene / Blackout in browser | Control shared backend state |
| Right arrow / B | Next scene / blackout (when not focused on a button) |
| Fullscreen | Display the canvas on the current screen; Escape exits |
| Temperature | Smoothly shift visual hue; 16–32 °C maps cool-to-warm |

Blackout renders black; it does not power off a projector. Controls remain available outside fullscreen. During blackout the sensors and network continue running. When sensor data becomes stale, the dashboard marks it unavailable and the visuals hold their previous temperature setting. Browser disconnection holds the last scene and blackout state.

## Demo without hardware

```bash
./build/projection_backend --demo
```

Demo mode generates a varying simulated temperature. It is explicitly labeled in the dashboard; the physical button is unavailable, and browser controls remain usable. Demo mode never opens a serial port. Do not run demo and hardware modes on the same port simultaneously.

## HTTP and serial protocol

- `GET /health`: service liveness (does not imply the Nano is connected).
- `GET /api/state`: current state and sensor freshness.
- `WS /ws`: JSON state snapshots and actions `{"action":"next_scene"}` or `{"action":"toggle_blackout"}`.
- Every serial packet must conform to firmware schema version 1, including event, sequence, uptime, temperature and availability fields. Malformed or oversized lines are counted and skipped. No valid packet for five seconds marks the device stale.
- A serial `STATUS` command is supported by the firmware, but normal operation does not need it because telemetry arrives every second.

The local server is intended for your SSH tunnel. It does not provide user authentication or TLS; keep the default loopback binding for this workflow. Static routes expose only the three frontend files, not the repository or configuration files.

## Tests

The Linux integration test uses a pseudo-terminal as a simulated Nano and real HTTP/WebSocket connections. It covers split serial lines, malformed and oversized packets, duplicate gesture suppression, synchronized browsers, stale readings, disconnect/reconnect, and restricted HTTP routes.

```bash
sudo apt install -y python3-websockets
python3 tests/integration.py build/projection_backend
```

Host tests cannot verify the real sensor wiring or projector output. Verify a short press and long hold on the physical Nano after deployment.
