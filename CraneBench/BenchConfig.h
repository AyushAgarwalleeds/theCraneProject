#pragma once

// Set to 0 for the handheld; set to 1 for the receiver.
#ifndef CRANE_BENCH_RECEIVER
#define CRANE_BENCH_RECEIVER 1
#endif
#define CRANE_BENCH_PHONE 1

// Change the setup password. Copy the same two private keys to both boards.
constexpr char benchPassword[] = "CHANGE-THIS-BENCH-PASS";
constexpr uint8_t primaryKey[16] = {0x91,0x45,0x27,0xa5,0x06,0xd6,0xc7,0x38,0x49,0x5a,0xbb,0x7c,0xde,0xe9,0xf5,0x10};
constexpr uint8_t peerKey[16] = {0x23,0xb4,0x35,0x46,0x57,0x68,0x79,0x8a,0x9b,0xac,0xbd,0xce,0x1f,0x20,0x31,0x42};

// Classic ESP32-WROOM dev board. Active-low bench switches only.
// Input order: up, down, left, right, front, back, simulated stop, AUX.
constexpr int buttonPins[] = {4, 32, 33, 25, 26, 27, 14, 13};
constexpr int modePin = 23; // Proposed pairing button GPIO on both boards.
constexpr unsigned heartbeatMs = 60;
constexpr unsigned debounceMs = 25;
