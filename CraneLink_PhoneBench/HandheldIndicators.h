#pragma once
#include <Arduino.h>

class HandheldIndicators {
public:
  void begin() {
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, LOW);
    ledReady_ = ledcAttach(ledPin, 1000, 8);
    pinMode(buzzerPin, OUTPUT);
    digitalWrite(buzzerPin, LOW);
    buzzerReady_ = ledcAttach(buzzerPin, 2200, 8);
    analogSetPinAttenuation(batteryPin, ADC_11db);
  }

  void select(uint8_t number, uint32_t now) {
    selection_ = number;
    selectedAt_ = now;
    linked_ = heard_ = false;
  }

  void update(uint32_t now, bool paired, bool linked, bool batteryEnabled, bool setupWifi = false, bool resting = false) {
    if (batteryEnabled && uint32_t(now - measuredAt_) >= 250) {
      measuredAt_ = now;
      uint32_t total = 0;
      for (int i = 0; i < 8; ++i) total += analogReadMilliVolts(batteryPin);
      batteryMv_ = total / 4; // Two equal 10 kΩ resistors divide the pack voltage by two.
      if (batteryMv_ < 2000 || batteryMv_ > 4600) {
        batteryMv_ = 0;
        samples_ = 0;
      } else if (batteryMv_ <= 3500 && !low_) {
        if (++samples_ >= 5) {
          low_ = true;
          alarmAt_ = now;
          samples_ = 0;
        }
      } else if (batteryMv_ >= 3650 && low_) {
        if (++samples_ >= 5) { low_ = false; samples_ = 0; }
      } else samples_ = 0;
    }
    if (!batteryEnabled) {
      batteryMv_ = 0;
      low_ = false;
      samples_ = 0;
    }
    if (linked != linked_) {
      linked_ = linked;
      heard_ |= linked;
      chirpAt_ = now;
      chirp_ = true;
    }
    const uint32_t selected = now - selectedAt_;
    if (selection_ && selected >= uint32_t(selection_) * 300 + 350) selection_ = 0;
    if (low_ && uint32_t(now - alarmAt_) >= 30000) alarmAt_ = now;
    const uint32_t alarm = now - alarmAt_;
    const uint32_t chirp = now - chirpAt_;
    if (chirp >= 440) chirp_ = false;

    bool led = linked;
    if (!paired) led = now % 2000 < 100;
    else if (!linked) led = heard_ ? now % 300 < 150 : now % 1000 < 175;
    if (low_ && now % 1600 < 600) led = now % 200 < 100;
    if (selection_) led = selected < uint32_t(selection_) * 300 && selected % 300 < 125;
    if (setupWifi && !low_) led = now % 1500 < 400 && now % 200 < 90;
    if (resting && !low_) led = now % 3000 < 80;
    if (ledReady_) ledcWrite(ledPin, led ? 48 : 0);
    else digitalWrite(ledPin, led ? HIGH : LOW);
    const bool searchSound = paired && !linked && !heard_ && !selection_ && !resting && now % 10000 < 240;
    const bool sounding = low_ ? alarm < 540 && alarm % 180 < 80 :
                          searchSound || (chirp_ && chirp < (linked ? 110u : 360u) && chirp % 180 < 100);
    if (buzzerReady_ && sounding != sounding_) {
      sounding_ = sounding;
      ledcWrite(buzzerPin, sounding ? 128 : 0);
    }
  }

  uint32_t batteryMv() const { return batteryMv_; }
  bool low() const { return low_; }
  bool showingSelection() const { return selection_ != 0; }

private:
  static constexpr uint8_t ledPin = 23, buzzerPin = 18, batteryPin = 34;
  uint32_t measuredAt_ = 0, selectedAt_ = 0, alarmAt_ = 0, chirpAt_ = 0, batteryMv_ = 0;
  uint8_t selection_ = 0, samples_ = 0;
  bool heard_ = false, linked_ = false, chirp_ = false, sounding_ = false, low_ = false, buzzerReady_ = false, ledReady_ = false;
};
