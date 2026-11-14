#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <freertos/queue.h>
#include <stddef.h>
#include "BenchConfig.h"
#include "BenchLease.h"
#if CRANE_BENCH_RECEIVER && CRANE_BENCH_PHONE
#include "PhonePage.h"
#endif

// Simulation only. This sketch has no actuator outputs.
struct Settings {
  uint32_t pairId = 1;
  uint8_t channel = 6;
  uint8_t peer[6] = {};
  uint8_t reserved = 0;
  uint8_t mapping[8] = {0, 1, 2, 3, 4, 5, 6, 7};
};

struct Packet {
  uint32_t version;
  uint32_t pairId;
  uint32_t nonce;
  uint32_t sequence; // Zero identifies the receiver's beacon.
  uint32_t mask;
};

struct Received {
  Packet packet;
  uint32_t arrived;
};

constexpr uint32_t protocolVersion = 0x43420102;
constexpr uint32_t stopMask = 128;
constexpr uint32_t requestRadio = 256;
constexpr uint32_t requestWifi = 257;
constexpr uint32_t inhibitMotion = 512;
constexpr uint32_t physicalInputs = 255;
constexpr unsigned setupHoldMs = 1200;
Settings settings;
Settings radioSettings; // Snapshot kept stable while the Wi-Fi callback is registered.
Preferences storage;
WebServer server(80);
QueueHandle_t inbox;
BenchLease lease;
bool setupMode = false;
bool wifiReady = false;
bool radioReady = false;
bool storageReady = false;
bool stopped = false;
bool feedbackFault = false;
uint32_t nonce = 0;
uint32_t sequence = 0;
uint32_t lastBeacon = 0;
uint32_t lastSend = 0;
bool peerInWifi = false;
uint32_t pendingMode = 0;
uint32_t modeRequested = 0;
bool modeRaw = false;
bool modeStable = false;
uint32_t modeChanged = 0;
uint32_t modePressedAt = 0;
const char* headerNames[] = {"X-Crane-Bench", "Origin"};
char phoneToken[17] = {};

bool parseUint(const String& s, uint32_t& out) {
  if (!s.length() || s.length() > 10) return false;
  out = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    uint32_t digit = s[i] - '0';
    if (out > (UINT32_MAX - digit) / 10) return false;
    out = out * 10 + digit;
  }
  return true;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hasPeer(const uint8_t* mac) {
  if (mac[0] & 1) return false;
  for (int i = 0; i < 6; ++i) if (mac[i]) return true;
  return false;
}

bool parseMac(const String& s, uint8_t* mac) {
  if (s.length() != 17) return false;
  for (int i = 0; i < 6; ++i) {
    int high = hexDigit(s[i * 3]);
    int low = hexDigit(s[i * 3 + 1]);
    if (high < 0 || low < 0 || (i < 5 && s[i * 3 + 2] != ':')) return false;
    mac[i] = uint8_t(high * 16 + low);
  }
  return hasPeer(mac);
}

bool validMapping(const uint8_t* mapping) {
  unsigned used = 0;
  for (int input = 0; input < 8; ++input) {
    if (mapping[input] > 7 || (used & (1u << mapping[input]))) return false;
    used |= 1u << mapping[input];
  }
  return true;
}

void reply(int code, const char* message) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "text/plain", message);
}

void settingsPage() {
  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           unsigned(settings.peer[0]), unsigned(settings.peer[1]), unsigned(settings.peer[2]),
           unsigned(settings.peer[3]), unsigned(settings.peer[4]), unsigned(settings.peer[5]));
  String html = "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<h1>Crane Bench Wi-Fi setup</h1><p>Bench simulation only.</p><p>My station MAC: ";
  uint8_t ownMac[6];
  esp_read_mac(ownMac, ESP_MAC_WIFI_STA);
  char own[18];
  snprintf(own, sizeof(own), "%02X:%02X:%02X:%02X:%02X:%02X",
           unsigned(ownMac[0]), unsigned(ownMac[1]), unsigned(ownMac[2]),
           unsigned(ownMac[3]), unsigned(ownMac[4]), unsigned(ownMac[5]));
  html += own;
  html += "</p><form action='/settings' method='post'><p><label>Pair ID "
          "<input name='id' inputmode='numeric' required value='";
  html += String(settings.pairId);
  html += "'></label></p><p><label>2.4 GHz channel (1–11) "
          "<input name='channel' type='number' min='1' max='11' required value='";
  html += String(settings.channel);
  html += "'></label></p><p><label>Other board's station MAC "
          "<input name='peer' maxlength='17' required value='";
  if (hasPeer(settings.peer)) html += mac;
  html += "'></label> (blank for standalone Wi-Fi)</p>";
#if CRANE_BENCH_RECEIVER
  const char* actions[] = {"Up", "Down", "Left", "Right", "Front", "Back", "Simulated stop", "AUX"};
  html += "<fieldset><legend>Handheld button assignments</legend>";
  for (int input = 0; input < 8; ++input) {
    html += "<p><label>Button " + String(input + 1) + " <select name='map" + String(input) + "'>";
    for (int action = 0; action < 8; ++action) {
      html += "<option value='" + String(action) + "'";
      if (settings.mapping[input] == action) html += " selected";
      html += ">";
      html += actions[action];
      html += "</option>";
    }
    html += "</select></label></p>";
  }
  html += "</fieldset>";
#endif
  html += "<button>Save</button></form><p>Saved radio settings apply at the next mode change.</p>";
#if CRANE_BENCH_RECEIVER && CRANE_BENCH_PHONE
  html += "<p><a href='/control'>Open phone simulation</a></p>";
#endif
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

void saveSettings() {
  // Keep normal browser forms simple, but reject cross-origin browser posts.
  if (server.header("Origin") != "http://192.168.4.1") return reply(403, "Open the setup page by its IP address.");
  Settings next;
  uint32_t channel;
  uint8_t ownMac[6];
  esp_read_mac(ownMac, ESP_MAC_WIFI_STA);
  String peerAddress = server.arg("peer");
  peerAddress.trim();
  if (!parseUint(server.arg("id"), next.pairId) || !next.pairId ||
      !parseUint(server.arg("channel"), channel) || channel < 1 || channel > 11 ||
      (peerAddress.length() && (!parseMac(peerAddress, next.peer) || !memcmp(ownMac, next.peer, 6))))
    return reply(400, "Enter a positive pair ID, channel 1–11, and a peer MAC or leave it blank.");
#if CRANE_BENCH_RECEIVER
  for (int input = 0; input < 8; ++input) {
    uint32_t action;
    if (!parseUint(server.arg("map" + String(input)), action) || action > 7)
      return reply(400, "Invalid button assignment.");
    next.mapping[input] = uint8_t(action);
  }
  if (!validMapping(next.mapping)) return reply(400, "Assign each action to exactly one button.");
#endif
  next.channel = uint8_t(channel);
  if (!storageReady || storage.putBytes("radio", &next, sizeof(next)) != sizeof(next))
    return reply(500, "Settings could not be saved.");
  settings = next;
  lease.revoke();
  server.sendHeader("Location", "/");
  reply(303, "Saved. Press the physical mode button to return to ESP-NOW.");
}

void receivePacket(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
  if (length != int(sizeof(Packet)) || memcmp(info->src_addr, radioSettings.peer, 6)) return;
  Received item;
  memcpy(&item.packet, data, sizeof(Packet));
  item.arrived = millis();
  xQueueSend(inbox, &item, 0);
}

void sendPacket(uint32_t seq, uint32_t mask) {
  Packet packet{protocolVersion, radioSettings.pairId, nonce, seq, mask};
  esp_now_send(radioSettings.peer, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
}

void resetSession() {
  lease.revoke();
  sequence = 0;
#if CRANE_BENCH_RECEIVER
  do { nonce = esp_random(); } while (!nonce);
#else
  nonce = 0;
#endif
}

void changeMode(bool useWifi) {
  if (radioReady) esp_now_deinit();
  if (wifiReady) server.stop();
  WiFi.mode(WIFI_OFF);
  radioReady = wifiReady = false;
  radioSettings = settings;
  const bool paired = hasPeer(radioSettings.peer);
  useWifi = useWifi || !paired;
  const bool useRadio = paired && (!useWifi || CRANE_BENCH_RECEIVER);
  setupMode = useWifi;
  xQueueReset(inbox);
  resetSession();
  pendingMode = 0;
  lease.select(useWifi ? Source::phone : Source::handheld);

  if (!WiFi.mode(useWifi ? (useRadio ? WIFI_AP_STA : WIFI_AP) : WIFI_STA)) {
    Serial.println("Wi-Fi driver initialization failed.");
    return;
  }
  if (useWifi) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ssid[30];
    snprintf(ssid, sizeof(ssid), "CraneBench-%s-%02X%02X",
             CRANE_BENCH_RECEIVER ? "RX" : "TX", unsigned(mac[4]), unsigned(mac[5]));
    const IPAddress ip(192, 168, 4, 1);
    wifiReady = WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) &&
                WiFi.softAP(ssid, benchPassword, radioSettings.channel, 0, 2);
    if (wifiReady) {
      server.begin();
      Serial.printf("Setup Wi-Fi: %s, http://192.168.4.1\n", ssid);
    } else Serial.println("Setup Wi-Fi failed.");
  }
  if (!useRadio) return;
  if (!WiFi.setSleep(false) ||
      (!wifiReady && esp_wifi_set_channel(radioSettings.channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) ||
      esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed.");
    return;
  }
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, radioSettings.peer, 6);
  memcpy(peer.lmk, peerKey, sizeof(peerKey));
  peer.ifidx = WIFI_IF_STA;
  peer.channel = radioSettings.channel;
  peer.encrypt = true;
  radioReady = esp_now_set_pmk(primaryKey) == ESP_OK && esp_now_add_peer(&peer) == ESP_OK &&
               esp_now_register_recv_cb(receivePacket) == ESP_OK;
  if (!radioReady) esp_now_deinit();
  else resetSession();
  Serial.printf("ESP-NOW %s. Pair %lu, channel %u.\n", radioReady ? "ready" : "failed",
                static_cast<unsigned long>(radioSettings.pairId), unsigned(radioSettings.channel));
}

#if CRANE_BENCH_RECEIVER
void latchStop() {
  stopped = true;
  resetSession();
  Serial.println("SIM ONLY: STOP latched. Release switches and send R to reset.");
}

void tickRadio(uint32_t now) {
  if (!setupMode) {
    bool wasActive = lease.active();
    lease.expire(now);
    if (wasActive && !lease.active()) {
      resetSession();
      Serial.println("Handheld link timed out. Waiting for the saved peer.");
    }
  }

  Received item;
  if (xQueueReceive(inbox, &item, 0) == pdTRUE) {
    const Packet& p = item.packet;
    if (uint32_t(millis() - item.arrived) < heartbeatMs && p.version == protocolVersion &&
        p.pairId == radioSettings.pairId && p.nonce == nonce && p.sequence > sequence) {
      if (p.mask == requestWifi || p.mask == requestRadio) {
        sequence = p.sequence;
        const bool wantsWifi = p.mask == requestWifi;
        if (wantsWifi != setupMode) {
          changeMode(wantsWifi);
          return;
        }
      } else if (!setupMode && !stopped && !(p.mask & ~(inhibitMotion | physicalInputs))) {
        uint32_t mapped = 0;
        for (int input = 0; input < 8; ++input) {
          if (p.mask & (1u << input)) mapped |= 1u << radioSettings.mapping[input];
        }
        if (mapped & stopMask) latchStop();
        else {
          if (p.mask & inhibitMotion) mapped = 0;
          if (!lease.active() && !mapped && lease.claim(Source::handheld, now))
            Serial.println("Handheld session started.");
          if (lease.command(true, p.sequence, mapped, now)) sequence = p.sequence;
        }
      }
    }
  }
  if (uint32_t(now - lastSend) >= heartbeatMs) {
    lastSend = now;
    sendPacket(0, setupMode ? requestWifi : requestRadio);
  }
}
#else
void tickRadio(uint32_t now) {
  static uint8_t previousRaw = 0;
  static uint8_t filtered = 0;
  static uint32_t changedAt[8] = {};
  static uint32_t lastMask = 0;
  static uint32_t neutralSince = 0;
  static bool needRelease = true;
  uint8_t raw = 0;
  for (int i = 0; i < 8; ++i) {
    const uint8_t bit = uint8_t(1u << i);
    if (digitalRead(buttonPins[i]) == LOW) raw |= bit;
    if ((raw ^ previousRaw) & bit) changedAt[i] = now;
    if (!(raw & bit)) filtered &= ~bit;
    else if (uint32_t(now - changedAt[i]) >= debounceMs) filtered |= bit;
  }
  previousRaw = raw;
  if (raw) neutralSince = now;
  if (nonce && uint32_t(now - lastBeacon) >= BenchLease::timeoutMs) resetSession();

  Received item;
  if (xQueueReceive(inbox, &item, 0) == pdTRUE) {
    const Packet& p = item.packet;
    if (uint32_t(millis() - item.arrived) < heartbeatMs && p.version == protocolVersion &&
        p.pairId == radioSettings.pairId && !p.sequence && p.nonce &&
        (p.mask == requestWifi || p.mask == requestRadio)) {
      lastBeacon = now;
      peerInWifi = p.mask == requestWifi;
      if (p.mask == pendingMode) {
        pendingMode = 0;
        Serial.println(peerInWifi ? "Receiver entered Wi-Fi mode." : "Receiver entered ESP-NOW mode.");
      }
      if (p.nonce != nonce) {
        nonce = p.nonce;
        sequence = 0;
        needRelease = true;
        neutralSince = now;
      }
    }
  }
  if (pendingMode && uint32_t(now - modeRequested) >= 1800) {
    pendingMode = 0;
    Serial.println("Receiver mode change not confirmed.");
  }
  if (!nonce || (peerInWifi && !pendingMode)) return;
  if (!raw && uint32_t(now - neutralSince) >= debounceMs) needRelease = false;
  uint32_t mask = filtered;
  // The receiver can still see a mapped stop while motion waits for neutral.
  if (!sequence || needRelease) mask |= inhibitMotion;
  if (pendingMode) mask = pendingMode;
  if (sequence && mask == lastMask && uint32_t(now - lastSend) < heartbeatMs) return;
  lastSend = now;
  lastMask = mask;
  sendPacket(++sequence, mask);
}
#endif

#if CRANE_BENCH_RECEIVER && CRANE_BENCH_PHONE
void claimPhone() {
  if (server.header("X-Crane-Bench") != "1") return reply(403, "Forbidden");
  if (stopped || server.arg("source") != "p" || !lease.claim(Source::phone, millis()))
    return reply(409, "Unavailable");
  snprintf(phoneToken, sizeof(phoneToken), "%08lx%08lx",
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  reply(200, phoneToken);
}

void phoneCommand() {
  if (server.header("X-Crane-Bench") != "1") return reply(403, "Forbidden");
  uint32_t seq, mask;
  if (!parseUint(server.arg("seq"), seq) || !parseUint(server.arg("mask"), mask))
    return reply(400, "Invalid");
  const bool matches = server.arg("token") == phoneToken;
  if (stopped || mask > stopMask ||
      !lease.command(matches, seq, mask == stopMask ? 0 : mask, millis()))
    return reply(409, "Rejected");
  if (mask == stopMask) latchStop();
  reply(200, "OK");
}
#endif

void setup() {
  Serial.begin(115200);
  Serial.println("\nBENCH SIMULATION ONLY.");
  pinMode(modePin, INPUT_PULLUP);
#if !CRANE_BENCH_RECEIVER
  Serial.println("Tap MODE to switch the receiver. Hold MODE, then release, for local setup.");
  for (int pin : buttonPins) pinMode(pin, INPUT_PULLUP);
#else
  Serial.println("Tap MODE to switch between ESP-NOW and Wi-Fi setup.");
  Serial.println("R: reset stop. F: inject simulated feedback fault. C: clear injected fault.");
#endif
  storageReady = storage.begin("crane-bench", false);
  if (storageReady) {
    Settings saved;
    const size_t length = storage.getBytesLength("radio");
    const size_t oldLength = offsetof(Settings, mapping);
    if ((length == sizeof(saved) || length == oldLength) &&
        storage.getBytes("radio", &saved, length) == length && saved.pairId &&
        saved.channel >= 1 && saved.channel <= 11 && validMapping(saved.mapping))
      settings = saved;
  }
  WiFi.persistent(false);
  inbox = xQueueCreate(8, sizeof(Received));
  if (!inbox) { Serial.println("Cannot allocate radio queue."); return; }
  server.collectHeaders(headerNames, 2);
  server.on("/", HTTP_GET, settingsPage);
  server.on("/settings", HTTP_POST, saveSettings);
#if CRANE_BENCH_RECEIVER && CRANE_BENCH_PHONE
  server.on("/claim", HTTP_POST, claimPhone);
  server.on("/command", HTTP_POST, phoneCommand);
  server.on("/control", HTTP_GET, [] {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", phonePage);
  });
#endif
  changeMode(false);
}

void loop() {
  if (!inbox) { delay(100); return; }
  uint32_t now = millis();
  bool modePressed = digitalRead(modePin) == LOW;
  if (modePressed != modeRaw) { modeRaw = modePressed; modeChanged = now; }
  if (modeRaw != modeStable && uint32_t(now - modeChanged) >= debounceMs) {
    modeStable = modeRaw;
#if CRANE_BENCH_RECEIVER
    if (modeStable) changeMode(!setupMode);
#else
    if (modeStable) {
      modePressedAt = now;
    } else if (setupMode) {
      changeMode(false);
    } else if (uint32_t(now - modePressedAt) >= setupHoldMs) {
      changeMode(true);
    } else if (nonce && uint32_t(now - lastBeacon) < BenchLease::timeoutMs) {
      pendingMode = peerInWifi ? requestRadio : requestWifi;
      modeRequested = now;
    } else {
      Serial.println("Receiver unavailable. Hold MODE to open local setup.");
    }
#endif
  }
#if CRANE_BENCH_RECEIVER
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'f' || c == 'F') {
      feedbackFault = true;
      Serial.println("SIM ONLY: injected feedback mismatch.");
      latchStop();
    } else if (c == 'c' || c == 'C') {
      feedbackFault = false;
      Serial.println("SIM ONLY: injected fault cleared. Send R to reset.");
    } else if (c == 'r' || c == 'R') {
      if (feedbackFault) Serial.println("SIM ONLY: reset blocked while feedback fault is present.");
      else {
        stopped = false;
        resetSession();
        Serial.println("SIM ONLY: reset.");
      }
    }
  }
#endif
  if (wifiReady) server.handleClient();
  if (radioReady) tickRadio(millis());
#if CRANE_BENCH_RECEIVER
  if (setupMode) lease.expire(millis());
  static uint8_t lastDisplayed = 255;
  if (lease.mask != lastDisplayed) {
    lastDisplayed = lease.mask;
    Serial.printf("SIM ONLY: mask 0x%02X\n", unsigned(lastDisplayed));
  }
#endif
  delay(1);
}
