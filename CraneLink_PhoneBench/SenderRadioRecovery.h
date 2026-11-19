#pragma once
#include <stdint.h>

// All intervals use unsigned elapsed time so millis() rollover is supported.
class SenderRadioRecovery {
public:
  static constexpr uint32_t leaseMs = 650;
  static constexpr uint32_t retryMs = 3000;
  static constexpr uint32_t quietMs = 60000;
  static constexpr uint32_t radioOffMs = 2200;
  static constexpr uint32_t listenMs = 800;

  constexpr void begin(uint32_t now) {
    heardAt_ = activityAt_ = attemptAt_ = now;
    heard_ = false;
  }

  constexpr void heard(uint32_t now) { heardAt_ = now; heard_ = true; }
  constexpr void lost() { heard_ = false; }
  constexpr void activity(uint32_t now) { activityAt_ = now; }
  constexpr void attempted(uint32_t now) { attemptAt_ = now; }
  constexpr bool linked(uint32_t now) const { return heard_ && uint32_t(now - heardAt_) < leaseMs; }
  constexpr bool retry(uint32_t now) const { return !linked(now) && uint32_t(now - attemptAt_) >= retryMs; }
  constexpr bool rest(uint32_t now) const {
    return !linked(now) && uint32_t(now - heardAt_) >= quietMs &&
           uint32_t(now - activityAt_) >= 5000 && uint32_t(now - attemptAt_) >= listenMs;
  }
  constexpr void suspend(uint32_t now) { suspendedAt_ = now; }
  constexpr bool probe(uint32_t now) const { return uint32_t(now - suspendedAt_) >= radioOffMs; }

private:
  uint32_t heardAt_ = 0, activityAt_ = 0, attemptAt_ = 0, suspendedAt_ = 0;
  bool heard_ = false;
};
