#include <Arduino.h>
#include <DHT.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <WiFiClientSecureBearSSL.h>
#include <cstring>
#include <time.h>

#include "bounded_response.h"
#include "google_root_ca.h"
#include "logger_protocol.h"
#include "sampling_window.h"
#include "status_led.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#include "wifi_secrets.example.h"
#endif

#if __has_include("logger_secrets.h")
#include "logger_secrets.h"
#else
#include "logger_secrets.example.h"
#endif

namespace {

constexpr unsigned long SENSOR_STARTUP_MS = 2500UL;
constexpr unsigned long WIFI_TIMEOUT_MS = 30000UL;
constexpr unsigned long TIME_SYNC_TIMEOUT_MS = 20000UL;
// Apps Script may spend 30 seconds waiting for its sheet lock, in addition to
// execution and network latency. HTTPClient takes a uint16_t millisecond value.
constexpr uint16_t HTTP_TIMEOUT_MS = 60000;
constexpr time_t MINIMUM_VALID_TIME = 1609459200;  // 2021-01-01 UTC
constexpr unsigned int MAX_RESPONSE_REDIRECTS = 2;

DHT indoorDht(D1, DHT22);
DHT outdoorDht(D2, DHT22);
BearSSL::X509List googleTrustAnchors(GOOGLE_ROOT_CA);
sensor_sampling::Window samplingWindow;
status_led::Pattern ledPattern;
Ticker ledTicker;
bool wifiWasConnected = false;

void updateLedOutput() {
    // The D1 mini built-in LED is active-low. Keep timer work to GPIO only.
    digitalWrite(LED_BUILTIN, ledPattern.isOn(millis()) ? LOW : HIGH);
}

void showLed(status_led::Signal signal, const char* description) {
    // Stop callbacks while replacing the pattern's state.
    ledTicker.detach();
    ledPattern.show(signal, millis());
    updateLedOutput();
    ledTicker.attach_ms(20, updateLedOutput);
    Serial.printf("[LED] %s\n", description);
}

void indicateWiFiConnected() {
    wifiWasConnected = true;
    showLed(status_led::Signal::Connected, "Wi-Fi connected: solid for 3 seconds.");
}

void observeWiFiChanges() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wifiWasConnected) {
        indicateWiFiConnected();
    } else if (!connected && wifiWasConnected) {
        wifiWasConnected = false;
        showLed(status_led::Signal::Failure, "Wi-Fi lost: repeating three rapid flashes.");
    }
}

bool uploadConfigured() {
    return logger_protocol::isValidScriptUrl(LOGGER_SCRIPT_URL) &&
           logger_protocol::isValidDeviceToken(LOGGER_DEVICE_TOKEN);
}

bool ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiWasConnected) indicateWiFiConnected();
        return true;
    }
    if (WIFI_SSID[0] == '\0') {
        Serial.println("[SKIP] Wi-Fi credentials are not configured.");
        return false;
    }

    Serial.println("[INFO] Connecting to Wi-Fi (bounded to 30 seconds)...");
    wifiWasConnected = false;
    showLed(status_led::Signal::Connecting, "Connecting: slow blinking.");
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[SKIP] Wi-Fi connection timed out; sample not uploaded.");
        return false;
    }

    Serial.println("[PASS] Wi-Fi connected.");
    indicateWiFiConnected();
    return true;
}

bool ensureNetworkTime() {
    if (time(nullptr) >= MINIMUM_VALID_TIME) {
        return true;
    }

    Serial.println("[INFO] Synchronizing network time for TLS verification...");
    configTime(0, 0, "time.google.com", "pool.ntp.org");
    const unsigned long started = millis();
    while (time(nullptr) < MINIMUM_VALID_TIME &&
           millis() - started < TIME_SYNC_TIMEOUT_MS) {
        delay(250);
    }
    if (time(nullptr) < MINIMUM_VALID_TIME) {
        Serial.println("[SKIP] Network time synchronization timed out; TLS not attempted.");
        return false;
    }

    Serial.println("[PASS] Network time synchronized.");
    return true;
}

bool readSensor(DHT& sensor,
                const char* label,
                float& temperature,
                float& humidity) {
    humidity = sensor.readHumidity(true);
    temperature = sensor.readTemperature();  // Same cached DHT transaction.
    if (!logger_protocol::isValidMeasurement(temperature, humidity)) {
        Serial.printf("[SKIP] %s sensor failed or returned an out-of-range value.\n", label);
        return false;
    }
    return true;
}

bool readMeasurements(logger_protocol::Measurements& measurements) {
    // Prime both sensors, then wait long enough for their next measurements.
    (void)indoorDht.readHumidity(true);
    (void)outdoorDht.readHumidity(true);
    delay(sensor_sampling::SENSOR_REFRESH_MS);

    const bool indoorOk = readSensor(indoorDht, "Indoor (D1/GPIO5)",
                                     measurements.indoorTemperature,
                                     measurements.indoorHumidity);
    const bool outdoorOk = readSensor(outdoorDht, "Outdoor (D2/GPIO4)",
                                      measurements.outdoorTemperature,
                                      measurements.outdoorHumidity);
    if (!indoorOk || !outdoorOk) {
        Serial.println("[SKIP] Incomplete sensor pair; discarded from this window.");
        return false;
    }
    return true;
}

void collectSample() {
    logger_protocol::Measurements measurements{};
    const bool sensorsOk = readMeasurements(measurements);
    const bool accepted = samplingWindow.addReading(millis(), measurements);
    if (!sensorsOk || !accepted) {
        if (sensorsOk) {
            Serial.println("[SKIP] Sensor read crossed the window boundary; pair discarded.");
        }
        showLed(status_led::Signal::Failure, "Sensor sample skipped: repeating three rapid flashes.");
        return;
    }

    Serial.printf("[SAMPLE] %u/%u valid pairs | Indoor %.1f C, %.1f %%RH | Outdoor %.1f C, %.1f %%RH\n",
                  static_cast<unsigned int>(samplingWindow.validCount()),
                  static_cast<unsigned int>(sensor_sampling::SAMPLES_PER_REPORT),
                  measurements.indoorTemperature, measurements.indoorHumidity,
                  measurements.outdoorTemperature, measurements.outdoorHumidity);
}

bool isRedirectStatus(int status) {
    return status == HTTP_CODE_MOVED_PERMANENTLY || status == HTTP_CODE_FOUND ||
           status == HTTP_CODE_SEE_OTHER || status == HTTP_CODE_TEMPORARY_REDIRECT ||
           status == HTTP_CODE_PERMANENT_REDIRECT;
}

bool parseHttpAcknowledgment(int status,
                             const char* response,
                             logger_protocol::Acknowledgment& acknowledgment) {
    if (status < 200 || status >= 300) {
        Serial.printf("[FAIL] Google response returned HTTP %d.\n", status);
        return false;
    }
    if (!logger_protocol::parseAcknowledgment(response, acknowledgment)) {
        Serial.println("[FAIL] Google response was not a valid acknowledgment.");
        return false;
    }
    return true;
}

bool readAcknowledgment(HTTPClient& request,
                        int status,
                        logger_protocol::Acknowledgment& acknowledgment) {
    const int responseSize = request.getSize();
    if (responseSize > static_cast<int>(LOGGER_MAX_REPLY_BYTES)) {
        Serial.println("[FAIL] Google response exceeded the size limit.");
        request.end();
        return false;
    }

    BoundedResponse response;
    const int bytesRead = request.writeToPrint(&response);
    request.end();
    if (response.overflowed()) {
        Serial.println("[FAIL] Google response exceeded the size limit.");
        return false;
    }
    if (bytesRead < 0) {
        Serial.println("[FAIL] Could not read the complete Google response.");
        return false;
    }
    return parseHttpAcknowledgment(status, response.c_str(), acknowledgment);
}

bool getRedirectedAcknowledgment(BearSSL::WiFiClientSecure& client,
                                 const String& initialUrl,
                                 logger_protocol::Acknowledgment& acknowledgment) {
    String url = initialUrl;
    for (unsigned int redirectCount = 0;
         redirectCount <= MAX_RESPONSE_REDIRECTS; ++redirectCount) {
        if (!logger_protocol::isAllowedResponseUrl(url.c_str())) {
            Serial.println("[FAIL] Google response redirect destination was rejected.");
            return false;
        }

        HTTPClient request;
        request.setTimeout(HTTP_TIMEOUT_MS);
        request.setReuse(false);
        const char* responseHeaders[] = {"Location"};
        request.collectHeaders(responseHeaders, 1);
        if (!request.begin(client, url)) {
            Serial.println("[FAIL] Could not start the response GET request.");
            return false;
        }

        const unsigned long responseStarted = millis();
        const int status = request.GET();
        Serial.printf("[INFO] Google response GET: status %d after %lu ms.\n",
                      status, millis() - responseStarted);
        if (status <= 0) {
            Serial.printf("[FAIL] Response GET transport error: %s.\n",
                          request.errorToString(status).c_str());
            request.end();
            return false;
        }
        if (isRedirectStatus(status)) {
            const String nextUrl = request.header("Location");
            request.end();
            if (redirectCount == MAX_RESPONSE_REDIRECTS) {
                Serial.println("[FAIL] Google response exceeded the redirect limit.");
                return false;
            }
            url = nextUrl;
            continue;
        }

        return readAcknowledgment(request, status, acknowledgment);
    }
    return false;
}

bool uploadMeasurements(const logger_protocol::Measurements& measurements,
                        logger_protocol::Acknowledgment& acknowledgment) {
    if (!uploadConfigured()) {
        Serial.println("[SKIP] Logger URL or device token is not configured correctly.");
        return false;
    }
    if (!ensureWiFi() || !ensureNetworkTime()) {
        return false;
    }

    char body[192] = {};
    if (!logger_protocol::buildFormBody(LOGGER_DEVICE_TOKEN, measurements, body,
                                        sizeof(body))) {
        Serial.println("[FAIL] Could not encode the measurement sample.");
        return false;
    }

    BearSSL::WiFiClientSecure client;
    client.setTrustAnchors(&googleTrustAnchors);
    client.setTimeout(HTTP_TIMEOUT_MS);

    HTTPClient request;
    request.setTimeout(HTTP_TIMEOUT_MS);
    request.setReuse(false);
    const char* responseHeaders[] = {"Location"};
    request.collectHeaders(responseHeaders, 1);
    if (!request.begin(client, LOGGER_SCRIPT_URL)) {
        Serial.println("[FAIL] Could not start the Google HTTPS POST.");
        return false;
    }
    request.addHeader("Content-Type", "application/x-www-form-urlencoded");

    Serial.printf("[INFO] Upload: Wi-Fi RSSI %ld dBm; HTTP read timeout %u ms.\n",
                  static_cast<long>(WiFi.RSSI()),
                  static_cast<unsigned int>(HTTP_TIMEOUT_MS));
    // Exactly one POST is issued. Any Apps Script redirect is followed with GET.
    const unsigned long postStarted = millis();
    const int status = request.POST(reinterpret_cast<uint8_t*>(body),
                                    std::strlen(body));
    Serial.printf("[INFO] Google POST: status %d after %lu ms.\n",
                  status, millis() - postStarted);
    if (status <= 0) {
        Serial.printf("[FAIL] Google POST transport error: %s.\n",
                      request.errorToString(status).c_str());
        request.end();
        return false;
    }
    if (isRedirectStatus(status)) {
        const String responseUrl = request.header("Location");
        request.end();
        return getRedirectedAcknowledgment(client, responseUrl, acknowledgment);
    }

    return readAcknowledgment(request, status, acknowledgment);
}

void reportWindow() {
    const size_t validCount = samplingWindow.validCount();
    logger_protocol::Measurements measurements{};
    if (!samplingWindow.finishReport(millis(), measurements)) {
        Serial.printf("[SKIP] Window has %u/%u valid pairs (minimum %u), or expired; no upload.\n",
                      static_cast<unsigned int>(validCount),
                      static_cast<unsigned int>(sensor_sampling::SAMPLES_PER_REPORT),
                      static_cast<unsigned int>(sensor_sampling::MIN_VALID_SAMPLES));
        showLed(status_led::Signal::Failure, "Window skipped: repeating three rapid flashes.");
        return;
    }

    Serial.printf("[REPORT] Trimmed mean of %u valid pairs (drop low/high per field) | Indoor %.1f C, %.1f %%RH | Outdoor %.1f C, %.1f %%RH\n",
                  static_cast<unsigned int>(validCount),
                  measurements.indoorTemperature, measurements.indoorHumidity,
                  measurements.outdoorTemperature, measurements.outdoorHumidity);
    logger_protocol::Acknowledgment acknowledgment{};
    if (uploadMeasurements(measurements, acknowledgment)) {
        Serial.printf("[PASS] Saved Google Sheet row %ld at %s.\n",
                      acknowledgment.row, acknowledgment.timestamp);
        showLed(status_led::Signal::Saved, "Saved both sensors: two flashes, pause, two flashes.");
    } else {
        Serial.println("[INFO] Save was not confirmed; no POST retry will be made.");
        showLed(status_led::Signal::Failure, "Save not confirmed: repeating three rapid flashes.");
    }
}

}  // namespace

void setup() {
    digitalWrite(LED_BUILTIN, HIGH);
    pinMode(LED_BUILTIN, OUTPUT);
    ledTicker.attach_ms(20, updateLedOutput);
    Serial.begin(115200);
    delay(1000);
    Serial.println();
    Serial.println("Indoor Outdoor Climate Monitor - Google Sheets logger");

    indoorDht.begin();
    outdoorDht.begin();
    delay(SENSOR_STARTUP_MS);

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // Do initial network setup before starting the first complete window.
    if (!ensureWiFi() || !ensureNetworkTime()) {
        showLed(status_led::Signal::Failure, "Startup network check failed: repeating three rapid flashes.");
    }
    samplingWindow.begin(millis());
    Serial.printf("[INFO] Sampling every %lu s; first report after %lu s; minimum %u/%u valid pairs.\n",
                  static_cast<unsigned long>(sensor_sampling::SAMPLE_INTERVAL_MS / 1000),
                  static_cast<unsigned long>(sensor_sampling::REPORT_INTERVAL_MS / 1000),
                  static_cast<unsigned int>(sensor_sampling::MIN_VALID_SAMPLES),
                  static_cast<unsigned int>(sensor_sampling::SAMPLES_PER_REPORT));
}

void loop() {
    observeWiFiChanges();
    if (samplingWindow.reportDue(millis())) {
        // Consume a completed window before uploading or collecting a new pair.
        reportWindow();
    } else if (samplingWindow.beginSample(millis())) {
        collectSample();
    }
    delay(50);
}
