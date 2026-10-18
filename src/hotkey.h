#pragma once
#include <cstdint>

// Merge native hotkey messages with a physical-key polling fallback. A press
// observed by polling also consumes its older queued WM_HOTKEY timestamp.
class HotkeyGate {
public:
    void Reset(bool keyDown) {
        down_ = keyDown;
        waitForRelease_ = keyDown;
        polled_ = false;
        pollTick_ = 0;
    }

    bool Poll(bool keyDown, bool modifiersMatch, bool allowed, std::uint32_t tick) {
        if (!keyDown) {
            down_ = false;
            waitForRelease_ = false;
            // WM_HOTKEY has queue priority over our polling timer, so queued
            // messages drain before a released-key timer expires this marker.
            // Expiry also keeps signed tick comparisons within their range.
            if (polled_ && tick - pollTick_ > 2000u) polled_ = false;
            return false;
        }
        if (down_) return false;
        down_ = true;
        if (waitForRelease_ || !modifiersMatch || !allowed) return false;
        polled_ = true;
        pollTick_ = tick;
        return true;
    }

    bool Message(bool keyDown, bool allowed, std::uint32_t eventTick) {
        if (!allowed || waitForRelease_) return false;
        if (polled_ && static_cast<std::int32_t>(eventTick - pollTick_) <= 0) return false;
        down_ = keyDown;
        return true;
    }

private:
    bool down_ = false;
    bool waitForRelease_ = false;
    bool polled_ = false;
    std::uint32_t pollTick_ = 0;
};
