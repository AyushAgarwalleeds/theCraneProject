#pragma once

#include <Arduino.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/queue.h>
#include <mbedtls/md.h>
#include <stddef.h>

constexpr uint8_t radioChannel = 6;
constexpr uint8_t stopCommand = 128;
constexpr uint8_t resetStopCommand = 129;
constexpr uint8_t waitingForRelease = 130;
constexpr uint8_t wireVersion = 4;
constexpr int maxSenders = 4;

struct PeerSettings {
  uint8_t mac[6]{};
  uint8_t key[16]{};

  bool configured() const {
    if (mac[0] & 1) return false;
    bool address = false;
    bool secret = false;
    for (uint8_t byte : mac) address |= byte != 0;
    for (uint8_t byte : key) secret |= byte != 0;
    return address && secret;
  }
};

struct RadioFrame {
  uint64_t nonce;
  uint32_t sequence;
  uint8_t command;
  uint8_t version;
  uint16_t reserved;
  uint8_t authenticator[16];
};

static_assert(sizeof(RadioFrame) == 32 && offsetof(RadioFrame, authenticator) == 16,
              "ESP-NOW frame layout changed");

struct ReceivedFrame {
  RadioFrame frame;
  uint8_t mac[6];
  uint32_t arrived;
  uint8_t slot;
};

inline String formatMac(const uint8_t* mac) {
  char formatted[18];
  snprintf(formatted, sizeof(formatted), "%02X:%02X:%02X:%02X:%02X:%02X",
           unsigned(mac[0]), unsigned(mac[1]), unsigned(mac[2]),
           unsigned(mac[3]), unsigned(mac[4]), unsigned(mac[5]));
  return String(formatted);
}

inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool parseMac(const String& text, uint8_t* mac) {
  if (text.length() != 17) return false;
  for (int i = 0; i < 6; ++i) {
    const int high = hexDigit(text[3 * i]);
    const int low = hexDigit(text[3 * i + 1]);
    if (high < 0 || low < 0 || (i < 5 && text[3 * i + 2] != ':')) return false;
    mac[i] = uint8_t((high << 4) | low);
  }
  return true;
}

inline bool parseKey(const String& text, uint8_t* key) {
  if (text.length() != 32) return false;
  for (int i = 0; i < 16; ++i) {
    const int high = hexDigit(text[2 * i]);
    const int low = hexDigit(text[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    key[i] = uint8_t((high << 4) | low);
  }
  return true;
}

class RadioLink {
public:
  bool begin() {
    if (started_) return true;
    if (!queue_) queue_ = xQueueCreate(8, sizeof(ReceivedFrame));
    if (!queue_ || esp_wifi_get_mac(WIFI_IF_STA, ownMac_) != ESP_OK ||
        esp_now_init() != ESP_OK) return false;
    instance_ = this;
    started_ = esp_now_register_recv_cb(receive) == ESP_OK;
    if (!started_) esp_now_deinit();
    return started_;
  }

  bool setPeer(const PeerSettings& settings, int slot = 0) {
    if (!started_ || slot < 0 || slot >= maxSenders) return false;
    PeerSettings& peer = peers_[slot];
    if (peer.configured()) esp_now_del_peer(peer.mac);
    peer = PeerSettings{};
    xQueueReset(queue_);
    if (!settings.configured()) return true;
    esp_now_peer_info_t info{};
    memcpy(info.peer_addr, settings.mac, sizeof(settings.mac));
    memcpy(info.lmk, settings.key, sizeof(settings.key));
    info.ifidx = WIFI_IF_STA;
    info.channel = radioChannel;
    info.encrypt = true;
    if (esp_now_add_peer(&info) != ESP_OK) return false;
    peer = settings;
    return true;
  }

  bool paired(int slot = 0) const { return peers_[slot].configured(); }

  bool next(ReceivedFrame& item) {
    if (!started_) return false;
    while (xQueueReceive(queue_, &item, 0) == pdTRUE) {
      if (item.frame.version != wireVersion || item.frame.reserved ||
          uint32_t(millis() - item.arrived) >= 180) continue;
      for (int slot = 0; slot < maxSenders; ++slot) {
        if (paired(slot) && !memcmp(item.mac, peers_[slot].mac, sizeof(item.mac)) &&
            authentic(item.frame, peers_[slot])) {
          item.slot = slot;
          return true;
        }
      }
    }
    return false;
  }

  void send(uint64_t nonce, uint32_t sequence, uint8_t command, int slot = 0) {
    if (!paired(slot)) return;
    const PeerSettings& peer = peers_[slot];
    RadioFrame packet{nonce, sequence, command, wireVersion, 0, {}};
    uint8_t digest[32];
    if (!sign(packet, peer, digest)) return;
    memcpy(packet.authenticator, digest, sizeof(packet.authenticator));
    esp_now_send(peer.mac, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
  }

private:
  static bool sign(const RadioFrame& packet, const PeerSettings& peer, uint8_t* digest) {
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                           peer.key, sizeof(peer.key),
                           reinterpret_cast<const uint8_t*>(&packet),
                           offsetof(RadioFrame, authenticator), digest) == 0;
  }

  static bool authentic(const RadioFrame& packet, const PeerSettings& peer) {
    uint8_t digest[32];
    if (!sign(packet, peer, digest)) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < sizeof(packet.authenticator); ++i)
      difference |= digest[i] ^ packet.authenticator[i];
    return difference == 0;
  }

  static void receive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (!instance_ || len != sizeof(RadioFrame) || !info->des_addr ||
        memcmp(info->des_addr, instance_->ownMac_, sizeof(instance_->ownMac_))) return;
    ReceivedFrame item{};
    memcpy(item.mac, info->src_addr, sizeof(item.mac));
    memcpy(&item.frame, data, sizeof(item.frame));
    item.arrived = millis();
    xQueueSend(instance_->queue_, &item, 0);
  }

  inline static RadioLink* instance_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  uint8_t ownMac_[6]{};
  PeerSettings peers_[maxSenders];
  bool started_ = false;
};
