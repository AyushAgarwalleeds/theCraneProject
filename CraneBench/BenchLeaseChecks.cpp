#include "BenchLease.h"

namespace {
constexpr bool firstDirectionWins(uint8_t first, uint8_t opposite, uint8_t other) {
  BenchLease lease;
  lease.select(Source::handheld);
  return lease.claim(Source::handheld, 0) &&
         lease.command(true, 1, 0, 1) &&
         lease.command(true, 2, first, 2) && lease.mask == first &&
         lease.command(true, 3, first | opposite | other, 3) && lease.mask == (first | other) &&
         lease.command(true, 4, opposite | other, 4) && lease.mask == other &&
         lease.command(true, 5, other, 5) &&
         lease.command(true, 6, opposite | other, 6) && lease.mask == (opposite | other);
}

constexpr bool simultaneousAndTimeout() {
  BenchLease lease;
  lease.select(Source::handheld);
  if (!lease.claim(Source::handheld, 0) || !lease.command(true, 1, 0, 1) ||
      !lease.command(true, 2, 3, 2) || lease.mask ||
      !lease.command(true, 3, 1, 3) || lease.mask ||
      !lease.command(true, 4, 0, 4) ||
      !lease.command(true, 5, 1, 5) || lease.mask != 1) return false;
  lease.expire(5 + BenchLease::timeoutMs);
  return !lease.active() && lease.mask == 0 &&
         !lease.command(true, 6, 1, 5 + BenchLease::timeoutMs);
}

static_assert(firstDirectionWins(1, 2, 4), "Up takes priority while held.");
static_assert(firstDirectionWins(2, 1, 4), "Down takes priority while held.");
static_assert(firstDirectionWins(4, 8, 16), "Left takes priority while held.");
static_assert(firstDirectionWins(8, 4, 16), "Right takes priority while held.");
static_assert(firstDirectionWins(16, 32, 1), "Front takes priority while held.");
static_assert(firstDirectionWins(32, 16, 1), "Back takes priority while held.");
static_assert(simultaneousAndTimeout(), "An ambiguous axis waits for neutral, and expiry clears commands.");
}
