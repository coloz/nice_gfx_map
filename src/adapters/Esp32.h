#pragma once
#include "../Pipeline.h"

namespace nice_gfx_map {
// Tries byte-addressable PSRAM first, then internal RAM. Release uses heap_caps_free.
// On a host build this is the ordinary malloc/free allocator.
Allocator esp32PsramAllocator();
struct HttpOptions {
  const char *caCertificate = nullptr; // PEM root CA, or the enabled IDF certificate bundle.
  const char *userAgent = "nice_gfx_map/0.2";
  uint32_t timeoutMs = 10000;
  size_t maxResponseBytes = 512 * 1024;
  Allocator allocator = {};
  bool reuseConnections = true; // Reuse completed HTTPS responses on the same origin.
};
struct HttpTransportStats {
  uint32_t requests = 0, clientInitializations = 0, reusedHandles = 0, retries = 0;
  uint64_t receivedBytes = 0;
};
class Esp32HttpTransport : public Transport {
 public:
  explicit Esp32HttpTransport(HttpOptions options = {}) : options_(options) {}
  ~Esp32HttpTransport() override;
  Esp32HttpTransport(const Esp32HttpTransport &) = delete;
  Esp32HttpTransport &operator=(const Esp32HttpTransport &) = delete;
  Status fetch(const TileRequest &request, ByteBuffer &output) override;
  int lastHttpStatus() const { return lastHttpStatus_; }
  const HttpTransportStats &stats() const { return stats_; }
  // Fetch/destruction belong to one worker task; a client handle is never shared.
 private:
  void resetClient();
  HttpOptions options_;
  int lastHttpStatus_ = 0;
  void *client_ = nullptr; // Keep SDK types out of portable public headers.
  std::string origin_; // Scheme + authority only, never provider query credentials.
  HttpTransportStats stats_;
};
} // namespace nice_gfx_map
