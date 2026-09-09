// Temporary bench diagnostic, selected with: pio run -e sensor_check
#include <Arduino.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <math.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#include "wifi_secrets.example.h"
#endif

constexpr unsigned long SAMPLE_INTERVAL_MS = 5000;
constexpr unsigned long SENSOR_REFRESH_MS = 2200;
constexpr unsigned long CONNECT_TIMEOUT_MS = 30000;

DHT indoorDht(D1, DHT22);
DHT outdoorDht(D2, DHT22);
unsigned long lastSampleMs = 0;
unsigned long sampleCount = 0;
unsigned long indoorValidCount = 0;
unsigned long outdoorValidCount = 0;

bool reportSensor(DHT& sensor, const char* label) {
    const float humidity = sensor.readHumidity(true);
    const float temperature = sensor.readTemperature(); // Same cached transaction.

    if (!isfinite(temperature) || !isfinite(humidity)) {
        Serial.printf("[FAIL] %s: read failed; check VCC, GND, DATA, and sensor type.\n", label);
        return false;
    }
    if (temperature < -40.0f || temperature > 80.0f || humidity < 0.0f || humidity > 99.9f) {
        Serial.printf("[FAIL] %s: out-of-range reading: %.1f C, %.1f %%RH.\n",
                      label, temperature, humidity);
        return false;
    }

    Serial.printf("[PASS] %s: %.1f C, %.1f %%RH\n", label, temperature, humidity);
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println();
    Serial.println("Indoor Outdoor Climate Monitor - Sensor diagnostic");
    Serial.println("DHT22 wiring: indoor DATA D1/GPIO5; outdoor DATA D2/GPIO4.");
    Serial.println("Both sensors: VCC to 3V3; GND to GND. Samples every 5 seconds.");

    indoorDht.begin();
    outdoorDht.begin();
    delay(2500); // Wait more than two seconds after sensor power-up.

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.disconnect();
    delay(100);
    if (WIFI_SSID[0] == '\0') {
        Serial.println("[SKIP] Wi-Fi: set include/wifi_secrets.h; sensor checks will continue.");
    } else {
        Serial.printf("Connecting to \"%s\" (up to 30 seconds)...\n", WIFI_SSID);
        WiFi.config(0U, 0U, 0U);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        const unsigned long started = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - started < CONNECT_TIMEOUT_MS) {
            delay(250);
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[FAIL] Wi-Fi connection timed out; sensor checks will continue.");
        }
    }
    lastSampleMs = millis() - SAMPLE_INTERVAL_MS;
}

void loop() {
    if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS) {
        lastSampleMs = millis();

        // Prime each sensor, then wait for fresh measurements as in the build guide.
        (void) indoorDht.readHumidity(true);
        (void) outdoorDht.readHumidity(true);
        delay(SENSOR_REFRESH_MS);

        Serial.printf("\n--- Sample %lu ---\n", ++sampleCount);
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[PASS] Wi-Fi: IP %s | RSSI: %ld dBm\n",
                          WiFi.localIP().toString().c_str(), static_cast<long>(WiFi.RSSI()));
        } else {
            Serial.printf("[FAIL] Wi-Fi: disconnected; status: %d.\n",
                          static_cast<int>(WiFi.status()));
        }

        if (reportSensor(indoorDht, "Indoor (D1/GPIO5)")) {
            ++indoorValidCount;
        }
        if (reportSensor(outdoorDht, "Outdoor (D2/GPIO4)")) {
            ++outdoorValidCount;
        }
        Serial.printf("[STATUS] Valid reads: indoor %lu/%lu; outdoor %lu/%lu.\n",
                      indoorValidCount, sampleCount, outdoorValidCount, sampleCount);
    }
    delay(50);
}
