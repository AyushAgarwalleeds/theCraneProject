#pragma once
#include <stdint.h>

class AuxLatch {
public:
  static constexpr uint8_t command = 64;
  static constexpr uint32_t holdMs = 3000;

  constexpr void update(uint8_t mask, bool stopped, uint32_t now, uint32_t idleMs,
                        uint32_t confirmedAt) {
    if (stopped) {
      on_ = holding_ = toggled_ = false;
      return;
    }
    if (on_ && uint32_t(now - lastActivity_) >= idleMs) on_ = false;
    if (mask) lastActivity_ = now;
    if (!(mask & command)) {
      holding_ = toggled_ = false;
      return;
    }
    if (!holding_) {
      holding_ = true;
      pressedAt_ = confirmedAt;
    }
    // Require a confirmed hold at the threshold, even if the lease is still
    // alive after the last packet.
    if (!toggled_ && uint32_t(confirmedAt - pressedAt_) >= holdMs) {
      on_ = !on_;
      toggled_ = true;
    }
  }

  constexpr bool on() const { return on_; }
  constexpr uint32_t remainingSeconds(uint32_t now, uint32_t idleMs) const {
    const uint32_t elapsed = now - lastActivity_;
    return on_ && elapsed < idleMs ? (idleMs - elapsed + 999) / 1000 : 0;
  }
  constexpr uint32_t progress(uint32_t confirmedAt) const {
    return !holding_ ? 0 : toggled_ ? holdMs : uint32_t(confirmedAt - pressedAt_);
  }

private:
  uint32_t pressedAt_ = 0;
  uint32_t lastActivity_ = 0;
  bool holding_ = false;
  bool toggled_ = false;
  bool on_ = false;
};
