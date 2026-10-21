#include "Esp32.h"
#include <algorithm>
#include <cstdlib>
#include <climits>
#include <utility>
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#endif

namespace nice_gfx_map {
namespace {
void *allocatePixels(size_t bytes, void *) {
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return memory;
#else
  return std::malloc(bytes);
#endif
}
void releasePixels(void *pointer, void *) {
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(pointer);
#else
  std::free(pointer);
#endif
}
}
Allocator esp32PsramAllocator() { return {allocatePixels, releasePixels, nullptr}; }

Esp32HttpTransport::~Esp32HttpTransport() { resetClient(); }
void Esp32HttpTransport::resetClient() {
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  if (client_) {
    auto client = static_cast<esp_http_client_handle_t>(client_);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  }
#endif
  client_ = nullptr;
  origin_.clear();
}

Status Esp32HttpTransport::fetch(const TileRequest &request, ByteBuffer &output) {
  lastHttpStatus_ = 0;
  for (unsigned char c : request.url) if (c <= 0x20 || c == 0x7f) return Status::InvalidArgument;
  if (options_.userAgent) {
    for (const char *p = options_.userAgent; *p; ++p)
      if (static_cast<unsigned char>(*p) < 0x20 || *p == 0x7f) return Status::InvalidArgument;
  }
  if (request.url.compare(0, 8, "https://") != 0 || request.url.size() > 8192 ||
      request.url.find_first_of("\r\n") != std::string::npos ||
      !options_.timeoutMs || options_.timeoutMs > uint32_t(INT_MAX) ||
      !options_.maxResponseBytes || options_.maxResponseBytes > 8 * 1024 * 1024 ||
      !options_.userAgent || !*options_.userAgent ||
      bool(options_.allocator.allocate) != bool(options_.allocator.release))
    return Status::InvalidArgument;
  const size_t authorityEnd = request.url.find_first_of("/?#", 8);
  const size_t authoritySize = (authorityEnd == std::string::npos ? request.url.size() : authorityEnd) - 8;
  // Userinfo can leave implicit Authorization state in a reused SDK client.
  // Map-provider credentials belong in the request query, not URL userinfo.
  if (!authoritySize || request.url.find('@', 8) < 8 + authoritySize) return Status::InvalidArgument;
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  std::string origin = request.url.substr(0, 8 + authoritySize);
  for (char &c : origin) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  if (client_ && origin_ != origin) resetClient();
  ++stats_.requests;
  const bool reused = client_ != nullptr;
  if (reused) ++stats_.reusedHandles;
  const int64_t deadline = esp_timer_get_time() + int64_t(options_.timeoutMs) * 1000;
  const auto remaining = [&]() -> int {
    const int64_t microseconds = deadline - esp_timer_get_time();
    return microseconds > 0 ? int((microseconds + 999) / 1000) : 0;
  };
  const auto fail = [&](Status status) { resetClient(); return status; };
  int64_t length = -1;
  esp_http_client_handle_t client = nullptr;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (!remaining()) return fail(Status::NetworkError);
    if (!client_) {
      esp_http_client_config_t config = {};
      config.url = request.url.c_str();
      config.timeout_ms = remaining();
      config.user_agent = options_.userAgent;
      config.disable_auto_redirect = true; // Never forward a provider key to another origin.
      config.transport_type = HTTP_TRANSPORT_OVER_SSL;
      config.keep_alive_enable = options_.reuseConnections;
      if (options_.caCertificate && *options_.caCertificate) config.cert_pem = options_.caCertificate;
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
      else config.crt_bundle_attach = esp_crt_bundle_attach;
#else
      else return Status::InvalidArgument;
#endif
      client_ = esp_http_client_init(&config);
      if (!client_) return Status::NoMemory;
      origin_ = origin;
      ++stats_.clientInitializations;
    } else if (esp_http_client_set_url(static_cast<esp_http_client_handle_t>(client_), request.url.c_str()) != ESP_OK) {
      return fail(Status::NetworkError);
    }
    client = static_cast<esp_http_client_handle_t>(client_);
    const int openTimeout = remaining();
    if (!openTimeout || esp_http_client_set_timeout_ms(client, openTimeout) != ESP_OK) return fail(Status::NetworkError);
    const bool opened = esp_http_client_open(client, 0) == ESP_OK;
    const int headerTimeout = remaining();
    if (opened && headerTimeout && esp_http_client_set_timeout_ms(client, headerTimeout) == ESP_OK)
      length = esp_http_client_fetch_headers(client);
    if (opened && length >= 0) break;
    // An idle server may have closed a retained socket. A GET can be retried
    // once with a fresh connection, within this request's original deadline.
    resetClient();
    if (!reused || attempt || !remaining()) return Status::NetworkError;
    ++stats_.retries;
  }
  lastHttpStatus_ = esp_http_client_get_status_code(client);
  if (lastHttpStatus_ != 200) return fail(Status::NetworkError);
  if (uint64_t(length) > options_.maxResponseBytes) return fail(Status::TooLarge);
  ByteBuffer received;
  Allocator allocator = options_.allocator;
  if (!allocator.allocate) allocator = esp32PsramAllocator();
  const size_t declaredLength = length > 0 ? size_t(length) : 0;
  // Chunked/unknown responses are commonly small PNG tiles. Requiring the
  // entire response limit as one contiguous allocation can fail after long UI
  // sessions even when enough memory remains for the actual encoded tile.
  Status status = received.reserve(declaredLength ? declaredLength : std::min<size_t>(8192, options_.maxResponseBytes), allocator);
  if (status != Status::Ok) return fail(status);
  uint8_t block[2048];
  for (;;) {
    const int timeout = remaining();
    if (!timeout || esp_http_client_set_timeout_ms(client, timeout) != ESP_OK) return fail(Status::NetworkError);
    const int count = esp_http_client_read(client, reinterpret_cast<char *>(block), sizeof(block));
    if (count < 0) return fail(Status::NetworkError);
    if (!count) break;
    const size_t bytes = size_t(count);
    if (bytes > options_.maxResponseBytes - received.size() ||
        (declaredLength && bytes > declaredLength - received.size())) return fail(Status::TooLarge);
    const size_t required = received.size() + bytes;
    if (required > received.capacity()) {
      // ByteBuffer append never reallocates; reserve preserves existing bytes
      // on failure. Doubling bounds copies and never exceeds the HTTP limit.
      size_t capacity = received.capacity();
      while (capacity < required) capacity = std::min(options_.maxResponseBytes, capacity * 2);
      status = received.reserve(capacity, allocator);
      if (status != Status::Ok) return fail(status);
    }
    status = received.append(block, bytes);
    if (status != Status::Ok) return fail(status);
  }
  if (!esp_http_client_is_complete_data_received(client) || !received.size()) return fail(Status::NetworkError);
  stats_.receivedBytes += received.size();
  output = std::move(received);
  // A complete response is essential: unread bytes must never become the next
  // tile's headers. Honor Connection: close instead of retaining a dead socket.
  if (!options_.reuseConnections || !esp_http_client_is_persistent_connection(client)) resetClient();
  return Status::Ok;
#else
  (void)output;
  return Status::Unsupported;
#endif
}
} // namespace nice_gfx_map
