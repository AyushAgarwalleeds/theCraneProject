#include "PhoneSession.h"
#include "AuxLatch.h"
#include "StopResetGesture.h"
#include "ReceiverSelectGesture.h"
#include "SenderRadioRecovery.h"

namespace {
constexpr bool ownerAndExpiry() {
  PhoneSession session;
  return session.arm(1, UINT32_MAX - 100) &&
         session.jog(1, 1, 0, UINT32_MAX - 90) &&
         session.jog(1, 2, 1, UINT32_MAX - 50) &&
         !session.arm(2, 20) &&
         !session.jog(2, 3, 2, 20) &&
         !session.jog(1, 2, 1, 300) &&
         session.mask() == 1 &&
         !session.jog(1, 3, 1, 600) &&
         session.mask() == 0 &&
         session.arm(2, 610) &&
         !session.jog(1, 4, 1, 620);
}

constexpr bool stopAndInterlock() {
  PhoneSession session;
  session.setInterlock(true);
  if (!session.arm(1, 0) || !session.jog(1, 1, 0, 1) ||
      !session.jog(1, 2, 41, 2) || session.mask() != 41 ||
      !session.jog(1, 3, 63, 3) || session.mask() != 41 ||
      !session.jog(1, 4, 22, 4) || session.mask() ||
      !session.jog(1, 5, 0, 5) ||
      !session.jog(1, 6, 22, 6) || session.mask() != 22) return false;
  session.end(2);
  if (session.mask() != 22) return false;
  session.stop();
  if (!session.stopped() || session.armed() || session.mask() || session.arm(3, 7)) return false;
  session.reset();
  return !session.stopped() && !session.armed() && session.arm(3, 8) &&
         !session.jog(3, 1, 4, 9);
}

constexpr bool allDirections() {
  PhoneSession session;
  return session.arm(1, 0) && session.jog(1, 1, 0, 1) &&
         session.jog(1, 2, 63, 2) && session.mask() == 63 &&
         session.jog(1, 3, 62, 3) && session.mask() == 62;
}

static_assert(ownerAndExpiry(), "Stale or foreign commands cannot renew the session.");
static_assert(stopAndInterlock(), "Interlock prevents opposing directions. Stop and reset clear commands.");
static_assert(allDirections(), "All six directions can run together by default.");

constexpr bool auxHoldAndExpiry() {
  AuxLatch aux;
  aux.update(64, false, 0, 60000, 0);
  aux.update(64, false, 3000, 60000, 2999);
  if (aux.on()) return false;
  aux.update(64, false, 3001, 60000, 3001);
  aux.update(64, false, 5000, 60000, 5000);
  if (!aux.on()) return false;
  aux.update(0, false, 6000, 60000, 6000);
  aux.update(1, false, 64000, 60000, 64000);
  aux.update(0, false, 123999, 60000, 123999);
  if (!aux.on()) return false;
  aux.update(0, false, 124000, 60000, 124000);
  return !aux.on();
}

constexpr bool auxToggleAndStop() {
  AuxLatch aux;
  aux.update(64, false, 10, 60000, 10);
  aux.update(0, false, 3000, 60000, 3000);
  aux.update(64, false, 4000, 60000, 4000);
  aux.update(64, false, 6999, 60000, 6999);
  if (aux.on()) return false;
  aux.update(64, false, 7000, 60000, 7000);
  if (!aux.on()) return false;
  aux.update(0, false, 7100, 60000, 7100);
  aux.update(64, false, 7200, 60000, 7200);
  aux.update(64, false, 10200, 60000, 10200);
  if (aux.on()) return false;
  aux.update(0, false, 10300, 60000, 10300);
  aux.update(64, false, 10400, 60000, 10400);
  aux.update(64, false, 13400, 60000, 13400);
  aux.update(0, true, 13500, 60000, 13500);
  return !aux.on();
}

static_assert(auxHoldAndExpiry(), "AUX should latch once and expire only after inactivity.");
static_assert(auxToggleAndStop(), "AUX needs a new hold, and Stop always clears it.");

constexpr bool resetRequiresNeutralAndRelease() {
  StopResetGesture gesture;
  const auto both = StopResetGesture::buttons;
  if (gesture.update(both, 0) || gesture.update(both, 3001) || gesture.update(0, 3002)) return false;
  if (gesture.update(both, 3100) || gesture.update(both, 6099) || gesture.update(0, 6100)) return false;
  if (gesture.update(both, 6200) || gesture.update(both, 9200) || gesture.update(128, 9201)) return false;
  if (gesture.update(0, 9202) || !gesture.update(0, 9227) ||
      !gesture.update(0, 9476) || gesture.update(0, 9477)) return false;
  gesture.update(both, 10000);
  gesture.update(both, 13000);
  gesture.requireRelease();
  return !gesture.update(0, 13001);
}

static_assert(resetRequiresNeutralAndRelease(), "Reset requires a new hold and release after reconnect.");

constexpr bool powerAndSelectionAreDistinct() {
  using Gesture = ReceiverSelectGesture;
  Gesture gesture;
  if (gesture.update(64, 100) != Gesture::normal ||
      gesture.update(64, 3300) != Gesture::normal ||
      gesture.update(0, 3400) != Gesture::normal) return false;
  if (gesture.update(64, 4000) != Gesture::normal ||
      gesture.update(0, 4090) != Gesture::normal ||
      gesture.update(64, 4250) != Gesture::suppressPower ||
      gesture.update(64, 6249) != Gesture::suppressPower ||
      gesture.update(64, 6250) != Gesture::selectNext ||
      gesture.update(64, 9300) != Gesture::suppressPower) return false;
  gesture.update(0, 9500);
  gesture.update(64, 9600);
  gesture.update(0, 9700);
  gesture.update(64, 9890);
  gesture.update(192, 10000);
  return gesture.update(64, 13000) == Gesture::suppressPower;
}

static_assert(powerAndSelectionAreDistinct(), "A normal Power hold stays available and Stop cancels selection.");

constexpr bool recoveryAcrossRollover() {
  SenderRadioRecovery radio;
  const uint32_t start = UINT32_MAX - 1000;
  radio.begin(start);
  if (radio.linked(start) || radio.retry(start + 2999) || !radio.retry(start + 3000)) return false;
  radio.heard(start + 500);
  if (!radio.linked(start + 1149) || radio.linked(start + 1150)) return false;
  radio.attempted(start + 59700);
  if (radio.rest(start + 60499) || !radio.rest(start + 60500)) return false;
  radio.activity(start + 60500);
  if (radio.rest(start + 65499) || !radio.rest(start + 65500)) return false;
  radio.suspend(start + 65500);
  return !radio.probe(start + 67699) && radio.probe(start + 67700);
}

constexpr bool connectedForFullDay() {
  SenderRadioRecovery radio;
  radio.begin(0);
  for (uint32_t now = 0; now < 24u * 60u * 60u * 1000u; now += 1000) {
    radio.heard(now);
    if (!radio.linked(now) || radio.retry(now) || radio.rest(now)) return false;
  }
  return true;
}

constexpr bool overnightSearchResumes() {
  SenderRadioRecovery radio;
  radio.begin(0);
  radio.attempted(60000);
  for (uint32_t now = 60800; now < 12u * 60u * 60u * 1000u; now += 3000) {
    if (!radio.rest(now)) return false;
    radio.suspend(now);
    if (radio.probe(now + 2199) || !radio.probe(now + 2200)) return false;
    radio.attempted(now + 2200);
  }
  const uint32_t returned = 13u * 60u * 60u * 1000u;
  radio.heard(returned);
  if (!radio.linked(returned) || radio.rest(returned) || radio.retry(returned)) return false;
  radio.lost();
  return !radio.linked(returned);
}

static_assert(recoveryAcrossRollover(), "Radio leases and retries must survive millis rollover.");
static_assert(connectedForFullDay(), "An idle connected handheld must not suspend its radio.");
static_assert(overnightSearchResumes(), "An out-of-range handheld must keep searching and accept a returning receiver.");
}
