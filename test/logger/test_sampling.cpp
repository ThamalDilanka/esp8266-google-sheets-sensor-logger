#include <unity.h>

#include <cstdint>
#include <limits>

#include "sampling_window.h"

using logger_protocol::Measurements;
using sensor_sampling::Window;

namespace {

const Measurements NORMAL{30.0f, 70.0f, 28.0f, 80.0f};

void collect(Window& window, uint32_t started, const Measurements& values) {
    TEST_ASSERT_TRUE(window.beginSample(started));
    TEST_ASSERT_TRUE(window.addReading(started + 2200, values));
}

void test_window_trims_each_channel_independently() {
    const Measurements readings[] = {
        {20, 50, -40, 99}, {30, 52, 21, 60}, {31, 54, 22, 62},
        {32, 56, 23, 64}, {33, 58, 24, 66}, {34, 60, 25, 68},
        {35, 62, 26, 70}, {36, 64, 27, 72}, {37, 90, 28, 0},
        {50, 10, 80, 74},
    };
    Window window;
    window.begin(0);
    for (uint32_t index = 0; index < 10; ++index) {
        collect(window, index * 30000, readings[index]);
    }

    Measurements report{};
    TEST_ASSERT_EQUAL_UINT(10, window.validCount());
    TEST_ASSERT_FALSE(window.finishReport(299999, report));
    TEST_ASSERT_EQUAL_UINT(10, window.validCount());
    TEST_ASSERT_TRUE(window.finishReport(300000, report));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 33.5f, report.indoorTemperature);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 57.0f, report.indoorHumidity);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 24.5f, report.outdoorTemperature);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 67.0f, report.outdoorHumidity);
    TEST_ASSERT_FALSE(window.finishReport(300000, report));
}

void test_window_requires_six_valid_pairs_and_trims_partial_windows() {
    const float temperatures[] = {-40, 10, 20, 30, 40, 50, 60, 70, 80, 80};
    const float expected[] = {25, 30, 35, 40, 45};
    for (uint32_t count = 0; count <= 10; ++count) {
        Window window;
        window.begin(0);
        for (uint32_t index = 0; index < count; ++index) {
            collect(window, index * 30000, {temperatures[index], 70, 28, 80});
        }

        Measurements report{1, 2, 3, 4};
        if (count < 6) {
            TEST_ASSERT_FALSE(window.finishReport(300000, report));
            TEST_ASSERT_EQUAL_FLOAT(1, report.indoorTemperature);
        } else {
            TEST_ASSERT_TRUE(window.finishReport(300000, report));
            TEST_ASSERT_FLOAT_WITHIN(0.001f, expected[count - 6], report.indoorTemperature);
        }
        TEST_ASSERT_EQUAL_UINT(0, window.validCount());
    }
}

void test_trimmed_values_use_the_existing_one_decimal_upload_contract() {
    const float temperatures[] = {30.1f, 30.2f, 30.1f, 30.3f, 30.2f,
                                  30.1f, 30.2f, 34.8f, 30.3f, 30.2f};
    Window window;
    window.begin(0);
    for (uint32_t index = 0; index < 10; ++index) {
        collect(window, index * 30000, {temperatures[index], 99.9f, 28, 80});
    }
    Measurements report{};
    TEST_ASSERT_TRUE(window.finishReport(300000, report));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.2f, report.indoorTemperature);
    char body[192] = {};
    TEST_ASSERT_TRUE(logger_protocol::buildFormBody(
        "0123456789abcdefABCDEF0123456789", report, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING(
        "token=0123456789abcdefABCDEF0123456789&indoor_t=30.2&indoor_rh=99.9"
        "&outdoor_t=28.0&outdoor_rh=80.0", body);
}

void test_repeated_boundary_values_remain_valid_after_averaging() {
    for (uint32_t count = 6; count <= 10; ++count) {
        Window window;
        window.begin(0);
        for (uint32_t index = 0; index < count; ++index) {
            collect(window, index * 30000, {-40, 0, 80, 99.9f});
        }
        Measurements report{};
        TEST_ASSERT_TRUE(window.finishReport(300000, report));
        TEST_ASSERT_TRUE(logger_protocol::isValidMeasurements(report));
        TEST_ASSERT_EQUAL_FLOAT(-40, report.indoorTemperature);
        TEST_ASSERT_EQUAL_FLOAT(0, report.indoorHumidity);
        TEST_ASSERT_EQUAL_FLOAT(80, report.outdoorTemperature);
        TEST_ASSERT_EQUAL_FLOAT(99.9f, report.outdoorHumidity);
    }
}

void test_invalid_readings_discard_the_whole_pair_and_allow_recovery() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const Measurements rejected[] = {
        {nan, 70, 28, 80}, {30, inf, 28, 80}, {30, 70, nan, 80},
        {30, 70, 28, inf}, {-40.1f, 70, 28, 80}, {30, -0.1f, 28, 80},
        {30, 70, 80.1f, 80}, {30, 70, 28, 100},
    };
    for (const Measurements& bad : rejected) {
        Window window;
        window.begin(0);
        TEST_ASSERT_TRUE(window.beginSample(0));
        TEST_ASSERT_FALSE(window.addReading(2200, bad));
        for (uint32_t index = 1; index <= 6; ++index) {
            collect(window, index * 30000, NORMAL);
        }
        TEST_ASSERT_EQUAL_UINT(6, window.validCount());
        Measurements report{};
        TEST_ASSERT_TRUE(window.finishReport(300000, report));
        TEST_ASSERT_EQUAL_FLOAT(30, report.indoorTemperature);
        TEST_ASSERT_EQUAL_FLOAT(70, report.indoorHumidity);
        TEST_ASSERT_EQUAL_FLOAT(28, report.outdoorTemperature);
        TEST_ASSERT_EQUAL_FLOAT(80, report.outdoorHumidity);
    }
}

void test_report_reset_does_not_reuse_previous_samples() {
    Window window;
    window.begin(0);
    for (uint32_t index = 0; index < 10; ++index) {
        collect(window, index * 30000, NORMAL);
    }
    Measurements report{};
    TEST_ASSERT_TRUE(window.finishReport(300000, report));
    // No upload acknowledgment is needed to clear the completed window.
    for (uint32_t index = 0; index < 5; ++index) {
        collect(window, 300000 + index * 30000, {40, 50, 20, 60});
    }
    TEST_ASSERT_FALSE(window.finishReport(600000, report));
    for (uint32_t index = 0; index < 6; ++index) {
        collect(window, 600000 + index * 30000, {40, 50, 20, 60});
    }
    TEST_ASSERT_TRUE(window.finishReport(900000, report));
    TEST_ASSERT_EQUAL_FLOAT(40, report.indoorTemperature);
    TEST_ASSERT_EQUAL_FLOAT(50, report.indoorHumidity);
    TEST_ASSERT_EQUAL_FLOAT(20, report.outdoorTemperature);
    TEST_ASSERT_EQUAL_FLOAT(60, report.outdoorHumidity);
}

void test_sampling_waits_thirty_seconds_and_never_catches_up_in_a_burst() {
    Window window;
    window.begin(1000);
    collect(window, 1000, NORMAL);
    TEST_ASSERT_FALSE(window.beginSample(30999));
    collect(window, 31000, NORMAL);
    // Resume after missed slots with one real observation, then wait 30 seconds.
    collect(window, 156000, NORMAL);
    TEST_ASSERT_FALSE(window.beginSample(158201));
    TEST_ASSERT_FALSE(window.beginSample(185999));
    collect(window, 186000, NORMAL);
    TEST_ASSERT_EQUAL_UINT(4, window.validCount());
    TEST_ASSERT_FALSE(window.reportDue(300999));
    TEST_ASSERT_TRUE(window.reportDue(301000));
}

void test_slow_upload_keeps_report_boundaries_and_skips_empty_windows() {
    Window window;
    window.begin(0);
    for (uint32_t index = 0; index < 10; ++index) {
        collect(window, index * 30000, NORMAL);
    }
    Measurements report{};
    TEST_ASSERT_TRUE(window.finishReport(300000, report));
    // Simulate a 95-second upload. The next report is still due at 600 seconds.
    for (uint32_t index = 0; index < 7; ++index) {
        collect(window, 395000 + index * 30000, NORMAL);
    }
    TEST_ASSERT_FALSE(window.reportDue(599999));
    TEST_ASSERT_TRUE(window.finishReport(600000, report));
    // An upload lasting more than a window must not generate catch-up reports.
    TEST_ASSERT_FALSE(window.finishReport(1240000, report));
    TEST_ASSERT_FALSE(window.reportDue(1240000));
    collect(window, 1240000, NORMAL);
    TEST_ASSERT_FALSE(window.beginSample(1242201));
    TEST_ASSERT_FALSE(window.reportDue(1499999));
    TEST_ASSERT_TRUE(window.reportDue(1500000));
}

void test_expired_window_is_discarded_instead_of_backfilled() {
    Window window;
    window.begin(0);
    for (uint32_t index = 0; index < 10; ++index) {
        collect(window, index * 30000, NORMAL);
    }
    Measurements report{};
    TEST_ASSERT_FALSE(window.finishReport(600000, report));
    TEST_ASSERT_EQUAL_UINT(0, window.validCount());
    TEST_ASSERT_FALSE(window.reportDue(600000));
    collect(window, 600000, NORMAL);
}

void test_samples_cannot_cross_the_report_boundary_or_be_added_twice() {
    Window window;
    window.begin(0);
    TEST_ASSERT_FALSE(window.addReading(0, NORMAL));
    collect(window, 0, NORMAL);
    TEST_ASSERT_FALSE(window.addReading(2201, NORMAL));
    TEST_ASSERT_EQUAL_UINT(1, window.validCount());
    TEST_ASSERT_FALSE(window.beginSample(297800));
    TEST_ASSERT_FALSE(window.beginSample(300000));

    Window delayed;
    delayed.begin(0);
    TEST_ASSERT_TRUE(delayed.beginSample(270000));
    TEST_ASSERT_FALSE(delayed.beginSample(300000));
    TEST_ASSERT_FALSE(delayed.addReading(300001, NORMAL));
    TEST_ASSERT_EQUAL_UINT(0, delayed.validCount());
}

void test_sensor_refresh_gap_is_preserved_across_windows() {
    Window window;
    window.begin(0);
    TEST_ASSERT_TRUE(window.beginSample(270000));
    TEST_ASSERT_TRUE(window.addReading(299000, NORMAL));
    Measurements report{};
    TEST_ASSERT_FALSE(window.finishReport(300000, report));
    TEST_ASSERT_FALSE(window.beginSample(300000));
    TEST_ASSERT_FALSE(window.beginSample(301199));
    collect(window, 301200, NORMAL);
}

void test_sampling_and_report_timers_survive_millis_rollover() {
    constexpr uint32_t start = UINT32_MAX - 149999;
    Window window;
    window.begin(start);
    for (uint32_t index = 0; index < 10; ++index) {
        const uint32_t sampleAt = start + index * 30000;
        collect(window, sampleAt, NORMAL);
        TEST_ASSERT_FALSE(window.beginSample(sampleAt + 29999));
    }
    Measurements report{};
    TEST_ASSERT_FALSE(window.reportDue(149999));
    TEST_ASSERT_TRUE(window.finishReport(150000, report));
    TEST_ASSERT_EQUAL_FLOAT(30, report.indoorTemperature);
    collect(window, 150000, NORMAL);
    TEST_ASSERT_FALSE(window.reportDue(449999));
    TEST_ASSERT_TRUE(window.reportDue(450000));
}

}  // namespace

void runSamplingTests() {
    Unity.TestFile = __FILE__;
    RUN_TEST(test_window_trims_each_channel_independently);
    RUN_TEST(test_window_requires_six_valid_pairs_and_trims_partial_windows);
    RUN_TEST(test_trimmed_values_use_the_existing_one_decimal_upload_contract);
    RUN_TEST(test_repeated_boundary_values_remain_valid_after_averaging);
    RUN_TEST(test_invalid_readings_discard_the_whole_pair_and_allow_recovery);
    RUN_TEST(test_report_reset_does_not_reuse_previous_samples);
    RUN_TEST(test_sampling_waits_thirty_seconds_and_never_catches_up_in_a_burst);
    RUN_TEST(test_slow_upload_keeps_report_boundaries_and_skips_empty_windows);
    RUN_TEST(test_expired_window_is_discarded_instead_of_backfilled);
    RUN_TEST(test_samples_cannot_cross_the_report_boundary_or_be_added_twice);
    RUN_TEST(test_sensor_refresh_gap_is_preserved_across_windows);
    RUN_TEST(test_sampling_and_report_timers_survive_millis_rollover);
}
