#pragma once
#include <stdint.h>

class ReceiverSelectGesture {
public:
  enum Result { normal, suppressPower, selectNext };

  constexpr Result update(uint8_t buttons, uint32_t now) {
    if (buttons & ~power) {
      phase_ = blocked;
      return normal;
    }
    if (!buttons) {
      if (phase_ == first && uint32_t(now - changed_) < 300) {
        phase_ = tapped;
        changed_ = now;
      } else if (phase_ != tapped || uint32_t(now - changed_) >= 600) {
        phase_ = idle;
      }
      return normal;
    }
    if (phase_ == tapped && uint32_t(now - changed_) < 600) {
      phase_ = second;
      changed_ = now;
    } else if (phase_ == idle || phase_ == tapped) {
      phase_ = first;
      changed_ = now;
    }
    if (phase_ == second && uint32_t(now - changed_) >= 2000) {
      phase_ = blocked;
      return selectNext;
    }
    return phase_ == second || phase_ == blocked ? suppressPower : normal;
  }

private:
  static constexpr uint8_t power = 64;
  enum Phase { idle, first, tapped, second, blocked };
  Phase phase_ = idle;
  uint32_t changed_ = 0;
};
