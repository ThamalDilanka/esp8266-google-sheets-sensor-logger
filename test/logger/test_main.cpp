#include <ArduinoJson.h>
#include <unity.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "bounded_response.h"
#include "logger_protocol.h"
#include "status_led.h"

using logger_protocol::Acknowledgment;
using logger_protocol::Measurements;

void runSamplingTests();

void test_led_connecting_blinks_slowly_until_replaced() {
    status_led::Pattern led;
    led.show(status_led::Signal::Connecting, 1000);
    TEST_ASSERT_TRUE(led.isOn(1000));
    TEST_ASSERT_TRUE(led.isOn(1499));
    TEST_ASSERT_FALSE(led.isOn(1500));
    TEST_ASSERT_FALSE(led.isOn(1999));
    TEST_ASSERT_TRUE(led.isOn(2000));
    led.show(status_led::Signal::Idle, 2100);
    TEST_ASSERT_FALSE(led.isOn(2100));
}

void test_led_connected_lights_for_three_seconds_then_stays_off() {
    status_led::Pattern led;
    led.show(status_led::Signal::Connected, 100);
    TEST_ASSERT_TRUE(led.isOn(100));
    TEST_ASSERT_TRUE(led.isOn(3099));
    TEST_ASSERT_FALSE(led.isOn(3100));
    TEST_ASSERT_FALSE(led.isOn(10100));
}

void test_led_saved_gives_two_separate_pairs_and_stops() {
    status_led::Pattern led;
    led.show(status_led::Signal::Saved, 0);
    const uint32_t onTimes[] = {0, 99, 200, 299, 700, 799, 900, 999};
    const uint32_t offTimes[] = {100, 199, 300, 699, 800, 899, 1000, 5000};
    for (uint32_t time : onTimes) TEST_ASSERT_TRUE(led.isOn(time));
    for (uint32_t time : offTimes) TEST_ASSERT_FALSE(led.isOn(time));
}

void test_led_failure_repeats_three_flashes_with_a_long_pause() {
    status_led::Pattern led;
    led.show(status_led::Signal::Failure, 0);
    const uint32_t onTimes[] = {0, 200, 400, 2500, 2700, 2900};
    const uint32_t offTimes[] = {100, 300, 500, 1500, 2499, 3000};
    for (uint32_t time : onTimes) TEST_ASSERT_TRUE(led.isOn(time));
    for (uint32_t time : offTimes) TEST_ASSERT_FALSE(led.isOn(time));
}

void test_led_save_waits_for_full_connection_light_and_dark_gap() {
    status_led::Pattern led;
    led.show(status_led::Signal::Connected, 1000);
    led.show(status_led::Signal::Saved, 1500);
    TEST_ASSERT_TRUE(led.isOn(3999));
    TEST_ASSERT_FALSE(led.isOn(4000));
    TEST_ASSERT_FALSE(led.isOn(4199));
    TEST_ASSERT_TRUE(led.isOn(4200));
    TEST_ASSERT_FALSE(led.isOn(4300));
    TEST_ASSERT_TRUE(led.isOn(4400));
    TEST_ASSERT_FALSE(led.isOn(4500));
    TEST_ASSERT_TRUE(led.isOn(4900));
    TEST_ASSERT_FALSE(led.isOn(5000));
    TEST_ASSERT_TRUE(led.isOn(5100));
    TEST_ASSERT_FALSE(led.isOn(5200));
}

void test_led_failure_preempts_success_and_can_recover() {
    status_led::Pattern led;
    led.show(status_led::Signal::Connected, 0);
    led.show(status_led::Signal::Saved, 100);
    led.show(status_led::Signal::Failure, 200);
    TEST_ASSERT_TRUE(led.isOn(200));
    TEST_ASSERT_FALSE(led.isOn(300));
    TEST_ASSERT_TRUE(led.isOn(400));
    led.show(status_led::Signal::Connected, 500);
    TEST_ASSERT_TRUE(led.isOn(3499));
    TEST_ASSERT_FALSE(led.isOn(3500));
    TEST_ASSERT_FALSE(led.isOn(3700));
}

void test_led_timing_survives_millis_rollover() {
    status_led::Pattern led;
    led.show(status_led::Signal::Connecting, UINT32_MAX - 99);
    TEST_ASSERT_TRUE(led.isOn(0));
    TEST_ASSERT_FALSE(led.isOn(400));
    TEST_ASSERT_TRUE(led.isOn(900));
    led.show(status_led::Signal::Connected, UINT32_MAX - 99);
    TEST_ASSERT_TRUE(led.isOn(2899));
    TEST_ASSERT_FALSE(led.isOn(2900));
}

class TestPrintBase {
   public:
    virtual ~TestPrintBase() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t* buffer, size_t size) {
        size_t written = 0;
        while (written < size && write(buffer[written]) == 1) {
            ++written;
        }
        return written;
    }
    virtual int availableForWrite() {
        return 0;
    }
    virtual bool outputCanTimeout() {
        return true;
    }
};

using TestResponse =
    BasicBoundedResponse<TestPrintBase, LOGGER_MAX_REPLY_BYTES>;

size_t transferLikeEspPeekBuffer(const uint8_t* source,
                                 size_t size,
                                 TestResponse& destination) {
    size_t consumed = 0;
    while (consumed < size) {
        const int available = destination.availableForWrite();
        if (available <= 0) {
            // The ESP core waits for a timeout when true, or stops immediately when false.
            return consumed;
        }
        const size_t remaining = size - consumed;
        const size_t offered =
            remaining < static_cast<size_t>(available)
                ? remaining
                : static_cast<size_t>(available);
        const size_t written = destination.write(source + consumed, offered);
        consumed += written;
        if (written == 0 && !destination.outputCanTimeout()) {
            break;
        }
    }
    return consumed;
}

void test_response_sink_advertises_capacity_for_core_transfer() {
    TestResponse response;
    const char payload[] = "{\"ok\":true}";

    TEST_ASSERT_FALSE(response.outputCanTimeout());
    TEST_ASSERT_EQUAL_INT(LOGGER_MAX_REPLY_BYTES + 1,
                          response.availableForWrite());
    TEST_ASSERT_EQUAL_UINT(sizeof(payload) - 1,
                           transferLikeEspPeekBuffer(
                               reinterpret_cast<const uint8_t*>(payload),
                               sizeof(payload) - 1, response));
    TEST_ASSERT_EQUAL_STRING(payload, response.c_str());
    TEST_ASSERT_FALSE(response.overflowed());
}

void test_response_sink_accepts_exact_capacity() {
    uint8_t payload[LOGGER_MAX_REPLY_BYTES];
    std::memset(payload, 'x', sizeof(payload));
    TestResponse response;

    TEST_ASSERT_EQUAL_UINT(sizeof(payload),
                           transferLikeEspPeekBuffer(payload, sizeof(payload), response));
    TEST_ASSERT_EQUAL_UINT(LOGGER_MAX_REPLY_BYTES, std::strlen(response.c_str()));
    TEST_ASSERT_FALSE(response.overflowed());
    TEST_ASSERT_EQUAL_INT(1, response.availableForWrite());
}

void test_response_sink_flags_unknown_length_byte_over_capacity() {
    uint8_t payload[LOGGER_MAX_REPLY_BYTES + 1];
    std::memset(payload, 'x', sizeof(payload));
    TestResponse response;

    TEST_ASSERT_EQUAL_UINT(LOGGER_MAX_REPLY_BYTES,
                           transferLikeEspPeekBuffer(payload, sizeof(payload), response));
    TEST_ASSERT_EQUAL_UINT(LOGGER_MAX_REPLY_BYTES, std::strlen(response.c_str()));
    TEST_ASSERT_TRUE(response.overflowed());
    TEST_ASSERT_EQUAL_INT(0, response.availableForWrite());
}

void test_measurements_accept_boundaries_and_reject_invalid_values() {
    TEST_ASSERT_TRUE(logger_protocol::isValidMeasurement(-40.0f, 0.0f));
    TEST_ASSERT_TRUE(logger_protocol::isValidMeasurement(80.0f, 99.9f));
    TEST_ASSERT_FALSE(logger_protocol::isValidMeasurement(-40.1f, 50.0f));
    TEST_ASSERT_FALSE(logger_protocol::isValidMeasurement(80.1f, 50.0f));
    TEST_ASSERT_FALSE(logger_protocol::isValidMeasurement(20.0f, -0.1f));
    TEST_ASSERT_FALSE(logger_protocol::isValidMeasurement(20.0f, 100.0f));
    TEST_ASSERT_FALSE(logger_protocol::isValidMeasurement(
        std::numeric_limits<float>::quiet_NaN(), 50.0f));
    TEST_ASSERT_FALSE(logger_protocol::isValidMeasurement(
        20.0f, std::numeric_limits<float>::infinity()));

    Measurements complete{20.0f, 50.0f, 21.0f, 60.0f};
    TEST_ASSERT_TRUE(logger_protocol::isValidMeasurements(complete));
    complete.outdoorHumidity = 100.0f;
    TEST_ASSERT_FALSE(logger_protocol::isValidMeasurements(complete));
}

void test_token_requires_exactly_32_hex_characters() {
    TEST_ASSERT_TRUE(logger_protocol::isValidDeviceToken(
        "0123456789abcdefABCDEF0123456789"));
    TEST_ASSERT_FALSE(logger_protocol::isValidDeviceToken(""));
    TEST_ASSERT_FALSE(logger_protocol::isValidDeviceToken(
        "0123456789abcdefABCDEF012345678"));
    TEST_ASSERT_FALSE(logger_protocol::isValidDeviceToken(
        "0123456789abcdefABCDEF01234567890"));
    TEST_ASSERT_FALSE(logger_protocol::isValidDeviceToken(
        "0123456789abcdefABCDEF01234567-g"));
}

void test_script_url_requires_google_exec_endpoint() {
    TEST_ASSERT_TRUE(logger_protocol::isValidScriptUrl(
        "https://script.google.com/macros/s/AKfycb_example-123/exec"));
    TEST_ASSERT_FALSE(logger_protocol::isValidScriptUrl(
        "http://script.google.com/macros/s/AKfycb_example-123/exec"));
    TEST_ASSERT_FALSE(logger_protocol::isValidScriptUrl(
        "https://evil.example/macros/s/AKfycb_example-123/exec"));
    TEST_ASSERT_FALSE(logger_protocol::isValidScriptUrl(
        "https://script.google.com.evil.example/macros/s/id/exec"));
    TEST_ASSERT_FALSE(logger_protocol::isValidScriptUrl(
        "https://script.google.com/macros/s/id/dev"));
    TEST_ASSERT_FALSE(logger_protocol::isValidScriptUrl(
        "https://script.google.com/macros/s/id/exec?unexpected=1"));
    TEST_ASSERT_FALSE(logger_protocol::isValidScriptUrl(
        "https://script.google.com/macros/s//exec"));
}

void test_redirect_url_allows_only_expected_https_host() {
    TEST_ASSERT_TRUE(logger_protocol::isAllowedResponseUrl(
        "https://script.googleusercontent.com/macros/echo?user_content_key=abc"));
    TEST_ASSERT_FALSE(logger_protocol::isAllowedResponseUrl(
        "http://script.googleusercontent.com/macros/echo?key=abc"));
    TEST_ASSERT_FALSE(logger_protocol::isAllowedResponseUrl(
        "https://script.googleusercontent.com.evil.example/macros/echo"));
    TEST_ASSERT_FALSE(logger_protocol::isAllowedResponseUrl(
        "https://script.googleusercontent.com:443/macros/echo"));
    TEST_ASSERT_FALSE(logger_protocol::isAllowedResponseUrl(
        "https://user@script.googleusercontent.com/macros/echo"));
}

void test_form_body_uses_contract_order_and_one_decimal() {
    const Measurements values{-3.26f, 0.04f, 80.0f, 99.89f};
    char body[192] = {};
    TEST_ASSERT_TRUE(logger_protocol::buildFormBody(
        "0123456789abcdefABCDEF0123456789", values, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING(
        "token=0123456789abcdefABCDEF0123456789&indoor_t=-3.3&indoor_rh=0.0"
        "&outdoor_t=80.0&outdoor_rh=99.9",
        body);

    char tooSmall[16] = {};
    TEST_ASSERT_FALSE(logger_protocol::buildFormBody(
        "0123456789abcdefABCDEF0123456789", values, tooSmall, sizeof(tooSmall)));
}

void test_acknowledgment_accepts_exact_success_contract() {
    Acknowledgment acknowledgment{};
    TEST_ASSERT_TRUE(logger_protocol::parseAcknowledgment(
        R"({"ok":true,"row":42,"timestamp":"2026-09-08T12:00:00.000Z"})",
        acknowledgment));
    TEST_ASSERT_EQUAL_INT32(42, acknowledgment.row);
    TEST_ASSERT_EQUAL_STRING("2026-09-08T12:00:00.000Z", acknowledgment.timestamp);
}

void test_acknowledgment_rejects_false_loose_or_malformed_success() {
    const char* rejected[] = {
        R"({"ok":false,"error":"bad_token"})",
        R"({"ok":1,"row":2,"timestamp":"2026-09-08T12:00:00.000Z"})",
        R"({"ok":"true","row":2,"timestamp":"2026-09-08T12:00:00.000Z"})",
        R"({"ok":true,"row":1,"timestamp":"2026-09-08T12:00:00.000Z"})",
        R"({"ok":true,"row":2.5,"timestamp":"2026-09-08T12:00:00.000Z"})",
        R"({"ok":true,"row":2,"timestamp":"2026-13-08T12:00:00.000Z"})",
        R"({"ok":true,"row":2,"timestamp":"2026-09-31T12:00:00.000Z"})",
        R"({"ok":true,"row":2,"timestamp":"2026-09-08T24:00:00.000Z"})",
        R"({"ok":true,"row":2,"timestamp":"2026-09-08 12:00:00"})",
        R"({"ok":true,"row":2})",
        R"({"ok":true,"row":2,"timestamp":"2026-09-08T12:00:00.000Z"} trailing)",
        R"({"ok":true,"row":2,"timestamp":"2026-09-08T12:00:00.000Z"} {})",
        "not json",
    };

    for (const char* json : rejected) {
        Acknowledgment acknowledgment{};
        TEST_ASSERT_FALSE(logger_protocol::parseAcknowledgment(json, acknowledgment));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    runSamplingTests();
    Unity.TestFile = __FILE__;
    RUN_TEST(test_led_connecting_blinks_slowly_until_replaced);
    RUN_TEST(test_led_connected_lights_for_three_seconds_then_stays_off);
    RUN_TEST(test_led_saved_gives_two_separate_pairs_and_stops);
    RUN_TEST(test_led_failure_repeats_three_flashes_with_a_long_pause);
    RUN_TEST(test_led_save_waits_for_full_connection_light_and_dark_gap);
    RUN_TEST(test_led_failure_preempts_success_and_can_recover);
    RUN_TEST(test_led_timing_survives_millis_rollover);
    RUN_TEST(test_response_sink_advertises_capacity_for_core_transfer);
    RUN_TEST(test_response_sink_accepts_exact_capacity);
    RUN_TEST(test_response_sink_flags_unknown_length_byte_over_capacity);
    RUN_TEST(test_measurements_accept_boundaries_and_reject_invalid_values);
    RUN_TEST(test_token_requires_exactly_32_hex_characters);
    RUN_TEST(test_script_url_requires_google_exec_endpoint);
    RUN_TEST(test_redirect_url_allows_only_expected_https_host);
    RUN_TEST(test_form_body_uses_contract_order_and_one_decimal);
    RUN_TEST(test_acknowledgment_accepts_exact_success_contract);
    RUN_TEST(test_acknowledgment_rejects_false_loose_or_malformed_success);
    return UNITY_END();
}
