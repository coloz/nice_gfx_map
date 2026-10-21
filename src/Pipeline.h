#pragma once
#include "Map.h"

namespace nice_gfx_map {
class ByteBuffer {
 public:
  ByteBuffer() = default;
  ~ByteBuffer();
  ByteBuffer(ByteBuffer &&other) noexcept;
  ByteBuffer &operator=(ByteBuffer &&other) noexcept;
  ByteBuffer(const ByteBuffer &) = delete;
  ByteBuffer &operator=(const ByteBuffer &) = delete;
  Status assign(const uint8_t *bytes, size_t count, Allocator allocator = {});
  Status reserve(size_t capacity, Allocator allocator = {});
  // Release unused capacity. Default heap storage uses realloc, avoiding a
  // second image-sized allocation while shrinking on embedded newlib heaps.
  // Custom allocators use allocate/copy/release; failure preserves the buffer.
  Status shrinkToFit();
  Status append(const uint8_t *bytes, size_t count);
  void clear() { size_ = 0; }
  void reset();
  uint8_t *data() { return data_; }
  const uint8_t *data() const { return data_; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
 private:
  uint8_t *data_ = nullptr;
  size_t size_ = 0, capacity_ = 0;
  Allocator allocator_;
};

// Fetch into a bounded owned buffer. Implementations handle HTTP error bodies,
// chunked transfers, TLS verification, timeouts and maximum response size.
class Transport {
 public:
  virtual ~Transport() = default;
  virtual Status fetch(const TileRequest &request, ByteBuffer &output) = 0;
};
class Decoder {
 public:
  virtual ~Decoder() = default;
  virtual Status decode(const TileRequest &request, const uint8_t *bytes,
                        size_t count, TileImage &output) = 0;
};
// Bounded synchronous convenience: at most one tile per call. Network I/O can
// block up to the transport timeout. For responsive input use nextRequest in
// the owner task, fetch/decode in a worker, then submit/fail in the owner task.
class TileLoader {
 public:
  // Optional monotonic clock samples completion time for failure backoff after
  // slow network requests. Without a clock, nowMs is also the retry time origin.
  TileLoader(Transport &transport, Decoder &decoder, uint32_t (*clock)() = nullptr)
      : transport_(transport), decoder_(decoder), clock_(clock) {}
  Status loadNext(Map &map, uint32_t nowMs = 0);
 private:
  Transport &transport_;
  Decoder &decoder_;
  uint32_t (*clock_)() = nullptr;
};
} // namespace nice_gfx_map
