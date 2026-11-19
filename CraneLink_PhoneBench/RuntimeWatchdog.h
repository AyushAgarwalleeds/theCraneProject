#pragma once
#include <Arduino.h>
#include <esp_task_wdt.h>

inline void startRuntimeWatchdog() {
  esp_task_wdt_config_t config{};
  config.timeout_ms = 8000;
  config.trigger_panic = true;
  esp_err_t status = esp_task_wdt_reconfigure(&config);
  if (status == ESP_ERR_INVALID_STATE) status = esp_task_wdt_init(&config);
  if (status != ESP_OK) {
    Serial.println("Could not configure the loop watchdog.");
    return;
  }
  // Arduino feeds the subscribed loop task before every call to loop().
  enableLoopWDT();
}
