#pragma once

#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include "RadioLink.h"
#include "SerialLine.h"
#include "StopResetGesture.h"
#include "ReceiverSelectGesture.h"
#include "HandheldIndicators.h"
#include "MasterSetupPage.h"

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
    memcpy(stationMac_, setup_.local[0] ? setup_.local : mac, sizeof(stationMac_));
    const bool macReady = !setup_.local[0] || esp_iface_mac_addr_set(setup_.local, ESP_MAC_WIFI_STA) == ESP_OK;
    setupMode_ = setupRequestedAtBoot();
    const bool wifiReady = WiFi.mode(setupMode_ ? WIFI_AP : WIFI_STA);
    if (wifiReady) {
      uint8_t factory[6]{};
      esp_efuse_mac_get_default(factory);
      loadSetupNetwork(factory);
    }
    if (setupMode_) {
      if (wifiReady && macReady) startSetupPortal();
      if (!portalReady_) {
        WiFi.mode(WIFI_OFF);
        Serial.println("Handheld Wi-Fi setup could not start. Power cycle and try again.");
      }
      return;
    }
    ready_ = macReady && wifiReady && WiFi.setSleep(false) &&
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
    if (setupMode_) {
      indicators_.update(now, false, false, batteryEnabled_, portalReady_);
      if (restartPending_ && uint32_t(now - restartRequestedAt_) >= 250) ESP.restart();
      if (portalReady_) {
        portal_.handleClient();
        if (uint32_t(now - lastSetupUse_) >= 15u * 60u * 1000u) {
          portal_.stop();
          WiFi.mode(WIFI_OFF);
          portalReady_ = false;
        }
      }
      return;
    }
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

  bool setupRequestedAtBoot() {
    const uint32_t start = millis();
    // Require only Power to be held continuously throughout startup.
    while (readButtons(millis()) == 64) {
      if (uint32_t(millis() - start) >= 3000) return true;
      delay(10);
    }
    return false;
  }

  static String randomHex() {
    uint8_t bytes[12];
    esp_fill_random(bytes, sizeof(bytes));
    char hex[sizeof(bytes) * 2 + 1]{};
    for (size_t i = 0; i < sizeof(bytes); ++i)
      snprintf(hex + 2 * i, 3, "%02X", unsigned(bytes[i]));
    return String(hex);
  }

  void loadSetupNetwork(const uint8_t* factory) {
    String address = formatMac(factory);
    address.replace(":", "");
    setupSsid_ = "CraneLink-TX-" + address;
    if (!storageReady_) return;
    setupPassword_ = storage_.getString("tx-wifi-key", "");
    if (setupPassword_.length() != 24) {
      setupPassword_ = randomHex();
      if (storage_.putString("tx-wifi-key", setupPassword_) != setupPassword_.length()) setupPassword_ = "";
    }
  }

  bool parseTarget(Target& target, const String& mac, const String& key, const String& name) const {
    if (!name.length() || name.length() >= sizeof(target.name) ||
        !parseMac(mac, target.peer.mac) || !parseKey(key, target.peer.key) ||
        !target.peer.configured() || !memcmp(target.peer.mac, stationMac_, 6)) return false;
    for (size_t i = 0; i < name.length(); ++i) {
      const char c = name[i];
      if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z') &&
          !(c >= 'a' && c <= 'z') && c != '-') return false;
    }
    name.toCharArray(target.name, sizeof(target.name));
    return true;
  }

  void sendSetupPage(int code = 200, const char* message = nullptr) {
    String page;
    page.reserve(4800);
    page += FPSTR(masterSetupHead);
    if (message) page += String(F("<p class=\"notice\" role=\"status\">")) + message + F("</p>");
    page += F("<p>Enter this sender station MAC on each receiver's settings page. <code>");
    page += formatMac(stationMac_) + F("</code></p><h2>Receiver order</h2>");
    if (!setup_.count) page += F("<p>Add your first receiver below.</p>");
    else {
      page += F("<ol>");
      for (uint8_t i = 0; i < setup_.count; ++i) {
        const auto& target = setup_.targets[i];
        page += String(F("<li>")) + target.name + F(" <code>") + formatMac(target.peer.mac) + F("</code></li>");
      }
      page += F("</ol>");
    }
    const String nonceField = String(F("<input type=\"hidden\" name=\"nonce\" value=\"")) + setupNonce_ + F("\">");
    page += String(F("<form method=\"post\" action=\"/receiver\">")) + nonceField;
    page += FPSTR(masterSetupFields);
    page += String(F("<form method=\"post\" action=\"/restart\">")) + nonceField;
    page += F("<button type=\"submit\">Finish and restart handheld</button></form></main></html>");
    portal_.sendHeader("Cache-Control", "no-store");
    portal_.sendHeader("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");
    portal_.send(code, "text/html; charset=utf-8", page);
  }

  bool allowSetupPost() {
    if (restartPending_ || portal_.arg("nonce") != setupNonce_) {
      portal_.send(403, "text/plain", "Open the handheld setup page again.");
      return false;
    }
    lastSetupUse_ = millis();
    return true;
  }

  void addWirelessTarget() {
    if (!allowSetupPost()) return;
    Target target;
    String name = portal_.arg("name"), mac = portal_.arg("mac"), key = portal_.arg("key");
    name.trim();
    mac.trim();
    key.trim();
    if (!parseTarget(target, mac, key, name)) {
      sendSetupPage(400, "Check the label, receiver station MAC and 32-digit shared key.");
      return;
    }
    TargetSetup next = setup_;
    uint8_t slot = 0;
    while (slot < next.count && memcmp(next.targets[slot].peer.mac, target.peer.mac, 6)) ++slot;
    if (slot == maxSenders) {
      sendSetupPage(409, "This handheld already has four receivers. Use USB master setup to replace the list.");
      return;
    }
    if (slot == next.count) ++next.count;
    next.targets[slot] = target;
    if (!saveSetup(next)) {
      sendSetupPage(500, "Could not save the receiver. Try again.");
      return;
    }
    setup_ = next;
    sendSetupPage(200, "Receiver saved. Add another or finish setup.");
  }

  void startSetupPortal() {
    const IPAddress ip(192, 168, 4, 1);
    if (!setupPassword_.length() || !WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) ||
        !WiFi.softAP(setupSsid_.c_str(), setupPassword_.c_str(), radioChannel, false, 1)) {
      return;
    }
    setupNonce_ = randomHex();
    portal_.on("/", HTTP_GET, [this] {
      lastSetupUse_ = millis();
      sendSetupPage();
    });
    portal_.on("/receiver", HTTP_POST, [this] { addWirelessTarget(); });
    portal_.on("/restart", HTTP_POST, [this] {
      if (!allowSetupPost()) return;
      if (readButtons(millis())) {
        sendSetupPage(409, "Release the handheld buttons, then retry.");
        return;
      }
      portal_.sendHeader("Cache-Control", "no-store");
      portal_.send(200, "text/plain", "Restarting the handheld. Disconnect your phone from the receiver Wi-Fi before using the handheld.");
      restartPending_ = true;
      restartRequestedAt_ = millis();
    });
    portal_.begin();
    lastSetupUse_ = millis();
    portalReady_ = true;
  }

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
      bool valid = keyEnd > macEnd && parseTarget(next, line.substring(12, macEnd),
                                                  line.substring(macEnd + 1, keyEnd), name);
      for (uint8_t i = 0; valid && i < pending_.count; ++i)
        valid = memcmp(next.peer.mac, pending_.targets[i].peer.mac, 6) != 0;
      if (!valid) {
        expectedTargets_ = 0;
        Serial.println("CL ERROR Invalid or duplicate receiver.");
        return;
      }
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
      uint8_t factory[6]{};
      esp_efuse_mac_get_default(factory);
      Serial.printf("CL IDENT SENDER %s %s\n", formatMac(stationMac_).c_str(), formatMac(factory).c_str());
      return;
    }
    if (line == "setup-info") {
      Serial.printf("CL PROTOCOL %u\n", unsigned(wireVersion));
      Serial.println("CL FEATURES MASTER BATTERY WIRELESS");
      if (setupPassword_.length())
        Serial.printf("CL SETUP AP %s %s 192.168.4.1 %u\n", setupSsid_.c_str(), setupPassword_.c_str(), unsigned(portalReady_));
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
    if (!memcmp(next.local[0] ? next.local : stationMac_, peer.mac, sizeof(stationMac_))) {
      Serial.println("Enter the receiver MAC, which must differ from this sender MAC.");
      return;
    }
    if (!ready_) {
      Serial.println("CL ERROR radio unavailable");
      return;
    }
    const bool changeMac = assignLocal && memcmp(next.local, stationMac_, sizeof(stationMac_));
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
    Serial.printf("Sender MAC %s, channel %u. Receiver %s\n", formatMac(stationMac_).c_str(),
                  unsigned(radioChannel), setup_.count ? formatMac(setup_.targets[selected_].peer.mac).c_str() : "not set");
  }

  RadioLink link_;
  Preferences storage_;
  WebServer portal_{80};
  String setupSsid_, setupPassword_, setupNonce_;
  uint8_t stationMac_[6]{};
  uint32_t lastSetupUse_ = 0, restartRequestedAt_ = 0;
  bool setupMode_ = false, portalReady_ = false, restartPending_ = false;
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
