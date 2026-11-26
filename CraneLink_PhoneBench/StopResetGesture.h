#pragma once
#include <stdint.h>

class StopResetGesture {
public:
  static constexpr uint8_t buttons = 0xC0; // Power + Emergency Stop.
  static constexpr uint32_t holdMs = 3000;

  constexpr void requireRelease() { *this = StopResetGesture{}; }

  // Send the reset only after both buttons have been released, in either order.
  constexpr bool update(uint8_t raw, uint32_t now) {
    if (!raw) {
      if (held_) {
        releasing_ = true;
        releasedAt_ = now;
      }
      ready_ = true;
      timing_ = held_ = false;
      const uint32_t elapsed = now - releasedAt_;
      if (elapsed >= 275) releasing_ = false;
      return releasing_ && elapsed >= 25;
    }
    releasing_ = false;
    if (!ready_ || (raw & ~buttons)) {
      requireRelease();
      return false;
    }
    if (raw == buttons) {
      if (!timing_) {
        timing_ = true;
        pressedAt_ = now;
      }
      if (uint32_t(now - pressedAt_) >= holdMs) held_ = true;
    } else {
      timing_ = false;
    }
    return false;
  }

private:
  uint32_t pressedAt_ = 0, releasedAt_ = 0;
  bool ready_ = false, timing_ = false, held_ = false, releasing_ = false;
};
