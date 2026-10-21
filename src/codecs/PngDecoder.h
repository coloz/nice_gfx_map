#pragma once
#include "../Pipeline.h"
#pragma push_macro("local")
#undef local
#include <PNGdec.h> // Optional external dependency: bitbank2/PNGdec, tested with 1.1.2.
#pragma pop_macro("local") // Upstream zlib defines `local`; do not leak it into sketches.
#include <climits>
#include <array>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace nice_gfx_map {
// Header-only optional adapter. Include this header only when PNGdec is installed.
// Owns its large PNG decoder on the heap, never on a FreeRTOS task's stack.
// Keeps straight RGB + 8-bit alpha, including indexed 1/2/4/8-bit tRNS labels.
class PngDecoder : public Decoder {
 public:
  explicit PngDecoder(Allocator allocator = {}, uint16_t baseBackground = 0xffff)
      : png_(new (std::nothrow) PNG), allocator_(allocator), background_(baseBackground) {}
  Status decode(const TileRequest &request, const uint8_t *bytes,
                size_t count, TileImage &output) override {
    static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!bytes || count < 33 || count > size_t(INT_MAX) || !request.tileSize ||
        request.tileSize > 512) return Status::InvalidArgument;
    if (std::memcmp(bytes, signature, 8) || read32(bytes + 8) != 13 ||
        std::memcmp(bytes + 12, "IHDR", 4)) return Status::DecodeError;
    const uint32_t width = read32(bytes + 16), height = read32(bytes + 20);
    if (width != request.tileSize || height != request.tileSize) return Status::InvalidArgument;
    const uint8_t bits = bytes[24], type = bytes[25];
    const bool packed = type == 0 || type == 3;
    if ((packed && bits != 1 && bits != 2 && bits != 4 && bits != 8) ||
        (!packed && bits != 8) || (type != 0 && type != 2 && type != 3 && type != 4 && type != 6) ||
        bytes[26] || bytes[27] || bytes[28]) return Status::Unsupported;
    const size_t channels = type == 2 ? 3 : type == 4 ? 2 : type == 6 ? 4 : 1;
    if (2 * ((size_t(width) * channels * bits + 7) / 8 + 1) > PNG_MAX_BUFFERED_PIXELS)
      return Status::TooLarge; // Build PNGdec AND the app with a larger matching line buffer for 512px RGBA.
    if (!png_) return Status::NoMemory;
    Context context;
    context.background = background_;
    // Keep truecolor/grayscale tRNS values as PNGdec's draw callback only gives
    // palette alpha. Chunk lengths are checked before any read or pointer step.
    bool sawData = false, sawEnd = false;
    for (size_t offset = 8; offset <= count - 12;) {
      const size_t length = read32(bytes + offset);
      if (length > count - offset - 12) return Status::DecodeError;
      const uint8_t *kind = bytes + offset + 4, *data = bytes + offset + 8;
      if (crc32(kind, length + 4) != read32(data + length)) return Status::DecodeError;
      if (!std::memcmp(kind, "tRNS", 4)) {
        if (sawData) return Status::DecodeError;
        if (type == 0 && length == 2) {
          context.transparentGray = uint16_t(data[0]) * 256 + data[1]; context.grayKey = true;
        } else if (type == 2 && length == 6) {
          for (int i = 0; i < 3; ++i) context.transparentRgb[i] = uint16_t(data[2*i]) * 256 + data[2*i+1];
          context.rgbKey = true;
        } else if (type != 3) return Status::DecodeError;
      }
      if (!std::memcmp(kind, "IDAT", 4)) sawData = true;
      if (!std::memcmp(kind, "IEND", 4)) {
        if (length || offset + 12 != count) return Status::DecodeError;
        sawEnd = true; break;
      }
      offset += length + 12;
    }
    if (!sawData || !sawEnd) return Status::DecodeError;
    using CallbackResult = typename std::invoke_result<PNG_DRAW_CALLBACK *, PNGDRAW *>::type;
    const int opened = png_->openRAM(const_cast<uint8_t *>(bytes), int(count), callback<CallbackResult>);
    if (opened != PNG_SUCCESS) return opened == PNG_TOO_BIG ? Status::TooLarge : Status::DecodeError;
    struct Close { PNG *png; ~Close() { png->close(); } } close{png_.get()};
    if (png_->getWidth() != int(width) || png_->getHeight() != int(height)) return Status::DecodeError;
    if (png_->isInterlaced()) return Status::Unsupported;
    TileImage decoded;
    Status status = decoded.allocate(uint16_t(width), uint16_t(height), request.overlay, allocator_);
    if (status != Status::Ok) return status;
    context.image = &decoded;
    const int result = png_->decode(&context, PNG_CHECK_CRC);
    if (result != PNG_SUCCESS || context.failed || context.rows != height) return Status::DecodeError;
    output = std::move(decoded);
    return Status::Ok;
  }
 private:
  struct Context {
    TileImage *image = nullptr;
    uint16_t background = 0xffff, transparentGray = 0, transparentRgb[3] = {};
    bool grayKey = false, rgbKey = false, failed = false;
    uint32_t rows = 0;
  };
  static uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  static uint32_t crc32(const uint8_t *bytes, size_t count) {
    static constexpr auto table = []() constexpr {
      std::array<uint32_t, 256> result{};
      for (unsigned i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit)
          value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
        result[i] = value;
      }
      return result;
    }();
    uint32_t value = 0xffffffffu;
    for (size_t i = 0; i < count; ++i) value = table[(value ^ bytes[i]) & 255] ^ (value >> 8);
    return value ^ 0xffffffffu;
  }
  // PNGdec 1.1.2 uses void callbacks; newer revisions allow an int early-abort.
  template<typename Result> static Result callback(PNGDRAW *line) {
    if constexpr (std::is_void<Result>::value) { drawLine(line); return; }
    else return Result(drawLine(line));
  }
  static int drawLine(PNGDRAW *line) {
    auto *context = line ? static_cast<Context *>(line->pUser) : nullptr;
    if (!context || !context->image) return 0;
    auto &image = *context->image;
    if (!line->pPixels || line->y != int(context->rows) || line->y < 0 ||
        line->y >= image.height() || line->iWidth != image.width()) {
      context->failed = true; return 0;
    }
    const size_t channels = line->iPixelType == 2 ? 3 : line->iPixelType == 4 ? 2 : line->iPixelType == 6 ? 4 : 1;
    const size_t rowBytes = (size_t(line->iWidth) * channels * size_t(line->iBpp) + 7) / 8;
    if (line->iPitch < 0 || size_t(line->iPitch) < rowBytes) { context->failed = true; return 0; }
    auto *pixels = image.pixels() + size_t(line->y) * image.width();
    auto *alpha = image.alpha();
    if (alpha) alpha += size_t(line->y) * image.width();
    for (int x = 0; x < line->iWidth; ++x) {
      uint8_t r = 0, g = 0, b = 0, a = 255;
      const uint8_t *source = line->pPixels;
      if (line->iPixelType == 3 || line->iPixelType == 0) {
        const unsigned bits = unsigned(line->iBpp);
        if (bits != 1 && bits != 2 && bits != 4 && bits != 8) { context->failed = true; return 0; }
        const unsigned mask = (1u << bits) - 1;
        const unsigned value = (source[size_t(x) * bits / 8] >> (8 - bits - (unsigned(x) * bits % 8))) & mask;
        if (line->iPixelType == 3) {
          if (!line->pPalette) { context->failed = true; return 0; }
          r = line->pPalette[value * 3]; g = line->pPalette[value * 3 + 1]; b = line->pPalette[value * 3 + 2];
          if (line->iHasAlpha) a = line->pPalette[768 + value];
        } else {
          r = g = b = uint8_t(value * 255 / mask);
          if (context->grayKey && value == context->transparentGray) a = 0;
        }
      } else if (line->iPixelType == 2 || line->iPixelType == 6) {
        source += size_t(x) * channels;
        r = source[0]; g = source[1]; b = source[2];
        if (line->iPixelType == 6) a = source[3];
        else if (context->rgbKey && r == context->transparentRgb[0] &&
                 g == context->transparentRgb[1] && b == context->transparentRgb[2]) a = 0;
      } else if (line->iPixelType == 4) {
        source += size_t(x) * 2; r = g = b = source[0]; a = source[1];
      } else { context->failed = true; return 0; }
      if (alpha) alpha[x] = a;
      else if (a != 255) {
        const uint16_t bg = context->background;
        const uint8_t br = uint8_t(((bg >> 11) & 31) * 255 / 31);
        const uint8_t bgreen = uint8_t(((bg >> 5) & 63) * 255 / 63);
        const uint8_t bb = uint8_t((bg & 31) * 255 / 31);
        r = uint8_t((unsigned(r) * a + unsigned(br) * (255 - a) + 127) / 255);
        g = uint8_t((unsigned(g) * a + unsigned(bgreen) * (255 - a) + 127) / 255);
        b = uint8_t((unsigned(b) * a + unsigned(bb) * (255 - a) + 127) / 255);
      }
      pixels[x] = uint16_t((uint16_t(r >> 3) << 11) | (uint16_t(g >> 2) << 5) | (b >> 3));
    }
    ++context->rows;
    return 1;
  }
  std::unique_ptr<PNG> png_;
  Allocator allocator_;
  uint16_t background_;
};
} // namespace nice_gfx_map
