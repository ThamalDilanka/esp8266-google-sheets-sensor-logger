#pragma once

#include <cstdint>

namespace status_led {

enum class Signal { Idle, Connecting, Connected, Failure, Saved };

class Pattern {
   public:
    void show(Signal signal, uint32_t now) {
        // Keep the full connected indication and a visible dark gap before
        // showing a save that finishes unusually quickly.
        if (signal == Signal::Saved && signal_ == Signal::Connected &&
            static_cast<uint32_t>(now - started_) < 3200) {
            savedPending_ = true;
            return;
        }
        signal_ = signal;
        started_ = now;
        savedPending_ = false;
    }

    bool isOn(uint32_t now) const {
        const uint32_t elapsed = now - started_;
        switch (signal_) {
            case Signal::Connecting:
                return elapsed % 1000 < 500;
            case Signal::Connected:
                if (elapsed < 3000) return true;
                return savedPending_ && elapsed >= 3200 && savedPulse(elapsed - 3200);
            case Signal::Failure: {
                const uint32_t phase = elapsed % 2500;
                return phase < 500 && phase % 200 < 100;
            }
            case Signal::Saved:
                return savedPulse(elapsed);
            case Signal::Idle:
                return false;
        }
        return false;
    }

   private:
    static bool savedPulse(uint32_t elapsed) {
        return elapsed < 100 || (elapsed >= 200 && elapsed < 300) ||
               (elapsed >= 700 && elapsed < 800) ||
               (elapsed >= 900 && elapsed < 1000);
    }

    Signal signal_ = Signal::Idle;
    uint32_t started_ = 0;
    bool savedPending_ = false;
};

}  // namespace status_led
