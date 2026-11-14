#include "PhoneSession.h"
#include "AuxLatch.h"

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
}
