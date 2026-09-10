#include "sampling_window.h"

#include <algorithm>

namespace sensor_sampling {

void Window::begin(uint32_t now) {
    count_ = 0;
    started_ = now;
    hasSampled_ = false;
    hasRead_ = false;
    readingPending_ = false;
}

bool Window::beginSample(uint32_t now) {
    const uint32_t elapsed = now - started_;
    if (readingPending_ || count_ == SAMPLES_PER_REPORT ||
        elapsed >= REPORT_INTERVAL_MS - SENSOR_REFRESH_MS ||
        (hasSampled_ && static_cast<uint32_t>(now - lastSampleStarted_) < SAMPLE_INTERVAL_MS) ||
        (hasRead_ && static_cast<uint32_t>(now - lastReadingFinished_) < SENSOR_REFRESH_MS)) {
        return false;
    }

    // Advance from the actual attempt time: never replay missed sample slots.
    lastSampleStarted_ = now;
    hasSampled_ = true;
    readingPending_ = true;
    return true;
}

bool Window::addReading(uint32_t now, const logger_protocol::Measurements& reading) {
    if (!readingPending_) return false;
    readingPending_ = false;
    lastReadingFinished_ = now;
    hasRead_ = true;
    if (reportDue(now) || !logger_protocol::isValidMeasurements(reading)) {
        return false;
    }
    readings_[count_++] = reading;
    return true;
}

bool Window::reportDue(uint32_t now) const {
    return static_cast<uint32_t>(now - started_) >= REPORT_INTERVAL_MS;
}

float Window::trimmedMean(float logger_protocol::Measurements::*field) const {
    float sorted[SAMPLES_PER_REPORT];
    for (size_t index = 0; index < count_; ++index) {
        sorted[index] = readings_[index].*field;
    }
    std::sort(sorted, sorted + count_);
    // Accumulate in double so repeated boundary values (e.g. 99.9 %RH)
    // cannot round beyond the protocol's validation range.
    double sum = 0;
    for (size_t index = 1; index + 1 < count_; ++index) {
        sum += sorted[index];
    }
    return static_cast<float>(sum / (count_ - 2));
}

bool Window::finishReport(uint32_t now, logger_protocol::Measurements& report) {
    if (!reportDue(now)) return false;

    const uint32_t elapsedWindows = (now - started_) / REPORT_INTERVAL_MS;
    const bool usable = elapsedWindows == 1 && count_ >= MIN_VALID_SAMPLES;
    if (usable) {
        using logger_protocol::Measurements;
        report = {trimmedMean(&Measurements::indoorTemperature),
                  trimmedMean(&Measurements::indoorHumidity),
                  trimmedMean(&Measurements::outdoorTemperature),
                  trimmedMean(&Measurements::outdoorHumidity)};
    }

    // Consume before any blocking upload, including unsuccessful uploads.
    // Keep the original report cadence, dropping fully missed windows.
    started_ += elapsedWindows * REPORT_INTERVAL_MS;
    count_ = 0;
    readingPending_ = false;
    // Retain sensor transaction timing across windows to avoid short read gaps.
    return usable;
}

}  // namespace sensor_sampling
