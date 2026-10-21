#include "Map.h"
#include "NativeCopy565.h"
#include "TileGrid.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <utility>

// The pixel allocations are explicitly fallible. When exceptions are enabled,
// metadata allocations are also reported as NoMemory without changing state.
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#define NICE_MAP_TRY try
#define NICE_MAP_CATCH_RETURN(value) catch (const std::bad_alloc &) { return value; }
#else
#define NICE_MAP_TRY if (true)
#define NICE_MAP_CATCH_RETURN(value)
#endif

namespace nice_gfx_map {
namespace {
constexpr uint8_t kMaximumZoom = 22;
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthCircumferenceMeters = 40075016.68557849;
double wrap(double value, double period) {
  double result = std::fmod(value, period);
  return result < 0 ? result + period : result;
}
double nearestDelta(double value, double world) {
  return wrap(value + world / 2, world) - world / 2;
}
GeoPoint normalized(GeoPoint point) {
  point.latitude = std::max(-MercatorLatitudeLimit, std::min(MercatorLatitudeLimit, point.latitude));
  point.longitude = normalizeLongitude(point.longitude);
  return point;
}
GeoPoint normalized(GeoPoint point, Projection projection) {
  if (projection == Projection::WebMercator) return normalized(point);
  point.longitude = normalizeLongitude(point.longitude); return point;
}
bool sameGrid(const TileGrid &a, const TileGrid &b) {
  if (a.projection != b.projection || a.identifier != b.identifier || a.supportedCrs != b.supportedCrs ||
      a.matrices.size() != b.matrices.size()) return false;
  for (size_t i = 0; i < a.matrices.size(); ++i) {
    const auto &x = a.matrices[i], &y = b.matrices[i];
    if (x.identifier != y.identifier || x.zoom != y.zoom || x.width != y.width || x.height != y.height ||
        x.tileWidth != y.tileWidth || x.tileHeight != y.tileHeight || x.limited != y.limited ||
        (x.limited && (x.minCol != y.minCol || x.maxCol != y.maxCol || x.minRow != y.minRow || x.maxRow != y.maxRow))) return false;
  }
  return true;
}
PixelPoint invalidPixel() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return {nan, nan};
}
GeoPoint invalidGeo() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return {nan, nan};
}
uint16_t blend565(uint16_t foreground, uint16_t background, uint8_t alpha) {
  if (!alpha) return background;
  if (alpha == 255) return foreground;
  const unsigned inverse = 255 - alpha;
  const unsigned r = (((foreground >> 11) & 31) * alpha + ((background >> 11) & 31) * inverse + 127) / 255;
  const unsigned g = (((foreground >> 5) & 63) * alpha + ((background >> 5) & 63) * inverse + 127) / 255;
  const unsigned b = ((foreground & 31) * alpha + (background & 31) * inverse + 127) / 255;
  return uint16_t((r << 11) | (g << 5) | b);
}
bool pinHit(double dx, double dy, double radius, double tolerance) {
  // Convex hull of the circular head and its tip; the center hole stays easy
  // to select. A circular head plus its two tangent edges gives exact distance
  // to this silhouette for the optional pixel tolerance.
  constexpr double rootThree = 1.7320508075688772935;
  if (std::hypot(dx, dy + 2 * radius) <= radius + tolerance ||
      (dy >= -1.5 * radius && dy <= 0 && std::abs(dx) <= -dy / rootThree)) return true;
  const auto edgeDistance = [&](double x) {
    const double y = -1.5 * radius;
    const double ratio = std::max(0.0, std::min(1.0, (dx * x + dy * y) / (3 * radius * radius)));
    return std::hypot(dx - ratio * x, dy - ratio * y);
  };
  return edgeDistance(rootThree * radius / 2) <= tolerance || edgeDistance(-rootThree * radius / 2) <= tolerance;
}
bool validConfig(const MapConfig &c) {
  return c.width && c.height && c.width <= 4096 && c.height <= 4096 &&
         c.maxCachedTiles && c.maxCachedTiles <= 4096 &&
         c.maxInFlight && c.maxInFlight <= 64 && c.maxMarkers <= 4096 &&
         c.maxRoutePoints <= 65536 && c.maxTrackPoints <= 65536 &&
         c.maxCircles <= 4096 && c.maxPolygons <= 4096 && c.maxPolygonPoints <= 65536 &&
         c.retryDelayMs <= uint32_t(std::numeric_limits<int32_t>::max());
}

// Drawing never writes outside this region, including stride padding.
template<bool Wire>
struct Raster {
  int x, y, width, height;
  uint16_t *pixels;
  size_t stride;
  static uint16_t encoded(uint16_t color) {
    if constexpr (!Wire) return color;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return uint16_t((color << 8) | (color >> 8));
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return color;
#else
    const uint8_t bytes[2]={uint8_t(color >> 8),uint8_t(color)};
    uint16_t stored;std::memcpy(&stored,bytes,2);return stored;
#endif
  }
  static uint16_t blended(uint16_t color,uint16_t stored,uint8_t opacity) {
    return encoded(blend565(color,encoded(stored),opacity));
  }
  static void copyOpaque(uint16_t *output,const uint16_t *input,size_t count) {
    if constexpr (!Wire) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
      detail::nativeCopy565(output,input,count);
#else
      std::memcpy(output,input,count*2);
#endif
      return;
    }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    std::memcpy(output,input,count*2);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // The target is at least halfword aligned. Peel a halfword prefix, then
    // stream pairs with aligned stores even when a tile starts at an odd x.
    if(count&&(reinterpret_cast<uintptr_t>(output)&3)){*output++=encoded(*input++);--count;}
    auto *destination=reinterpret_cast<unsigned char *>(output);
    auto *source=reinterpret_cast<const unsigned char *>(input);
#if defined(__GNUC__) || defined(__clang__)
    destination=static_cast<unsigned char *>(__builtin_assume_aligned(destination,4));
    source=static_cast<const unsigned char *>(__builtin_assume_aligned(source,2));
#endif
    for(;count>=2;count-=2,source+=4,destination+=4){
      uint32_t pair;
#if defined(__GNUC__) || defined(__clang__)
      __builtin_memcpy(&pair,source,4);
      pair=__builtin_bswap32((pair<<16)|(pair>>16));
      __builtin_memcpy(destination,&pair,4);
#else
      std::memcpy(&pair,source,4);
      pair=((pair&0x00ff00ffu)<<8)|((pair>>8)&0x00ff00ffu);
      std::memcpy(destination,&pair,4);
#endif
    }
    if(count){uint16_t last;std::memcpy(&last,source,2);last=encoded(last);std::memcpy(destination,&last,2);}
#else
    for(size_t i=0;i<count;++i)output[i]=encoded(input[i]);
#endif
  }
  static void sampleOpaque32(uint16_t *output,const uint16_t *source,size_t count,uint32_t coordinate,uint32_t step) {
#if defined(CONFIG_IDF_TARGET_ESP32S31) && defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // The caller has proved every Q24 sample lies inside this source row.
    // Paired stores avoid S31 PSRAM's costly halfword write stream without
    // changing nearest-neighbor coordinates, pixel order or clipping.
    typedef uint32_t SampleWord __attribute__((__may_alias__));
    if(count&&(reinterpret_cast<uintptr_t>(output)&2)){*output++=encoded(source[coordinate>>24]);coordinate+=step;--count;}
    auto *words=reinterpret_cast<SampleWord *>(output);
    while(count>=2){
      const uint16_t first=encoded(source[coordinate>>24]);coordinate+=step;
      const uint16_t second=encoded(source[coordinate>>24]);coordinate+=step;
      *words++=uint32_t(first)|(uint32_t(second)<<16);count-=2;
    }
    if(count)*reinterpret_cast<uint16_t *>(words)=encoded(source[coordinate>>24]);
#else
    while(count--){*output++=encoded(source[coordinate>>24]);coordinate+=step;}
#endif
  }
  void span(int py, int first, int end, uint16_t color) const {
    if (py < y || py >= y + height) return;
    first = std::max(first, x); end = std::min(end, x + width);
    if (first >= end) return;
    uint16_t *output = pixels + size_t(py - y) * stride + size_t(first - x);
    const uint16_t stored = encoded(color);
#if defined(CONFIG_IDF_TARGET_ESP32S31) && defined(__GNUC__)
    // PSRAM halfword stores are costly on S31. The clipped span is nonempty;
    // peel its odd pixel, then write two identical stored colors per word.
    typedef uint32_t FillWord __attribute__((__may_alias__));
    size_t count = size_t(end - first);
    if (reinterpret_cast<uintptr_t>(output) & 3) { *output++ = stored; --count; }
    auto *words = reinterpret_cast<FillWord *>(output);
    const uint32_t pair = uint32_t(stored) | (uint32_t(stored) << 16);
    while (count >= 8) { words[0] = pair; words[1] = pair; words[2] = pair; words[3] = pair; words += 4; count -= 8; }
    while (count >= 2) { *words++ = pair; count -= 2; }
    if (count) *reinterpret_cast<uint16_t *>(words) = stored;
#else
    std::fill(output, output + size_t(end - first), stored);
#endif
  }
  void blendSpan(int py, int first, int end, uint16_t color, uint8_t opacity) const {
    if (!opacity || py < y || py >= y + height) return;
    if (opacity == 255) { span(py, first, end, color); return; }
    first = std::max(first, x); end = std::min(end, x + width);
    if (first >= end) return;
    uint16_t *output = pixels + size_t(py - y) * stride + size_t(first - x);
    for (int column = first; column < end; ++column, ++output) *output = blended(color, *output, opacity);
  }
  void disk(double cx, double cy, double radius, uint16_t color) const {
    if (!std::isfinite(cx) || !std::isfinite(cy) || radius < 0 ||
        cx + radius < x || cx - radius >= x + width ||
        cy + radius < y || cy - radius >= y + height) return;
    const int first = std::max(y, int(std::ceil(cy - radius)));
    const int end = std::min(y + height - 1, int(std::floor(cy + radius)));
    for (int py = first; py <= end; ++py) {
      const double dy = py - cy;
      const double extent = std::sqrt(std::max(0.0, radius * radius - dy * dy));
      span(py, int(std::ceil(cx - extent)), int(std::floor(cx + extent)) + 1, color);
    }
  }
  void pin(double cx, double tipY, double radius, uint16_t color) const {
    if (cx + radius < x || cx - radius >= x + width || tipY < y || tipY - 3 * radius >= y + height) return;
    constexpr double rootThree = 1.7320508075688772935;
    const double headY = tipY - 2 * radius, hole = radius * 0.35;
    const int first = int(std::max(double(y), std::ceil(tipY - 3 * radius)));
    const int last = int(std::min(double(y + height - 1), std::floor(tipY)));
    for (int row = first; row <= last; ++row) {
      const double dy = row - headY;
      const double extent = row <= tipY - 1.5 * radius ?
        std::sqrt(std::max(0.0, radius * radius - dy * dy)) : (tipY - row) / rootThree;
      const int left = int(std::ceil(cx - extent)), right = int(std::floor(cx + extent)) + 1;
      if (std::abs(dy) <= hole) {
        const double gap = std::sqrt(std::max(0.0, hole * hole - dy * dy));
        span(row, left, std::min(right, int(std::ceil(cx - gap))), color);
        span(row, std::max(left, int(std::floor(cx + gap)) + 1), right, color);
      } else span(row, left, right, color);
    }
  }
  void line(double ax, double ay, double bx, double by, uint16_t color, uint8_t stroke) const {
    const double radius = stroke <= 1 ? 0.0 : double(stroke) / 2;
    double low = 0, high = 1;
    const double dx = bx - ax, dy = by - ay;
    const auto clip = [&](double p, double q) {
      if (p == 0) return q >= 0;
      const double t = q / p;
      if (p < 0) { if (t > high) return false; low = std::max(low, t); }
      else { if (t < low) return false; high = std::min(high, t); }
      return true;
    };
    if (!clip(-dx, ax - (x - radius - 1)) ||
        !clip(dx, x + width + radius - ax) ||
        !clip(-dy, ay - (y - radius - 1)) ||
        !clip(dy, y + height + radius - ay)) return;
    // Keep the original line's sampling phase. Re-starting a DDA at each strip
    // boundary would make striped rendering disagree with a full-frame render.
    const uint64_t steps = uint64_t(std::ceil(std::max(std::abs(dx), std::abs(dy))));
    if (!steps) { disk(std::round(ax), std::round(ay), radius, color); return; }
    const uint64_t first = low <= 0 ? 0 : uint64_t(std::max(0.0, std::floor(low * steps) - 1));
    const uint64_t last = high >= 1 ? steps : std::min(steps, uint64_t(std::ceil(high * steps) + 1));
    // DDA stamps have integer centers and the same half-integer radius. Build
    // its disk spans once, rather than taking a square root for every row of
    // every sample. This preserves the original strip-independent coverage.
    const int verticalRadius = stroke <= 1 ? 0 : stroke / 2;
    uint8_t extents[255]; // stroke <= 255: at most 255 rows, no render allocation.
    for (int dy = -verticalRadius; dy <= verticalRadius; ++dy)
      extents[dy + verticalRadius] = uint8_t(std::floor(std::sqrt(std::max(0.0, radius * radius - dy * dy))));
    // Axis-aligned DDA centers visit every integer between the endpoint
    // samples: abs(delta)/ceil(abs(delta)) <= 1. Their discrete disk union
    // is therefore one span per row, using the same integer disk extents.
    // Preserve the clipped first/last sample arithmetic and endpoint rounding.
    if (dx == 0 || dy == 0) {
    // Near half-integer ties, floating DDA arithmetic (and round away from
    // zero) can skip a center even for an exact unit step. Keep that path
    // scalar unless a conservative rounding-error margin proves dense centers.
    const double roundingMargin = 64 * std::numeric_limits<double>::epsilon() *
      (std::abs(ax) + std::abs(ay) + std::abs(dx) + std::abs(dy) + 1);
    const double axisDelta = dy == 0 ? dx : dy;
    const double axisStep = std::abs(axisDelta) / double(steps);
    const double axisStart = dy == 0 ? ax : ay;
    const bool denseCenters = axisStep < 1 - roundingMargin ||
      (axisStep == 1 && std::abs(axisStart - std::floor(axisStart) - 0.5) > roundingMargin);
    if (roundingMargin < 0.125 && denseCenters) {
      const double firstRatio = double(first) / double(steps), lastRatio = double(last) / double(steps);
      const int firstCx = int(std::round(ax + dx * firstRatio)), firstCy = int(std::round(ay + dy * firstRatio));
      const int lastCx = int(std::round(ax + dx * lastRatio)), lastCy = int(std::round(ay + dy * lastRatio));
      if (dy == 0) {
        const int left = std::min(firstCx, lastCx), right = std::max(firstCx, lastCx);
        const int beginRow = std::max(y, firstCy - verticalRadius), endRow = std::min(y + height, firstCy + verticalRadius + 1);
        for (int py = beginRow; py < endRow; ++py) {
          const int extent = extents[py - firstCy + verticalRadius];
          span(py, left - extent, right + extent + 1, color);
        }
      } else {
        const int top = std::min(firstCy, lastCy), bottom = std::max(firstCy, lastCy);
        const int beginRow = std::max(y, top - verticalRadius), endRow = std::min(y + height, bottom + verticalRadius + 1);
        for (int py = beginRow; py < endRow; ++py) {
          const int distance = py < top ? top - py : py > bottom ? py - bottom : 0;
          const int extent = extents[distance + verticalRadius];
          span(py, firstCx - extent, firstCx + extent + 1, color);
        }
      }
      return;
    }
    }
    for (uint64_t step = first; step <= last; ++step) {
      const double ratio = double(step) / double(steps);
      const int cx = int(std::round(ax + dx * ratio)), cy = int(std::round(ay + dy * ratio));
      const int firstRow = std::max(y, cy - verticalRadius), endRow = std::min(y + height, cy + verticalRadius + 1);
      for (int py = firstRow; py < endRow; ++py) {
        const int extent = extents[py - cy + verticalRadius];
        span(py, cx - extent, cx + extent + 1, color);
      }
    }
  }
};
} // namespace

struct Map::Impl {
  enum class State : uint8_t { Pending, Ready, Failed };
  struct Entry {
    TileKey key;
    State state = State::Pending;
    TileImage image;
    uint64_t requestId = 0, used = 0;
    uint32_t failedAt = 0;
    std::string requestUrl;
  };
  struct Composite {
    TileKey key; // layer zero identifies a complete XYZ layer stack.
    TileImage image;
    std::vector<const uint16_t *> inputs; // immutable owner identities, not reads.
    uint64_t used = 0;
  };
  struct Viewport {
    double world = 256, worldHeight = 256, scale = 1, displayTileSize = 256;
    PixelPoint center;
    int64_t left = 0, top = 0, firstX = 0, lastX = 0, firstY = 0, lastY = -1;
    uint32_t columns = 1, rows = 1;
    uint16_t tileSize = 256;
  };
  struct RenderTransform {
    PixelPoint center;
    double world, scale, halfWidth, halfHeight;
    PixelPoint screen(PixelPoint projected) const {
      return {halfWidth + nearestDelta(projected.x * scale - center.x, world),
              halfHeight + projected.y * scale - center.y};
    }
  };
  struct FallbackPiece {
    const TileImage *image;
    double left, top, scale;
    int x, y, right, bottom;
  };
  struct Polygon {
    std::vector<PixelPoint> points; // Unit-world coordinates, unwrapped once.
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    uint16_t fillColor = 0x07e0, strokeColor = 0x03e0;
    uint8_t fillOpacity = 48, strokeWidth = 2;
  };
  MapConfig config;
  bool configured = true, hasSource = false, follow = false, hasLocation = false;
  TileSource source;
  TileGrid grid;
  GeoPoint center{}, location{};
  uint8_t zoom = 0, routeWidth = 3;
  uint16_t routeColor = 0x001f;
  double heading = 0, viewScale = 1;
  uint32_t generation = 1;
  uint64_t sequence = 0, clock = 0, revision = 1;
  uint64_t hits = 0, misses = 0, evictions = 0, failures = 0;
  uint64_t compositeBuilds = 0;
  std::vector<Entry> entries;
  std::vector<Composite> composites;
  std::vector<Marker> markers;
  std::vector<CircleOverlay> circles;
  std::vector<Polygon> polygons;
  // Project immutable geometry on input, not for each segment of every strip.
  // A 256-pixel zoom-zero world scales by exact powers of two for all sources.
  std::vector<PixelPoint> route, track;
  size_t trackHead = 0;

  // Exact input/result memo only: retain the original projection arithmetic.
  // A fixed metadata budget covers the navigator's route-prefix trail, fixed
  // markers, and both center projection levels without allocating in render.
  struct ProjectionValue {
    GeoPoint input;
    PixelPoint output;
    uint64_t used = 0;
    uint16_t tileSize = 0;
    uint8_t level = 0;
    Projection projection = Projection::WebMercator;
  };
  mutable std::array<ProjectionValue, 16> projectionMemo{};
  mutable uint64_t projectionMemoClock = 0;
  static_assert(sizeof(projectionMemo) + sizeof(projectionMemoClock) <= 800,
                "Projection memo must remain bounded metadata, never pixels");

  explicit Impl(const MapConfig &value) : config(value), configured(validConfig(value)) {}
  Status status() const { return configured ? Status::Ok : Status::InvalidArgument; }
  uint16_t tileSize() const { return hasSource ? source.tileSize : 256; }
  double worldSize() const { return std::ldexp(double(tileSize()), zoom) * viewScale; }
  double worldHeight(double world) const { return grid.projection == Projection::Geographic ? world / 2 : world; }
  // NICE_MAP_PROJECTION_MEMO_BEGIN
  PixelPoint projected(GeoPoint point, uint8_t level, uint16_t size) const {
    if (++projectionMemoClock == 0) {
      for (auto &value : projectionMemo) value.used = 0;
      projectionMemoClock = 1;
    }
    ProjectionValue *oldest = &projectionMemo[0];
    for (auto &value : projectionMemo) {
      if (value.used && value.level == level && value.tileSize == size && value.projection == grid.projection &&
          !std::memcmp(&value.input.latitude, &point.latitude, sizeof(double)) &&
          !std::memcmp(&value.input.longitude, &point.longitude, sizeof(double))) {
        value.used = projectionMemoClock;
        return value.output;
      }
      if (value.used < oldest->used) oldest = &value;
    }
    const auto output = project(point, level, size, grid.projection);
    *oldest = {point, output, projectionMemoClock, size, level, grid.projection};
    return output;
  }
  // NICE_MAP_PROJECTION_MEMO_END
  GeoPoint geographic(PixelPoint point, uint8_t level, uint16_t size) const { return unproject(point, level, size, grid.projection); }
  GeoPoint normalize(GeoPoint point) const { return normalized(point, grid.projection); }
  Viewport viewport() const {
    Viewport v;
    v.tileSize = tileSize(); v.columns = grid.columns(zoom); v.rows = grid.rows(zoom); v.world = worldSize();
    v.worldHeight = worldHeight(v.world);
    v.scale = viewScale; v.displayTileSize = v.tileSize * v.scale;
    PixelPoint c = projected(center, zoom, v.tileSize); c.x *= v.scale; c.y *= v.scale;
    v.center = c;
    // Raster translation is snapped to whole pixels, keeping placements and
    // striped rendering identical even at fractional geographic centers.
    v.left = int64_t(std::floor(c.x - double(config.width) / 2));
    v.top = int64_t(std::floor(c.y - double(config.height) / 2));
    v.firstX = int64_t(std::floor((double(v.left) + 0.5) / v.displayTileSize));
    v.lastX = int64_t(std::floor((double(v.left + config.width) - 0.5) / v.displayTileSize));
    v.firstY = std::max<int64_t>(0, int64_t(std::floor((double(v.top) + 0.5) / v.displayTileSize)));
    v.lastY = std::min<int64_t>(int64_t(v.rows) - 1, int64_t(std::floor((double(v.top + config.height) - 0.5) / v.displayTileSize)));
    return v;
  }
  uint32_t wrappedX(int64_t column, uint32_t count) const {
    const int64_t remainder = column % int64_t(count);
    return uint32_t(remainder < 0 ? remainder + count : remainder);
  }
  template<class Callback> void placements(const Viewport &v, Callback callback) const {
    if (!hasSource) return;
    for (size_t layer = 0; layer < source.layers.size(); ++layer)
      for (int64_t ty = v.firstY; ty <= v.lastY; ++ty)
        for (int64_t tx = v.firstX; tx <= v.lastX; ++tx)
        {
          const TileKey key{zoom, wrappedX(tx, v.columns), uint32_t(ty), uint8_t(layer)};
          if (!grid.matrices.empty() && !grid.contains(key)) continue;
          const int32_t x = int32_t(std::ceil(tx * v.displayTileSize - double(v.left) - 0.5));
          const int32_t y = int32_t(std::ceil(ty * v.displayTileSize - double(v.top) - 0.5));
          const int32_t right = int32_t(std::ceil((tx + 1) * v.displayTileSize - double(v.left) - 0.5));
          const int32_t bottom = int32_t(std::ceil((ty + 1) * v.displayTileSize - double(v.top) - 0.5));
          callback(TilePlacement{key,
                                x, y, uint16_t(right - x), uint16_t(bottom - y)});
        }
  }
  bool visible(const TileKey &key, const Viewport &v) const {
    if (key.zoom != zoom || key.layer >= source.layers.size() ||
        int64_t(key.y) < v.firstY || int64_t(key.y) > v.lastY ||
        (!grid.matrices.empty() && !grid.contains(key))) return false;
    if (uint64_t(v.lastX - v.firstX + 1) >= v.columns) return key.x < v.columns;
    const uint32_t first = wrappedX(v.firstX, v.columns);
    return key.x < v.columns && ((uint64_t(key.x) + v.columns - first) % v.columns) <= uint64_t(v.lastX - v.firstX);
  }
  bool fallbackNeeded(const Entry &entry, const Viewport &v) const {
    if (entry.state != State::Ready || entry.key.zoom == zoom) return false;
    const double size = tileSize() * std::ldexp(viewScale, int(zoom) - int(entry.key.zoom));
    const double left = double(config.width) / 2 + nearestDelta((entry.key.x + 0.5) * size - v.center.x, v.world) - size / 2;
    const double top = entry.key.y * size - double(v.top);
    if (left >= config.width || left + size <= 0 || top >= config.height || top + size <= 0) return false;
    const bool wholeStack = findComposite(entry.key) != nullptr;
    const auto replacementReady = [&](TileKey key) {
      if (!wholeStack) {
        const auto *ready = find(key); return ready && ready->state == State::Ready;
      }
      for (size_t layer = 0; layer < source.layers.size(); ++layer) {
        if (!source.layers[layer].opacity) continue;
        key.layer = uint8_t(layer); const auto *ready = find(key);
        if (!ready || ready->state != State::Ready) return false;
      }
      return true;
    };
    if (entry.key.zoom > zoom) {
      const unsigned shift = entry.key.zoom - zoom;
      const TileKey parent{zoom, entry.key.x >> shift, entry.key.y >> shift, entry.key.layer};
      return !replacementReady(parent);
    }
    bool needed = false;
    const unsigned shift = zoom - entry.key.zoom;
    placements(v, [&](const TilePlacement &placement) {
      if (needed || placement.key.layer != entry.key.layer ||
          (placement.key.x >> shift) != entry.key.x || (placement.key.y >> shift) != entry.key.y) return;
      if (!replacementReady(placement.key)) needed = true;
    });
    return needed;
  }
  size_t expectedBytes(const TileKey &key) const {
    return size_t(source.tileSize) * source.tileSize * (source.layers[key.layer].overlay ? 3 : 2);
  }
  Entry *find(const TileKey &key) {
    for (auto &entry : entries) if (entry.key == key) return &entry;
    return nullptr;
  }
  const Entry *find(const TileKey &key) const {
    for (const auto &entry : entries) if (entry.key == key) return &entry;
    return nullptr;
  }
  static bool sameXyz(const TileKey &a, const TileKey &b) {
    return a.zoom == b.zoom && a.x == b.x && a.y == b.y;
  }
  const Composite *findComposite(const TileKey &key) const {
    for (const auto &item : composites) if (sameXyz(item.key, key)) return &item;
    return nullptr;
  }
  size_t compositeBytes() const {
    size_t bytes = 0;
    for (const auto &item : composites) bytes += item.image.bytes();
    return bytes;
  }
  void discardComposite(const TileKey &key) {
    composites.erase(std::remove_if(composites.begin(), composites.end(),
      [&](const Composite &item) { return sameXyz(item.key, key); }), composites.end());
  }
  void tryCompose(const TileKey &key, const Viewport &v) {
    const size_t needed = size_t(tileSize()) * tileSize() * sizeof(uint16_t);
    if (!hasSource || source.layers.size() < 2 || source.layers[0].overlay || source.layers[0].opacity != 255 ||
        config.compositeCacheBytes < needed || findComposite(key)) return;
    NICE_MAP_TRY {
      Composite fresh;
      fresh.key = key; fresh.key.layer = 0;
      fresh.inputs.resize(source.layers.size(), nullptr);
      const Entry *base = nullptr;
      for (size_t layer = 0; layer < source.layers.size(); ++layer) {
        if (!source.layers[layer].opacity) continue;
        TileKey inputKey = fresh.key; inputKey.layer = uint8_t(layer);
        const auto *entry = find(inputKey);
        if (!entry || entry->state != State::Ready) return;
        fresh.inputs[layer] = entry->image.pixels();
        if (!layer) base = entry;
      }
      if (!base) return;
      composites.reserve(std::min(config.maxCachedTiles, composites.size() + 1));
      // Derived pixels use the same memory class and allocator lifetime as the
      // bottom tile (PSRAM on ESP32), never an accidental large internal heap.
      size_t bytes = compositeBytes();
      while (!composites.empty() && (composites.size() >= config.maxCachedTiles ||
             bytes > config.compositeCacheBytes || needed > config.compositeCacheBytes - bytes)) {
        size_t victim = 0;
        for (size_t i = 1; i < composites.size(); ++i) {
          const auto rank = [&](const Composite &item) {
            if (visible(item.key, v)) return 2;
            const auto *raw = find(item.key);
            return raw && fallbackNeeded(*raw, v) ? 1 : 0;
          };
          const int a = rank(composites[i]), b = rank(composites[victim]);
          if (a < b || (a == b && composites[i].used < composites[victim].used)) victim = i;
        }
        if (!visible(fresh.key, v) && visible(composites[victim].key, v)) return;
        bytes -= composites[victim].image.bytes();
        composites.erase(composites.begin() + victim);
      }
      if (fresh.image.allocate(tileSize(), tileSize(), false, base->image.allocator()) != Status::Ok) return;
      const size_t count = size_t(tileSize()) * tileSize();
      std::memcpy(fresh.image.pixels(), base->image.pixels(), needed);
      for (size_t layer = 1; layer < source.layers.size(); ++layer) {
        const uint8_t opacity = source.layers[layer].opacity;
        if (!opacity) continue;
        TileKey inputKey = fresh.key; inputKey.layer = uint8_t(layer);
        const TileImage &image = find(inputKey)->image;
        const uint16_t *input = image.pixels(); const uint8_t *alpha = image.alpha();
        uint16_t *output = fresh.image.pixels();
        if (!alpha && opacity == 255) { std::memcpy(output, input, needed); continue; }
        for (size_t pixel = 0; pixel < count; ++pixel) {
          const uint8_t effective = alpha ? uint8_t((unsigned(alpha[pixel]) * opacity + 127) / 255) : opacity;
          if (effective == 255) output[pixel] = input[pixel];
          else if (effective) output[pixel] = blend565(input[pixel], output[pixel], effective);
        }
      }
      fresh.used = touch(); composites.push_back(std::move(fresh)); ++compositeBuilds;
    } NICE_MAP_CATCH_RETURN()
  }
  void rebuildComposites() {
    if (!config.compositeCacheBytes || !hasSource) return;
    const auto v = viewport();
    for (unsigned pass = 0; pass < 2; ++pass) for (const auto &entry : entries)
      if (!entry.key.layer && entry.state == State::Ready &&
          (visible(entry.key, v) ? 0u : 1u) == pass) tryCompose(entry.key, v);
  }
  void takeCompositesFrom(Impl &from, const Viewport &v) {
    if (!config.compositeCacheBytes) return;
    for (size_t layer = 0; layer < source.layers.size(); ++layer)
      if (source.layers[layer].opacity != from.source.layers[layer].opacity) return;
    NICE_MAP_TRY {
      composites.reserve(std::min(config.maxCachedTiles, composites.size() + from.composites.size()));
      size_t bytes = compositeBytes();
      for (unsigned pass = 0; pass < 2; ++pass) for (auto &item : from.composites) {
        if (!item.image.valid() || item.inputs.size() != source.layers.size() || findComposite(item.key) ||
            (visible(item.key, v) ? 0u : 1u) != pass || composites.size() >= config.maxCachedTiles ||
            bytes > config.compositeCacheBytes || item.image.bytes() > config.compositeCacheBytes - bytes) continue;
        bool matching = true;
        for (size_t layer = 0; layer < item.inputs.size(); ++layer) {
          if (!item.inputs[layer]) continue;
          TileKey inputKey = item.key; inputKey.layer = uint8_t(layer);
          const auto *entry = find(inputKey);
          if (!entry || entry->state != State::Ready || entry->image.pixels() != item.inputs[layer]) { matching = false; break; }
        }
        if (!matching) continue;
        bytes += item.image.bytes(); composites.push_back(std::move(item)); composites.back().used = touch();
      }
    } NICE_MAP_CATCH_RETURN()
  }
  uint64_t touch() {
    if (++clock == 0) { for (auto &entry : entries) entry.used = 0; clock = 1; }
    return clock;
  }
  size_t pendingCount() const {
    size_t count = 0;
    for (const auto &entry : entries) if (entry.state == State::Pending) ++count;
    return count;
  }
  size_t committedBytes() const {
    size_t bytes = 0;
    for (const auto &entry : entries) {
      if (entry.state == State::Ready) bytes += entry.image.bytes();
      else if (entry.state == State::Pending) bytes += expectedBytes(entry.key);
    }
    return bytes;
  }
  bool visibleFits(const Viewport &v) const {
    if (v.lastY < v.firstY) return true;
    const uint64_t columns = std::min<uint64_t>(uint64_t(v.lastX - v.firstX + 1), v.columns);
    const uint64_t tiles = columns * uint64_t(v.lastY - v.firstY + 1);
    const uint64_t count = tiles * source.layers.size();
    if (count > config.maxCachedTiles) return false;
    uint64_t bytesPerPosition = 0;
    for (const auto &layer : source.layers)
      bytesPerPosition += uint64_t(source.tileSize) * source.tileSize * (layer.overlay ? 3 : 2);
    return !tiles || bytesPerPosition <= config.cacheBytes / tiles;
  }
  bool retryReady(const Entry &entry, uint32_t now) const {
    return !config.retryDelayMs || int32_t(now - (entry.failedAt + config.retryDelayMs)) >= 0;
  }
  bool matching(const TileRequest &request, const Entry &entry) const {
    return entry.state == State::Pending && request.id && request.id == entry.requestId &&
           request.generation == generation && request.key == entry.key &&
           request.tileSize == source.tileSize && request.overlay == source.layers[entry.key.layer].overlay &&
           request.url == entry.requestUrl;
  }
  void clear() {
    entries.clear();
    composites.clear();
    if (++generation == 0) ++generation;
    ++revision;
  }
  PixelPoint screen(GeoPoint point) const {
    const auto p = projected(point, zoom, tileSize()), c = projected(center, zoom, tileSize());
    return {double(config.width) / 2 + nearestDelta((p.x - c.x) * viewScale, worldSize()),
            double(config.height) / 2 + (p.y - c.y) * viewScale};
  }
  RenderTransform renderTransform(const Viewport &v) const {
    return {v.center, v.world, v.world / 256, double(config.width) / 2, double(config.height) / 2};
  }
  template<bool Wire>
  static void sampledTile(const Raster<Wire> &raster, const TileImage &image, uint8_t opacity,
                          double left, double top, double scale,
                          int firstX, int firstY, int endX, int endY) {
    // Global Q24 sample coordinates make independently rendered strips choose
    // identical texels. The bounded zoom/viewport range fits signed int64.
    constexpr int64_t unit = int64_t(1) << 24;
    const double inverse = 1 / scale;
    const int64_t step = int64_t(std::llround(inverse * unit));
    const int64_t originX = int64_t(std::llround((0.5 - left) * inverse * unit));
    const int64_t originY = int64_t(std::llround((0.5 - top) * inverse * unit));
    const auto sample = [](int64_t value, uint16_t extent) {
      return value <= 0 ? size_t(0) : std::min(size_t(uint64_t(value) >> 24), size_t(extent - 1));
    };
    const uint16_t *input = image.pixels(); const uint8_t *alpha = image.alpha();
    if (!alpha && opacity == 255) {
      const int64_t begin = originX + int64_t(firstX) * step;
      const int64_t last = originX + int64_t(endX - 1) * step;
      // The common 256px opaque/composite tile fits unsigned Q24. Keep its
      // inner loop native-width on 32-bit MCUs; larger/edge cases stay exact.
      if (begin >= 0 && last >= begin && uint64_t(last) <= UINT32_MAX &&
          uint64_t(last) < uint64_t(image.width()) * unit && uint64_t(step) <= UINT32_MAX) {
        size_t previousRow = size_t(-1);
        uint16_t *previousOutput = nullptr;
        for (int row = firstY; row < endY; ++row) {
          const size_t sourceRow = sample(originY + int64_t(row) * step, image.height());
          const auto *source = input + sourceRow * image.width();
          uint16_t *output = raster.pixels + size_t(row - raster.y) * raster.stride + size_t(firstX - raster.x);
          // Nearest-neighbor enlargement repeats source rows. Opaque output
          // is independent of the destination, so reuse its encoded pixels.
          if (previousOutput && previousRow == sourceRow) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
            detail::nativeCopy565(output,previousOutput,size_t(endX-firstX));
#else
            std::memcpy(output, previousOutput, size_t(endX - firstX) * sizeof(uint16_t));
#endif
            continue;
          }
          previousRow = sourceRow; previousOutput = output;
          Raster<Wire>::sampleOpaque32(output,source,size_t(endX-firstX),uint32_t(begin),uint32_t(step));
        }
        return;
      }
      size_t previousRow = size_t(-1);
      uint16_t *previousOutput = nullptr;
      for (int row = firstY; row < endY; ++row) {
        const size_t sourceRow = sample(originY + int64_t(row) * step, image.height());
        const auto *source = input + sourceRow * image.width();
        uint16_t *output = raster.pixels + size_t(row - raster.y) * raster.stride + size_t(firstX - raster.x);
        if (previousOutput && previousRow == sourceRow) {
#if defined(CONFIG_IDF_TARGET_ESP32S31)
          detail::nativeCopy565(output,previousOutput,size_t(endX-firstX));
#else
          std::memcpy(output, previousOutput, size_t(endX - firstX) * sizeof(uint16_t));
#endif
          continue;
        }
        previousRow = sourceRow; previousOutput = output;
        int64_t coordinate = begin;
        for (int column = firstX; column < endX; ++column, coordinate += step)
          *output++ = Raster<Wire>::encoded(source[sample(coordinate, image.width())]);
      }
      return;
    }
    for (int row = firstY; row < endY; ++row) {
      const size_t sourceRow = sample(originY + int64_t(row) * step, image.height()) * image.width();
      int64_t coordinate = originX + int64_t(firstX) * step;
      uint16_t *output = raster.pixels + size_t(row - raster.y) * raster.stride + size_t(firstX - raster.x);
      for (int column = firstX; column < endX; ++column, coordinate += step, ++output) {
        const size_t index = sourceRow + sample(coordinate, image.width());
        const uint8_t effective = alpha ? uint8_t((unsigned(alpha[index]) * opacity + 127) / 255) : opacity;
        if (effective == 255) *output = Raster<Wire>::encoded(input[index]);
        else if (effective) *output = Raster<Wire>::blended(input[index], *output, effective);
      }
    }
  }
  size_t compositeFallback(const Viewport &v, const TilePlacement &placement,
                           std::array<FallbackPiece, 32> &pieces) const {
    if (composites.empty()) return 0;
    bool incomplete = false;
    for (size_t layer = 0; layer < source.layers.size(); ++layer) {
      if (!source.layers[layer].opacity) continue;
      TileKey key = placement.key; key.layer = uint8_t(layer);
      const auto *entry = find(key);
      if (!entry || entry->state != State::Ready) { incomplete = true; break; }
    }
    if (!incomplete) return 0;
    const int firstX = std::max<int>(0, placement.x), firstY = std::max<int>(0, placement.y);
    const int endX = std::min<int>(config.width, placement.x + placement.width);
    const int endY = std::min<int>(config.height, placement.y + placement.height);
    uint32_t levels = 0;
    for (const auto &entry : composites) {
      if (entry.key.zoom == zoom) continue;
      const double size = tileSize() * std::ldexp(viewScale, int(zoom) - int(entry.key.zoom));
      const double top = entry.key.y * size - double(v.top);
      if (top > endY - 0.5 || top + size <= firstY + 0.5) continue;
      const double left = entry.key.x * size - double(v.left);
      if (std::ceil((firstX + 0.5 - left - size) / v.world) >
          std::floor((endX - 0.5 - left) / v.world)) continue;
      levels |= uint32_t(1) << entry.key.zoom;
    }
    const auto collect = [&](int selected) {
    size_t count = 0;
    for (const auto &entry : composites) {
      if (entry.key.zoom != selected) continue;
      const double scale = std::ldexp(viewScale, int(zoom) - selected), size = tileSize() * scale;
      const double top = entry.key.y * size - double(v.top);
      if (top > endY - 0.5 || top + size <= firstY + 0.5) continue;
      const int beginY = int(std::max(double(firstY), std::ceil(top - 0.5)));
      const int finishY = int(std::min(double(endY), std::ceil(top + size - 0.5)));
      if (beginY >= finishY) continue;
      const double left = entry.key.x * size - double(v.left);
      const int firstCopy = int(std::ceil((firstX + 0.5 - left - size) / v.world));
      const int lastCopy = int(std::floor((endX - 0.5 - left) / v.world));
      for (int copy = firstCopy; copy <= lastCopy; ++copy) {
        const double at = left + copy * v.world;
        const int beginX = int(std::max(double(firstX), std::ceil(at - 0.5)));
        const int finishX = int(std::min(double(endX), std::ceil(at + size - 0.5)));
        if (beginX >= finishX) continue;
        if (count == pieces.size()) return size_t(0); // bounded stack/work; try another complete level.
        pieces[count++] = {&entry.image, at, top, scale, beginX, beginY, finishX, finishY};
      }
    }
    if (!count) return size_t(0);
    std::sort(pieces.begin(), pieces.begin() + count, [](const FallbackPiece &a, const FallbackPiece &b) { return a.x < b.x; });
    // Complete coverage is required before suppressing the other source layers.
    // Test all rows against the full placement so clipping/strips agree exactly.
    for (int row = firstY; row < endY; ++row) {
      int cursor = firstX;
      for (size_t i = 0; i < count && cursor < endX; ++i) {
        const auto &piece = pieces[i];
        if (row < piece.y || row >= piece.bottom) continue;
        if (piece.x > cursor) return size_t(0);
        cursor = std::max(cursor, piece.right);
      }
      if (cursor < endX) return size_t(0);
    }
    return count;
    };
    while (levels) {
      int selected = -1, distance = 256;
      for (int level = 0; level <= kMaximumZoom; ++level) if (levels & (uint32_t(1) << level)) {
        const int difference = std::abs(level - int(zoom));
        if (difference < distance || (difference == distance && level > selected)) { selected = level; distance = difference; }
      }
      if (const size_t count = collect(selected)) return count;
      levels &= ~(uint32_t(1) << selected);
    }
    return 0;
  }
  template<bool Wire>
  void fallbackTiles(const Raster<Wire> &raster, const Viewport &v, const TilePlacement &missing,
                     int firstX, int firstY, int endX, int endY, uint8_t opacity) const {
    std::array<FallbackPiece, 32> pieces;
    size_t count = 0;
    int selected = -1, distance = 256;
    // Choose a level for the full visible placement, independent of the
    // caller's rendering strip. Partial old levels must not change between
    // a whole-frame render and a clipped/striped render of that same frame.
    const int chooseX = std::max<int>(0, missing.x), chooseY = std::max<int>(0, missing.y);
    const int chooseRight = std::min<int>(config.width, missing.x + missing.width);
    const int chooseBottom = std::min<int>(config.height, missing.y + missing.height);
    // Only existing ready cache entries participate; rendering neither starts
    // requests nor allocates/copies retained tile pixels.
    for (const auto &entry : entries) {
      if (entry.state != State::Ready || entry.key.layer != missing.key.layer || entry.key.zoom == zoom) continue;
      const double scale = std::ldexp(viewScale, int(zoom) - int(entry.key.zoom));
      const double size = tileSize() * scale, top = entry.key.y * size - double(v.top);
      if (top > chooseBottom - 0.5 || top + size <= chooseY + 0.5) continue;
      const double left = entry.key.x * size - double(v.left);
      const int firstCopy = int(std::ceil((chooseX + 0.5 - left - size) / v.world));
      const int lastCopy = int(std::floor((chooseRight - 0.5 - left) / v.world));
      if (firstCopy > lastCopy) continue;
      const int difference = std::abs(int(entry.key.zoom) - int(zoom));
      if (difference < distance || (difference == distance && int(entry.key.zoom) > selected)) {
        distance = difference; selected = entry.key.zoom;
      }
    }
    if (selected >= 0) for (const auto &entry : entries) {
      if (entry.state != State::Ready || entry.key.layer != missing.key.layer || entry.key.zoom != selected) continue;
      const double scale = std::ldexp(viewScale, int(zoom) - selected), size = tileSize() * scale;
      const double top = entry.key.y * size - double(v.top);
      if (top > endY - 0.5 || top + size <= firstY + 0.5) continue;
      const int beginY = int(std::max(double(firstY), std::ceil(top - 0.5)));
      const int finishY = int(std::min(double(endY), std::ceil(top + size - 0.5)));
      if (beginY >= finishY) continue;
      const double left = entry.key.x * size - double(v.left);
      const int firstCopy = int(std::ceil((firstX + 0.5 - left - size) / v.world));
      const int lastCopy = int(std::floor((endX - 0.5 - left) / v.world));
      for (int copy = firstCopy; copy <= lastCopy && count < pieces.size(); ++copy) {
        const double at = left + copy * v.world;
        const int beginX = int(std::max(double(firstX), std::ceil(at - 0.5)));
        const int finishX = int(std::min(double(endX), std::ceil(at + size - 0.5)));
        if (beginX < finishX) pieces[count++] = {&entry.image, at, top, scale, beginX, beginY, finishX, finishY};
      }
    }
    for (size_t i = 0; i < count; ++i) {
      const auto &piece = pieces[i];
      sampledTile(raster, *piece.image, opacity, piece.left, piece.top, piece.scale,
                  piece.x, piece.y, piece.right, piece.bottom);
    }
    // Ready tiles at one level never overlap. Fill uncovered bottom-layer
    // spans only once, preserving partial layer opacity and transparent labels.
    if (!source.layers[missing.key.layer].overlay) {
      std::sort(pieces.begin(), pieces.begin() + count, [](const FallbackPiece &a, const FallbackPiece &b) { return a.x < b.x; });
      for (int row = firstY; row < endY; ++row) {
        int cursor = firstX;
        for (size_t i = 0; i < count; ++i) {
          const auto &piece = pieces[i]; if (row < piece.y || row >= piece.bottom) continue;
          if (piece.x > cursor) raster.blendSpan(row, cursor, piece.x, config.missingTileColor, opacity);
          cursor = std::max(cursor, piece.right);
        }
        if (cursor < endX) raster.blendSpan(row, cursor, endX, config.missingTileColor, opacity);
      }
    }
  }
  Status fit(double westUnit, double longitudeSpan, double northY, double southY, uint16_t padding) {
    if (uint32_t(padding) * 2 >= config.width || uint32_t(padding) * 2 >= config.height)
      return Status::InvalidArgument;
    const double availableWidth = config.width - uint32_t(padding) * 2;
    const double availableHeight = config.height - uint32_t(padding) * 2;
    const int minimum = hasSource ? source.minZoom : 0;
    const int maximum = hasSource ? source.maxZoom : kMaximumZoom;
    int selected = -1;
    for (int candidate = maximum; candidate >= minimum; --candidate) {
      const double world = std::ldexp(double(tileSize()), candidate);
      if (longitudeSpan * world <= availableWidth + 1e-9 &&
          (southY - northY) * world <= availableHeight + 1e-9) { selected = candidate; break; }
    }
    if (selected < 0) return Status::TooLarge;
    const double world = std::ldexp(double(tileSize()), selected);
    const GeoPoint next = normalize(geographic({(westUnit + longitudeSpan / 2) * world,
                                                 (northY + southY) / 2 * world}, uint8_t(selected), tileSize()));
    if (next.latitude != center.latitude || next.longitude != center.longitude || zoom != selected || viewScale != 1 || follow) {
      center = next; zoom = uint8_t(selected); viewScale = 1; follow = false; ++revision;
    }
    return Status::Ok;
  }
  template<bool Wire>
  void drawCircle(const Raster<Wire> &raster, const CircleOverlay &circle, const RenderTransform &transform) const {
    if (!circle.fillOpacity && !circle.strokeWidth) return;
    const PixelPoint p = transform.screen(projected(circle.center, 0, 256));
    const double world = worldSize();
    if (grid.projection == Projection::Geographic) {
      // Degrees have separate local east/west and north/south meter scales.
      // This remains a local approximation, not a geodesic circle at the poles.
      const double ry = circle.radiusMeters * world / kEarthCircumferenceMeters;
      const double rx = ry / std::max(1e-12, std::cos(circle.center.latitude * kPi / 180));
      const double halfStroke = double(circle.strokeWidth) / 2;
      const double outerX = rx + halfStroke, outerY = ry + halfStroke;
      const double innerX = std::max(0.0, rx - halfStroke), innerY = std::max(0.0, ry - halfStroke);
      if (p.y + outerY < raster.y || p.y - outerY >= raster.y + raster.height) return;
      const int first = int(std::max(double(raster.y), std::ceil(p.y - outerY)));
      const int end = int(std::min(double(raster.y + raster.height - 1), std::floor(p.y + outerY)));
      for (int row = first; row <= end; ++row) {
        const double dy = row - p.y;
        auto *output = raster.pixels + size_t(row - raster.y) * raster.stride;
        for (int column = raster.x; column < raster.x + raster.width; ++column, ++output) {
          const double dx = nearestDelta(column - p.x, world);
          if ((dx / rx) * (dx / rx) + (dy / ry) * (dy / ry) <= 1)
            *output = Raster<Wire>::blended(circle.fillColor, *output, circle.fillOpacity);
          if (circle.strokeWidth && (dx / outerX) * (dx / outerX) + (dy / outerY) * (dy / outerY) <= 1 &&
              (!innerX || !innerY || (dx / innerX) * (dx / innerX) + (dy / innerY) * (dy / innerY) >= 1))
            *output = Raster<Wire>::encoded(circle.strokeColor);
        }
      }
      return;
    }
    const double metersPerPixel = kEarthCircumferenceMeters *
      std::cos(circle.center.latitude * kPi / 180) / world;
    const double radius = circle.radiusMeters / metersPerPixel;
    const double outer = radius + double(circle.strokeWidth) / 2;
    const double inner = std::max(0.0, radius - double(circle.strokeWidth) / 2);
    if (p.y + outer < raster.y || p.y - outer >= raster.y + raster.height) return;
    const int firstRow = int(std::max(double(raster.y), std::ceil(p.y - outer)));
    const int lastRow = int(std::min(double(raster.y + raster.height - 1), std::floor(p.y + outer)));
    const double radiusSquared = radius * radius, outerSquared = outer * outer, innerSquared = inner * inner;
    for (int row = firstRow; row <= lastRow; ++row) {
      const double dy = row - p.y, dySquared = dy * dy;
      const double extent = std::sqrt(std::max(0.0, outerSquared - dySquared));
      const auto draw = [&](int first, int end) {
        uint16_t *output = raster.pixels + size_t(row - raster.y) * raster.stride + size_t(first - raster.x);
        for (int column = first; column < end; ++column, ++output) {
          const double dx = nearestDelta(column - p.x, world);
          const double distanceSquared = dx * dx + dySquared;
          if (distanceSquared <= radiusSquared)
            *output = Raster<Wire>::blended(circle.fillColor, *output, circle.fillOpacity);
          if (circle.strokeWidth && distanceSquared >= innerSquared && distanceSquared <= outerSquared)
            *output = Raster<Wire>::encoded(circle.strokeColor);
        }
      };
      // The closest periodic copy is evaluated once per pixel, even when a
      // large local circle spans a whole world and several copies overlap.
      if (extent >= world / 2) draw(raster.x, raster.x + raster.width);
      else {
        const int firstCopy = int(std::ceil((raster.x - extent - p.x) / world));
        const int lastCopy = int(std::floor((raster.x + raster.width - 1 + extent - p.x) / world));
        for (int copy = firstCopy; copy <= lastCopy; ++copy) {
          const double cx = p.x + copy * world;
          const int first = int(std::max(double(raster.x), std::ceil(cx - extent)));
          const int end = int(std::min(double(raster.x + raster.width), std::floor(cx + extent) + 1));
          if (first < end) draw(first, end);
        }
      }
    }
  }
  template<bool Wire>
  void drawPolygon(const Raster<Wire> &raster, const Polygon &polygon, const RenderTransform &transform) const {
    if (!polygon.fillOpacity && !polygon.strokeWidth) return;
    const double world = worldSize();
    const PixelPoint c = transform.center;
    const double offsetX = double(config.width) / 2 - c.x, offsetY = double(config.height) / 2 - c.y;
    const double strokeMargin = double(polygon.strokeWidth) / 2 + 1;
    const double minY = polygon.minY * world + offsetY, maxY = polygon.maxY * world + offsetY;
    if (maxY + strokeMargin < raster.y || minY - strokeMargin >= raster.y + raster.height) return;
    const double minX = polygon.minX * world + offsetX, maxX = polygon.maxX * world + offsetX;
    const int firstCopy = int(std::ceil((raster.x - strokeMargin - maxX) / world));
    const int lastCopy = int(std::floor((raster.x + raster.width - 1 + strokeMargin - minX) / world));
    struct Crossing { double x; size_t edge; };
    const auto less = [](const Crossing &a, const Crossing &b) {
      return a.x < b.x || (a.x == b.x && a.edge < b.edge);
    };
    for (int copy = firstCopy; copy <= lastCopy; ++copy) {
      const double copyOffset = offsetX + copy * world;
      if (polygon.fillOpacity) {
        const int firstRow = int(std::max(double(raster.y), std::ceil(minY - 0.5)));
        const int endRow = int(std::min(double(raster.y + raster.height), std::ceil(maxY - 0.5)));
        for (int row = firstRow; row < endRow; ++row) {
          const double scan = row + 0.5;
          Crossing lower{};
          bool hasLower = false, inside = false;
          double start = 0;
          for (;;) {
            // A small fixed heap batches crossings without render allocations
            // or an unbounded task-stack buffer. Edge index breaks equal-x ties.
            Crossing crossings[64];
            size_t count = 0;
            for (size_t i = 0; i < polygon.points.size(); ++i) {
              const auto &a = polygon.points[i];
              const auto &b = polygon.points[(i + 1) % polygon.points.size()];
              const double ay = a.y * world + offsetY, by = b.y * world + offsetY;
              if ((ay > scan) == (by > scan)) continue;
              const Crossing crossing{(a.x + (b.x - a.x) * ((scan - ay) / (by - ay))) * world + copyOffset, i};
              if (hasLower && !less(lower, crossing)) continue;
              if (count < 64) {
                crossings[count++] = crossing;
                std::push_heap(crossings, crossings + count, less);
              } else if (less(crossing, crossings[0])) {
                std::pop_heap(crossings, crossings + count, less);
                crossings[count - 1] = crossing;
                std::push_heap(crossings, crossings + count, less);
              }
            }
            if (!count) break;
            std::sort(crossings, crossings + count, less);
            for (size_t i = 0; i < count; ++i) {
              if (!inside) start = crossings[i].x;
              else {
                const double first = std::max(double(raster.x), std::ceil(start - 0.5));
                const double end = std::min(double(raster.x + raster.width), std::ceil(crossings[i].x - 0.5));
                if (first < end) raster.blendSpan(row, int(first), int(end), polygon.fillColor, polygon.fillOpacity);
              }
              inside = !inside;
            }
            lower = crossings[count - 1]; hasLower = true;
            if (count < 64) break;
          }
        }
      }
      if (polygon.strokeWidth)
        for (size_t i = 0; i < polygon.points.size(); ++i) {
          const auto &a = polygon.points[i];
          const auto &b = polygon.points[(i + 1) % polygon.points.size()];
          raster.line(a.x * world + copyOffset, a.y * world + offsetY,
                      b.x * world + copyOffset, b.y * world + offsetY,
                      polygon.strokeColor, polygon.strokeWidth);
        }
    }
  }
  template<bool Wire>
  void drawPoint(const Raster<Wire> &raster, GeoPoint point, uint16_t color, double radius, const RenderTransform &transform) const {
    const PixelPoint p = transform.screen(projected(point, 0, 256));
    if (p.y + radius < raster.y || p.y - radius >= raster.y + raster.height) return;
    const double world = worldSize();
    const int first = int(std::ceil((raster.x - radius - p.x) / world));
    const int last = int(std::floor((raster.x + raster.width - 1 + radius - p.x) / world));
    for (int copy = first; copy <= last; ++copy) raster.disk(p.x + copy * world, p.y, radius, color);
  }
  template<bool Wire>
  void drawMarker(const Raster<Wire> &raster, const Marker &marker, const RenderTransform &transform) const {
    if (marker.shape == MarkerShape::Circle) { drawPoint(raster, marker.position, marker.color, marker.radius, transform); return; }
    const auto p = transform.screen(projected(marker.position, 0, 256));
    if (p.y < raster.y || p.y - 3 * marker.radius >= raster.y + raster.height) return;
    const double world = worldSize(), radius = marker.radius;
    const int first = int(std::ceil((raster.x - radius - p.x) / world));
    const int last = int(std::floor((raster.x + raster.width - 1 + radius - p.x) / world));
    for (int copy = first; copy <= last; ++copy) raster.pin(p.x + copy * world, p.y, radius, marker.color);
  }
  template<bool Wire>
  void drawSegment(const Raster<Wire> &raster, PixelPoint from, PixelPoint to, uint16_t color, uint8_t stroke,
                   const RenderTransform &transform) const {
    PixelPoint a = transform.screen(from), b = transform.screen(to);
    const double world = worldSize();
    b.x = a.x + nearestDelta(b.x - a.x, world);
    const double radius = double(stroke) / 2 + 1;
    if (std::max(a.y, b.y) + radius < raster.y || std::min(a.y, b.y) - radius >= raster.y + raster.height) return;
    const int first = int(std::ceil((raster.x - radius - std::max(a.x, b.x)) / world));
    const int last = int(std::floor((raster.x + raster.width - 1 + radius - std::min(a.x, b.x)) / world));
    for (int copy = first; copy <= last; ++copy)
      raster.line(a.x + copy * world, a.y, b.x + copy * world, b.y, color, stroke);
  }
};

Map::Map(const MapConfig &config) : impl_(new (std::nothrow) Impl(config)) {}
Map::~Map() = default;
Status Map::setSource(const TileSource &source) {
  return setSource(source, TileGrid{});
}
Status Map::setSource(const TileSource &source, const TileGrid &grid) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured) return Status::InvalidArgument;
  NICE_MAP_TRY {
    if (!source.valid() || !grid.valid()) return Status::InvalidArgument;
    if (!grid.matrices.empty()) for (unsigned level = source.minZoom; level <= source.maxZoom; ++level) {
      const auto *matrix = grid.matrix(uint8_t(level));
      if (!matrix || matrix->tileWidth != source.tileSize || matrix->tileHeight != source.tileSize) return Status::InvalidArgument;
    }
    TileSource copy = source;
    TileGrid gridCopy = grid;
    auto &s = *impl_;
    if (s.grid.projection != grid.projection) {
      // Finish all allocations before committing metadata or touching leases.
      auto route = s.route, track = s.track; auto polygons = s.polygons;
      const auto y = [&](double value) {
        return project(unproject({0, value}, 0, 256, s.grid.projection), 0, 256, grid.projection).y;
      };
      for (auto &point : route) point.y = y(point.y);
      for (auto &point : track) point.y = y(point.y);
      for (auto &polygon : polygons) {
        polygon.minY = std::numeric_limits<double>::infinity(); polygon.maxY = -polygon.minY;
        for (auto &point : polygon.points) {
          point.y = y(point.y * 256) / 256;
          polygon.minY = std::min(polygon.minY, point.y); polygon.maxY = std::max(polygon.maxY, point.y);
        }
      }
      s.route.swap(route); s.track.swap(track); s.polygons.swap(polygons);
      s.center = normalized(s.center, grid.projection); s.location = normalized(s.location, grid.projection);
      for (auto &marker : s.markers) marker.position = normalized(marker.position, grid.projection);
      for (auto &circle : s.circles) circle.center = normalized(circle.center, grid.projection);
    }
    impl_->source = std::move(copy);
    impl_->grid = std::move(gridCopy);
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  impl_->hasSource = true;
  const double visualZoom = std::max(double(impl_->source.minZoom), std::min(double(impl_->source.maxZoom), viewZoom()));
  impl_->zoom = uint8_t(std::floor(visualZoom)); impl_->viewScale = std::exp2(visualZoom - impl_->zoom);
  impl_->clear();
  return Status::Ok;
}
const TileSource &Map::source() const { static const TileSource empty; return impl_ ? impl_->source : empty; }
const TileGrid &Map::sourceGrid() const { static const TileGrid empty; return impl_ ? impl_->grid : empty; }
Status Map::setLayer(uint8_t layer, const SourceLayer &replacement) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || !s.hasSource || size_t(layer) >= s.source.layers.size()) return Status::InvalidArgument;
  NICE_MAP_TRY {
    // replacement may refer to our own layer. Finish all copies and validation
    // before changing source metadata or releasing any image/request owners.
    TileSource candidate = s.source;
    candidate.layers[layer] = replacement;
    if (!candidate.valid()) return Status::InvalidArgument;
    s.source.layers[layer] = std::move(candidate.layers[layer]);
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  s.entries.erase(std::remove_if(s.entries.begin(), s.entries.end(),
      [layer](const Impl::Entry &entry) { return entry.key.layer == layer; }), s.entries.end());
  s.composites.clear();
  s.rebuildComposites();
  ++s.revision;
  return Status::Ok;
}
Status Map::setLayerOpacity(uint8_t layer, uint8_t opacity) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured || !impl_->hasSource || size_t(layer) >= impl_->source.layers.size()) return Status::InvalidArgument;
  if (impl_->source.layers[layer].opacity != opacity) {
    impl_->source.layers[layer].opacity = opacity; ++impl_->revision;
    impl_->composites.clear(); impl_->rebuildComposites();
  }
  return Status::Ok;
}
Status Map::resize(uint16_t width, uint16_t height) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured || !width || !height || width > 4096 || height > 4096) return Status::InvalidArgument;
  if (impl_->config.width != width || impl_->config.height != height) {
    impl_->config.width = width; impl_->config.height = height; ++impl_->revision;
  }
  return Status::Ok;
}
uint16_t Map::width() const { return impl_ ? impl_->config.width : 0; }
uint16_t Map::height() const { return impl_ ? impl_->config.height : 0; }
Status Map::setCenter(GeoPoint center) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured || !validCoordinate(center)) return Status::InvalidArgument;
  center = impl_->normalize(center);
  if (center.latitude != impl_->center.latitude || center.longitude != impl_->center.longitude) {
    impl_->center = center; ++impl_->revision;
  }
  return Status::Ok;
}
GeoPoint Map::center() const { return impl_ ? impl_->center : GeoPoint{}; }
Status Map::setZoom(uint8_t zoom) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured || zoom > kMaximumZoom ||
      (impl_->hasSource && (zoom < impl_->source.minZoom || zoom > impl_->source.maxZoom))) return Status::InvalidArgument;
  if (impl_->zoom != zoom || impl_->viewScale != 1) { impl_->zoom = zoom; impl_->viewScale = 1; ++impl_->revision; }
  return Status::Ok;
}
uint8_t Map::zoom() const { return impl_ ? impl_->zoom : 0; }
double Map::viewZoom() const { return impl_ ? impl_->zoom + std::log2(impl_->viewScale) : 0; }
Status Map::setViewZoomAt(double zoom, double screenX, double screenY) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || !std::isfinite(zoom) || !std::isfinite(screenX) || !std::isfinite(screenY) ||
      zoom < (s.hasSource ? s.source.minZoom : 0) || zoom > (s.hasSource ? s.source.maxZoom : kMaximumZoom)) return Status::InvalidArgument;
  if (zoom == viewZoom()) return Status::Ok;
  const GeoPoint anchor = fromScreen({screenX, screenY});
  const uint8_t level = uint8_t(std::floor(zoom));
  const double scale = std::exp2(zoom - level), world = std::ldexp(double(s.tileSize()), level) * scale;
  auto projected = s.projected(anchor, level, s.tileSize()); projected.x *= scale; projected.y *= scale;
  projected.x = wrap(projected.x - (screenX - double(width()) / 2), world);
  projected.y = std::max(0.0, std::min(s.worldHeight(world), projected.y - (screenY - double(height()) / 2)));
  projected.x /= scale; projected.y /= scale;
  s.center = s.normalize(s.geographic(projected, level, s.tileSize()));
  s.zoom = level; s.viewScale = scale; ++s.revision;
  return Status::Ok;
}
Status Map::panBy(double dx, double dy) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured || !std::isfinite(dx) || !std::isfinite(dy)) return Status::InvalidArgument;
  auto c = impl_->projected(impl_->center, impl_->zoom, impl_->tileSize());
  const double world = std::ldexp(double(impl_->tileSize()), impl_->zoom);
  c.x = wrap(c.x - std::fmod(dx / impl_->viewScale, world), world);
  c.y = std::max(0.0, std::min(impl_->worldHeight(world), c.y - dy / impl_->viewScale));
  const GeoPoint next = impl_->normalize(impl_->geographic(c, impl_->zoom, impl_->tileSize()));
  if (next.latitude != impl_->center.latitude || next.longitude != impl_->center.longitude || impl_->follow) {
    impl_->center = next; impl_->follow = false; ++impl_->revision;
  }
  return Status::Ok;
}
Status Map::zoomAt(uint8_t zoom, double screenX, double screenY) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured || zoom > kMaximumZoom || !std::isfinite(screenX) || !std::isfinite(screenY) ||
      screenX < 0 || screenX > width() || screenY < 0 || screenY > height() ||
      (impl_->hasSource && (zoom < impl_->source.minZoom || zoom > impl_->source.maxZoom))) return Status::InvalidArgument;
  if (impl_->viewScale != 1) return setViewZoomAt(zoom, screenX, screenY);
  if (zoom == impl_->zoom) return Status::Ok;
  const auto current = impl_->projected(impl_->center, impl_->zoom, impl_->tileSize());
  const double factor = std::ldexp(1.0, int(zoom) - int(impl_->zoom));
  const double world = std::ldexp(double(impl_->tileSize()), zoom);
  const double offsetX = screenX - double(width()) / 2, offsetY = screenY - double(height()) / 2;
  const PixelPoint next{wrap((current.x + offsetX) * factor - offsetX, world),
                        std::max(0.0, std::min(impl_->worldHeight(world), (current.y + offsetY) * factor - offsetY))};
  impl_->center = impl_->normalize(impl_->geographic(next, zoom, impl_->tileSize())); impl_->zoom = zoom; ++impl_->revision;
  return Status::Ok;
}
Status Map::fitBounds(GeoBounds bounds, uint16_t padding) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured || !validCoordinate({bounds.south, bounds.west}) ||
      !validCoordinate({bounds.north, bounds.east}) || bounds.south > bounds.north ||
      bounds.west < -180 || bounds.west > 180 || bounds.east < -180 || bounds.east > 180)
    return Status::InvalidArgument;
  double span = bounds.east - bounds.west;
  if (span < 0) span += 360;
  const double northY = impl_->projected({bounds.north, 0}, 0, 256).y / 256;
  const double southY = impl_->projected({bounds.south, 0}, 0, 256).y / 256;
  return impl_->fit((bounds.west + 180) / 360, span / 360, northY, southY, padding);
}
Status Map::fitPoints(const GeoPoint *points, size_t count, uint16_t padding) {
  if (!impl_) return Status::NoMemory;
  if (!impl_->configured || !count || count > 65536 || !points ||
      uint32_t(padding) * 2 >= width() || uint32_t(padding) * 2 >= height()) return Status::InvalidArgument;
  for (size_t i = 0; i < count; ++i) if (!validCoordinate(points[i])) return Status::InvalidArgument;
  NICE_MAP_TRY {
    std::vector<double> longitudes;
    longitudes.reserve(count);
    double south = points[0].latitude, north = south;
    for (size_t i = 0; i < count; ++i) {
      south = std::min(south, points[i].latitude); north = std::max(north, points[i].latitude);
      longitudes.push_back(normalizeLongitude(points[i].longitude) + 180);
    }
    std::sort(longitudes.begin(), longitudes.end());
    size_t gapIndex = count - 1;
    double largestGap = longitudes[0] + 360 - longitudes[count - 1];
    for (size_t i = 0; i + 1 < count; ++i) {
      const double gap = longitudes[i + 1] - longitudes[i];
      if (gap > largestGap) { largestGap = gap; gapIndex = i; }
    }
    const double west = longitudes[(gapIndex + 1) % count] / 360;
    const double span = std::max(0.0, 360 - largestGap) / 360;
    return impl_->fit(west, span, impl_->projected({north, 0}, 0, 256).y / 256,
                      impl_->projected({south, 0}, 0, 256).y / 256, padding);
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  return Status::NoMemory;
}
PixelPoint Map::toScreen(GeoPoint point) const {
  if (!impl_ || !impl_->configured || !validCoordinate(point)) return invalidPixel();
  return impl_->screen(impl_->normalize(point));
}
GeoPoint Map::fromScreen(PixelPoint point) const {
  if (!impl_ || !impl_->configured || !std::isfinite(point.x) || !std::isfinite(point.y)) return invalidGeo();
  const auto c = impl_->projected(impl_->center, impl_->zoom, impl_->tileSize());
  const double world = std::ldexp(double(impl_->tileSize()), impl_->zoom);
  const PixelPoint p{wrap(c.x + std::fmod((point.x - double(width()) / 2) / impl_->viewScale, world), world),
                     std::max(0.0, std::min(impl_->worldHeight(world), c.y + (point.y - double(height()) / 2) / impl_->viewScale))};
  return impl_->normalize(impl_->geographic(p, impl_->zoom, impl_->tileSize()));
}
std::vector<TilePlacement> Map::visibleTiles() const {
  std::vector<TilePlacement> result;
  if (!impl_ || !impl_->configured || !impl_->hasSource) return result;
  NICE_MAP_TRY {
    const auto viewport = impl_->viewport();
    if (viewport.lastY >= viewport.firstY)
      result.reserve(size_t(viewport.lastX - viewport.firstX + 1) * size_t(viewport.lastY - viewport.firstY + 1) * impl_->source.layers.size());
    impl_->placements(viewport, [&](TilePlacement placement) { result.push_back(placement); });
  } NICE_MAP_CATCH_RETURN(std::vector<TilePlacement>{})
  return result;
}

Status Map::nextRequest(TileRequest &request, uint32_t nowMs) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || !s.hasSource) return Status::InvalidArgument;
  const auto viewport = s.viewport();
  for (auto &item : s.composites) if (s.visible(item.key, viewport)) item.used = s.touch();
  if (!s.visibleFits(viewport)) return Status::NoMemory;
  if (s.pendingCount() >= s.config.maxInFlight) return Status::Busy;
  TileKey best{};
  bool found = false;
  double bestDistance = 0;
  uint8_t bestLayer = 0;
  s.placements(viewport, [&](const TilePlacement &placement) {
    const auto *entry = s.find(placement.key);
    if (entry && (entry->state != Impl::State::Failed || !s.retryReady(*entry, nowMs))) return;
    const double dx = placement.x + double(placement.width) / 2 - double(s.config.width) / 2;
    const double dy = placement.y + double(placement.height) / 2 - double(s.config.height) / 2;
    const double distance = dx * dx + dy * dy;
    if (!found || placement.key.layer < bestLayer || (placement.key.layer == bestLayer && distance < bestDistance)) {
      found = true; best = placement.key; bestLayer = best.layer; bestDistance = distance;
    }
  });
  if (!found) return Status::NotFound;
  if (s.sequence == std::numeric_limits<uint64_t>::max()) return Status::TooLarge;
  NICE_MAP_TRY {
    TileRequest candidate;
    candidate.key = best; candidate.id = s.sequence + 1; candidate.generation = s.generation;
    candidate.tileSize = s.source.tileSize; candidate.overlay = s.source.layers[best.layer].overlay;
    candidate.url = s.grid.matrices.empty() ? s.source.url(best) : s.source.url(best, s.grid);
    if (candidate.url.empty()) return Status::InvalidArgument;
    Impl::Entry fresh;
    fresh.key = best; fresh.requestId = candidate.id; fresh.requestUrl = candidate.url;
    // Allocate output metadata before any cache eviction. Moving it into the
    // caller below does not allocate and a failed request leaves output intact.
    TileRequest output = candidate;
    auto *existing = s.find(best);
    if (!existing) s.entries.reserve(s.entries.size() + 1);
    const size_t needed = s.expectedBytes(best);
    size_t bytes = s.committedBytes();
    size_t count = s.entries.size() + (existing ? 0 : 1);
    // Plan all evictions first, on the heap rather than the small UI task stack.
    // No cache contents change if off-screen pending work prevents admission.
    std::vector<size_t> victims;
    victims.reserve(s.entries.size());
    while (count > s.config.maxCachedTiles || bytes > s.config.cacheBytes || needed > s.config.cacheBytes - bytes) {
      size_t index = s.entries.size();
      for (size_t i = 0; i < s.entries.size(); ++i) {
        const auto &entry = s.entries[i];
        if (entry.key == best || entry.state == Impl::State::Pending || s.visible(entry.key, viewport)) continue;
        bool selected = false;
        for (size_t victim : victims) if (victim == i) { selected = true; break; }
        if (!selected && (index == s.entries.size() ||
            (s.fallbackNeeded(entry, viewport) != s.fallbackNeeded(s.entries[index], viewport) ?
              !s.fallbackNeeded(entry, viewport) : entry.used < s.entries[index].used))) index = i;
      }
      if (index == s.entries.size()) return s.pendingCount() ? Status::Busy : Status::NoMemory;
      victims.push_back(index); --count;
      if (s.entries[index].state == Impl::State::Ready) bytes -= s.entries[index].image.bytes();
    }
    std::sort(victims.begin(), victims.end(), std::greater<size_t>());
    for (size_t victim : victims) {
      if (s.entries[victim].state == Impl::State::Ready) ++s.evictions;
      s.discardComposite(s.entries[victim].key);
      s.entries.erase(s.entries.begin() + victim);
    }
    if ((existing = s.find(best))) *existing = std::move(fresh);
    else s.entries.push_back(std::move(fresh));
    s.sequence = candidate.id;
    for (auto &entry : s.entries) if (s.visible(entry.key, viewport)) {
      entry.used = s.touch();
      if (entry.state == Impl::State::Ready) ++s.hits;
    }
    ++s.misses;
    request = std::move(output);
    return Status::Ok;
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  return Status::NoMemory;
}
Status Map::submit(const TileRequest &request, TileImage &&image) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured) return Status::InvalidArgument;
  if (!s.hasSource || request.generation != s.generation) return Status::Canceled;
  auto *entry = s.find(request.key);
  if (!entry || !s.matching(request, *entry)) return Status::Canceled;
  if (!image.valid() || image.width() != s.source.tileSize || image.height() != s.source.tileSize ||
      (!request.overlay && image.alpha()) || image.bytes() > s.expectedBytes(request.key)) return Status::InvalidArgument;
  entry->image = std::move(image); entry->state = Impl::State::Ready;
  entry->requestUrl.clear(); entry->used = s.touch(); ++s.revision;
  s.discardComposite(request.key); s.tryCompose(request.key, s.viewport());
  return Status::Ok;
}
Status Map::fail(const TileRequest &request, Status reason, uint32_t nowMs) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || reason == Status::Ok || uint8_t(reason) > uint8_t(Status::IoError)) return Status::InvalidArgument;
  if (!s.hasSource || request.generation != s.generation) return Status::Canceled;
  auto *entry = s.find(request.key);
  if (!entry || !s.matching(request, *entry)) return Status::Canceled;
  entry->state = Impl::State::Failed; entry->failedAt = nowMs; entry->requestUrl.clear();
  entry->used = s.touch(); ++s.failures; ++s.revision;
  if (reason == Status::NoMemory) {
    // A shared/fragmented allocator can exhaust contiguous space before our
    // byte budget fills. Retrying the same decode with the same retained cache
    // cannot recover: release bounded off-screen owners first. Current tiles
    // and pending request tokens stay intact; fallback tiles are the last choice.
    const auto viewport = s.viewport();
    size_t released = 0, needed = s.expectedBytes(request.key);
    while (released < needed) {
      size_t victim = s.entries.size();
      for (size_t i = 0; i < s.entries.size(); ++i) {
        const auto &candidate = s.entries[i];
        if (candidate.state != Impl::State::Ready || s.visible(candidate.key, viewport)) continue;
        if (victim == s.entries.size() ||
            (s.fallbackNeeded(candidate, viewport) != s.fallbackNeeded(s.entries[victim], viewport) ?
              !s.fallbackNeeded(candidate, viewport) : candidate.used < s.entries[victim].used)) victim = i;
      }
      if (victim == s.entries.size()) break;
      released += s.entries[victim].image.bytes();
      s.discardComposite(s.entries[victim].key);
      s.entries.erase(s.entries.begin() + victim); ++s.evictions;
    }
  }
  return Status::Ok;
}
void Map::clearCache() { if (impl_ && impl_->configured) impl_->clear(); }
size_t Map::reclaimCacheEntry() {
  if (!impl_ || !impl_->configured || !impl_->hasSource) return 0;
  auto &s = *impl_;
  const auto viewport = s.viewport();
  size_t victim = s.composites.size();
  for (size_t i = 0; i < s.composites.size(); ++i) {
    const auto &candidate = s.composites[i];
    if (s.visible(candidate.key, viewport)) continue;
    if (victim == s.composites.size() || candidate.used < s.composites[victim].used) victim = i;
  }
  if (victim != s.composites.size()) {
    const size_t released = s.composites[victim].image.bytes();
    s.composites.erase(s.composites.begin() + victim);
    // Old-level composites can still supply visible fallback even though their
    // keys are not at the current zoom. Republish after every successful trim.
    ++s.evictions; ++s.revision;
    return released;
  }
  victim = s.entries.size();
  bool victimFallback = false;
  for (size_t i = 0; i < s.entries.size(); ++i) {
    const auto &candidate = s.entries[i];
    if (candidate.state != Impl::State::Ready || s.visible(candidate.key, viewport)) continue;
    const bool fallback = s.fallbackNeeded(candidate, viewport);
    if (victim == s.entries.size() || (fallback != victimFallback ? !fallback : candidate.used < s.entries[victim].used)) {
      victim = i; victimFallback = fallback;
    }
  }
  if (victim == s.entries.size()) return 0;
  size_t released = s.entries[victim].image.bytes();
  const size_t derivedBefore = s.compositeBytes();
  s.discardComposite(s.entries[victim].key);
  released += derivedBefore - s.compositeBytes();
  s.entries.erase(s.entries.begin() + victim);
  ++s.evictions; ++s.revision;
  return released;
}
Status Map::takeCacheFrom(Map &donor) {
  if (!impl_ || !donor.impl_) return Status::NoMemory;
  auto &s = *impl_; auto &from = *donor.impl_;
  if (!s.configured || !from.configured || !s.hasSource || !from.hasSource ||
      s.source.tileSize != from.source.tileSize || s.source.layers.size() != from.source.layers.size() ||
      !sameGrid(s.grid, from.grid))
    return Status::InvalidArgument;
  if (this == &donor) return Status::Ok;
  for (size_t layer = 0; layer < s.source.layers.size(); ++layer)
    if (s.source.layers[layer].urlTemplate != from.source.layers[layer].urlTemplate ||
        s.source.layers[layer].overlay != from.source.layers[layer].overlay) return Status::InvalidArgument;
  NICE_MAP_TRY {
    // Complete the only potentially allocating operation before either cache
    // changes. A failure leaves both maps and all request tokens untouched.
    s.entries.reserve(std::min(s.config.maxCachedTiles, s.entries.size() + from.entries.size()));
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  const auto viewport = s.viewport();
  size_t bytes = s.committedBytes();
  bool changed = false;
  for (unsigned pass = 0; pass < 2; ++pass) for (auto &entry : from.entries) {
    if (entry.state != Impl::State::Ready || !entry.image.valid() ||
        (s.visible(entry.key, viewport) ? 0u : 1u) != pass || s.find(entry.key) ||
        s.entries.size() >= s.config.maxCachedTiles || bytes > s.config.cacheBytes ||
        entry.image.bytes() > s.config.cacheBytes - bytes) continue;
    bytes += entry.image.bytes();
    s.entries.push_back(std::move(entry));
    s.entries.back().used = s.touch();
    changed = true;
  }
  if (changed) ++s.revision;
  s.takeCompositesFrom(from, viewport);
  from.clear();
  s.rebuildComposites();
  return Status::Ok;
}
CacheStats Map::cacheStats() const {
  CacheStats result;
  if (!impl_) return result;
  result.budget = impl_->config.cacheBytes;
  result.compositeBudget = impl_->config.compositeCacheBytes;
  result.compositeTiles = impl_->composites.size(); result.compositeBytes = impl_->compositeBytes();
  result.compositeBuilds = impl_->compositeBuilds;
  result.hits = impl_->hits; result.misses = impl_->misses; result.evictions = impl_->evictions; result.failures = impl_->failures;
  for (const auto &entry : impl_->entries) {
    if (entry.state == Impl::State::Ready) { ++result.readyTiles; result.bytes += entry.image.bytes(); }
    else if (entry.state == Impl::State::Pending) ++result.inFlight;
  }
  return result;
}
uint64_t Map::revision() const { return impl_ ? impl_->revision : 0; }

Status Map::setMarkers(const Marker *markers, size_t count) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || count > s.config.maxMarkers || (count && !markers)) return Status::InvalidArgument;
  for (size_t i = 0; i < count; ++i) if (!validCoordinate(markers[i].position) || !markers[i].radius ||
      (markers[i].shape != MarkerShape::Circle && markers[i].shape != MarkerShape::Pin)) return Status::InvalidArgument;
  NICE_MAP_TRY {
    std::vector<Marker> copy;
    if (count) copy.assign(markers, markers + count);
    for (auto &marker : copy) marker.position = s.normalize(marker.position);
    s.markers.swap(copy); ++s.revision;
    for (const auto &marker : s.markers) s.projected(marker.position, 0, 256);
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  return Status::Ok;
}
int32_t Map::markerAt(PixelPoint point, double tolerance) const {
  if (!impl_ || !impl_->configured || !std::isfinite(point.x) || !std::isfinite(point.y) ||
      !std::isfinite(tolerance) || tolerance < 0 || point.x < 0 || point.x >= width() ||
      point.y < 0 || point.y >= height()) return -1;
  const auto &s = *impl_;
  for (size_t i = s.markers.size(); i > 0; --i) {
    const auto &marker = s.markers[i - 1];
    const auto screen = s.screen(marker.position);
    if (marker.shape == MarkerShape::Pin) {
      if (pinHit(nearestDelta(point.x - screen.x, s.worldSize()), point.y - screen.y, marker.radius, tolerance)) return int32_t(i - 1);
      continue;
    }
    if (std::hypot(nearestDelta(point.x - screen.x, s.worldSize()), point.y - screen.y) <=
        double(marker.radius) + tolerance) return int32_t(i - 1);
  }
  return -1;
}
Status Map::setCircles(const CircleOverlay *circles, size_t count) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || count > s.config.maxCircles || (count && !circles)) return Status::InvalidArgument;
  for (size_t i = 0; i < count; ++i) {
    if (!validCoordinate(circles[i].center) || !std::isfinite(circles[i].radiusMeters) || circles[i].radiusMeters <= 0)
      return Status::InvalidArgument;
    if (circles[i].radiusMeters > kEarthCircumferenceMeters / 2) return Status::TooLarge;
  }
  NICE_MAP_TRY {
    std::vector<CircleOverlay> copy;
    if (count) copy.assign(circles, circles + count);
    for (auto &circle : copy) circle.center = s.normalize(circle.center);
    s.circles.swap(copy); ++s.revision;
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  return Status::Ok;
}
Status Map::setPolygons(const PolygonOverlay *polygons, size_t count) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || count > s.config.maxPolygons || (count && !polygons)) return Status::InvalidArgument;
  size_t pointCount = 0;
  for (size_t i = 0; i < count; ++i) {
    if (polygons[i].points.size() < 3 || polygons[i].points.size() > s.config.maxPolygonPoints - pointCount)
      return Status::InvalidArgument;
    pointCount += polygons[i].points.size();
    for (const auto &point : polygons[i].points) if (!validCoordinate(point)) return Status::InvalidArgument;
  }
  NICE_MAP_TRY {
    std::vector<Impl::Polygon> prepared;
    prepared.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const auto &input = polygons[i];
      Impl::Polygon polygon;
      polygon.fillColor = input.fillColor; polygon.strokeColor = input.strokeColor;
      polygon.fillOpacity = input.fillOpacity; polygon.strokeWidth = input.strokeWidth;
      polygon.points.reserve(input.points.size());
      for (const auto &point : input.points) {
        PixelPoint p = s.projected(point, 0, 256); p.x /= 256; p.y /= 256;
        if (!polygon.points.empty()) p.x = polygon.points.back().x + nearestDelta(p.x - polygon.points.back().x, 1);
        else polygon.minX = polygon.maxX = p.x, polygon.minY = polygon.maxY = p.y;
        polygon.minX = std::min(polygon.minX, p.x); polygon.maxX = std::max(polygon.maxX, p.x);
        polygon.minY = std::min(polygon.minY, p.y); polygon.maxY = std::max(polygon.maxY, p.y);
        polygon.points.push_back(p);
      }
      const double closing = polygon.points.back().x + nearestDelta(polygon.points.front().x - polygon.points.back().x, 1);
      // A ring winding around a pole/world has no single planar Mercator
      // interior. Accept local/date-line rings; reject ambiguous global ones.
      if (polygon.maxX - polygon.minX > 1 + 1e-9 || std::abs(closing - polygon.points.front().x) > 1e-9)
        return Status::Unsupported;
      prepared.push_back(std::move(polygon));
    }
    s.polygons.swap(prepared); ++s.revision;
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  return Status::Ok;
}
Status Map::setRoute(const GeoPoint *points, size_t count, uint16_t color, uint8_t width) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || !width || count > s.config.maxRoutePoints || (count && !points)) return Status::InvalidArgument;
  for (size_t i = 0; i < count; ++i) if (!validCoordinate(points[i])) return Status::InvalidArgument;
  NICE_MAP_TRY {
    std::vector<PixelPoint> copy;
    copy.reserve(count);
    for (size_t i = 0; i < count; ++i) copy.push_back(s.projected(s.normalize(points[i]), 0, 256));
    s.route.swap(copy); s.routeColor = color; s.routeWidth = width; ++s.revision;
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  return Status::Ok;
}
Status Map::appendTrack(GeoPoint point) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || !s.config.maxTrackPoints || !validCoordinate(point)) return Status::InvalidArgument;
  const PixelPoint projected = s.projected(s.normalize(point), 0, 256);
  NICE_MAP_TRY {
    if (s.track.size() < s.config.maxTrackPoints) s.track.push_back(projected);
    else { s.track[s.trackHead] = projected; s.trackHead = (s.trackHead + 1) % s.track.size(); }
  } NICE_MAP_CATCH_RETURN(Status::NoMemory)
  ++s.revision;
  return Status::Ok;
}
void Map::clearTrack() { if (impl_ && impl_->configured && !impl_->track.empty()) { impl_->track.clear(); impl_->trackHead = 0; ++impl_->revision; } }
Status Map::setLocation(GeoPoint point, double headingDegrees) {
  if (!impl_) return Status::NoMemory;
  auto &s = *impl_;
  if (!s.configured || !validCoordinate(point) || !std::isfinite(headingDegrees)) return Status::InvalidArgument;
  s.location = s.normalize(point); s.heading = wrap(headingDegrees, 360); s.hasLocation = true;
  if (s.follow) s.center = s.location;
  ++s.revision;
  return Status::Ok;
}
void Map::clearLocation() { if (impl_ && impl_->configured && impl_->hasLocation) { impl_->hasLocation = false; ++impl_->revision; } }
void Map::setFollow(bool enabled) {
  if (!impl_ || !impl_->configured) return;
  bool changed = impl_->follow != enabled;
  impl_->follow = enabled;
  if (enabled && impl_->hasLocation) {
    changed = changed || impl_->center.latitude != impl_->location.latitude || impl_->center.longitude != impl_->location.longitude;
    impl_->center = impl_->location;
  }
  if (changed) ++impl_->revision;
}
bool Map::following() const { return impl_ && impl_->configured && impl_->follow; }

Status Map::renderRegion(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                         uint16_t *pixels, size_t capacityPixels, size_t stride) const {
  return renderRegionImpl<false>(x, y, width, height, pixels, capacityPixels, stride, nullptr, nullptr, {});
}

Status Map::renderRegionWithBlitter(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                         uint16_t *pixels, size_t capacityPixels, size_t stride, RasterBlitter blitter) const {
  return renderRegionImpl<false>(x, y, width, height, pixels, capacityPixels, stride, nullptr, nullptr, blitter);
}

Status Map::renderRegionWire(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                         uint16_t *pixels, size_t capacityPixels, size_t stride) const {
  return renderRegionImpl<true>(x, y, width, height, pixels, capacityPixels, stride, nullptr, nullptr, {});
}

Status Map::renderRegionWithTile(const TileKey &key, const TileImage &image,
                         uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                         uint16_t *pixels, size_t capacityPixels, size_t stride) const {
  return renderRegionImpl<false>(x, y, width, height, pixels, capacityPixels, stride, &key, &image, {});
}

template<bool Wire>
Status Map::renderRegionImpl(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                         uint16_t *pixels, size_t capacityPixels, size_t stride,
                         const TileKey *borrowedKey, const TileImage *borrowedImage, RasterBlitter blitter) const {
  if (!impl_) return Status::NoMemory;
  const auto &s = *impl_;
  if (!s.configured || !pixels || !width || !height || x >= s.config.width || y >= s.config.height ||
      width > s.config.width - x || height > s.config.height - y) return Status::InvalidArgument;
  if (!stride) stride = width;
  if (stride < width || size_t(height - 1) > (std::numeric_limits<size_t>::max() - width) / stride ||
      size_t(height - 1) * stride + width > capacityPixels) return Status::InvalidArgument;
  const size_t neededPixels = size_t(height - 1) * stride + width;
  const uintptr_t address = reinterpret_cast<uintptr_t>(pixels);
  if (address % alignof(uint16_t) || capacityPixels > std::numeric_limits<size_t>::max() / sizeof(uint16_t) ||
      neededPixels > size_t(std::numeric_limits<ptrdiff_t>::max()) / sizeof(uint16_t) ||
      neededPixels > (std::numeric_limits<uintptr_t>::max() - address) / sizeof(uint16_t)) return Status::InvalidArgument;
  if (blitter.callback && capacityPixels > (std::numeric_limits<uintptr_t>::max() - address) / sizeof(uint16_t))
    return Status::InvalidArgument;
  if (borrowedKey) {
    if (!s.hasSource || borrowedKey->zoom != s.zoom || borrowedKey->layer >= s.source.layers.size() ||
        !s.grid.contains(*borrowedKey) ||
        !borrowedImage || !borrowedImage->valid() || borrowedImage->width() != s.tileSize() ||
        borrowedImage->height() != s.tileSize() ||
        (s.source.layers[borrowedKey->layer].overlay && !borrowedImage->alpha())) return Status::InvalidArgument;
    const uintptr_t input = reinterpret_cast<uintptr_t>(borrowedImage->pixels());
    if (input > std::numeric_limits<uintptr_t>::max() - borrowedImage->bytes() ||
        (input < address + neededPixels * sizeof(uint16_t) && address < input + borrowedImage->bytes())) return Status::InvalidArgument;
  }
  Raster<Wire> raster{int(x), int(y), int(width), int(height), pixels, stride};
  const auto viewport = s.viewport();
  // An opaque bottom layer covers every in-world pixel, including missing
  // tiles. Avoid a redundant full-buffer write (particularly costly in PSRAM).
  const auto *matrix = s.grid.matrix(s.zoom);
  const bool opaqueBottom = s.hasSource && (!matrix || !matrix->limited) && !s.source.layers[0].overlay && s.source.layers[0].opacity == 255 &&
    viewport.top + y >= 0 && viewport.top + y + height <= int64_t(viewport.worldHeight) &&
    !(borrowedKey && borrowedKey->layer == 0 && borrowedImage->alpha());
  if (!opaqueBottom)
    for (int row = y; row < int(y) + height; ++row) raster.span(row, x, int(x) + width, s.config.background);
  Status blitStatus=Status::Ok;
  s.placements(viewport, [&](const TilePlacement &placement) {
    if(blitStatus!=Status::Ok)return;
    // A borrowed image must override this entire XYZ stack, including any
    // precomposed copy. Geometry overlays remain independent of this cache.
    const auto *composite = borrowedKey && Impl::sameXyz(placement.key, *borrowedKey) ? nullptr : s.findComposite(placement.key);
    if (composite && placement.key.layer) return;
    std::array<Impl::FallbackPiece, 32> previous;
    const size_t previousCount = composite || (borrowedKey && Impl::sameXyz(placement.key, *borrowedKey)) ? 0 :
      s.compositeFallback(viewport, placement, previous);
    if (previousCount && placement.key.layer) return;
    const uint8_t opacity = s.source.layers[placement.key.layer].opacity;
    if (!opacity) return;
    const int firstX = std::max<int>(x, placement.x), firstY = std::max<int>(y, placement.y);
    const int endX = std::min<int>(int(x) + width, placement.x + placement.width);
    const int endY = std::min<int>(int(y) + height, placement.y + placement.height);
    if (firstX >= endX || firstY >= endY) return;
    if (previousCount) {
      for (size_t i = 0; i < previousCount; ++i) {
        const auto &piece = previous[i];
        const int left = std::max(firstX, piece.x), top = std::max(firstY, piece.y);
        const int right = std::min(endX, piece.right), bottom = std::min(endY, piece.bottom);
        if (left < right && top < bottom)
          Impl::sampledTile(raster, *piece.image, 255, piece.left, piece.top, piece.scale, left, top, right, bottom);
      }
      return;
    }
    const auto *entry = s.find(placement.key);
    const TileImage *image = composite ? &composite->image : borrowedKey && placement.key == *borrowedKey ? borrowedImage :
      entry && entry->state == Impl::State::Ready ? &entry->image : nullptr;
    if (!image) {
      s.fallbackTiles(raster, viewport, placement, firstX, firstY, endX, endY, opacity);
      return;
    }
    if (viewport.scale != 1) {
      // Recover this wrapped placement's exact unsnapped tile origin. Pixel
      // centers choose source texels; rounded adjacent bounds stay contiguous.
      const double nominal = placement.key.x * viewport.displayTileSize - double(viewport.left);
      const double left = nominal + std::round((placement.x - nominal) / viewport.world) * viewport.world;
      const double top = placement.key.y * viewport.displayTileSize - double(viewport.top);
      Impl::sampledTile(raster, *image, opacity, left, top, viewport.scale, firstX, firstY, endX, endY);
      return;
    }
    const uint16_t *input = image->pixels();
    const uint8_t *alpha = image->alpha();
    if constexpr (!Wire) if(blitter.callback&&!alpha&&opacity==255){
      RasterBlitRequest request;
      request.source=input;request.sourceCapacityPixels=image->bytes()/sizeof(uint16_t);
      request.sourceWidth=request.sourceStride=image->width();request.sourceHeight=image->height();
      request.sourceX=size_t(firstX-placement.x);request.sourceY=size_t(firstY-placement.y);
      request.destination=pixels;request.destinationCapacityPixels=capacityPixels;
      request.destinationWidth=width;request.destinationHeight=height;request.destinationStride=stride;
      request.destinationX=size_t(firstX-x);request.destinationY=size_t(firstY-y);
      request.width=size_t(endX-firstX);request.height=size_t(endY-firstY);
      const Status copied=blitter.callback(request,blitter.context);
      if(copied==Status::Ok)return;
      if(copied!=Status::Unsupported){blitStatus=copied;return;}
    }
    for (int row = firstY; row < endY; ++row) {
      const size_t sourceOffset = size_t(row - placement.y) * viewport.tileSize + size_t(firstX - placement.x);
      uint16_t *output = pixels + size_t(row - y) * stride + size_t(firstX - x);
      if (!alpha && opacity == 255) {
        Raster<Wire>::copyOpaque(output, input + sourceOffset, size_t(endX - firstX));
        continue;
      }
      for (int column = firstX; column < endX; ++column) {
        const size_t index = sourceOffset + size_t(column - firstX);
        const uint8_t effective = alpha ? uint8_t((unsigned(alpha[index]) * opacity + 127) / 255) : opacity;
        if (effective == 255) *output = Raster<Wire>::encoded(input[index]);
        else if (effective) *output = Raster<Wire>::blended(input[index], *output, effective);
        ++output;
      }
    }
  });
  if(blitStatus!=Status::Ok)return blitStatus;
  const auto transform = s.renderTransform(viewport);
  for (const auto &polygon : s.polygons) s.drawPolygon(raster, polygon, transform);
  for (const auto &circle : s.circles) s.drawCircle(raster, circle, transform);
  for (size_t i = 1; i < s.route.size(); ++i) s.drawSegment(raster, s.route[i - 1], s.route[i], s.routeColor, s.routeWidth, transform);
  for (size_t i = 1; i < s.track.size(); ++i)
    s.drawSegment(raster, s.track[(s.trackHead + i - 1) % s.track.size()],
                  s.track[(s.trackHead + i) % s.track.size()], 0x07e0, 2, transform);
  for (const auto &marker : s.markers) s.drawMarker(raster, marker, transform);
  if (s.hasLocation) {
    const auto p = transform.screen(s.projected(s.location, 0, 256));
    const double angle = s.heading * kPi / 180;
    const double world = s.worldSize(), radius = 16;
    const int first = int(std::ceil((x - radius - p.x) / world));
    const int last = int(std::floor((int(x) + width - 1 + radius - p.x) / world));
    for (int copy = first; copy <= last; ++copy) {
      const double cx = p.x + copy * world;
      raster.line(cx, p.y, cx + std::sin(angle) * 14, p.y - std::cos(angle) * 14, 0x001f, 3);
      raster.disk(cx, p.y, 7, 0xffff); raster.disk(cx, p.y, 5, 0x001f);
    }
  }
  return Status::Ok;
}
} // namespace nice_gfx_map

#undef NICE_MAP_TRY
#undef NICE_MAP_CATCH_RETURN
