#pragma once
#include <stdint.h>

enum class Source : uint8_t { off, handheld, phone };

class BenchLease {
public:
  static constexpr uint32_t timeoutMs = 350;
  Source selected = Source::off;
  uint8_t mask = 0;
  constexpr bool active() const { return active_; }

  constexpr void revoke() {
    active_ = false;
    mask = 0;
    blockedAxes_ = 0;
  }

  constexpr void select(Source source) {
    revoke();
    selected = source;
  }

  constexpr void expire(uint32_t now) {
    if (active_ && uint32_t(now - seen_) >= timeoutMs) revoke();
  }

  constexpr bool claim(Source source, uint32_t now) {
    expire(now);
    if (active_ || source == Source::off || source != selected) return false;
    active_ = true;
    sequence_ = 0;
    seen_ = now;
    return true;
  }

  constexpr bool command(bool matchingToken, uint32_t seq, uint32_t next, uint32_t now) {
    expire(now);
    if (!active_ || !matchingToken || seq <= sequence_) return false;
    if ((!sequence_ && next) || next > 127) {
      revoke();
      return false;
    }
    sequence_ = seq;
    seen_ = now;
    uint8_t accepted = next & 64; // AUX is independent of the three axes.
    for (uint8_t axis = 0; axis < 3; ++axis) {
      const uint8_t axisFlag = uint8_t(1u << axis);
      const uint8_t pair = uint8_t(3u << (2 * axis));
      const uint8_t requested = next & pair;
      const uint8_t held = mask & pair;
      if (!requested) {
        blockedAxes_ &= ~axisFlag;
      } else if (!(blockedAxes_ & axisFlag)) {
        if (held && (held & requested)) accepted |= held;
        else if (held || requested == pair) blockedAxes_ |= axisFlag;
        else accepted |= requested;
      }
    }
    mask = accepted;
    return true;
  }

private:
  bool active_ = false;
  uint8_t blockedAxes_ = 0;
  uint32_t sequence_ = 0;
  uint32_t seen_ = 0;
};
