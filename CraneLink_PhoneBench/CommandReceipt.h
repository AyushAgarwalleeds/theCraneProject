#pragma once
#include <stdint.h>
#include "PhoneSession.h"

// A beacon proves only that the handheld can hear the receiver. An advancing
// receipt echoes the sequence of a command the receiver has processed.
class CommandReceipt {
public:
  constexpr void begin(uint32_t alreadySent) {
    last_ = alreadySent;
    seen_ = false;
    firstStop_ = 0;
  }

  constexpr bool accept(uint32_t sequence, uint32_t sent, uint32_t arrived) {
    if (sequence <= last_ || sequence > sent) return false;
    last_ = sequence;
    arrived_ = arrived;
    seen_ = true;
    return true;
  }

  constexpr bool fresh(uint32_t now) const {
    return seen_ && uint32_t(now - arrived_) < PhoneSession::leaseMs;
  }

  constexpr void newStop() { firstStop_ = 0; }
  constexpr void sentStop(uint32_t sequence) {
    if (!firstStop_) firstStop_ = sequence;
  }
  constexpr bool confirmsStop(bool stopped) const {
    return stopped && firstStop_ && last_ >= firstStop_;
  }

private:
  uint32_t last_ = 0, arrived_ = 0, firstStop_ = 0;
  bool seen_ = false;
};
