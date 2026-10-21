#pragma once
#include "../Map.h"

namespace nice_gfx_map {
struct Esp32PpaRasterStats {
  uint64_t completed = 0, pixels = 0, driverMicros = 0, syncMicros = 0;
  uint32_t rejected = 0, failures = 0;
  int lastError = 0;
};
// Optional, single-owner-task native RGB565 copier. Every accepted operation
// blocks through DMA completion and makes its result readable by the CPU.
// Unsupported is returned before modifying either image. Other errors abort
// the caller's frame; a partially written destination must not be published.
RasterBlitter esp32PpaRasterBlitter();
const Esp32PpaRasterStats &esp32PpaRasterStats();
// Dedicated publisher storage: S31 uses 64-byte-aligned PSRAM, then internal
// RAM. Other targets retain esp32PsramAllocator(). No blitter pixel scratch.
Allocator esp32PpaRasterAllocator();
} // namespace nice_gfx_map
