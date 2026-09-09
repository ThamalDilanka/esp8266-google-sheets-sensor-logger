#include "logger_protocol.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace logger_protocol {
namespace {

bool isAsciiDigit(char value) {
    return value >= '0' && value <= '9';
}

bool isHexDigit(char value) {
    return isAsciiDigit(value) || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool isDeploymentIdChar(char value) {
    return isAsciiDigit(value) || (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_' || value == '-';
}

bool isUtcIsoTimestamp(const char* timestamp) {
    if (timestamp == nullptr || std::strlen(timestamp) != 24) {
        return false;
    }

    constexpr size_t DIGIT_POSITIONS[] = {
        0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 22,
    };
    for (size_t position : DIGIT_POSITIONS) {
        if (!isAsciiDigit(timestamp[position])) {
            return false;
        }
    }

    if (timestamp[4] != '-' || timestamp[7] != '-' ||
        timestamp[10] != 'T' || timestamp[13] != ':' ||
        timestamp[16] != ':' || timestamp[19] != '.' ||
        timestamp[23] != 'Z') {
        return false;
    }

    const int year = (timestamp[0] - '0') * 1000 + (timestamp[1] - '0') * 100 +
                     (timestamp[2] - '0') * 10 + timestamp[3] - '0';
    const int month = (timestamp[5] - '0') * 10 + timestamp[6] - '0';
    const int day = (timestamp[8] - '0') * 10 + timestamp[9] - '0';
    const int hour = (timestamp[11] - '0') * 10 + timestamp[12] - '0';
    const int minute = (timestamp[14] - '0') * 10 + timestamp[15] - '0';
    const int second = (timestamp[17] - '0') * 10 + timestamp[18] - '0';

    constexpr int DAYS_PER_MONTH[] = {31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return false;
    }
    int maxDay = DAYS_PER_MONTH[month - 1];
    const bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leapYear) {
        maxDay = 29;
    }

    return day >= 1 && day <= maxDay && hour <= 23 && minute <= 59 &&
           second <= 59;
}

bool isJsonWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool containsOneJsonObject(const char* json) {
    while (isJsonWhitespace(*json)) {
        ++json;
    }
    if (*json != '{') {
        return false;
    }

    int depth = 0;
    bool insideString = false;
    bool escaped = false;
    for (const char* cursor = json; *cursor != '\0'; ++cursor) {
        if (insideString) {
            if (escaped) {
                escaped = false;
            } else if (*cursor == '\\') {
                escaped = true;
            } else if (*cursor == '"') {
                insideString = false;
            }
            continue;
        }

        if (*cursor == '"') {
            insideString = true;
        } else if (*cursor == '{' || *cursor == '[') {
            ++depth;
        } else if (*cursor == '}' || *cursor == ']') {
            --depth;
            if (depth == 0) {
                do {
                    ++cursor;
                } while (isJsonWhitespace(*cursor));
                return *cursor == '\0';
            }
        }
    }
    return false;
}

}  // namespace

bool isValidMeasurement(float temperature, float humidity) {
    return std::isfinite(temperature) && std::isfinite(humidity) &&
           temperature >= -40.0f && temperature <= 80.0f &&
           humidity >= 0.0f && humidity <= 99.9f;
}

bool isValidMeasurements(const Measurements& measurements) {
    return isValidMeasurement(measurements.indoorTemperature,
                              measurements.indoorHumidity) &&
           isValidMeasurement(measurements.outdoorTemperature,
                              measurements.outdoorHumidity);
}

bool isValidDeviceToken(const char* token) {
    if (token == nullptr || std::strlen(token) != 32) {
        return false;
    }

    for (size_t index = 0; index < 32; ++index) {
        if (!isHexDigit(token[index])) {
            return false;
        }
    }
    return true;
}

bool isValidScriptUrl(const char* url) {
    constexpr char PREFIX[] = "https://script.google.com/macros/s/";
    constexpr char SUFFIX[] = "/exec";

    if (url == nullptr || std::strncmp(url, PREFIX, sizeof(PREFIX) - 1) != 0) {
        return false;
    }

    const char* deploymentId = url + sizeof(PREFIX) - 1;
    const char* suffix = std::strstr(deploymentId, SUFFIX);
    if (suffix == nullptr || suffix == deploymentId ||
        suffix[sizeof(SUFFIX) - 1] != '\0') {
        return false;
    }

    for (const char* cursor = deploymentId; cursor < suffix; ++cursor) {
        if (!isDeploymentIdChar(*cursor)) {
            return false;
        }
    }
    return true;
}

bool isAllowedResponseUrl(const char* url) {
    constexpr char PREFIX[] = "https://script.googleusercontent.com/";
    if (url == nullptr || std::strncmp(url, PREFIX, sizeof(PREFIX) - 1) != 0 ||
        url[sizeof(PREFIX) - 1] == '\0') {
        return false;
    }

    for (const char* cursor = url + sizeof(PREFIX) - 1; *cursor != '\0'; ++cursor) {
        const unsigned char value = static_cast<unsigned char>(*cursor);
        if (value <= 0x20 || value >= 0x7f || value == '#') {
            return false;
        }
    }
    return true;
}

bool buildFormBody(const char* token,
                   const Measurements& measurements,
                   char* output,
                   size_t outputSize) {
    if (output == nullptr || outputSize == 0 || !isValidDeviceToken(token) ||
        !isValidMeasurements(measurements)) {
        return false;
    }

    const int written = std::snprintf(
        output, outputSize,
        "token=%s&indoor_t=%.1f&indoor_rh=%.1f&outdoor_t=%.1f&outdoor_rh=%.1f",
        token, static_cast<double>(measurements.indoorTemperature),
        static_cast<double>(measurements.indoorHumidity),
        static_cast<double>(measurements.outdoorTemperature),
        static_cast<double>(measurements.outdoorHumidity));
    return written >= 0 && static_cast<size_t>(written) < outputSize;
}

bool parseAcknowledgment(const char* json, Acknowledgment& acknowledgment) {
    if (json == nullptr || !containsOneJsonObject(json)) {
        return false;
    }

    JsonDocument document;
    const DeserializationError error = deserializeJson(document, json);
    if (error || !document.is<JsonObject>()) {
        return false;
    }

    const JsonVariantConst ok = document["ok"];
    const JsonVariantConst row = document["row"];
    const JsonVariantConst timestamp = document["timestamp"];
    if (!ok.is<bool>() || !ok.as<bool>() || !row.is<long>() ||
        row.as<long>() < 2 || !timestamp.is<const char*>() ||
        !isUtcIsoTimestamp(timestamp.as<const char*>())) {
        return false;
    }

    acknowledgment.row = row.as<long>();
    std::memcpy(acknowledgment.timestamp, timestamp.as<const char*>(),
                sizeof(acknowledgment.timestamp));
    return true;
}

}  // namespace logger_protocol
