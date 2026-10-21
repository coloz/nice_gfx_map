#pragma once
// Optional ESP32 adapter: PNGdec for transparent PNG labels and the official
// ESP-IDF hardware JPEG engine on S31/P4, or the esp_jpeg software component
// on earlier ESP32 targets. No camera hardware or camera driver is used.
#if !defined(ARDUINO_ARCH_ESP32) && !defined(ESP_PLATFORM)
#error "Esp32RasterDecoder requires the ESP32 esp_jpeg SDK component. Use PngDecoder on other targets."
#endif
#include "PngDecoder.h"
#include "JpegHeader.h"
#include <soc/soc_caps.h>
#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED
#include "Esp32HardwareJpegDecoder.h"
#else
#include <jpeg_decoder.h>
#endif
#include <cstring>
#include <limits>

namespace nice_gfx_map {

// A decoder instance has one owner task, matching PngDecoder/Map. Independent
// instances can be used by independent workers. On S31/P4, hardware produces
// aligned PSRAM RGB565 output directly. Software JPEG/PNG use the supplied
// allocator; the legacy software path needs temporary RGB888 pixels.
class Esp32RasterDecoder : public Decoder {
 public:
  explicit Esp32RasterDecoder(Allocator allocator = {}, uint16_t pngBackground = 0xffff,
                              size_t maxEncodedBytes = 512 * 1024)
      : png_(allocator, pngBackground), allocator_(allocator), maxEncodedBytes_(maxEncodedBytes)
#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED
      , hardwareJpeg_(maxEncodedBytes)
#endif
      {}

  enum class Backend : uint8_t { None, Png, SoftwareJpeg, HardwareJpeg };
  Backend lastBackend() const { return lastBackend_; }
  const char *lastBackendName() const {
    switch (lastBackend_) {
      case Backend::Png: return "png";
      case Backend::SoftwareJpeg: return "software-jpeg";
      case Backend::HardwareJpeg: return "hardware-jpeg";
      default: return "none";
    }
  }
  uint64_t hardwareJpegTiles() const {
#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED
    return hardwareJpeg_.decodedTiles();
#else
    return 0;
#endif
  }

  Status decode(const TileRequest &request, const uint8_t *bytes,
                size_t count, TileImage &output) override {
    lastBackend_ = Backend::None;
    if (!bytes || !count || !request.tileSize || request.tileSize > 512)
      return Status::InvalidArgument;
    if (count > maxEncodedBytes_ || count > std::numeric_limits<uint32_t>::max())
      return Status::TooLarge;
    static constexpr uint8_t pngSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (count >= sizeof(pngSignature) && !std::memcmp(bytes, pngSignature, sizeof(pngSignature))) {
      lastBackend_ = Backend::Png;
      return png_.decode(request, bytes, count, output);
    }
    if (count < 3 || bytes[0] != 0xff || bytes[1] != 0xd8 || bytes[2] != 0xff)
      return Status::DecodeError; // Includes HTML, JSON and service error responses.
    if (request.overlay) return Status::Unsupported; // JPEG has no label transparency.
#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED
    lastBackend_ = Backend::HardwareJpeg;
    return hardwareJpeg_.decode(request, bytes, count, output);
#else
    lastBackend_ = Backend::SoftwareJpeg;

    // The SDK's get_image_info implementation reads marker fields before
    // checking every input boundary. Validate headers ourselves so malformed
    // HTTP bodies and truncated files never reach that unsafe helper.
    detail::JpegHeader header;
    Status status = detail::jpegHeader(bytes, count, header);
    if (status != Status::Ok) return status;
    if (header.width != request.tileSize || header.height != request.tileSize)
      return Status::InvalidArgument;
    esp_jpeg_image_cfg_t config{};
    config.indata = const_cast<uint8_t *>(bytes); // SDK API is non-const; JPEG input is read only.
    config.indata_size = static_cast<uint32_t>(count);
    config.out_format = JPEG_IMAGE_FORMAT_RGB888;
    config.out_scale = JPEG_IMAGE_SCALE_0;
    const size_t pixels = size_t(request.tileSize) * request.tileSize;
    const size_t rgbBytes = pixels * 3; // tileSize <= 512 keeps arithmetic bounded.

    ByteBuffer rgb;
    status = rgb.reserve(rgbBytes, allocator_);
    if (status != Status::Ok) return status;
    config.outbuf = rgb.data();
    config.outbuf_size = static_cast<uint32_t>(rgbBytes);
    config.priv.read = 0;
    esp_jpeg_image_output_t decodedInfo{};
    const esp_err_t result = esp_jpeg_decode(&config, &decodedInfo);
    if (result != ESP_OK) return result == ESP_ERR_NO_MEM ? Status::NoMemory : Status::DecodeError;
    if (decodedInfo.width != request.tileSize || decodedInfo.height != request.tileSize ||
        decodedInfo.output_len != rgbBytes) return Status::DecodeError;

    TileImage decoded;
    status = decoded.allocate(request.tileSize, request.tileSize, false, allocator_);
    if (status != Status::Ok) return status;
    const uint8_t *source = rgb.data();
    uint16_t *destination = decoded.pixels();
    for (size_t i = 0; i < pixels; ++i, source += 3)
      destination[i] = uint16_t((uint16_t(source[0] >> 3) << 11) |
                                (uint16_t(source[1] >> 2) << 5) | (source[2] >> 3));
    output = std::move(decoded); // Failure paths leave the caller's output unchanged.
    return Status::Ok;
#endif
  }

 private:
  PngDecoder png_;
  Allocator allocator_;
  size_t maxEncodedBytes_;
#if defined(SOC_JPEG_DECODE_SUPPORTED) && SOC_JPEG_DECODE_SUPPORTED
  Esp32HardwareJpegDecoder hardwareJpeg_;
#endif
  Backend lastBackend_ = Backend::None;
};

} // namespace nice_gfx_map
