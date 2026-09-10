#pragma once

#include <cstddef>
#include <cstdint>

#include "logger_protocol.h"

namespace sensor_sampling {

constexpr uint32_t SAMPLE_INTERVAL_MS = 30000;
constexpr uint32_t REPORT_INTERVAL_MS = 300000;
constexpr uint32_t SENSOR_REFRESH_MS = 2200;
constexpr size_t SAMPLES_PER_REPORT = REPORT_INTERVAL_MS / SAMPLE_INTERVAL_MS;
constexpr size_t MIN_VALID_SAMPLES = 6;

static_assert(SAMPLE_INTERVAL_MS > 2 * SENSOR_REFRESH_MS,
              "Allow a refresh gap before and after the fresh DHT read.");
static_assert(REPORT_INTERVAL_MS % SAMPLE_INTERVAL_MS == 0,
              "A report must contain a whole number of sample intervals.");
static_assert(MIN_VALID_SAMPLES >= 3 && MIN_VALID_SAMPLES <= SAMPLES_PER_REPORT,
              "Require enough samples to trim both extremes.");

// Owns one bounded window of complete sensor pairs and its wrap-safe schedule.
// Networking happens only after finishReport has consumed the window.
class Window {
   public:
    void begin(uint32_t now);
    bool beginSample(uint32_t now);
    bool addReading(uint32_t now, const logger_protocol::Measurements& reading);
    bool reportDue(uint32_t now) const;
    // Returns false for early, undersampled, or expired reports. Once due,
    // consumes the window regardless of validity or the later upload result.
    bool finishReport(uint32_t now, logger_protocol::Measurements& report);
    size_t validCount() const { return count_; }

   private:
    float trimmedMean(float logger_protocol::Measurements::*field) const;

    logger_protocol::Measurements readings_[SAMPLES_PER_REPORT]{};
    size_t count_ = 0;
    uint32_t started_ = 0;
    uint32_t lastSampleStarted_ = 0;
    uint32_t lastReadingFinished_ = 0;
    bool hasSampled_ = false;
    bool hasRead_ = false;
    bool readingPending_ = false;
};

}  // namespace sensor_sampling
