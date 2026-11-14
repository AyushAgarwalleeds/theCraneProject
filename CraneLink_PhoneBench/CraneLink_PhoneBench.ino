#include <Arduino.h>
#include <sdkconfig.h>

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "CraneLink Phone Bench needs a classic ESP32-WROOM. Select Tools > Board > esp32 > ESP32 Dev Module."
#else

#include "BuildRole.h"

#if CRANELINK_SENDER
#include "Handheld.h"
Handheld handheld;
void setup() { handheld.begin(); }
void loop() {
  handheld.loop();
  delay(1);
}
#else

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <atomic>
#include "RelayController.h"
#include "PhonePage.h"
#include "RadioLink.h"
#include "SerialLine.h"

// Drives a relay module with its contacts disconnected from the crane.
const IPAddress benchIp(192, 168, 4, 1);
const char* requestHeaders[] = {"X-CraneLink-Bench"};
WebServer server(80);
RelayController relays;
char ssid[32];
char password[64];
bool apReady = false;
bool relaysReady = false;
bool phoneConnected = false;
std::atomic<bool> phoneAssociationChanged{false};
bool restarting = false;
uint32_t restartRequestedAt = 0;
uint32_t lastApAttempt = 0;
uint32_t lastRadioAttempt = 0;
uint32_t lastPhoneCheck = 0;
uint8_t receiverMac[6]{};
RadioLink radio;
bool radioReady = false;
SerialLine serialLine;
struct RadioSession {
  uint64_t nonce = 0;
  uint32_t received = 0, sequence = 0;
  bool neutral = false, holding = false, stopHeld = false, resetSeen = false;
};
RadioSession radioSessions[maxSenders];
uint32_t lastBeacon = 0;
PeerSettings pendingPeers[maxSenders];
int pendingPeerCount = 0, receivedPeerCount = 0;

struct LegacyOptions {
  uint16_t idleMinutes = 60;
  uint8_t assignment[7] = {0, 1, 2, 3, 4, 5, 6};
  PeerSettings sender;
};

struct ReceiverOptions {
  uint16_t idleMinutes = 60;
  uint8_t assignment[7] = {0, 1, 2, 3, 4, 5, 6};
  bool interlock = false;
  PeerSettings senders[maxSenders];
};

ReceiverOptions options;

struct NetworkSettings {
  char name[13]{};
  char key[64]{};
};

void sendJson(int status, const String& data) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json", data);
}

void sendState(const RelayState& state) {
  String json = String("{\"mask\":") + String(state.mask);
  json += String(",\"requested\":") + String(state.requested);
  json += String(",\"relayMask\":") + String(state.relayMask);
  json += state.armed ? ",\"armed\":true" : ",\"armed\":false";
  json += state.stopped ? ",\"stopped\":true" : ",\"stopped\":false";
  json += state.handheld ? ",\"handheld\":true" : ",\"handheld\":false";
  json += phoneConnected ? ",\"phoneConnected\":true" : ",\"phoneConnected\":false";
  json += state.auxOn ? ",\"auxOn\":true" : ",\"auxOn\":false";
  json += String(",\"auxSeconds\":") + String(state.auxRemainingSeconds);
  json += String(",\"auxHoldMs\":") + String(state.auxHoldMs) + "}";
  sendJson(200, json);
}

bool allowPost() {
  // A cross-site HTML form cannot attach this header, and CORS is disabled.
  if (server.header(requestHeaders[0]) == "1") return true;
  sendJson(403, "{\"error\":\"Open the bench page directly.\"}");
  return false;
}

bool readDecimal(const String& raw, uint32_t& value) {
  if (!raw.length() || raw.length() > 10) return false;
  value = 0;
  for (size_t i = 0; i < raw.length(); ++i) {
    if (raw[i] < '0' || raw[i] > '9') return false;
    const uint32_t digit = raw[i] - '0';
    if (value > (UINT32_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}

bool readToken(const String& raw, uint64_t& token) {
  if (raw.length() != 16) return false;
  token = 0;
  for (size_t i = 0; i < raw.length(); ++i) {
    char c = raw[i];
    int digit = c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
    if (digit < 0 || digit > 15 || (c > '9' && c < 'a')) return false;
    token = (token << 4) | uint64_t(digit);
  }
  return token != 0;
}

uint64_t randomToken() {
  uint64_t token;
  do { esp_fill_random(&token, sizeof(token)); } while (!token || token >= firstHandheldToken);
  return token;
}

bool validOptions(const ReceiverOptions& next) {
  if (!next.idleMinutes || next.idleMinutes > 720) return false;
  unsigned used = 0;
  for (uint8_t relay : next.assignment) {
    if (relay > 6 || (used & (1u << relay))) return false;
    used |= 1u << relay;
  }
  return true;
}

void resetRadioSession(int slot) {
  auto& control = radioSessions[slot];
  // Keep a recent held Stop visible through a Wi-Fi handoff until a new frame
  // confirms release or the handheld stops responding.
  const bool stopHeld = control.stopHeld;
  const uint32_t received = control.received;
  control = RadioSession{};
  control.stopHeld = stopHeld;
  control.received = received;
  control.nonce = randomToken();
  relays.senderDisconnected(firstHandheldToken + slot);
}

void updatePhoneConnection() {
  lastPhoneCheck = millis();
  const bool changed = phoneAssociationChanged.exchange(false);
  const bool connected = apReady && WiFi.softAPgetStationNum() != 0;
  if (!changed && phoneConnected == connected) return;
  phoneConnected = connected;
  relays.phoneHandoff();
  for (int slot = 0; slot < maxSenders; ++slot) resetRadioSession(slot);
  lastBeacon = millis() - 100;
}

bool senderStopHeld(uint32_t now) {
  for (const auto& sender : radioSessions) {
    if (sender.stopHeld && uint32_t(now - sender.received) < PhoneSession::leaseMs) return true;
  }
  return false;
}

bool saveNetwork(const String& name, const String& key) {
  if (name.length() < 4 || name.length() > 12 || key.length() < 8 || key.length() > 63) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z')) return false;
  }
  for (size_t i = 0; i < key.length(); ++i) {
    const char c = key[i];
    if (c < 33 || c > 126) return false;
  }
  NetworkSettings network;
  name.toCharArray(network.name, sizeof(network.name));
  key.toCharArray(network.key, sizeof(network.key));
  Preferences prefs;
  return prefs.begin("cranelink-bench", false) &&
         prefs.putBytes("network", &network, sizeof(network)) == sizeof(network);
}

void tickRadio() {
  const uint32_t now = millis();
  for (int slot = 0; slot < maxSenders; ++slot) {
    const auto& control = radioSessions[slot];
    if (control.sequence && uint32_t(now - control.received) >= PhoneSession::leaseMs)
      resetRadioSession(slot);
  }
  ReceivedFrame item;
  for (int n = 0; n < 8 && radio.next(item); ++n) {
    const RadioFrame& frame = item.frame;
    auto& control = radioSessions[item.slot];
    const uint64_t token = firstHandheldToken + item.slot;
    const uint8_t mask = frame.command;
    if (frame.nonce != control.nonce || frame.sequence <= control.sequence || mask > waitingForRelease) continue;
    control.sequence = frame.sequence;
    control.received = item.arrived;
    control.stopHeld = mask == stopCommand;
    const bool newReset = mask == resetStopCommand && !control.resetSeen;
    control.resetSeen = mask == resetStopCommand;
    if (mask == waitingForRelease) relays.senderDisconnected(token);
    relays.senderFrame(item.slot, item.arrived, [&](PhoneSession& session, uint32_t time) {
      if (mask == stopCommand) {
        session.stop();
        for (auto& sender : radioSessions) sender.neutral = sender.holding = false;
      } else if (mask == resetStopCommand) {
        if (newReset && session.stopped() && !senderStopHeld(time)) {
          session.reset();
          for (auto& sender : radioSessions) sender.neutral = sender.holding = false;
        }
      } else if (mask == waitingForRelease) {
        control.neutral = control.holding = false;
      } else if (!mask) {
        session.end(token);
        control.neutral = true;
        control.holding = false;
      } else if (!phoneConnected && !phoneAssociationChanged.load() && control.neutral && !session.stopped()) {
        if (control.holding && !session.ownedBy(token)) {
          control.neutral = control.holding = false;
          return;
        }
        if (!control.holding) {
          if (!session.arm(token, time)) {
            control.neutral = false; // A blocked press needs a fresh release.
            return;
          }
          session.jog(token, 1, 0, time);
          control.holding = true;
        }
        if (frame.sequence == UINT32_MAX ||
            !session.jog(token, frame.sequence + 1, mask, time)) {
          session.end(token);
          control.neutral = control.holding = false;
        }
      } else {
        control.neutral = false;
      }
    });
  }
  if (uint32_t(millis() - lastBeacon) >= 100) {
    lastBeacon = millis();
    for (int slot = 0; slot < maxSenders; ++slot)
      radio.send(radioSessions[slot].nonce, 0, 0, slot);
  }
}

void sendSettings() {
  String mapping;
  for (uint8_t pin : options.assignment) mapping += char('1' + pin);
  String json = String("{\"idleMinutes\":") + String(options.idleMinutes);
  json += ",\"mapping\":\"" + mapping + "\",\"receiverMac\":\"" + formatMac(receiverMac);
  json += options.interlock ? "\",\"interlock\":true" : "\",\"interlock\":false";
  json += String(",\"channel\":") + String(radioChannel);
  json += radioReady ? ",\"radioReady\":true" : ",\"radioReady\":false";
  json += ",\"senders\":[";
  for (int slot = 0; slot < maxSenders; ++slot) {
    if (slot) json += ",";
    json += "{\"mac\":\"";
    if (options.senders[slot].configured()) json += formatMac(options.senders[slot].mac);
    const auto& control = radioSessions[slot];
    const bool linked = radio.paired(slot) && control.sequence &&
                        uint32_t(millis() - control.received) < PhoneSession::leaseMs;
    json += linked ? "\",\"linked\":true}" : "\",\"linked\":false}";
  }
  json += "]}";
  sendJson(200, json);
}

bool saveOptions(const ReceiverOptions& next) {
  Preferences prefs;
  return prefs.begin("cranelink-bench", false) &&
         prefs.putBytes("options-v2", &next, sizeof(next)) == sizeof(next);
}

bool savePeer(int slot, const PeerSettings& peer) {
  for (int i = 0; i < maxSenders; ++i) {
    if (i != slot && peer.configured() && options.senders[i].configured() &&
        !memcmp(peer.mac, options.senders[i].mac, sizeof(peer.mac))) return false;
  }
  ReceiverOptions next = options;
  next.senders[slot] = peer;
  if (!radio.setPeer(peer, slot) || !saveOptions(next)) {
    radio.setPeer(options.senders[slot], slot);
    resetRadioSession(slot);
    return false;
  }
  options = next;
  resetRadioSession(slot);
  return true;
}

bool replacePeers() {
  const RelayState state = relays.state();
  if (!radioReady || state.armed || state.relayMask) return false;
  ReceiverOptions next = options;
  memcpy(next.senders, pendingPeers, sizeof(next.senders));
  for (int slot = 0; slot < maxSenders; ++slot) radio.setPeer(PeerSettings{}, slot);
  bool loaded = true;
  for (int slot = 0; slot < maxSenders; ++slot) {
    if (!radio.setPeer(next.senders[slot], slot)) {
      loaded = false;
      break;
    }
  }
  if (loaded && saveOptions(next)) options = next;
  else {
    loaded = false;
    for (int slot = 0; slot < maxSenders; ++slot) radio.setPeer(PeerSettings{}, slot);
    for (int slot = 0; slot < maxSenders; ++slot) radio.setPeer(options.senders[slot], slot);
  }
  for (int slot = 0; slot < maxSenders; ++slot) resetRadioSession(slot);
  return loaded;
}

void saveSettings() {
  if (!allowPost()) return;
  const RelayState state = relays.state();
  if (state.armed) {
    sendJson(409, "{\"error\":\"Turn Web Run off and release the sender before saving.\"}");
    return;
  }
  ReceiverOptions next = options;
  uint32_t minutes;
  const String mapping = server.arg("mapping");
  if (!readDecimal(server.arg("idleMinutes"), minutes) || !minutes || minutes > 720 || mapping.length() != 7) {
    sendJson(400, "{\"error\":\"Enter 1–720 minutes and choose each of R1–R7 once.\"}");
    return;
  }
  next.idleMinutes = uint16_t(minutes);
  next.interlock = server.arg("interlock") == "1";
  for (int i = 0; i < 7; ++i) next.assignment[i] = uint8_t(mapping[i] - '1');
  if (!validOptions(next)) {
    sendJson(400, "{\"error\":\"Assign each of R1–R7 exactly once.\"}");
    return;
  }
  const bool newMapping = memcmp(next.assignment, options.assignment, sizeof(next.assignment)) != 0;
  if (state.auxOn && newMapping) {
    sendJson(409, "{\"error\":\"Turn AUX off before changing relay assignments.\"}");
    return;
  }
  if (!saveOptions(next)) {
    sendJson(500, "{\"error\":\"Settings could not be saved.\"}");
    return;
  }
  options = next;
  relays.configure(options.assignment, options.idleMinutes, options.interlock);
  sendSettings();
}

void handlePeer() {
  if (!allowPost()) return;
  const RelayState state = relays.state();
  if (state.armed || state.relayMask) {
    sendJson(409, "{\"error\":\"Release controls, turn off Web Run and AUX before pairing.\"}");
    return;
  }
  uint32_t index;
  PeerSettings peer;
  const String mac = server.arg("senderMac"), key = server.arg("key");
  if (!readDecimal(server.arg("slot"), index) || !index || index > maxSenders ||
      (!mac.isEmpty() && (!parseMac(mac, peer.mac) || !parseKey(key, peer.key) ||
                         !peer.configured() || !memcmp(peer.mac, receiverMac, 6)))) {
    sendJson(400, "{\"error\":\"Select a sender slot and enter its MAC and 32-digit hex key.\"}");
    return;
  }
  if (!savePeer(index - 1, peer)) {
    sendJson(409, "{\"error\":\"Could not save the sender. Check for a duplicate MAC and radio availability.\"}");
    return;
  }
  sendSettings();
}

void receiverSerial(String line) {
  line.trim();
  if (line == "identify") {
    uint8_t factory[6]{};
    esp_efuse_mac_get_default(factory);
    Serial.printf("CL IDENT RECEIVER %s %s\n", formatMac(receiverMac).c_str(), formatMac(factory).c_str());
    return;
  }
  if (line == "setup-info") {
    if (apReady) Serial.printf("CL WIFI %s %s %s\n", ssid, password, WiFi.softAPIP().toString().c_str());
    else Serial.println("CL WIFI DOWN");
    Serial.println("CL FEATURES NETWORK");
    Serial.printf("CL PROTOCOL %u\n", unsigned(wireVersion));
    Serial.printf("CL RADIO %u %u\n", unsigned(WiFi.channel()), unsigned(radioReady));
    const RelayState state = relays.state();
    Serial.printf("CL STATE %u %u\n", unsigned(state.stopped),
                  unsigned(state.armed && !state.handheld));
    Serial.printf("CL POWER %d %lu %u\n", int(esp_reset_reason()), millis() / 1000UL,
                  unsigned(WiFi.softAPgetStationNum()));
    for (int slot = 0; slot < maxSenders; ++slot) {
      const auto& sender = options.senders[slot];
      const auto& control = radioSessions[slot];
      const bool linked = radio.paired(slot) && control.sequence &&
                          uint32_t(millis() - control.received) < PhoneSession::leaseMs;
      Serial.printf("CL SLOT %d %s %u\n", slot + 1,
                    sender.configured() ? formatMac(sender.mac).c_str() : "-", unsigned(linked));
    }
    Serial.println("CL INFO DONE");
    return;
  }
  if (line.startsWith("wifi-set ")) {
    const int separator = line.indexOf(' ', 9);
    if (separator < 0) {
      Serial.println("CL ERROR Enter a Wi-Fi name and password.");
      return;
    }
    const String name = line.substring(9, separator);
    const String key = line.substring(separator + 1);
    const RelayState state = relays.state();
    updatePhoneConnection();
    if (state.armed || state.relayMask || phoneConnected) {
      Serial.println("CL ERROR Disconnect the phone from receiver Wi-Fi, turn off Power and release the controls.");
      return;
    }
    if (!saveNetwork(name, key)) {
      Serial.println("CL ERROR Wi-Fi settings could not be saved. Check the name and password.");
      return;
    }
    relays.phoneHandoff();
    restarting = true;
    restartRequestedAt = millis();
    Serial.println("CL WIFI SAVED");
    return;
  }
  if (line.startsWith("peers-begin ")) {
    uint32_t count;
    pendingPeerCount = receivedPeerCount = 0;
    if (!readDecimal(line.substring(12), count) || count < 1 || count > maxSenders) {
      Serial.println("CL ERROR invalid sender count");
      return;
    }
    for (auto& peer : pendingPeers) peer = PeerSettings{};
    pendingPeerCount = count;
    Serial.println("CL PEERS READY");
    return;
  }
  if (line.startsWith("peers-add ")) {
    PeerSettings peer;
    const int separator = line.indexOf(' ', 10);
    bool valid = receivedPeerCount < pendingPeerCount && separator >= 0 &&
                 parseMac(line.substring(10, separator), peer.mac) &&
                 parseKey(line.substring(separator + 1), peer.key) && peer.configured() &&
                 memcmp(peer.mac, receiverMac, sizeof(peer.mac));
    for (int i = 0; i < receivedPeerCount && valid; ++i)
      valid = memcmp(peer.mac, pendingPeers[i].mac, sizeof(peer.mac)) != 0;
    if (!valid) {
      pendingPeerCount = receivedPeerCount = 0;
      Serial.println("CL ERROR invalid or duplicate sender");
      return;
    }
    pendingPeers[receivedPeerCount++] = peer;
    Serial.println("CL PEERS ADDED");
    return;
  }
  if (line == "peers-save") {
    if (pendingPeerCount && receivedPeerCount == pendingPeerCount && replacePeers())
      Serial.println("CL PEERS SAVED");
    else
      Serial.println("CL ERROR could not save sender slots. Release all controls and turn off Web Run and AUX.");
    pendingPeerCount = receivedPeerCount = 0;
    return;
  }
  if (line == "clear-peers") {
    const RelayState state = relays.state();
    ReceiverOptions next = options;
    for (auto& sender : next.senders) sender = PeerSettings{};
    if (!radioReady || state.armed || state.relayMask || !saveOptions(next)) {
      Serial.println("CL ERROR sender slots could not be cleared");
      return;
    }
    options = next;
    for (int slot = 0; slot < maxSenders; ++slot) {
      radio.setPeer(options.senders[slot], slot);
      resetRadioSession(slot);
    }
    Serial.println("CL CLEARED");
    return;
  }
  const int slotEnd = line.indexOf(' ', 5);
  const int macEnd = slotEnd < 0 ? -1 : line.indexOf(' ', slotEnd + 1);
  uint32_t index;
  PeerSettings peer;
  const RelayState state = relays.state();
  if (!line.startsWith("peer ") || slotEnd < 0 || macEnd < 0 ||
      !readDecimal(line.substring(5, slotEnd), index) || !index || index > maxSenders ||
      !parseMac(line.substring(slotEnd + 1, macEnd), peer.mac) ||
      !parseKey(line.substring(macEnd + 1), peer.key) || !peer.configured() ||
      !memcmp(peer.mac, receiverMac, 6) || state.armed || state.relayMask ||
      !savePeer(index - 1, peer)) {
    Serial.println("CL ERROR peer not saved");
    return;
  }
  Serial.printf("CL PEER %u\n", unsigned(index));
}

void handleArm() {
  if (!allowPost()) return;
  updatePhoneConnection();
  if (!phoneConnected) {
    sendJson(409, "{\"error\":\"Join the receiver Wi-Fi first.\"}");
    return;
  }
  const uint64_t token = randomToken();
  bool armed;
  const RelayState state = relays.update([&](PhoneSession& session, uint32_t now) {
    armed = !phoneAssociationChanged.load() && session.arm(token, now);
  });
  if (!armed) {
    sendJson(409, state.stopped ? "{\"error\":\"Stop is latched.\"}"
                 : state.handheld ? "{\"error\":\"Release the sender before enabling Web Run.\"}"
                 : "{\"error\":\"Another tab has Web Run. Turn it off or wait a moment.\"}");
    return;
  }
  char json[32];
  snprintf(json, sizeof(json), "{\"token\":\"%016llx\"}", static_cast<unsigned long long>(token));
  sendJson(200, json);
}

void handleJog() {
  if (!allowPost()) return;
  updatePhoneConnection();
  uint64_t token;
  uint32_t sequence, mask;
  if (!readToken(server.arg("token"), token) || token >= firstHandheldToken ||
      !readDecimal(server.arg("seq"), sequence) ||
      !readDecimal(server.arg("mask"), mask)) {
    sendJson(400, "{\"error\":\"Invalid command.\"}");
    return;
  }
  bool accepted;
  const RelayState state = relays.update([&](PhoneSession& session, uint32_t now) {
    accepted = phoneConnected && !phoneAssociationChanged.load() && session.jog(token, sequence, mask, now);
  });
  if (!accepted) {
    sendJson(409, "{\"error\":\"Web Run ended. Release and enable it again.\"}");
    return;
  }
  sendState(state);
}

void handleEnd() {
  if (!allowPost()) return;
  uint64_t token = 0;
  if (!readToken(server.arg("token"), token) || token >= firstHandheldToken) {
    sendJson(400, "{\"error\":\"Invalid session.\"}");
    return;
  }
  sendState(relays.endPhone(token, server.arg("leaving") == "1"));
}

void tryRadio() {
  lastRadioAttempt = millis();
  radioReady = (WiFi.channel() == radioChannel ||
                esp_wifi_set_channel(radioChannel, WIFI_SECOND_CHAN_NONE) == ESP_OK) && radio.begin();
  if (!radioReady) {
    Serial.println("ESP-NOW unavailable. Retrying.");
    return;
  }
  for (int slot = 0; slot < maxSenders; ++slot) {
    if (!radio.setPeer(options.senders[slot], slot)) {
      radioReady = false;
      Serial.printf("Could not load sender %d. Retrying.\n", slot + 1);
    }
    resetRadioSession(slot);
  }
}

void startAp() {
  lastApAttempt = millis();
  if (!WiFi.mode(WIFI_AP_STA)) {
    Serial.println("Wi-Fi mode failed. Retrying.");
    return;
  }
  WiFi.setSleep(false);
  uint8_t actualMac[6]{};
  if (esp_wifi_get_mac(WIFI_IF_STA, actualMac) == ESP_OK)
    memcpy(receiverMac, actualMac, sizeof(receiverMac));
  if (!password[0]) {
    Preferences prefs;
    bool saved = prefs.begin("cranelink-bench", false);
    String existing = saved ? prefs.getString("ap-key", "") : "";
    if (existing.length() == 16) existing.toCharArray(password, sizeof(password));
    else {
      snprintf(password, sizeof(password), "%016llx", static_cast<unsigned long long>(randomToken()));
      if (!saved || prefs.putString("ap-key", password) != 16)
        Serial.println("Could not save the bench password. Use the password printed for this boot.");
    }
    if (saved) prefs.end();
  }
  if (!WiFi.softAPConfig(benchIp, benchIp, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(ssid, password, radioChannel, 0, 1)) {
    Serial.println("Access point failed to start. Retrying.");
    return;
  }
  server.begin();
  apReady = true;
  esp_wifi_set_inactive_time(WIFI_IF_AP, 10);
  Serial.printf("Wi-Fi network: %s\nWi-Fi password: %s\nOpen: http://192.168.4.1/\n", ssid, password);
  Serial.printf("Receiver STA MAC: %s, channel %u\n", formatMac(receiverMac).c_str(), unsigned(radioChannel));
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Receiver reset reason: %d\n", int(esp_reset_reason()));
  Preferences prefs;
  if (prefs.begin("cranelink-bench", true)) {
    ReceiverOptions stored;
    LegacyOptions old;
    if (prefs.getBytesLength("options-v2") == sizeof(stored) &&
        prefs.getBytes("options-v2", &stored, sizeof(stored)) == sizeof(stored) && validOptions(stored)) {
      options = stored;
    } else if (prefs.getBytesLength("options-v1") == sizeof(old) &&
               prefs.getBytes("options-v1", &old, sizeof(old)) == sizeof(old)) {
      stored.idleMinutes = old.idleMinutes;
      memcpy(stored.assignment, old.assignment, sizeof(old.assignment));
      stored.senders[0] = old.sender;
      if (validOptions(stored)) options = stored;
    }
    prefs.end();
  }
  relaysReady = relays.begin(options.assignment, options.idleMinutes, options.interlock);
  if (!relaysReady) {
    Serial.println("\nRelay initialization failed. Web Run is unavailable.");
    return;
  }
  Serial.println("\nCraneLink Phone Bench. Physical relay outputs enabled.");
  esp_read_mac(receiverMac, ESP_MAC_WIFI_STA);
  uint64_t id = 0;
  for (uint8_t byte : receiverMac) id = (id << 8) | byte;
  for (int i = 9; i >= 0; --i) {
    ssid[i] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[id % 36];
    id /= 36;
  }
  NetworkSettings network;
  if (prefs.begin("cranelink-bench", true) &&
      prefs.getBytesLength("network") == sizeof(network) &&
      prefs.getBytes("network", &network, sizeof(network)) == sizeof(network) &&
      network.name[0] && !network.name[12] && network.key[0] && !network.key[63]) {
    strcpy(ssid, network.name);
    strcpy(password, network.key);
  }
  prefs.end();
  WiFi.persistent(false);
  WiFi.onEvent([](arduino_event_id_t event) {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED || event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
      phoneAssociationChanged.store(true);
      relays.phoneHandoff();
    }
  });
  server.collectHeaders(requestHeaders, 1);
  server.on("/", HTTP_GET, [] {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", phonePage);
  });
  server.on("/api/status", HTTP_GET, [] { sendState(relays.state()); });
  server.on("/api/settings", HTTP_GET, sendSettings);
  server.on("/api/settings", HTTP_POST, saveSettings);
  server.on("/api/peer", HTTP_POST, handlePeer);
  server.on("/api/arm", HTTP_POST, handleArm);
  server.on("/api/jog", HTTP_POST, handleJog);
  server.on("/api/end", HTTP_POST, handleEnd);
  server.on("/api/stop", HTTP_POST, [] {
    if (!allowPost()) return;
    for (auto& sender : radioSessions) sender.neutral = sender.holding = false;
    sendState(relays.update([](PhoneSession& session, uint32_t) { session.stop(); }));
  });
  server.on("/api/reset", HTTP_POST, [] {
    if (!allowPost()) return;
    if (senderStopHeld(millis())) {
      sendJson(409, "{\"error\":\"Release Emergency Stop on the handheld first.\"}");
      return;
    }
    // Reset acknowledges an existing latch only. It never starts Web Run.
    sendState(relays.update([](PhoneSession& session, uint32_t) {
      if (session.stopped()) session.reset();
    }));
  });
  startAp();
  tryRadio();
}

void loop() {
  if (!relaysReady) {
    delay(1000);
    return;
  }
  serialLine.poll(receiverSerial);
  if (restarting) {
    if (uint32_t(millis() - restartRequestedAt) >= 250) ESP.restart();
    delay(1);
    return;
  }
  if (!apReady && uint32_t(millis() - lastApAttempt) >= 3000) startAp();
  if (uint32_t(millis() - lastPhoneCheck) >= 25) updatePhoneConnection();
  if (!radioReady && uint32_t(millis() - lastRadioAttempt) >= 3000) tryRadio();
  if (radioReady) tickRadio();
  if (apReady) server.handleClient();
  const RelayState state = relays.state();
  static uint8_t lastMask = 255;
  static bool lastStop = false;
  if (lastMask != state.relayMask || lastStop != state.stopped) {
    lastMask = state.relayMask;
    lastStop = state.stopped;
    Serial.printf("Relay outputs 0x%02X, stop %s\n", unsigned(lastMask), lastStop ? "latched" : "clear");
  }
  delay(1);
}

#endif
#endif
