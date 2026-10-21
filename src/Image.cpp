#include "Map.h"
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace nice_gfx_map {
namespace {
void *allocateHeap(size_t bytes, void *) { return std::malloc(bytes); }
void releaseHeap(void *pointer, void *) { std::free(pointer); }
} // namespace

TileImage::~TileImage() { reset(); }

TileImage::TileImage(TileImage &&other) noexcept { *this = std::move(other); }

TileImage &TileImage::operator=(TileImage &&other) noexcept {
  if (this == &other) return *this;
  reset();
  storage_ = other.storage_;
  width_ = other.width_;
  height_ = other.height_;
  bytes_ = other.bytes_;
  alpha_ = other.alpha_;
  allocator_ = other.allocator_;
  other.storage_ = nullptr;
  other.width_ = other.height_ = 0;
  other.bytes_ = 0;
  other.alpha_ = false;
  other.allocator_ = {};
  return *this;
}

Status TileImage::allocate(uint16_t width, uint16_t height, bool withAlpha,
                           Allocator allocator) {
  if (!width || !height || bool(allocator.allocate) != bool(allocator.release))
    return Status::InvalidArgument;
  if (width > 4096 || height > 4096) return Status::TooLarge;
  const size_t channels = sizeof(uint16_t) + (withAlpha ? 1u : 0u);
  const size_t limit = std::numeric_limits<size_t>::max();
  if (size_t(height) > limit / size_t(width)) return Status::TooLarge;
  const size_t count = size_t(width) * size_t(height);
  if (count > limit / channels) return Status::TooLarge;
  const size_t bytes = count * channels;
  if (!allocator.allocate) allocator = {allocateHeap, releaseHeap, nullptr};
  void *storage = allocator.allocate(bytes, allocator.context);
  if (!storage) return Status::NoMemory;
  // RGB565 access requires word alignment even when a custom allocator is used.
  if (reinterpret_cast<uintptr_t>(storage) % alignof(uint16_t)) {
    allocator.release(storage, allocator.context);
    return Status::InvalidArgument;
  }
  std::memset(storage, 0, count * sizeof(uint16_t));
  if (withAlpha)
    std::memset(static_cast<uint8_t *>(storage) + count * sizeof(uint16_t), 255, count);
  // Publish only after allocation succeeds, preserving the previous image on
  // all parameter/allocation failures, including allocator mismatches.
  reset();
  storage_ = storage;
  width_ = width;
  height_ = height;
  bytes_ = bytes;
  alpha_ = withAlpha;
  allocator_ = allocator;
  return Status::Ok;
}

void TileImage::reset() {
  if (storage_ && allocator_.release) allocator_.release(storage_, allocator_.context);
  storage_ = nullptr;
  width_ = height_ = 0;
  bytes_ = 0;
  alpha_ = false;
  allocator_ = {};
}

bool TileImage::valid() const {
  if (!storage_ || !width_ || !height_ || width_ > 4096 || height_ > 4096 ||
      !allocator_.allocate || !allocator_.release) return false;
  const size_t count = size_t(width_) * size_t(height_);
  return bytes_ == count * (sizeof(uint16_t) + (alpha_ ? 1u : 0u));
}

uint8_t *TileImage::alpha() {
  return alpha_ && storage_
      ? static_cast<uint8_t *>(storage_) + size_t(width_) * height_ * sizeof(uint16_t)
      : nullptr;
}

const uint8_t *TileImage::alpha() const {
  return alpha_ && storage_
      ? static_cast<const uint8_t *>(storage_) + size_t(width_) * height_ * sizeof(uint16_t)
      : nullptr;
}

} // namespace nice_gfx_map
