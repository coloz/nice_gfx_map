#pragma once

// Example-local native 256px PNG renderer. A caller-owned RGB565 row is the
// only pixel buffer: base rows go straight to the LCD and annotation rows use
// LCD readback. The shared decoder workspace remains single-owner.
#include "WioTileDecoder.h"
#include <algorithm>

namespace wio_map {

struct NativeTileTarget {
  int tileX = 0, tileY = 0;
  uint16_t viewportWidth = 0, viewportHeight = 0;
  uint16_t background = 0xffff;
  uint16_t *row = nullptr;
  size_t rowCapacity = 0;
  void *context = nullptr;
  nice_gfx_map::Status (*readRow)(void *, int x, int y, uint16_t *colors, size_t width) = nullptr;
  nice_gfx_map::Status (*writeRow)(void *, int x, int y, const uint16_t *colors, size_t width) = nullptr;
};

namespace native_detail {
using nice_gfx_map::Status;
struct Context {
  const NativeTileTarget *target = nullptr;
  detail::PngInfo info;
  int left = 0, right = 0, top = 0, bottom = 0;
  uint16_t rows = 0;
  bool overlay = false;
  Status status = Status::Ok;
};

inline uint16_t composite(const detail::Rgba &source, uint16_t background) {
  if (!source.a) return background;
  if (source.a == 255)
    return uint16_t((source.r >> 3) << 11 | (source.g >> 2) << 5 | (source.b >> 3));
  const unsigned inverse = 255 - source.a;
  const unsigned red = (unsigned(source.r) * source.a + ((background >> 11) & 31) * 255 / 31 * inverse + 127) / 255;
  const unsigned green = (unsigned(source.g) * source.a + ((background >> 5) & 63) * 255 / 63 * inverse + 127) / 255;
  const unsigned blue = (unsigned(source.b) * source.a + (background & 31) * 255 / 31 * inverse + 127) / 255;
  return uint16_t((red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3));
}

inline int drawRow(PNGDRAW *line) {
  auto *context = line ? static_cast<Context *>(line->pUser) : nullptr;
  if (!context || !context->target || context->status != Status::Ok) return 0;
  const size_t channels = line->iPixelType == 2 ? 3 : line->iPixelType == 6 ? 4 : line->iPixelType == 4 ? 2 : 1;
  if (!line->pPixels || line->y != context->rows || line->y < 0 || line->y >= 256 ||
      line->iWidth != 256 || line->iBpp != context->info.bits || line->iPixelType != context->info.type ||
      line->iPitch < 0 || size_t(line->iPitch) < (256 * channels * size_t(line->iBpp) + 7) / 8) {
    context->status = Status::DecodeError;
    return 0;
  }
  ++context->rows;
  const auto &target = *context->target;
  const int y = target.tileY + line->y;
  if (y < context->top || y >= context->bottom) return 1;
  const int sourceLeft = context->left - target.tileX;
  const size_t width = size_t(context->right - context->left);
  if (context->overlay) {
    bool visible = false;
    for (size_t x = 0; x < width; ++x) {
      detail::Rgba color;
      if (!detail::pixel(*line, sourceLeft + int(x), context->info, color)) {
        context->status = Status::DecodeError;
        return 0;
      }
      visible = visible || color.a != 0;
    }
    if (!visible) return 1; // No display readback for an entirely transparent row.
    context->status = target.readRow(target.context, context->left, y, target.row, width);
    if (context->status != Status::Ok) return 0;
  }
  for (size_t x = 0; x < width; ++x) {
    detail::Rgba color;
    if (!detail::pixel(*line, sourceLeft + int(x), context->info, color)) {
      context->status = Status::DecodeError;
      return 0;
    }
    target.row[x] = composite(color, context->overlay ? target.row[x] : target.background);
  }
  context->status = target.writeRow(target.context, context->left, y, target.row, width);
  return context->status == Status::Ok ? 1 : 0;
}

template<class Result> inline Result callback(PNGDRAW *line) {
  if constexpr (std::is_void<Result>::value) { drawRow(line); return; }
  else return Result(drawRow(line));
}
} // namespace native_detail

// x/y are viewport-local; the LCD adapter adds its HUD offset. Late decode or
// I/O errors can leave rows drawn, so the caller must clear that tile on error.
inline nice_gfx_map::Status drawNativePng(const uint8_t *bytes, size_t count,
                                        const NativeTileTarget &target, bool overlay) {
  using nice_gfx_map::Status;
  if (!bytes || count < 8 || !target.viewportWidth || !target.viewportHeight ||
      !target.writeRow || (overlay && !target.readRow)) return Status::InvalidArgument;
  detail::PngInfo info;
  const Status inspected = detail::inspectPng(bytes, count, info);
  if (inspected != Status::Ok) return inspected;
  const int left = int(std::max<int64_t>(0, target.tileX));
  const int top = int(std::max<int64_t>(0, target.tileY));
  const int right = int(std::min<int64_t>(target.viewportWidth, int64_t(target.tileX) + 256));
  const int bottom = int(std::min<int64_t>(target.viewportHeight, int64_t(target.tileY) + 256));
  if (right <= left || bottom <= top) return Status::Ok;
  if (!target.row || target.rowCapacity < size_t(right - left)) return Status::InvalidArgument;
#if defined(SEEED_WIO_TERMINAL)
  detail::WorkspaceDecoder<PNG> png;
  if (!png) return Status::Busy;
#else
  std::unique_ptr<PNG> png(new (std::nothrow) PNG);
  if (!png) return Status::NoMemory;
#endif
  native_detail::Context context;
  context.target = &target; context.info = info; context.overlay = overlay;
  context.left = left; context.top = top; context.right = right; context.bottom = bottom;
  using Result = typename std::invoke_result<PNG_DRAW_CALLBACK *, PNGDRAW *>::type;
  const int opened = png->openRAM(const_cast<uint8_t *>(bytes), int(count), native_detail::callback<Result>);
  struct Close { PNG *png; ~Close() { png->close(); } } close{png.get()};
  if (opened != PNG_SUCCESS || png->getWidth() != 256 || png->getHeight() != 256) return Status::DecodeError;
  if (png->isInterlaced()) return Status::Unsupported;
  const int decoded = png->decode(&context, PNG_CHECK_CRC);
  if (context.status != Status::Ok) return context.status;
  return decoded == PNG_SUCCESS && context.rows == 256 ? Status::Ok : Status::DecodeError;
}
} // namespace wio_map
