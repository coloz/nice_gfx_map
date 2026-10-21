#pragma once
// Example-local, single-owner decoder. The sketch bounds its encoded download
// buffer; a 16 KiB RGB332 work tile leaves room for the PNG decoder on SAMD51.
// Expand to RGB565 only after the encoded buffer/decoder have been released.
#include <Map.h>
#pragma push_macro("local")
#undef local
#include <PNGdec.h>
#pragma pop_macro("local")
#include <JPEGDEC.h>
#include <climits>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#if defined(SEEED_WIO_TERMINAL)
#include <atomic>
#endif

namespace wio_map {
class ReducedTile {
 public:
  ReducedTile() = default;
  ReducedTile(ReducedTile &&) noexcept = default;
  ReducedTile &operator=(ReducedTile &&) noexcept = default;
  ReducedTile(const ReducedTile &) = delete;
  ReducedTile &operator=(const ReducedTile &) = delete;
  nice_gfx_map::Status allocate() {
    std::unique_ptr<uint8_t[]> bytes(new (std::nothrow) uint8_t[128 * 128]);
    if (!bytes) return nice_gfx_map::Status::NoMemory;
    pixels_ = std::move(bytes);
    return nice_gfx_map::Status::Ok;
  }
  void reset() { pixels_.reset(); }
  bool valid() const { return bool(pixels_); }
  uint16_t width() const { return pixels_ ? 128 : 0; }
  uint16_t height() const { return pixels_ ? 128 : 0; }
  size_t bytes() const { return pixels_ ? 128 * 128 : 0; }
  uint16_t read565(size_t index) const {
    if (!pixels_ || index >= 128 * 128) return 0;
    const unsigned color = pixels_[index];
    const unsigned r = color >> 5, g = (color >> 2) & 7, b = color & 3;
    return uint16_t(((r << 2 | r >> 1) << 11) | ((g << 3 | g) << 5) | (b << 3 | b << 1 | b >> 1));
  }
  void write565(size_t index, uint16_t color) {
    if (pixels_ && index < 128 * 128)
      pixels_[index] = uint8_t(((color >> 8) & 0xe0) | ((color >> 6) & 0x1c) | ((color >> 3) & 3));
  }
  // Caller releases its encoded download buffer first. Reset the destination
  // before allocating, so a previous RGB565 tile never doubles the peak.
  nice_gfx_map::Status expandTo(nice_gfx_map::TileImage &output) const {
    if (!pixels_) return nice_gfx_map::Status::InvalidArgument;
    output.reset();
    const auto status = output.allocate(128, 128);
    if (status != nice_gfx_map::Status::Ok) return status;
    for (size_t i = 0; i < 128 * 128; ++i) output.pixels()[i] = read565(i);
    return nice_gfx_map::Status::Ok;
  }
 private:
  std::unique_ptr<uint8_t[]> pixels_;
};
namespace detail {
using nice_gfx_map::Status;
using nice_gfx_map::TileImage;
constexpr uint16_t kEncodedSize = 256, kTileSize = 128;
constexpr size_t kMaximumEncodedBytes = 512 * 1024;

#if defined(SEEED_WIO_TERMINAL)
// One aligned BSS allocation is shared by PNG and JPEG. Repeatedly allocating
// the large decoder objects fragments SAMD51's small newlib heap. Only one
// decoder may own this workspace; concurrent/reentrant use returns Busy.
using DecoderStorage = std::aligned_union_t<0, PNG, JPEGDEC>;
inline DecoderStorage decoderStorage;
inline std::atomic_flag decoderBusy = ATOMIC_FLAG_INIT;
template<class Decoder> class WorkspaceDecoder {
 public:
  WorkspaceDecoder() {
    if (!decoderBusy.test_and_set(std::memory_order_acquire))
      decoder_ = ::new (static_cast<void *>(&decoderStorage)) Decoder;
  }
  ~WorkspaceDecoder() {
    if (decoder_) {
      decoder_->~Decoder();
      decoderBusy.clear(std::memory_order_release);
    }
  }
  WorkspaceDecoder(const WorkspaceDecoder &) = delete;
  WorkspaceDecoder &operator=(const WorkspaceDecoder &) = delete;
  explicit operator bool() const { return decoder_ != nullptr; }
  Decoder *get() const { return decoder_; }
  Decoder *operator->() const { return decoder_; }
 private:
  Decoder *decoder_ = nullptr;
};
#endif

inline uint32_t read32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint32_t updateCrc(uint32_t value, const uint8_t *bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    value ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
  }
  return value;
}
struct PngInfo {
  uint8_t type = 0, bits = 0;
  bool grayKey = false, rgbKey = false;
  uint16_t transparentGray = 0, transparentRgb[3] = {};
};
// Validate complete chunk boundaries/CRCs before altering the base image and
// retain grayscale/truecolor tRNS (not exposed in PNGDRAW's palette alpha).
inline Status inspectPng(const uint8_t *bytes, size_t size, PngInfo &info) {
  if (!bytes || size < 45) return Status::DecodeError;
  if (size > kMaximumEncodedBytes) return Status::TooLarge;
  const uint8_t expected[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (std::memcmp(bytes, expected, 8)) return Status::DecodeError;
  size_t offset = 8;
  bool header = false, data = false, ended = false, transparency = false;
  while (offset <= size - 12) {
    const uint8_t *chunk = bytes + offset;
    const size_t length = read32(chunk);
    if (length > size - offset - 12) return Status::DecodeError;
    const uint8_t *kind = chunk + 4, *payload = chunk + 8;
    if (!header && (std::memcmp(kind, "IHDR", 4) || length != 13)) return Status::DecodeError;
    const bool ihdr = !std::memcmp(kind, "IHDR", 4);
    const bool trns = !std::memcmp(kind, "tRNS", 4);
    if ((ihdr && header) || (trns && (data || transparency))) return Status::DecodeError;
    if (ihdr || trns) {
      if (ihdr) {
        if (read32(payload) != kEncodedSize || read32(payload + 4) != kEncodedSize)
          return Status::InvalidArgument;
        info.bits = payload[8]; info.type = payload[9];
        const bool packed = info.type == 0 || info.type == 3;
        if ((packed && info.bits != 1 && info.bits != 2 && info.bits != 4 && info.bits != 8) ||
            (!packed && info.bits != 8) ||
            (info.type != 0 && info.type != 2 && info.type != 3 && info.type != 4 && info.type != 6) ||
            payload[10] || payload[11] || payload[12]) return Status::Unsupported;
        header = true;
      } else {
        transparency = true;
        if (info.type == 0 && length == 2) {
          info.grayKey = true; info.transparentGray = uint16_t(payload[0]) * 256 + payload[1];
        } else if (info.type == 2 && length == 6) {
          info.rgbKey = true;
          for (unsigned i = 0; i < 3; ++i)
            info.transparentRgb[i] = uint16_t(payload[2 * i]) * 256 + payload[2 * i + 1];
        } else if (info.type != 3 || !length || length > 256) return Status::DecodeError;
      }
    }
    const uint32_t crc = updateCrc(0xffffffffu, kind, length + 4) ^ 0xffffffffu;
    if (crc != read32(payload + length)) return Status::DecodeError;
    if (!std::memcmp(kind, "IDAT", 4)) data = true;
    if (!std::memcmp(kind, "IEND", 4)) {
      if (length || !data || offset + 12 != size) return Status::DecodeError;
      ended = true; break;
    }
    offset += length + 12;
  }
  return header && data && ended ? Status::Ok : Status::DecodeError;
}
struct Rgba { uint8_t r = 0, g = 0, b = 0, a = 255; };
inline bool pixel(const PNGDRAW &line, int x, const PngInfo &info, Rgba &out) {
  const uint8_t *source = line.pPixels;
  if (line.iPixelType == 0 || line.iPixelType == 3) {
    const unsigned bits = unsigned(line.iBpp);
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8) return false;
    const unsigned mask = (1u << bits) - 1;
    const unsigned value = (source[size_t(x) * bits / 8] >> (8 - bits - (unsigned(x) * bits % 8))) & mask;
    if (line.iPixelType == 3) {
      if (!line.pPalette) return false;
      out.r = line.pPalette[value * 3]; out.g = line.pPalette[value * 3 + 1]; out.b = line.pPalette[value * 3 + 2];
      if (line.iHasAlpha) out.a = line.pPalette[768 + value];
    } else {
      out.r = out.g = out.b = uint8_t(value * 255 / mask);
      if (info.grayKey && value == info.transparentGray) out.a = 0;
    }
  } else if (line.iPixelType == 2 || line.iPixelType == 6) {
    source += size_t(x) * (line.iPixelType == 6 ? 4 : 3);
    out.r = source[0]; out.g = source[1]; out.b = source[2];
    if (line.iPixelType == 6) out.a = source[3];
    else if (info.rgbKey && out.r == info.transparentRgb[0] && out.g == info.transparentRgb[1] && out.b == info.transparentRgb[2]) out.a = 0;
  } else if (line.iPixelType == 4) {
    source += size_t(x) * 2;
    out.r = out.g = out.b = source[0]; out.a = source[1];
  } else return false;
  return true;
}
struct PngContext {
  struct Sum { uint32_t r = 0, g = 0, b = 0; uint16_t a = 0; } top[kTileSize];
  ReducedTile *image = nullptr;
  PngInfo info;
  uint16_t rows = 0;
  bool failed = false, overlay = false;
};
inline int drawPng(PNGDRAW *line) {
  auto *context = line ? static_cast<PngContext *>(line->pUser) : nullptr;
  if (!context || !context->image) return 0;
  const size_t channels = line->iPixelType == 2 ? 3 : line->iPixelType == 6 ? 4 : line->iPixelType == 4 ? 2 : 1;
  if (!line->pPixels || line->y != context->rows || line->y < 0 || line->y >= kEncodedSize ||
      line->iWidth != kEncodedSize || line->iBpp != context->info.bits || line->iPixelType != context->info.type ||
      line->iPitch < 0 || size_t(line->iPitch) < (kEncodedSize * channels * size_t(line->iBpp) + 7) / 8) {
    context->failed = true; return 0;
  }
  for (unsigned x = 0; x < kTileSize; ++x) {
    Rgba a, b;
    if (!pixel(*line, int(x * 2), context->info, a) || !pixel(*line, int(x * 2 + 1), context->info, b)) {
      context->failed = true; return 0;
    }
    auto &sum = context->top[x];
    if (!(line->y & 1)) sum = {};
    sum.r += unsigned(a.r) * a.a + unsigned(b.r) * b.a;
    sum.g += unsigned(a.g) * a.a + unsigned(b.g) * b.a;
    sum.b += unsigned(a.b) * a.a + unsigned(b.b) * b.a;
    sum.a += unsigned(a.a) + b.a;
    if (!(line->y & 1)) continue;
    const size_t outputIndex = size_t(line->y / 2) * kTileSize + x;
    if (context->overlay && !sum.a) continue;
    const uint16_t bg = context->overlay ? context->image->read565(outputIndex) : 0xffff;
    const unsigned br = ((bg >> 11) & 31) * 255 / 31;
    const unsigned bgGreen = ((bg >> 5) & 63) * 255 / 63;
    const unsigned bb = (bg & 31) * 255 / 31;
    // Average premultiplied coverage over 2 x 2 before compositing, retaining
    // thin annotation strokes without dark fringes at transparent edges.
    const unsigned inverse = 1020 - sum.a;
    const unsigned r = (sum.r + br * inverse + 510) / 1020;
    const unsigned g = (sum.g + bgGreen * inverse + 510) / 1020;
    const unsigned blue = (sum.b + bb * inverse + 510) / 1020;
    context->image->write565(outputIndex, uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (blue >> 3)));
  }
  ++context->rows;
  return 1;
}
template<class Result> inline Result pngCallback(PNGDRAW *line) {
  if constexpr (std::is_void<Result>::value) { drawPng(line); return; }
  else return Result(drawPng(line));
}
inline Status decodePng(const uint8_t *bytes, size_t count, ReducedTile &image, bool overlay) {
  if (!image.valid()) return Status::InvalidArgument;
  PngInfo info;
  const Status inspected = inspectPng(bytes, count, info);
  if (inspected != Status::Ok) return inspected;
#if defined(SEEED_WIO_TERMINAL)
  WorkspaceDecoder<PNG> png;
  if (!png) return Status::Busy;
#else
  std::unique_ptr<PNG> png(new (std::nothrow) PNG);
  if (!png) return Status::NoMemory;
#endif
  std::unique_ptr<PngContext> context(new (std::nothrow) PngContext);
  if (!context) return Status::NoMemory;
  context->image = &image; context->info = info; context->overlay = overlay;
  using Result = typename std::invoke_result<PNG_DRAW_CALLBACK *, PNGDRAW *>::type;
  const int opened = png->openRAM(const_cast<uint8_t *>(bytes), int(count), pngCallback<Result>);
  struct Close { PNG *png; ~Close() { png->close(); } } close{png.get()};
  if (opened != PNG_SUCCESS || png->getWidth() != kEncodedSize || png->getHeight() != kEncodedSize) return Status::DecodeError;
  if (png->isInterlaced()) return Status::Unsupported;
  const int decoded = png->decode(context.get(), PNG_CHECK_CRC);
  return decoded == PNG_SUCCESS && !context->failed && context->rows == kEncodedSize ? Status::Ok : Status::DecodeError;
}
struct JpegContext { ReducedTile *image = nullptr; size_t pixels = 0; bool failed = false; };
inline uint16_t read16(const uint8_t *bytes) { return uint16_t(uint16_t(bytes[0]) << 8 | bytes[1]); }
// Bound every marker before the external decoder reads its header fields.
// Only complete, 8-bit, single-scan baseline grayscale/RGB tiles are used.
inline Status inspectJpeg(const uint8_t *bytes, size_t count) {
  if (count < 4 || bytes[0] != 0xff || bytes[1] != 0xd8 || bytes[count - 2] != 0xff || bytes[count - 1] != 0xd9)
    return Status::DecodeError;
  size_t offset = 2;
  uint8_t components = 0;
  while (offset < count) {
    if (bytes[offset++] != 0xff) return Status::DecodeError;
    while (offset < count && bytes[offset] == 0xff) ++offset;
    if (offset >= count) return Status::DecodeError;
    const uint8_t marker = bytes[offset++];
    if (!marker || marker == 0xd8 || marker == 0xd9 || marker == 0x01 ||
        (marker >= 0xd0 && marker <= 0xd7) || count - offset < 2) return Status::DecodeError;
    const size_t length = read16(bytes + offset);
    if (length < 2 || length > count - offset) return Status::DecodeError;
    const uint8_t *payload = bytes + offset + 2;
    const bool sof = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
    if (sof) {
      if (marker != 0xc0) return Status::Unsupported;
      if (components || length < 8) return Status::DecodeError;
      if (payload[0] != 8) return Status::Unsupported;
      components = payload[5];
      if (components != 1 && components != 3) return Status::Unsupported;
      if (length != size_t(8 + 3 * components)) return Status::DecodeError;
      if (read16(payload + 1) != kEncodedSize || read16(payload + 3) != kEncodedSize) return Status::InvalidArgument;
    }
    if (marker == 0xda) {
      if (!components || length != size_t(6 + 2 * components) || payload[0] != components) return Status::Unsupported;
      if (payload[1 + 2 * components] || payload[2 + 2 * components] != 63 || payload[3 + 2 * components]) return Status::Unsupported;
      offset += length;
      if (offset >= count - 2) return Status::DecodeError;
      while (offset < count) {
        if (bytes[offset++] != 0xff) continue;
        while (offset < count && bytes[offset] == 0xff) ++offset;
        if (offset >= count) return Status::DecodeError;
        const uint8_t code = bytes[offset++];
        if (!code || (code >= 0xd0 && code <= 0xd7)) continue;
        if (code == 0xd9) return offset == count ? Status::Ok : Status::DecodeError;
        return Status::Unsupported;
      }
      return Status::DecodeError;
    }
    offset += length;
  }
  return Status::DecodeError;
}
inline int drawJpeg(JPEGDRAW *block) {
  auto *context = block ? static_cast<JpegContext *>(block->pUser) : nullptr;
  if (!context || !context->image) return 0;
  if (!block->pPixels || block->iBpp != 16 || block->x < 0 || block->y < 0 ||
      block->iWidth <= 0 || block->iWidthUsed <= 0 || block->iWidthUsed > block->iWidth || block->iHeight <= 0 ||
      block->x + block->iWidthUsed > kTileSize || block->y + block->iHeight > kTileSize) {
    context->failed = true; return 0;
  }
  for (int row = 0; row < block->iHeight; ++row)
    for (int column = 0; column < block->iWidthUsed; ++column)
      context->image->write565(size_t(block->y + row) * kTileSize + block->x + column,
                              block->pPixels[size_t(row) * block->iWidth + column]);
  context->pixels += size_t(block->iWidthUsed) * block->iHeight;
  return 1;
}
inline Status decodeJpeg(const uint8_t *bytes, size_t count, ReducedTile &image) {
  const Status inspected = inspectJpeg(bytes, count);
  if (inspected != Status::Ok) return inspected;
#if defined(SEEED_WIO_TERMINAL)
  WorkspaceDecoder<JPEGDEC> jpeg;
  if (!jpeg) return Status::Busy;
#else
  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC);
  if (!jpeg) return Status::NoMemory;
#endif
  const int opened = jpeg->openRAM(const_cast<uint8_t *>(bytes), int(count), drawJpeg);
  struct Close { JPEGDEC *jpeg; ~Close() { jpeg->close(); } } close{jpeg.get()};
  if (!opened || jpeg->getWidth() != kEncodedSize || jpeg->getHeight() != kEncodedSize) return Status::DecodeError;
  if (jpeg->getJPEGType() != JPEG_MODE_BASELINE || (jpeg->getBpp() != 8 && jpeg->getBpp() != 24)) return Status::Unsupported;
  JpegContext context{&image};
  jpeg->setUserPointer(&context);
  jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
  const int decoded = jpeg->decode(0, 0, JPEG_SCALE_HALF);
  return decoded && !context.failed && context.pixels == size_t(kTileSize) * kTileSize ? Status::Ok : Status::DecodeError;
}
} // namespace detail

// Decode genuine 256px PNG/baseline JPEG into a 16 KiB RGB332 work tile. On
// failure output retains its previous image; no synthetic fallback is made.
inline nice_gfx_map::Status decodeBase(const uint8_t *bytes, size_t count, ReducedTile &output) {
  using nice_gfx_map::Status;
  if (!bytes || count < 8) return Status::InvalidArgument;
  if (count > detail::kMaximumEncodedBytes || count > size_t(INT_MAX)) return Status::TooLarge;
  const uint8_t pngSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  const bool png = !std::memcmp(bytes, pngSignature, 8);
  const bool jpeg = bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
  if (!png && !jpeg) return Status::Unsupported;
  ReducedTile image;
  const Status allocated = image.allocate();
  if (allocated != Status::Ok) return allocated;
  const Status status = png ? detail::decodePng(bytes, count, image, false) : detail::decodeJpeg(bytes, count, image);
  if (status == Status::Ok) output = std::move(image);
  return status;
}

// Blend PNG labels directly into the base tile. A late decoding/I/O error can
// leave partial labels; callers should discard/redownload that tile on error.
inline nice_gfx_map::Status blendLabels(const uint8_t *bytes, size_t count, ReducedTile &image) {
  if (!bytes || count < 8) return nice_gfx_map::Status::InvalidArgument;
  return detail::decodePng(bytes, count, image, true);
}
} // namespace wio_map
