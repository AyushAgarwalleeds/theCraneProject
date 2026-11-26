#pragma once
#include <stdint.h>

// One control source owns a jog session. The relay driver applies its mask.
class PhoneSession {
public:
  static constexpr uint32_t leaseMs = 650;

  constexpr uint64_t tick(uint32_t now) {
    if (!token_ || uint32_t(now - updated_) < leaseMs) return 0;
    const uint64_t expired = token_;
    revoke();
    return expired;
  }

  constexpr bool arm(uint64_t token, uint32_t now) {
    tick(now);
    if (token_ || stopped_ || !token) return false;
    token_ = token;
    updated_ = now;
    return true;
  }

  constexpr bool jog(uint64_t token, uint32_t sequence, uint32_t mask, uint32_t now) {
    return jog(token, sequence, mask, now, now);
  }

  constexpr bool jog(uint64_t token, uint32_t sequence, uint32_t mask, uint32_t now, uint32_t arrived) {
    tick(now);
    if (uint32_t(now - arrived) >= leaseMs) return false;
    if (!token_ || token_ != token || sequence <= sequence_) return false;
    if ((!sequence_ && mask) || mask > 127) {
      revoke();
      return false;
    }
    sequence_ = sequence;
    updated_ = arrived;
    requested_ = uint8_t(mask);
    uint8_t selected = requested_ & 64;
    for (int axis = 0; axis < 3; ++axis) {
      const uint8_t pair = 3u << (2 * axis);
      const uint8_t requests = requested_ & pair;
      const uint8_t held = mask_ & pair;
      if (!interlock_) selected |= requests;
      else if (!requests) blocked_ &= ~pair;
      else if (!(blocked_ & pair)) {
        if (held & requests) selected |= held;
        else if (held || requests == pair) blocked_ |= pair;
        else selected |= requests;
      }
    }
    mask_ = selected;
    return true;
  }

  constexpr void end(uint64_t token) {
    if (token_ && token_ == token) revoke();
  }

  constexpr void stop() {
    revoke();
    stopped_ = true;
  }

  constexpr void reset() {
    revoke();
    stopped_ = false;
  }

  constexpr bool armed() const { return token_ != 0; }
  constexpr uint64_t owner() const { return token_; }
  constexpr bool ownedBy(uint64_t token) const { return token && token_ == token; }
  constexpr bool stopped() const { return stopped_; }
  constexpr uint8_t mask() const { return mask_; }
  constexpr uint8_t requested() const { return requested_; }
  constexpr void setInterlock(bool enabled) { interlock_ = enabled; revoke(); }
  constexpr uint32_t lastCommandAt() const { return updated_; }

private:
  constexpr void revoke() {
    token_ = 0;
    sequence_ = 0;
    mask_ = 0;
    requested_ = blocked_ = 0;
  }

  uint64_t token_ = 0;
  uint32_t sequence_ = 0;
  uint32_t updated_ = 0;
  uint8_t mask_ = 0;
  uint8_t requested_ = 0;
  uint8_t blocked_ = 0;
  bool interlock_ = false;
  bool stopped_ = false;
};
