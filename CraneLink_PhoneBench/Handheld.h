#pragma once

#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include "RadioLink.h"
#include "SerialLine.h"
#include "StopResetGesture.h"
#include "ReceiverSelectGesture.h"
#include "HandheldIndicators.h"

class Handheld {
public:
  void begin() {
    Serial.begin(115200);
    for (int pin : pins_) pinMode(pin, INPUT_PULLUP);
    indicators_.begin();
    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.print("\nSender MAC: ");
    Serial.println(formatMac(mac));
    Serial.println("Enter: pair RECEIVER_MAC 32_HEX_KEY");
    Serial.println("Enter: info");
    WiFi.persistent(false);
    if (storage_.begin("cranelink-tx", false)) {
      storageReady_ = true;
      loadSetup();
      batteryEnabled_ = storage_.getBool("battery", false);
    }
    ready_ = (!setup_.local[0] || esp_iface_mac_addr_set(setup_.local, ESP_MAC_WIFI_STA) == ESP_OK) &&
             WiFi.mode(WIFI_STA) && WiFi.setSleep(false) &&
             esp_wifi_set_channel(radioChannel, WIFI_SECOND_CHAN_NONE) == ESP_OK && link_.begin();
    if (!ready_) {
      Serial.println("ESP-NOW failed to start.");
      return;
    }
    activate(0, millis());
    showInfo();
  }

  void loop() {
    serial_.poll([&](String line) { processLine(line); });
    const uint32_t now = millis();
    const uint8_t raw = readButtons(now);
    indicators_.update(now, setup_.count != 0, nonce_ != 0 && !switching_, batteryEnabled_);
    const uint8_t selectionButtons = (filtered_ & 64) | (raw & ~uint8_t(64));
    const auto gesture = setup_.count > 1 ? selectGesture_.update(selectionButtons, now) : ReceiverSelectGesture::normal;
    if (gesture == ReceiverSelectGesture::selectNext && raw == 64) {
      switching_ = true;
      switchStarted_ = now;
    }
    if (raw & stopCommand) switching_ = false;
    if (switching_ && uint32_t(now - switchStarted_) >= 1000) {
      switching_ = false;
      activate((selected_ + 1) % setup_.count, now);
    }
    if (ready_ && setup_.count && !link_.paired() && uint32_t(now - lastPeerAttempt_) >= 1000)
      activate(selected_, now, false);
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
    uint8_t next = command(raw, now);
    if (switching_) next = waitingForRelease;
    else if (gesture != ReceiverSelectGesture::normal && next < stopCommand) next &= ~uint8_t(64);
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
  struct Target {
    PeerSettings peer;
    char name[33]{};
  };
  struct TargetSetup {
    Target targets[maxSenders];
    uint8_t local[6]{};
    uint8_t count = 0;
  };

  void loadSetup() {
    if (storage_.getBytesLength("targets-v1") == sizeof(setup_) &&
        storage_.getBytes("targets-v1", &setup_, sizeof(setup_)) == sizeof(setup_) &&
        setup_.count && setup_.count <= maxSenders) return;
    setup_ = TargetSetup{};
    SenderSetup old;
    if (storage_.getBytesLength("setup-v2") == sizeof(old))
      storage_.getBytes("setup-v2", &old, sizeof(old));
    else if (storage_.getBytesLength("peer") == sizeof(old.peer))
      storage_.getBytes("peer", &old.peer, sizeof(old.peer));
    memcpy(setup_.local, old.local, sizeof(old.local));
    setup_.targets[0].peer = old.peer;
    setup_.count = old.peer.configured() ? 1 : 0;
  }

  void activate(uint8_t slot, uint32_t now, bool show = true) {
    selected_ = slot;
    lastPeerAttempt_ = now;
    nonce_ = sequenceNonce_ = 0;
    sequence_ = 0;
    needNeutral_ = true;
    neutralSince_ = now;
    resetGesture_.requireRelease();
    if (show && setup_.count > 1) indicators_.select(slot + 1, now);
    if (!link_.setPeer(setup_.targets[slot].peer)) Serial.println("Could not load the saved receiver.");
  }

  bool saveSetup(const TargetSetup& next) {
    if (!storageReady_ || storage_.putBytes("targets-v1", &next, sizeof(next)) != sizeof(next)) {
      Serial.println("CL ERROR Could not save receiver pairing.");
      return false;
    }
    return true;
  }

  void configureMaster(const String& line) {
    if (line == "battery 0" || line == "battery 1") {
      const bool enable = line.endsWith("1");
      if (storageReady_ && storage_.putBool("battery", enable) == 1) {
        batteryEnabled_ = enable;
        Serial.println("CL BATTERY SAVED");
      } else Serial.println("CL ERROR Battery setting could not be saved.");
      return;
    }
    if (line.startsWith("targets-begin ")) {
      pending_ = TargetSetup{};
      memcpy(pending_.local, setup_.local, sizeof(setup_.local));
      expectedTargets_ = 0;
      if (line.length() == 15 && line[14] >= '2' && line[14] <= '0' + maxSenders) {
        expectedTargets_ = line[14] - '0';
        Serial.println("CL TARGETS READY");
      } else Serial.println("CL ERROR Select two to four receivers.");
      return;
    }
    if (line.startsWith("targets-add ") && pending_.count < expectedTargets_) {
      const int macEnd = line.indexOf(' ', 12);
      const int keyEnd = macEnd < 0 ? -1 : line.indexOf(' ', macEnd + 1);
      Target next;
      const String name = keyEnd < 0 ? "" : line.substring(keyEnd + 1);
      bool valid = keyEnd > macEnd && name.length() && name.length() < sizeof(next.name) &&
                   parseMac(line.substring(12, macEnd), next.peer.mac) &&
                   parseKey(line.substring(macEnd + 1, keyEnd), next.peer.key) && next.peer.configured();
      for (size_t i = 0; valid && i < name.length(); ++i) {
        const char c = name[i];
        valid = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
      }
      for (uint8_t i = 0; valid && i < pending_.count; ++i)
        valid = memcmp(next.peer.mac, pending_.targets[i].peer.mac, 6) != 0;
      uint8_t own[6]{};
      valid = valid && esp_wifi_get_mac(WIFI_IF_STA, own) == ESP_OK && memcmp(next.peer.mac, own, 6);
      if (!valid) {
        expectedTargets_ = 0;
        Serial.println("CL ERROR Invalid or duplicate receiver.");
        return;
      }
      name.toCharArray(next.name, sizeof(next.name));
      pending_.targets[pending_.count++] = next;
      Serial.println("CL TARGET ADDED");
      return;
    }
    if (line == "targets-save" && expectedTargets_ && pending_.count == expectedTargets_) {
      if (!ready_ || readButtons(millis()) || switching_) {
        Serial.println("CL ERROR Release all sender buttons and retry.");
        return;
      }
      if (!saveSetup(pending_)) return;
      if (nonce_) link_.send(nonce_, ++sequence_, waitingForRelease);
      setup_ = pending_;
      expectedTargets_ = 0;
      activate(0, millis());
      Serial.println("CL TARGETS SAVED");
      return;
    }
    Serial.println("CL ERROR Receiver list is incomplete. Start the save again.");
  }
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
    if (indicators_.showingSelection()) {
      needNeutral_ = true;
      neutralSince_ = now;
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
      Serial.println("CL FEATURES MASTER BATTERY");
      Serial.printf("CL BATTERY %u %lu %u\n", unsigned(batteryEnabled_),
                    static_cast<unsigned long>(indicators_.batteryMv()), unsigned(indicators_.low()));
      for (uint8_t i = 0; i < setup_.count; ++i) {
        const auto& target = setup_.targets[i];
        Serial.printf("CL PICK %u %s %s %u\n", i + 1, formatMac(target.peer.mac).c_str(),
                      target.name[0] ? target.name : "-", unsigned(i == selected_));
      }
      Serial.printf("CL TARGET %s %u %u %u\n",
                    setup_.count ? formatMac(setup_.targets[selected_].peer.mac).c_str() : "-",
                    unsigned(WiFi.channel()), unsigned(ready_ && link_.paired()), unsigned(nonce_ != 0));
      uint8_t raw = 0;
      for (int i = 0; i < 8; ++i) {
        if (digitalRead(pins_[i]) == LOW) raw |= 1u << i;
      }
      Serial.printf("CL INPUTS %02X\n", unsigned(raw));
      Serial.println("CL INFO DONE");
      return;
    }
    if (line.startsWith("targets-") || line.startsWith("battery ")) {
      configureMaster(line);
      return;
    }
    const bool assignLocal = line.startsWith("pair-as ");
    const int start = assignLocal ? 8 : 5;
    const int separator = line.indexOf(' ', start);
    const int localStart = assignLocal && separator >= 0 ? line.indexOf(' ', separator + 1) : -1;
    TargetSetup next;
    memcpy(next.local, setup_.local, sizeof(next.local));
    auto& peer = next.targets[0].peer;
    next.count = 1;
    if ((!assignLocal && !line.startsWith("pair ")) || separator < 0 ||
        !parseMac(line.substring(start, separator), peer.mac) ||
        !parseKey(localStart < 0 ? line.substring(separator + 1) :
                 line.substring(separator + 1, localStart), peer.key) || !peer.configured() ||
        (assignLocal && (localStart < 0 || !parseMac(line.substring(localStart + 1), next.local) ||
                         (next.local[0] & 3) != 2))) {
      Serial.println("Use: pair RECEIVER_MAC 32_HEX_KEY");
      return;
    }
    uint8_t own[6]{};
    esp_wifi_get_mac(WIFI_IF_STA, own);
    if (!memcmp(next.local[0] ? next.local : own, peer.mac, sizeof(own))) {
      Serial.println("Enter the receiver MAC, which must differ from this sender MAC.");
      return;
    }
    if (!ready_) {
      Serial.println("CL ERROR radio unavailable");
      return;
    }
    const bool changeMac = assignLocal && memcmp(next.local, own, sizeof(own));
    if (!changeMac && !link_.setPeer(peer)) {
      activate(selected_, millis(), false);
      Serial.println("CL ERROR could not set receiver peer");
      return;
    }
    if (!saveSetup(next)) {
      if (!changeMac) activate(selected_, millis(), false);
      return;
    }
    setup_ = next;
    Serial.println("CL PAIRED");
    if (changeMac) {
      Serial.flush();
      ESP.restart();
    }
    activate(0, millis());
    showInfo();
  }

  void showInfo() {
    uint8_t mac[6]{};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.printf("Sender MAC %s, channel %u. Receiver %s\n", formatMac(mac).c_str(),
                  unsigned(radioChannel), setup_.count ? formatMac(setup_.targets[selected_].peer.mac).c_str() : "not set");
  }

  RadioLink link_;
  Preferences storage_;
  TargetSetup setup_, pending_;
  HandheldIndicators indicators_;
  ReceiverSelectGesture selectGesture_;
  uint8_t selected_ = 0, expectedTargets_ = 0;
  uint32_t lastPeerAttempt_ = 0, switchStarted_ = 0;
  bool switching_ = false, batteryEnabled_ = false;
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
