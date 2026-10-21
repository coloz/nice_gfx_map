#include "Esp32PpaRaster.h"
#include "Esp32.h"
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#include <soc/soc_caps.h>
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S31) && defined(SOC_PPA_SUPPORTED) && SOC_PPA_SUPPORTED
#define NICE_MAP_S31_PPA_RASTER 1
#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#else
#define NICE_MAP_S31_PPA_RASTER 0
#endif

namespace nice_gfx_map {
namespace {
struct RasterContext {
  Esp32PpaRasterStats stats;
#if NICE_MAP_S31_PPA_RASTER
  ppa_client_handle_t client = nullptr;
  ~RasterContext() { if (client) ppa_unregister_client(client); }
#endif
};
RasterContext &context() { static RasterContext value; return value; }

#if NICE_MAP_S31_PPA_RASTER
bool eligible(const RasterBlitRequest &r, size_t &outputBytes, size_t &line) {
  // DMA2D picture dimensions are 14-bit. Use the original picture base and
  // complete rows: an interior pointer would misdescribe the final row.
  if (!r.source || !r.destination || !r.width || !r.height ||
      !r.sourceWidth || !r.sourceHeight || !r.destinationWidth || !r.destinationHeight ||
      r.sourceWidth > r.sourceStride || r.destinationWidth > r.destinationStride ||
      r.sourceStride >= 0x4000 || r.sourceHeight >= 0x4000 ||
      r.destinationStride >= 0x4000 || r.destinationHeight >= 0x4000 ||
      r.sourceX > r.sourceWidth || r.width > r.sourceWidth - r.sourceX ||
      r.sourceY > r.sourceHeight || r.height > r.sourceHeight - r.sourceY ||
      r.destinationX > r.destinationWidth || r.width > r.destinationWidth - r.destinationX ||
      r.destinationY > r.destinationHeight || r.height > r.destinationHeight - r.destinationY ||
      r.sourceCapacityPixels > SIZE_MAX / 2 || r.destinationCapacityPixels > SIZE_MAX / 2 ||
      r.width * r.height < 4096) return false;
  const size_t sourcePixels = r.sourceStride * r.sourceHeight;
  const size_t outputPixels = r.destinationStride * r.destinationHeight;
  if (r.sourceCapacityPixels < sourcePixels || r.destinationCapacityPixels < outputPixels) return false;
  const uintptr_t source = reinterpret_cast<uintptr_t>(r.source);
  const uintptr_t output = reinterpret_cast<uintptr_t>(r.destination);
  const size_t sourceCapacity = r.sourceCapacityPixels * 2;
  const size_t outputCapacity = r.destinationCapacityPixels * 2;
  outputBytes = outputPixels * 2;
  line = esp_cache_get_line_size_by_addr(r.destination);
  // Logical picture bytes must be aligned themselves; the allocator's hidden
  // rounding is deliberately not treated as caller-visible capacity.
  if ((source & 1) || (output & 63) || !line || (line & (line - 1)) || line > 64 ||
      (outputBytes & 63) || outputBytes > UINT32_MAX ||
      source > UINTPTR_MAX - sourceCapacity || output > UINTPTR_MAX - outputCapacity ||
      (output < source + sourceCapacity && source < output + outputCapacity) ||
      (!esp_ptr_internal(r.destination) && !esp_ptr_external_ram(r.destination))) return false;
  return true;
}

void *allocate(size_t bytes, void *) {
  if (!bytes || bytes > SIZE_MAX - 63) return nullptr;
  bytes = (bytes + 63) & ~size_t(63);
  void *pixels = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pixels) pixels = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return pixels;
}
void release(void *pixels, void *) { heap_caps_free(pixels); }
#endif

Status copy(const RasterBlitRequest &request, void *opaque) {
  auto &state = *static_cast<RasterContext *>(opaque);
#if NICE_MAP_S31_PPA_RASTER
  size_t outputBytes = 0, line = 0;
  if (!eligible(request, outputBytes, line)) { ++state.stats.rejected; return Status::Unsupported; }
  const auto fail = [&](int error) { ++state.stats.failures; state.stats.lastError = error; return Status::IoError; };
  if (!state.client) {
    ppa_client_config_t config = {};
    config.oper_type = PPA_OPERATION_SRM; config.max_pending_trans_num = 1;
    config.data_burst_length = PPA_DATA_BURST_LENGTH_64;
    const auto error = ppa_register_client(&config, &state.client);
    if (error != ESP_OK) return fail(error);
  }
  // The SDK invalidates complete stride rows, rounded to cache lines, before
  // DMA. Preserve dirty neighboring pixels/padding before that invalidation.
  const size_t begin = (request.destinationY * request.destinationStride * 2) & ~(line - 1);
  const size_t end = ((request.destinationY + request.height) * request.destinationStride * 2 + line - 1) & ~(line - 1);
  auto *region = reinterpret_cast<uint8_t *>(request.destination) + begin;
  int64_t started = esp_timer_get_time();
  auto error = esp_cache_msync(region, end - begin, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  state.stats.syncMicros += uint64_t(esp_timer_get_time() - started);
  if (error != ESP_OK) return fail(error);
  ppa_srm_oper_config_t operation = {};
  operation.in.buffer = request.source;
  operation.in.pic_w = request.sourceStride; operation.in.pic_h = request.sourceHeight;
  operation.in.block_w = request.width; operation.in.block_h = request.height;
  operation.in.block_offset_x = request.sourceX; operation.in.block_offset_y = request.sourceY;
  operation.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  operation.out.buffer = request.destination; operation.out.buffer_size = outputBytes;
  operation.out.pic_w = request.destinationStride; operation.out.pic_h = request.destinationHeight;
  operation.out.block_offset_x = request.destinationX; operation.out.block_offset_y = request.destinationY;
  operation.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  operation.scale_x = operation.scale_y = 1.0f;
  operation.mode = PPA_TRANS_MODE_BLOCKING;
  // The SDK writes back the immutable source's complete affected rows.
  started = esp_timer_get_time();
  error = ppa_do_scale_rotate_mirror(state.client, &operation);
  state.stats.driverMicros += uint64_t(esp_timer_get_time() - started);
  if (error != ESP_OK) return fail(error);
  started = esp_timer_get_time();
  error = esp_cache_msync(region, end - begin, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  state.stats.syncMicros += uint64_t(esp_timer_get_time() - started);
  if (error != ESP_OK) return fail(error);
  ++state.stats.completed; state.stats.pixels += request.width * request.height;
  state.stats.lastError = ESP_OK;
  return Status::Ok;
#else
  (void)request; ++state.stats.rejected; return Status::Unsupported;
#endif
}
} // namespace
RasterBlitter esp32PpaRasterBlitter() { return {copy, &context()}; }
const Esp32PpaRasterStats &esp32PpaRasterStats() { return context().stats; }
Allocator esp32PpaRasterAllocator() {
#if NICE_MAP_S31_PPA_RASTER
  return {allocate, release, nullptr};
#else
  return esp32PsramAllocator();
#endif
}
} // namespace nice_gfx_map
