#pragma once

#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include "RadioLink.h"
#include "SerialLine.h"
#include "StopResetGesture.h"

class Handheld {
public:
  void begin() {
    Serial.begin(115200);
    for (int pin : pins_) pinMode(pin, INPUT_PULLUP);
    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.print("\nSender MAC: ");
    Serial.println(formatMac(mac));
    Serial.println("Enter: pair RECEIVER_MAC 32_HEX_KEY");
    Serial.println("Enter: info");
    WiFi.persistent(false);
    if (storage_.begin("cranelink-tx", false)) {
      storageReady_ = true;
      if (storage_.getBytesLength("setup-v2") == sizeof(setup_))
        storage_.getBytes("setup-v2", &setup_, sizeof(setup_));
      else if (storage_.getBytesLength("peer") == sizeof(setup_.peer))
        storage_.getBytes("peer", &setup_.peer, sizeof(setup_.peer));
    }
    ready_ = (!setup_.local[0] || esp_iface_mac_addr_set(setup_.local, ESP_MAC_WIFI_STA) == ESP_OK) &&
             WiFi.mode(WIFI_STA) && WiFi.setSleep(false) &&
             esp_wifi_set_channel(radioChannel, WIFI_SECOND_CHAN_NONE) == ESP_OK && link_.begin();
    if (!ready_) {
      Serial.println("ESP-NOW failed to start.");
      return;
    }
    if (!link_.setPeer(setup_.peer)) Serial.println("Could not load the saved receiver.");
    showInfo();
  }

  void loop() {
    serial_.poll([&](String line) { processLine(line); });
    const uint32_t now = millis();
    const uint8_t raw = readButtons(now);
    if (nonce_ && uint32_t(now - lastBeacon_) >= 650) {
      nonce_ = 0;
      needNeutral_ = true;
      neutralSince_ = now;
      resetGesture_.requireRelease();
      Serial.println("Receiver link lost.");
    }
    ReceivedFrame item;
    for (int n = 0; n < 8 && link_.next(item); ++n) {
      const RadioFrame& frame = item.frame;
      if (!frame.nonce || frame.sequence || frame.command) continue;
      if (!nonce_) Serial.println("Receiver link found.");
      lastBeacon_ = millis();
      if (nonce_ != frame.nonce) {
        nonce_ = frame.nonce;
        if (sequenceNonce_ != nonce_) {
          sequence_ = 0;
          sequenceNonce_ = nonce_;
        }
        needNeutral_ = true;
        neutralSince_ = now;
        resetGesture_.requireRelease();
      }
    }
    if (!nonce_) return;
    const uint8_t next = command(raw, now);
    if (!sequence_ || next != sent_ || uint32_t(now - lastSend_) >= 65) {
      sent_ = next;
      lastSend_ = now;
      link_.send(nonce_, ++sequence_, next);
    }
  }

private:
  struct SenderSetup {
    PeerSettings peer;
    uint8_t local[6]{};
  };
  // Up, Down, Left, Right, Front, Back, AUX, Stop. Switches close to GND.
  static constexpr int pins_[8] = {4, 32, 33, 25, 26, 27, 13, 14};

  uint8_t readButtons(uint32_t now) {
    uint8_t raw = 0;
    for (int i = 0; i < 8; ++i) {
      const uint8_t bit = 1u << i;
      if (digitalRead(pins_[i]) == LOW) raw |= bit;
      if ((raw ^ lastRaw_) & bit) changed_[i] = now;
      if (!(raw & bit)) filtered_ &= ~bit;
      else if (uint32_t(now - changed_[i]) >= 25) filtered_ |= bit;
    }
    lastRaw_ = raw;
    return raw;
  }

  uint8_t command(uint8_t raw, uint32_t now) {
    if (raw) neutralSince_ = now;
    else if (uint32_t(now - neutralSince_) >= 25) needNeutral_ = false;
    if (resetGesture_.update(raw, now)) return resetStopCommand;
    if (raw & stopCommand) {
      needNeutral_ = true;
      return stopCommand;
    }
    return needNeutral_ ? waitingForRelease : filtered_;
  }

  void processLine(String line) {
    line.trim();
    if (line == "info") {
      showInfo();
      return;
    }
    if (line == "identify") {
      uint8_t mac[6]{}, factory[6]{};
      esp_efuse_mac_get_default(factory);
      if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) memcpy(mac, factory, sizeof(mac));
      Serial.printf("CL IDENT SENDER %s %s\n", formatMac(mac).c_str(), formatMac(factory).c_str());
      return;
    }
    if (line == "setup-info") {
      Serial.printf("CL PROTOCOL %u\n", unsigned(wireVersion));
      Serial.printf("CL TARGET %s %u %u %u\n",
                    setup_.peer.configured() ? formatMac(setup_.peer.mac).c_str() : "-",
                    unsigned(WiFi.channel()), unsigned(ready_ && link_.paired()), unsigned(nonce_ != 0));
      uint8_t raw = 0;
      for (int i = 0; i < 8; ++i) {
        if (digitalRead(pins_[i]) == LOW) raw |= 1u << i;
      }
      Serial.printf("CL INPUTS %02X\n", unsigned(raw));
      Serial.println("CL INFO DONE");
      return;
    }
    const bool assignLocal = line.startsWith("pair-as ");
    const int start = assignLocal ? 8 : 5;
    const int separator = line.indexOf(' ', start);
    const int localStart = assignLocal && separator >= 0 ? line.indexOf(' ', separator + 1) : -1;
    SenderSetup next = setup_;
    if ((!assignLocal && !line.startsWith("pair ")) || separator < 0 ||
        !parseMac(line.substring(start, separator), next.peer.mac) ||
        !parseKey(localStart < 0 ? line.substring(separator + 1) :
                 line.substring(separator + 1, localStart), next.peer.key) || !next.peer.configured() ||
        (assignLocal && (localStart < 0 || !parseMac(line.substring(localStart + 1), next.local) ||
                         (next.local[0] & 3) != 2))) {
      Serial.println("Use: pair RECEIVER_MAC 32_HEX_KEY");
      return;
    }
    uint8_t own[6]{};
    esp_wifi_get_mac(WIFI_IF_STA, own);
    if (!memcmp(next.local[0] ? next.local : own, next.peer.mac, sizeof(own))) {
      Serial.println("Enter the receiver MAC, which must differ from this sender MAC.");
      return;
    }
    if (!ready_) {
      Serial.println("CL ERROR radio unavailable");
      return;
    }
    const bool changeMac = assignLocal && memcmp(next.local, own, sizeof(own));
    if (!changeMac && !link_.setPeer(next.peer)) {
      link_.setPeer(setup_.peer);
      Serial.println("CL ERROR could not set receiver peer");
      return;
    }
    if (!storageReady_ || storage_.putBytes("setup-v2", &next, sizeof(next)) != sizeof(next)) {
      if (!changeMac) link_.setPeer(setup_.peer);
      Serial.println("CL ERROR could not save the receiver");
      return;
    }
    setup_ = next;
    nonce_ = 0;
    Serial.println("CL PAIRED");
    if (changeMac) {
      Serial.flush();
      ESP.restart();
    }
    showInfo();
  }

  void showInfo() {
    uint8_t mac[6]{};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.printf("Sender MAC %s, channel %u. Receiver %s\n", formatMac(mac).c_str(),
                  unsigned(radioChannel), setup_.peer.configured() ? formatMac(setup_.peer.mac).c_str() : "not set");
  }

  RadioLink link_;
  Preferences storage_;
  SenderSetup setup_;
  bool ready_ = false;
  bool storageReady_ = false;
  bool needNeutral_ = true;
  uint64_t nonce_ = 0;
  uint64_t sequenceNonce_ = 0;
  uint32_t sequence_ = 0, lastBeacon_ = 0, lastSend_ = 0, neutralSince_ = 0;
  uint32_t changed_[8]{};
  uint8_t lastRaw_ = 0, filtered_ = 0, sent_ = 0;
  SerialLine serial_;
  StopResetGesture resetGesture_;
};
