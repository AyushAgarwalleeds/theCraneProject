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
#include "SenderRadioRecovery.h"

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
    // Keep the radio fully awake while linked; reduce CPU power, not radio range.
    setCpuFrequencyMhz(80);
    uint8_t factory[6]{};
    esp_efuse_mac_get_default(factory);
    loadSetupNetwork(factory);
    recovery_.begin(millis());
    if (setupRequestedAtBoot()) enterSetup(millis());
    else startRadio(millis());
    if (!setupMode_ && setup_.count > 1) indicators_.select(selected_ + 1, millis());
    showInfo();
  }

  void loop() {
    serial_.poll([&](String line) { processLine(line); });
    uint32_t now = millis();
    const uint8_t previousRaw = lastRaw_;
    const uint8_t raw = readButtons(now);
    if (raw) recovery_.activity(now);
    if (setupMode_) {
      indicators_.update(now, false, false, batteryEnabled_, portalReady_);
      if (!raw) setupReleased_ = true;
      if (setupReleased_ && raw == 64) setupExitPressed_ = true;
      if ((!raw && setupExitPressed_) ||
          (restartPending_ && uint32_t(now - restartRequestedAt_) >= 250) ||
          uint32_t(now - lastSetupUse_) >= (portalReady_ ? 15u * 60u * 1000u : 3000u)) {
        exitSetup(now);
        return;
      }
      if (portalReady_) {
        portal_.handleClient();
      }
      return;
    }
    // Stop reaches the receiver immediately. Holding Stop alone for eight seconds,
    // then releasing, also opens setup without requiring a physical power switch.
    if (raw == stopCommand) {
      if (!stopSetupHeld_) { stopSetupHeld_ = true; stopSetupAt_ = now; }
      if (uint32_t(now - stopSetupAt_) >= 8000) setupArmed_ = true;
    } else {
      stopSetupHeld_ = false;
      if (!raw && setupArmed_) { enterSetup(now); return; }
      if (raw) setupArmed_ = false;
    }
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
    if (nonce_ && !recovery_.linked(now)) {
      clearSession(now);
      Serial.println("Receiver link lost.");
    }
    const bool wakePress = raw && !previousRaw && !recovery_.linked(now);
    if ((radioResting_ && (raw || recovery_.probe(now))) || wakePress ||
        (!radioResting_ && recovery_.retry(now))) startRadio(now);
    ReceivedFrame item;
    for (int n = 0; n < 8 && link_.next(item); ++n) {
      const RadioFrame& frame = item.frame;
      if (!frame.nonce || frame.sequence || frame.command) continue;
      if (!nonce_) Serial.println("Receiver link found.");
      recovery_.heard(item.arrived);
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
    // Radio start/stop may block briefly; never compare a callback timestamp with
    // an older loop timestamp (which would look like rollover to unsigned math).
    now = millis();
    if (!raw && !switching_ && !radioResting_ && recovery_.rest(now)) {
      stopRadio(now);
      radioResting_ = true;
      recovery_.suspend(now);
    }
    indicators_.update(now, setup_.count != 0, nonce_ != 0 && !switching_, batteryEnabled_, false, radioResting_);
    if (!nonce_ || !ready_) return;
    uint8_t next = command(raw, now);
    if (switching_) next = waitingForRelease;
    else if (gesture != ReceiverSelectGesture::normal && next < stopCommand) next &= ~uint8_t(64);
    const uint32_t interval = next == 0 ? 125 : 65;
    if (!sequence_ || next != sent_ || uint32_t(now - lastSend_) >= interval) {
      // Never wrap a sequence into a packet the receiver could mistake for a beacon.
      if (sequence_ == UINT32_MAX) { startRadio(now); return; }
      sent_ = next;
      lastSend_ = now;
      if (link_.send(nonce_, ++sequence_, next)) sendErrors_ = 0;
      else if (++sendErrors_ >= 3) startRadio(now);
    }
  }

private:
  void clearSession(uint32_t now) {
    nonce_ = 0;
    needNeutral_ = true;
    neutralSince_ = now;
    resetGesture_.requireRelease();
  }

  void stopRadio(uint32_t now) {
    // Deliberately stop renewing the receiver's lease during radio recovery.
    link_.end();
    ready_ = false;
    clearSession(now);
    WiFi.mode(WIFI_OFF);
  }

  void startRadio(uint32_t now) {
    stopRadio(now);
    radioResting_ = false;
    recovery_.lost();
    ++radioStarts_;
    const bool macReady = !setup_.local[0] ||
                          esp_iface_mac_addr_set(setup_.local, ESP_MAC_WIFI_STA) == ESP_OK;
    ready_ = macReady && WiFi.mode(WIFI_STA) && WiFi.setSleep(false) &&
             esp_wifi_set_channel(radioChannel, WIFI_SECOND_CHAN_NONE) == ESP_OK && link_.begin();
    if (ready_ && setup_.count) {
      activate(selected_, now, false, false);
      ready_ = link_.paired();
    }
    if (!ready_) {
      link_.end();
      WiFi.mode(WIFI_OFF);
      Serial.println("ESP-NOW unavailable. Retrying automatically.");
    }
    recovery_.attempted(millis());
  }

  void enterSetup(uint32_t now) {
    if (nonce_ && sequence_ != UINT32_MAX) link_.send(nonce_, ++sequence_, stopCommand);
    stopRadio(now);
    setupMode_ = true;
    switching_ = setupArmed_ = stopSetupHeld_ = false;
    setupReleased_ = setupExitPressed_ = restartPending_ = false;
    selectGesture_ = ReceiverSelectGesture{};
    lastSetupUse_ = now;
    if (WiFi.mode(WIFI_AP)) startSetupPortal();
    if (!portalReady_) Serial.println("Setup Wi-Fi unavailable. Returning to radio automatically.");
  }

  void exitSetup(uint32_t now) {
    portal_.stop();
    portalReady_ = setupMode_ = restartPending_ = false;
    setupExitPressed_ = setupArmed_ = false;
    recovery_.begin(now);
    selectGesture_ = ReceiverSelectGesture{};
    startRadio(now);
    Serial.println("Handheld returned to ESP-NOW. Release all buttons before operating.");
  }

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
    page += F("<button type=\"submit\">Finish and return to radio</button></form><p>You can also tap and release Power to return to radio. Setup ends after 15 minutes without use.</p></main></html>");
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
    if (!portalRoutesAdded_) {
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
      portal_.send(200, "text/plain", "Returning to radio. Disconnect your phone from receiver Wi-Fi, release all buttons and reset Emergency Stop before operating.");
      restartPending_ = true;
      restartRequestedAt_ = millis();
    });
    portalRoutesAdded_ = true;
    }
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

  void activate(uint8_t slot, uint32_t now, bool show = true, bool newSearch = true) {
    selected_ = slot;
    if (newSearch) recovery_.begin(now);
    sendErrors_ = 0;
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
      if (!ready_ && !setupMode_) startRadio(millis());
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
    recovery_.activity(millis());
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
      Serial.printf("CL HEALTH %u %lu %lu %u %u %lu\n", unsigned(esp_reset_reason()),
                    static_cast<unsigned long>(millis() / 1000),
                    static_cast<unsigned long>(radioStarts_), unsigned(radioResting_),
                    unsigned(setupMode_), static_cast<unsigned long>(ESP.getFreeHeap()));
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
    if (!ready_ && !setupMode_) startRadio(millis());
    if (!ready_ || setupMode_) {
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
  bool setupMode_ = false, portalReady_ = false, restartPending_ = false, portalRoutesAdded_ = false;
  bool setupReleased_ = false, setupExitPressed_ = false, setupArmed_ = false, stopSetupHeld_ = false;
  uint32_t stopSetupAt_ = 0, radioStarts_ = 0;
  bool radioResting_ = false;
  uint8_t sendErrors_ = 0;
  SenderRadioRecovery recovery_;
  TargetSetup setup_, pending_;
  HandheldIndicators indicators_;
  ReceiverSelectGesture selectGesture_;
  uint8_t selected_ = 0, expectedTargets_ = 0;
  uint32_t switchStarted_ = 0;
  bool switching_ = false, batteryEnabled_ = false;
  bool ready_ = false;
  bool storageReady_ = false;
  bool needNeutral_ = true;
  uint64_t nonce_ = 0;
  uint64_t sequenceNonce_ = 0;
  uint32_t sequence_ = 0, lastSend_ = 0, neutralSince_ = 0;
  uint32_t changed_[8]{};
  uint8_t lastRaw_ = 0, filtered_ = 0, sent_ = 0;
  SerialLine serial_;
  StopResetGesture resetGesture_;
};
