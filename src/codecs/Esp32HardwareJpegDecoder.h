#pragma once
// Optional ESP-IDF hardware JPEG adapter (ESP32-S31 / ESP32-P4).
#include "../Pipeline.h"
#include "JpegHeader.h"
#include <soc/soc_caps.h>
#if !defined(SOC_JPEG_DECODE_SUPPORTED) || !SOC_JPEG_DECODE_SUPPORTED
#error "Esp32HardwareJpegDecoder requires the SoC hardware JPEG decoder."
#endif
#include <driver/jpeg_decode.h>
#include <esp_heap_caps.h>
#include <limits>

namespace nice_gfx_map {
// A decoder has one owner task. Its lazily created engine is reused by tiles.
// Hardware writes native RGB565 directly into a driver's aligned PSRAM tile;
// no RGB888 image or CPU color conversion is allocated. The final tile owns
// that allocation and may outlive the decoder. PNG labels use PngDecoder.
// Baseline RGB/YUV square tiles of 16..512 px, in multiples of 16, are accepted.
// This excludes padded DMA output and grayscale-to-RGB conversion ambiguity.
class Esp32HardwareJpegDecoder : public Decoder {
 public:
  explicit Esp32HardwareJpegDecoder(size_t maxEncodedBytes = 512 * 1024, int timeoutMs = 1000)
      : maxEncodedBytes_(maxEncodedBytes), timeoutMs_(timeoutMs > 0 ? timeoutMs : 1000) {}
  ~Esp32HardwareJpegDecoder() override { if (engine_) jpeg_del_decoder_engine(engine_); }
  Esp32HardwareJpegDecoder(const Esp32HardwareJpegDecoder &) = delete;
  Esp32HardwareJpegDecoder &operator=(const Esp32HardwareJpegDecoder &) = delete;
  Status decode(const TileRequest &request, const uint8_t *bytes, size_t count, TileImage &output) override {
    if (!bytes || !count || !request.tileSize || request.tileSize > 512) return Status::InvalidArgument;
    if (request.overlay) return Status::Unsupported;
    if (count > maxEncodedBytes_ || count > std::numeric_limits<uint32_t>::max()) return Status::TooLarge;
    detail::JpegHeader header;
    Status status = detail::jpegHeader(bytes, count, header);
    if (status != Status::Ok) return status;
    if (header.width != request.tileSize || header.height != request.tileSize) return Status::InvalidArgument;
    if (request.tileSize % 16 || header.components != 3 ||
        (header.sampling != 0x11 && header.sampling != 0x21 && header.sampling != 0x22)) return Status::Unsupported;
    if (!engine_) {
      jpeg_decode_engine_cfg_t config{}; config.timeout_ms = timeoutMs_;
      const auto result = jpeg_new_decoder_engine(&config, &engine_);
      if (result != ESP_OK) return fromEsp(result);
    }
    TileImage decoded;
    status = decoded.allocate(request.tileSize, request.tileSize, false, outputAllocator());
    if (status != Status::Ok) return status;
    jpeg_decode_cfg_t config{};
    config.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // The SDK names these values by its byte/element-order convention:
    // BGR emits little-endian RGB565 words, matching TileImage native pixels.
    config.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    config.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t written = 0;
    const auto result = jpeg_decoder_process(engine_, &config, bytes, uint32_t(count),
      reinterpret_cast<uint8_t *>(decoded.pixels()), uint32_t(decoded.bytes()), &written);
    if (result != ESP_OK) return fromEsp(result);
    if (written != decoded.bytes()) return Status::DecodeError;
    output = std::move(decoded); ++decodedTiles_;
    return Status::Ok;
  }
  uint64_t decodedTiles() const { return decodedTiles_; }
 private:
  static void *allocateOutput(size_t bytes, void *) {
    jpeg_decode_memory_alloc_cfg_t config{}; config.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    size_t allocated = 0; void *memory = jpeg_alloc_decoder_mem(bytes, &config, &allocated);
    if (memory && allocated < bytes) { heap_caps_free(memory); return nullptr; }
    return memory;
  }
  static void releaseOutput(void *memory, void *) { heap_caps_free(memory); }
  static Allocator outputAllocator() { return {allocateOutput, releaseOutput, nullptr}; }
  static Status fromEsp(esp_err_t result) {
    if (result == ESP_ERR_NO_MEM) return Status::NoMemory;
    if (result == ESP_ERR_NOT_SUPPORTED) return Status::Unsupported;
    return Status::DecodeError;
  }
  jpeg_decoder_handle_t engine_ = nullptr;
  size_t maxEncodedBytes_;
  int timeoutMs_;
  uint64_t decodedTiles_ = 0;
};
} // namespace nice_gfx_map
