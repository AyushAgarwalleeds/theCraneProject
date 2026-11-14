#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "PhoneSession.h"
#include "AuxLatch.h"

// IN1–IN8 on the original CraneLink receiver. Change the trigger level
// here if the relay module's inputs are active-high.
constexpr gpio_num_t relayPins[] = {
  GPIO_NUM_26, GPIO_NUM_21, GPIO_NUM_33, GPIO_NUM_32,
  GPIO_NUM_27, GPIO_NUM_14, GPIO_NUM_13, GPIO_NUM_4
};
constexpr bool relayActiveLow = true;
constexpr uint64_t firstHandheldToken = UINT64_MAX - 3;

struct RelayState {
  uint8_t mask;
  uint8_t requested;
  uint8_t relayMask;
  bool armed;
  bool stopped;
  bool handheld;
  bool auxOn;
  uint32_t auxRemainingSeconds;
  uint32_t auxHoldMs;
};

class RelayController {
public:
  bool begin(const uint8_t* assignment, uint16_t idleMinutes, bool interlock) {
    // Preload the inactive level before enabling the outputs.
    gpio_config_t config{};
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = relayActiveLow ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    config.pull_down_en = relayActiveLow ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    for (gpio_num_t pin : relayPins) {
      if (gpio_set_level(pin, levelFor(false)) != ESP_OK) return false;
      config.pin_bit_mask |= 1ULL << pin;
    }
    if (gpio_config(&config) != ESP_OK) return false;

    setOptions(assignment, idleMinutes);
    session_.setInterlock(interlock);
    mutex_ = xSemaphoreCreateMutex();
    return mutex_ &&
           xTaskCreate(monitor, "relay-expiry", 3072, this, 2, nullptr) == pdPASS;
  }

  template<class Change>
  RelayState update(Change change) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const uint32_t now = millis();
    session_.tick(now);
    change(session_, now);
    const uint8_t mask = session_.mask();
    const uint32_t confirmedAt = session_.lastCommandAt();
    aux_.update(mask, session_.stopped(), now, idleMs_, confirmedAt);
    const uint8_t logicalOutputs = (mask & 63) | (aux_.on() ? AuxLatch::command : 0);
    uint8_t physicalOutputs = (mask & 63) ? enableRelay : 0;
    for (int action = 0; action < 7; ++action) {
      if (logicalOutputs & (1u << action)) physicalOutputs |= 1u << assignment_[action];
    }
    apply(physicalOutputs);
    bool handheld = false;
    for (uint8_t i = 0; i < 4; ++i) handheld |= session_.ownedBy(firstHandheldToken + i);
    const RelayState state{mask, session_.requested(), applied_, session_.armed(), session_.stopped(),
                           handheld, aux_.on(),
                           aux_.remainingSeconds(now, idleMs_), aux_.progress(confirmedAt)};
    xSemaphoreGive(mutex_);
    return state;
  }

  RelayState state() {
    return update([](PhoneSession&, uint32_t) {});
  }

  void configure(const uint8_t* assignment, uint16_t idleMinutes, bool interlock) {
    update([&](PhoneSession& session, uint32_t) {
      setOptions(assignment, idleMinutes);
      session.setInterlock(interlock);
    });
  }

private:
  static constexpr unsigned relayCount = sizeof(relayPins) / sizeof(relayPins[0]);
  static constexpr uint8_t enableRelay = 1u << (relayCount - 1);

  void setOptions(const uint8_t* assignment, uint16_t idleMinutes) {
    memcpy(assignment_, assignment, sizeof(assignment_));
    idleMs_ = uint32_t(idleMinutes) * 60000;
  }

  static uint32_t levelFor(bool on) {
    return uint32_t(on != relayActiveLow);
  }

  void apply(uint8_t next) {
    const uint8_t changed = applied_ ^ next;
    // Drop R8 first when turning off. Energize R8 last on a new jog.
    for (int i = relayCount - 1; i >= 0; --i) {
      const uint8_t bit = 1u << i;
      if (changed & applied_ & bit) gpio_set_level(relayPins[i], levelFor(false));
    }
    for (unsigned i = 0; i < relayCount; ++i) {
      const uint8_t bit = 1u << i;
      if (changed & next & bit) gpio_set_level(relayPins[i], levelFor(true));
    }
    applied_ = next;
  }

  static void monitor(void* arg) {
    auto& controller = *static_cast<RelayController*>(arg);
    constexpr TickType_t interval = pdMS_TO_TICKS(10);
    for (;;) {
      vTaskDelay(interval ? interval : 1);
      controller.state();
    }
  }

  PhoneSession session_;
  AuxLatch aux_;
  uint8_t assignment_[7]{};
  uint32_t idleMs_ = 3600000;
  SemaphoreHandle_t mutex_ = nullptr;
  uint8_t applied_ = 0;
};
