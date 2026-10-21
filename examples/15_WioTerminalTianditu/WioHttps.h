#pragma once

// Example-only synchronous HTTPS transport for Wio Terminal's RTL8720 RPC stack.
// Downloads into a bounded compressed-image buffer, with no SD requirement.
// Failures clear the output (and release its capacity unless reuse is enabled).
// The caller owns Wi-Fi initialization.
#include <Arduino.h>
#include <rpcWiFi.h>
#include <rpcWiFiClientSecure.h>
#include <Pipeline.h>
#include <cstring>
#include <string>

namespace wio_map {

struct DownloadResult {
  nice_gfx_map::Status status = nice_gfx_map::Status::NetworkError;
  int httpStatus = 0;
  size_t bytes = 0;
  const char *phase = "configuration";
  int networkCode = 0;
};

namespace https_detail {
constexpr size_t MaxHeaderBytes = 4096;
constexpr size_t MaxBodyBytes = 192 * 1024;
constexpr uint32_t ConnectTimeoutMs = 8000;
constexpr uint32_t IdleTimeoutMs = 8000;
constexpr uint32_t TotalTimeoutMs = 45000;

inline bool asciiEqual(const char *left, const char *right) {
  while (*left && *right) {
    char a = *left++, b = *right++;
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (a != b) return false;
  }
  return *left == *right;
}

inline char *trim(char *text) {
  while (*text == ' ' || *text == '\t') ++text;
  char *end = text + std::strlen(text);
  while (end != text && (end[-1] == ' ' || end[-1] == '\t')) --end;
  *end = 0;
  return text;
}

inline bool decimal(const char *text, size_t limit, size_t &value) {
  if (!*text) return false;
  value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const size_t digit = static_cast<size_t>(*text - '0');
    if (value > limit / 10 || (value == limit / 10 && digit > limit % 10))
      return false;
    value = value * 10 + digit;
  }
  return true;
}

struct Url {
  std::string host, target, authority;
  uint16_t port = 443;
};

inline bool splitUrl(const std::string &url, Url &out) {
  if (url.compare(0, 8, "https://") != 0 || url.size() > 2048) return false;
  for (unsigned char c : url)
    if (c <= 0x20 || c >= 0x7f || c == '#') return false;
  const size_t targetAt = url.find_first_of("/?", 8);
  const size_t authorityEnd = targetAt == std::string::npos ? url.size() : targetAt;
  out.authority = url.substr(8, authorityEnd - 8);
  const size_t colon = out.authority.find(':');
  out.host = out.authority.substr(0, colon);
  if (out.host.empty() || out.host.size() > 127) return false;
  for (unsigned char c : out.host)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
  if (colon != std::string::npos) {
    size_t port = 0;
    if (!decimal(out.authority.c_str() + colon + 1, 65535, port) || !port)
      return false;
    out.port = static_cast<uint16_t>(port);
  }
  if (targetAt == std::string::npos) out.target = "/";
  else if (url[targetAt] == '?') out.target = "/" + url.substr(targetAt);
  else out.target = url.substr(targetAt);
  return true;
}

class Reader {
 public:
  explicit Reader(WiFiClientSecure &client)
      : client_(client), started_(millis()), lastData_(started_) {}

  bool expired() const {
    const uint32_t now = millis();
    return static_cast<uint32_t>(now - started_) >= TotalTimeoutMs ||
           static_cast<uint32_t>(now - lastData_) >= IdleTimeoutMs;
  }

  bool read(uint8_t *buffer, size_t capacity, size_t &received) {
    received = 0;
    while (!expired()) {
      const int available = client_.available();
      if (available > 0) {
        const size_t amount = static_cast<size_t>(available) < capacity
                                  ? static_cast<size_t>(available) : capacity;
        const int count = client_.read(buffer, amount);
        if (count > 0) {
          // A blocking RPC can itself return after the application's deadline.
          if (expired()) return false;
          received = static_cast<size_t>(count);
          lastData_ = millis();
          return true;
        }
      }
      if (!client_.connected()) return false;
      delay(1);
    }
    return false;
  }

  bool exact(uint8_t *buffer, size_t size) {
    while (size) {
      size_t count = 0;
      if (!read(buffer, size, count)) return false;
      buffer += count;
      size -= count;
    }
    return true;
  }

  // Includes CRLF in the global bound. Rejects bare LF, embedded NUL, control
  // characters, and oversized lines rather than accumulating unbounded String.
  bool line(char *buffer, size_t capacity, size_t &headerBytes) {
    size_t used = 0;
    bool cr = false;
    while (true) {
      uint8_t byte = 0;
      if (!exact(&byte, 1)) return false;
      if (++headerBytes > MaxHeaderBytes) { tooLarge_ = true; return false; }
      if (cr) {
        if (byte != '\n') return false;
        buffer[used] = 0;
        return true;
      }
      if (byte == '\r') { cr = true; continue; }
      if (byte == '\n' || byte == 0 || (byte < 0x20 && byte != '\t'))
        return false;
      if (used + 1 >= capacity) { tooLarge_ = true; return false; }
      buffer[used++] = static_cast<char>(byte);
    }
  }

  bool tooLarge() const { return tooLarge_; }

 private:
  WiFiClientSecure &client_;
  uint32_t started_, lastData_;
  bool tooLarge_ = false;
};

struct Cleanup {
  WiFiClientSecure &client;
  nice_gfx_map::ByteBuffer &output;
  bool retainCapacity = false;
  bool keep = false;
  ~Cleanup() {
    client.stop();
    if (!keep) {
      if (retainCapacity) output.clear();
      else output.reset();
    }
  }
};

inline bool chunkSize(const char *text, size_t &size) {
  size = 0;
  bool digitSeen = false;
  while (*text && *text != ';') {
    unsigned digit;
    if (*text >= '0' && *text <= '9') digit = *text - '0';
    else if (*text >= 'a' && *text <= 'f') digit = *text - 'a' + 10;
    else if (*text >= 'A' && *text <= 'F') digit = *text - 'A' + 10;
    else return false;
    if (size > MaxBodyBytes / 16 ||
        (size == MaxBodyBytes / 16 && digit > MaxBodyBytes % 16)) return false;
    size = size * 16 + digit;
    digitSeen = true;
    ++text;
  }
  // Extensions are ignored after the semicolon, within line/header bounds.
  return digitSeen;
}
}  // namespace https_detail

namespace https_detail {
inline DownloadResult downloadResponse(const std::string &url,
                                   nice_gfx_map::ByteBuffer &output,
                                   const char *rootCa, size_t maxBytes, bool retainCapacity) {
  using nice_gfx_map::Status;
  using namespace https_detail;
  DownloadResult result;
  if (retainCapacity) output.clear();
  else output.reset();
  Url parsed;
  if (!maxBytes || maxBytes > MaxBodyBytes || !rootCa || !*rootCa ||
      !splitUrl(url, parsed)) {
    result.status = Status::InvalidArgument;
    return result;
  }
  // Reserve the reusable image buffer before TLS/RPC allocations can split the
  // free heap. Later smaller responses reuse it without shrinking or freeing.
  if (retainCapacity) {
    result.phase = "allocate";
    result.status = output.reserve(maxBytes);
    if (result.status != Status::Ok) return result;
    result.status = Status::NetworkError;
  }
  WiFiClientSecure client;
  Cleanup cleanup{client, output, retainCapacity};
  client.setCACert(rootCa);
  result.phase = "tls";
  // rpcWiFi's setTimeout() is a no-op; this overload sets the actual TLS timeout.
  if (!client.connect(parsed.host.c_str(), parsed.port, ConnectTimeoutMs)) {
#if defined(SEEED_WIO_TERMINAL)
    char message[100] = {};
    result.networkCode = client.lastError(message, sizeof(message));
#endif
    return result;
  }
  result.phase = "request";
  const std::string request =
      // HTTP/1.0 permits close-delimited replies, which the RPC client cannot
      // reliably frame. HTTP/1.1 lets the service use bounded chunked replies.
      "GET " + parsed.target + " HTTP/1.1\r\nHost: " + parsed.authority +
      "\r\nUser-Agent: Mozilla/5.0 (compatible; NICE_GFX_MAP; WioTerminal)\r\n"
      "Accept: image/png,image/jpeg\r\n"
      "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
  if (client.write(reinterpret_cast<const uint8_t *>(request.data()),
                   request.size()) != request.size()) return result;

  Reader reader(client);
  result.phase = "headers";
  char line[512];
  size_t headerBytes = 0;
  if (!reader.line(line, sizeof(line), headerBytes)) {
    if (reader.tooLarge()) result.status = Status::TooLarge;
    return result;
  }
  if (std::strlen(line) < 12 ||
      (std::strncmp(line, "HTTP/1.0 ", 9) && std::strncmp(line, "HTTP/1.1 ", 9)) ||
      line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' ||
      line[11] < '0' || line[11] > '9' || (line[12] && line[12] != ' '))
    return result;
  result.httpStatus = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
  if (result.httpStatus != 200) return result;  // Never follow redirects.

  size_t contentLength = 0;
  bool hasLength = false, chunked = false, hasEncoding = false;
  while (true) {
    if (!reader.line(line, sizeof(line), headerBytes)) {
      if (reader.tooLarge()) result.status = Status::TooLarge;
      return result;
    }
    if (!line[0]) break;
    char *colon = std::strchr(line, ':');
    if (!colon || colon == line || line[0] == ' ' || line[0] == '\t') return result;
    *colon = 0;
    char *value = trim(colon + 1);
    if (asciiEqual(line, "Content-Length")) {
      if (hasLength || !decimal(value, maxBytes, contentLength)) {
        result.status = Status::TooLarge;
        result.phase = "content_length";
        return result;
      }
      hasLength = true;
    } else if (asciiEqual(line, "Transfer-Encoding")) {
      if (hasEncoding || !asciiEqual(value, "chunked")) {
        result.status = Status::Unsupported;
        result.phase = "transfer_encoding";
        return result;
      }
      hasEncoding = chunked = true;
    } else if (asciiEqual(line, "Content-Encoding") && !asciiEqual(value, "identity")) {
      result.status = Status::Unsupported;
      result.phase = "content_encoding";
      return result;
    }
  }
  // No close-delimited body: RPC connected() is not a reliable framing signal.
  if ((!hasLength && !chunked) || (hasLength && chunked)) {
    result.status = Status::Unsupported;
    result.phase = hasLength ? "framing_conflict" : "framing_missing";
    return result;
  }
  if (hasLength && !contentLength) return result;
  // Known lengths reserve only the required bytes. Chunked replies reserve the
  // caller's bound, never an unbounded body or the global 192 KiB ceiling.
  result.phase = "allocate";
  result.status = output.reserve(hasLength ? contentLength : maxBytes);
  if (result.status != Status::Ok) return result;
  result.status = Status::NetworkError;
  result.phase = "body";
  uint8_t buffer[512];
  while (true) {
    size_t remaining = contentLength;
    if (chunked) {
      if (!reader.line(line, sizeof(line), headerBytes)) {
        if (reader.tooLarge()) result.status = Status::TooLarge;
        return result;
      }
      if (!chunkSize(line, remaining)) return result;
      if (!remaining) {
        // Consume bounded trailers; they must retain ordinary header syntax.
        while (true) {
          if (!reader.line(line, sizeof(line), headerBytes)) {
            if (reader.tooLarge()) result.status = Status::TooLarge;
            return result;
          }
          if (!line[0]) break;
          if (line[0] == ' ' || line[0] == '\t' || !std::strchr(line, ':'))
            return result;
        }
        break;
      }
    }
    if (remaining > maxBytes - result.bytes) {
      result.status = Status::TooLarge;
      return result;
    }
    while (remaining) {
      const size_t amount = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
      size_t received = 0;
      if (!reader.read(buffer, amount, received)) return result;
      const Status appended = output.append(buffer, received);
      if (appended != Status::Ok) { result.status = appended; return result; }
      result.bytes += received;
      remaining -= received;
      delay(1);
    }
    if (!chunked) break;
    uint8_t crlf[2];
    if (!reader.exact(crlf, 2) || crlf[0] != '\r' || crlf[1] != '\n') return result;
  }
  if (!result.bytes) return result;
  if (output.size() != result.bytes) return result;
  cleanup.keep = true;
  result.phase = "complete";
  result.status = Status::Ok;
  return result;
}
}  // namespace https_detail

inline DownloadResult downloadToBuffer(const std::string &url,
                                   nice_gfx_map::ByteBuffer &output,
                                   const char *rootCa, size_t maxBytes = 48 * 1024,
                                   bool retainCapacity = false) {
  // downloadResponse's client and Cleanup have both been destroyed here, so
  // TLS/RPC receive buffers are released before compacting a chunked response.
  DownloadResult result = https_detail::downloadResponse(url, output, rootCa, maxBytes, retainCapacity);
  if (result.status != nice_gfx_map::Status::Ok) return result;
  if (retainCapacity) return result;
  result.phase = "compact";
  result.status = output.shrinkToFit();
  if (result.status != nice_gfx_map::Status::Ok) {
    output.reset();
    return result;
  }
  result.phase = "complete";
  return result;
}

}  // namespace wio_map
