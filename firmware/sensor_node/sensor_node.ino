#include <Wire.h>
#include <SparkFunTMP102.h>
#include <SparkFun_Qwiic_Button.h>
#include <math.h>
#include <string.h>

// projection_mapping: USB JSON telemetry and button gestures. No servo control.
// Commands: STATUS followed by CR, LF, or CRLF.
// Events: ready, telemetry, short_press, long_press, button_release, status.
// Each message includes a sequence number (per boot) and device uptime.
constexpr uint8_t TMP102_ADDRESS = 0x48;
constexpr uint32_t BUTTON_POLL_MS = 10;
constexpr uint32_t DEBOUNCE_MS = 35;
constexpr uint32_t LONG_PRESS_MS = 2000;
constexpr uint32_t TEMPERATURE_READ_MS = 500;
constexpr uint32_t TELEMETRY_MS = 1000;

TMP102 temperatureSensor;
QwiicButton qwiicButton;
bool temperatureAvailable = false;
bool buttonAvailable = false;
float temperatureC = NAN;
bool rawPressed = false;
bool stablePressed = false;
bool gestureActive = false;
bool longPressSent = false;
uint32_t rawChangedAt = 0;
uint32_t pressedAt = 0;
uint32_t lastButtonPoll = 0;
uint32_t lastTemperatureRead = 0;
uint32_t lastTelemetry = 0;
uint32_t sequence = 0;

void emitEvent(const char *event, uint32_t now)
{
    Serial.print("{\"schema_version\":1,\"event\":\"");
    Serial.print(event);
    Serial.print("\",\"seq\":");
    Serial.print(sequence++);
    Serial.print(",\"uptime_ms\":");
    Serial.print(now);
    Serial.print(",\"temperature_c\":");
    if (temperatureAvailable && isfinite(temperatureC))
        Serial.print(temperatureC, 2);
    else
        Serial.print("null");
    Serial.print(",\"temperature_available\":");
    Serial.print(temperatureAvailable && isfinite(temperatureC) ? "true" : "false");
    Serial.print(",\"button_available\":");
    Serial.print(buttonAvailable ? "true" : "false");
    Serial.print(",\"button_pressed\":");
    Serial.print(stablePressed ? "true" : "false");
    Serial.println('}');
}

void updateTemperature(uint32_t now)
{
    if (!temperatureAvailable ||
        uint32_t(now - lastTemperatureRead) < TEMPERATURE_READ_MS)
        return;
    lastTemperatureRead = now;
    temperatureC = temperatureSensor.readTempC();
}

void pollButton(uint32_t now)
{
    if (!buttonAvailable || uint32_t(now - lastButtonPoll) < BUTTON_POLL_MS)
        return;
    lastButtonPoll = now;
    const bool sample = qwiicButton.isPressed();
    if (sample != rawPressed)
    {
        rawPressed = sample;
        rawChangedAt = now;
    }

    if (rawPressed != stablePressed && uint32_t(now - rawChangedAt) >= DEBOUNCE_MS)
    {
        stablePressed = rawPressed;
        if (stablePressed)
        {
            pressedAt = now;
            gestureActive = true;
            longPressSent = false;
        }
        else
        {
            if (gestureActive && !longPressSent)
            {
                // Use the first release sample so debounce doesn't extend a hold.
                const bool wasLong = uint32_t(rawChangedAt - pressedAt) >= LONG_PRESS_MS;
                emitEvent(wasLong ? "long_press" : "short_press", now);
            }
            gestureActive = false;
            emitEvent("button_release", now);
        }
    }

    if (stablePressed && rawPressed && gestureActive && !longPressSent &&
        uint32_t(now - pressedAt) >= LONG_PRESS_MS)
    {
        longPressSent = true;
        emitEvent("long_press", now);
    }
}

void pollSerial(uint32_t now)
{
    static char command[32];
    static size_t length = 0;
    static bool overflow = false;
    for (int count = 0; count < 64 && Serial.available() > 0; ++count)
    {
        const char c = char(Serial.read());
        if (c == '\n' || c == '\r')
        {
            command[length] = '\0';
            if (overflow)
                emitEvent("command_too_long", now);
            else if (length > 0)
                emitEvent(strcmp(command, "STATUS") == 0 ? "status" : "unknown_command", now);
            length = 0;
            overflow = false;
        }
        else if (!overflow)
        {
            if (length < sizeof(command) - 1)
                command[length++] = c;
            else
                overflow = true;
        }
    }
}

void setup()
{
    Serial.begin(115200);
    Wire.begin();
    temperatureAvailable = temperatureSensor.begin(TMP102_ADDRESS, Wire);
    buttonAvailable = qwiicButton.begin();
    if (temperatureAvailable)
    {
        temperatureSensor.setConversionRate(2);
        temperatureC = temperatureSensor.readTempC();
    }
    if (buttonAvailable)
        rawPressed = stablePressed = qwiicButton.isPressed();

    // A button held during startup must be released before it starts a gesture.
    const uint32_t now = millis();
    rawChangedAt = lastButtonPoll = lastTemperatureRead = lastTelemetry = now;
    emitEvent("ready", now);
}

void loop()
{
    const uint32_t now = millis();
    pollButton(now);
    updateTemperature(now);
    pollSerial(now);
    if (uint32_t(now - lastTelemetry) >= TELEMETRY_MS)
    {
        lastTelemetry = now;
        emitEvent("telemetry", now);
    }
}
