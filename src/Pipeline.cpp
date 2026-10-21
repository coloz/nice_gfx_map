#include "Pipeline.h"
#include <cstdlib>
#include <cstring>
#include <utility>

namespace nice_gfx_map {
namespace {
void *defaultAllocate(size_t count, void *) { return std::malloc(count); }
void defaultRelease(void *pointer, void *) { std::free(pointer); }
bool configureAllocator(Allocator &allocator) {
  if (bool(allocator.allocate) != bool(allocator.release)) return false;
  if (!allocator.allocate) allocator = {defaultAllocate, defaultRelease, nullptr};
  return true;
}
}

ByteBuffer::~ByteBuffer() { reset(); }
ByteBuffer::ByteBuffer(ByteBuffer &&other) noexcept { *this = std::move(other); }
ByteBuffer &ByteBuffer::operator=(ByteBuffer &&other) noexcept {
  if (this == &other) return *this;
  reset();
  data_ = other.data_; size_ = other.size_; capacity_ = other.capacity_;
  allocator_ = other.allocator_;
  other.data_ = nullptr; other.size_ = other.capacity_ = 0; other.allocator_ = {};
  return *this;
}
void ByteBuffer::reset() {
  if (data_) allocator_.release(data_, allocator_.context);
  data_ = nullptr; size_ = capacity_ = 0; allocator_ = {};
}
Status ByteBuffer::reserve(size_t capacity, Allocator allocator) {
  if (!configureAllocator(allocator)) return Status::InvalidArgument;
  if (capacity > 8 * 1024 * 1024) return Status::TooLarge;
  if (data_ && capacity <= capacity_) return Status::Ok;
  if (!capacity) return Status::Ok;
  auto *memory = static_cast<uint8_t *>(allocator.allocate(capacity, allocator.context));
  if (!memory) return Status::NoMemory;
  if (size_) std::memcpy(memory, data_, size_);
  if (data_) allocator_.release(data_, allocator_.context);
  data_ = memory; capacity_ = capacity; allocator_ = allocator;
  return Status::Ok;
}
Status ByteBuffer::append(const uint8_t *bytes, size_t count) {
  if ((!bytes && count) || size_ > capacity_) return Status::InvalidArgument;
  if (count > capacity_ - size_) return Status::TooLarge;
  if (count) std::memmove(data_ + size_, bytes, count);
  size_ += count;
  return Status::Ok;
}
Status ByteBuffer::shrinkToFit() {
  if (!size_) { reset(); return Status::Ok; }
  if (size_ == capacity_) return Status::Ok;
  uint8_t *memory = nullptr;
  if (allocator_.allocate == defaultAllocate && allocator_.release == defaultRelease) {
    memory = static_cast<uint8_t *>(std::realloc(data_, size_));
    if (!memory) return Status::NoMemory;
  } else {
    memory = static_cast<uint8_t *>(allocator_.allocate(size_, allocator_.context));
    if (!memory) return Status::NoMemory;
    std::memcpy(memory, data_, size_);
    allocator_.release(data_, allocator_.context);
  }
  data_ = memory;
  capacity_ = size_;
  return Status::Ok;
}
Status ByteBuffer::assign(const uint8_t *bytes, size_t count, Allocator allocator) {
  if (!bytes && count) return Status::InvalidArgument;
  ByteBuffer temporary;
  Status status = temporary.reserve(count, allocator);
  if (status != Status::Ok) return status;
  status = temporary.append(bytes, count);
  if (status == Status::Ok) *this = std::move(temporary);
  return status;
}
Status TileLoader::loadNext(Map &map, uint32_t nowMs) {
  TileRequest request;
  Status status = map.nextRequest(request, clock_ ? clock_() : nowMs);
  if (status != Status::Ok) return status;
  ByteBuffer input;
  status = transport_.fetch(request, input);
  if (status != Status::Ok) { map.fail(request, status, clock_ ? clock_() : nowMs); return status; }
  TileImage image;
  status = decoder_.decode(request, input.data(), input.size(), image);
  if (status != Status::Ok) { map.fail(request, status, clock_ ? clock_() : nowMs); return status; }
  status = map.submit(request, std::move(image));
  if (status != Status::Ok && status != Status::Canceled) map.fail(request, status, clock_ ? clock_() : nowMs);
  return status;
}
} // namespace nice_gfx_map
