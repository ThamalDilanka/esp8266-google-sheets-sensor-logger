// Temporary hardware diagnostic, selected with: pio run -e wifi_check
#include <Arduino.h>
#include <ESP8266WiFi.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#include "wifi_secrets.example.h"
#endif

constexpr unsigned long CONNECT_TIMEOUT_MS = 30000;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println();
    Serial.println("Indoor Outdoor Climate Monitor - Wi-Fi diagnostic");

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    delay(100);

    Serial.println("Scanning for Wi-Fi networks...");
    const int networkCount = WiFi.scanNetworks();
    if (networkCount < 0) {
        Serial.println("[FAIL] Network scan failed.");
    } else if (networkCount == 0) {
        Serial.println("[INFO] Scan completed; no networks found.");
    } else {
        Serial.printf("[PASS] Radio scan found %d network(s).\n", networkCount);
        for (int i = 0; i < networkCount; ++i) {
            Serial.printf("  \"%s\" | RSSI: %ld dBm | channel: %d\n",
                          WiFi.SSID(i).c_str(),
                          static_cast<long>(WiFi.RSSI(i)), WiFi.channel(i));
        }
    }
    WiFi.scanDelete();

    if (WIFI_SSID[0] == '\0') {
        Serial.println("[SKIP] Connection test: set credentials in include/wifi_secrets.h.");
        Serial.println("Rebuild and upload the wifi_check environment after editing.");
        return;
    }

    Serial.printf("Connecting to \"%s\" (up to 30 seconds)...\n", WIFI_SSID);
    WiFi.config(0U, 0U, 0U); // Obtain the IP address using DHCP.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < CONNECT_TIMEOUT_MS) {
        delay(250);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[FAIL] Connection timed out; Wi-Fi status: %d.\n",
                      static_cast<int>(WiFi.status()));
        Serial.println("Check the network name, password, signal, and router settings.");
        WiFi.disconnect(true);
        return;
    }

    Serial.println("[PASS] Wi-Fi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.printf("RSSI: %ld dBm\n", static_cast<long>(WiFi.RSSI()));
    Serial.println("This confirms the local Wi-Fi connection; internet access is not tested.");
    Serial.println("Press RESET to repeat the diagnostic.");
}

void loop() {
    delay(1000);
}
