#pragma once

#include <stddef.h>
#include <stdint.h>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#endif

constexpr size_t LOGGER_MAX_REPLY_BYTES = 512;

template <typename PrintBase, size_t Capacity>
class BasicBoundedResponse final : public PrintBase {
   public:
    using PrintBase::write;

    size_t write(uint8_t value) override {
        return write(&value, 1);
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        const size_t available = Capacity - length_;
        const size_t copied = size < available ? size : available;
        if (copied > 0) {
            std::memcpy(data_ + length_, buffer, copied);
            length_ += copied;
            data_[length_] = '\0';
        }
        if (copied != size) {
            overflowed_ = true;
        }
        return copied;
    }

    int availableForWrite() override {
        if (overflowed_) {
            return 0;
        }
        // Advertise one byte beyond storage so unknown-length/chunked transfers
        // can prove overflow instead of silently accepting a full valid prefix.
        return static_cast<int>(Capacity - length_ + 1);
    }

    bool outputCanTimeout() override {
        return false;
    }

    const char* c_str() const {
        return data_;
    }

    bool overflowed() const {
        return overflowed_;
    }

   private:
    char data_[Capacity + 1] = {};
    size_t length_ = 0;
    bool overflowed_ = false;
};

#ifdef ARDUINO
using BoundedResponse = BasicBoundedResponse<Print, LOGGER_MAX_REPLY_BYTES>;
#endif
