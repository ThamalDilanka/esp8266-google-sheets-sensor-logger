#pragma once

#include <stddef.h>

namespace logger_protocol {

struct Measurements {
    float indoorTemperature;
    float indoorHumidity;
    float outdoorTemperature;
    float outdoorHumidity;
};

struct Acknowledgment {
    long row;
    char timestamp[25];
};

bool isValidMeasurement(float temperature, float humidity);
bool isValidMeasurements(const Measurements& measurements);
bool isValidDeviceToken(const char* token);
bool isValidScriptUrl(const char* url);
bool isAllowedResponseUrl(const char* url);
bool buildFormBody(const char* token,
                   const Measurements& measurements,
                   char* output,
                   size_t outputSize);
bool parseAcknowledgment(const char* json, Acknowledgment& acknowledgment);

}  // namespace logger_protocol
