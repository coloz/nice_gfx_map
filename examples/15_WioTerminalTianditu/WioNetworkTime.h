#pragma once
#include <Arduino.h>
#include <rpcWiFi.h>
#include <WiFiUdp.h>
#include <cstdint>
#include <cstring>

namespace wio_map {
namespace clock_detail {
inline uint32_t &epoch() { static uint32_t value = 0; return value; }
inline uint32_t &sampledAt() { static uint32_t value = 0; return value; }
inline uint32_t read32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline bool query(const char *host) {
  WiFiUDP udp;
  if (!udp.begin(uint16_t(49152 + (millis() & 0x3fff)))) return false;
  struct Close { WiFiUDP &udp; ~Close() { udp.stop(); } } close{udp};
  uint8_t request[48] = {};
  request[0] = 0x23; // NTP v4 client.
  // Correlate the reply with this request. This is not the TLS entropy source;
  // the TLS client seeds CTR_DRBG directly from SAMD51's hardware TRNG.
  const uint32_t nonce = micros() ^ uint32_t(reinterpret_cast<uintptr_t>(&udp));
  for (unsigned i = 0; i < 4; ++i) {
    request[40 + i] = uint8_t(0x9e3779b9u >> (24 - 8 * i));
    request[44 + i] = uint8_t(nonce >> (24 - 8 * i));
  }
  if (!udp.beginPacket(host, 123) || udp.write(request, sizeof(request)) != sizeof(request) || !udp.endPacket()) return false;
  const uint32_t started = millis();
  while (uint32_t(millis() - started) < 3500) {
    const int size = udp.parsePacket();
    if (size >= 48) {
      uint8_t response[48];
      if (udp.remotePort() != 123 || udp.read(response, sizeof(response)) != sizeof(response)) return false;
      const unsigned leap = response[0] >> 6, version = response[0] >> 3 & 7, mode = response[0] & 7;
      if (leap == 3 || version < 3 || mode != 4 || response[1] < 1 || response[1] > 15 ||
          std::memcmp(response + 24, request + 40, 8) != 0) return false;
      uint64_t seconds = read32(response + 40);
      if (seconds < 2208988800ull) seconds += 0x100000000ull; // NTP era after 2036.
      seconds -= 2208988800ull;
      if (seconds < 1704067200ull || seconds > 4102444800ull) return false; // 2024..2100.
      epoch() = uint32_t(seconds); sampledAt() = millis();
      Serial.print("WIO_MAP_CLOCK epoch="); Serial.println(epoch());
      return true;
    }
    delay(10); yield();
  }
  return false;
}
} // namespace clock_detail

inline uint32_t tlsEpoch() {
  return clock_detail::epoch() ? clock_detail::epoch() + uint32_t(millis() - clock_detail::sampledAt()) / 1000 : 0;
}
inline bool ensureNetworkTime() {
  if (clock_detail::epoch() && uint32_t(millis() - clock_detail::sampledAt()) < 6u * 60 * 60 * 1000) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  const char *servers[] = {"ntp.aliyun.com", "pool.ntp.org"};
  for (const char *server : servers) if (clock_detail::query(server)) return true;
  Serial.println("WIO_MAP_CLOCK unavailable");
  return false; // Certificate date verification must never use an unset clock.
}
} // namespace wio_map
