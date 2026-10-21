#pragma once
#include "../Map.h"
#include <Graphics/Gfx.h>
#include <algorithm>

namespace nice_gfx_map {
class NiceGfxRenderer {
 public:
  // Blocking strip renderer. Own the display for the whole call; every accepted
  // transfer finishes before scratch is reused. Do not draw over a running View.
  static Status draw(const Map &map, NICE_GFX &display, int32_t x, int32_t y,
                     uint16_t *scratch, size_t capacityPixels) {
    const size_t width = map.width(), height = map.height();
    if (!scratch || !width || !height || capacityPixels < width || x < 0 || y < 0 ||
        int64_t(x) + int64_t(width) > display.width() ||
        int64_t(y) + int64_t(height) > display.height()) return Status::InvalidArgument;
    const size_t rows = std::min(height, capacityPixels / width);
    for (size_t top = 0; top < height; top += rows) {
      const size_t count = std::min(rows, height - top);
      Status status = map.renderRegion(0, uint16_t(top), uint16_t(width), uint16_t(count),
                                       scratch, capacityPixels, width);
      if (status != Status::Ok) return status;
      const auto sent = display.flush(x, y + int32_t(top),
        nice_gfx::PixelView(scratch, width, count, width, nice_gfx::PixelOrder::Native), true);
      if (sent != nice_gfx::Status::Ok) { display.wait(); return Status::Busy; }
      if (display.wait() != nice_gfx::Status::Ok) return Status::Busy;
    }
    return Status::Ok;
  }
};
} // namespace nice_gfx_map
