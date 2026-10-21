#pragma once
#include "../Map.h"

namespace nice_gfx_map { namespace detail {
struct JpegHeader {
  uint16_t width = 0, height = 0;
  uint8_t components = 0, sampling = 0;
};
inline uint16_t jpegRead16(const uint8_t *bytes) {
  return uint16_t(uint16_t(bytes[0]) << 8 | bytes[1]);
}
// Validate marker boundaries before invoking an SDK parser. Only a complete,
// single-scan baseline JPEG is accepted; service error bodies, truncated
// headers, progressive/CMYK/arithmetic JPEGs do not enter a device decoder.
inline Status jpegHeader(const uint8_t *bytes, size_t count, JpegHeader &output) {
  if (!bytes || count < 4 || bytes[0] != 0xff || bytes[1] != 0xd8 ||
      bytes[count - 2] != 0xff || bytes[count - 1] != 0xd9) return Status::DecodeError;
  JpegHeader header;
  size_t offset = 2;
  while (offset < count) {
    if (bytes[offset++] != 0xff) return Status::DecodeError;
    while (offset < count && bytes[offset] == 0xff) ++offset;
    if (offset >= count) return Status::DecodeError;
    const uint8_t marker = bytes[offset++];
    if (!marker || marker == 0xd8 || marker == 0xd9 || marker == 0x01 ||
        (marker >= 0xd0 && marker <= 0xd7) || count - offset < 2) return Status::DecodeError;
    const size_t length = jpegRead16(bytes + offset);
    if (length < 2 || length > count - offset) return Status::DecodeError;
    const uint8_t *payload = bytes + offset + 2;
    const bool sof = marker >= 0xc0 && marker <= 0xcf &&
                     marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
    if (sof) {
      if (marker != 0xc0) return Status::Unsupported;
      if (header.components || length < 8) return Status::DecodeError;
      if (payload[0] != 8) return Status::Unsupported;
      header.components = payload[5];
      if (header.components != 1 && header.components != 3) return Status::Unsupported;
      if (length != size_t(8 + 3 * header.components)) return Status::DecodeError;
      header.height = jpegRead16(payload + 1); header.width = jpegRead16(payload + 3);
      if (!header.width || !header.height) return Status::DecodeError;
      header.sampling = payload[7];
    }
    if (marker == 0xda) {
      if (!header.components || length != size_t(6 + 2 * header.components) || payload[0] != header.components)
        return Status::Unsupported;
      if (payload[1 + 2 * header.components] || payload[2 + 2 * header.components] != 63 ||
          payload[3 + 2 * header.components]) return Status::Unsupported;
      if (offset + length >= count - 2) return Status::DecodeError;
      output = header; return Status::Ok;
    }
    offset += length;
  }
  return Status::DecodeError;
}
} } // namespace nice_gfx_map::detail
