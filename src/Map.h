#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nice_gfx_map {
enum class Projection : uint8_t;
struct TileGrid;

enum class Status : uint8_t {
  Ok, InvalidArgument, NoMemory, NotFound, Busy, NetworkError,
  DecodeError, Unsupported, TooLarge, Canceled, IoError
};
const char *statusName(Status status);

struct GeoPoint { double latitude = 0, longitude = 0; };
struct PixelPoint { double x = 0, y = 0; };
// west > east crosses the antimeridian. Use [-180,180] for a full world.
struct GeoBounds { double south = 0, west = 0, north = 0, east = 0; };
constexpr double MercatorLatitudeLimit = 85.0511287798066;
bool validCoordinate(GeoPoint point);
double normalizeLongitude(double longitude);
PixelPoint project(GeoPoint point, uint8_t zoom, uint16_t tileSize = 256);
GeoPoint unproject(PixelPoint point, uint8_t zoom, uint16_t tileSize = 256);
PixelPoint project(GeoPoint point, uint8_t zoom, uint16_t tileSize, Projection projection);
GeoPoint unproject(PixelPoint point, uint8_t zoom, uint16_t tileSize, Projection projection);

struct TileKey {
  uint8_t zoom = 0;
  uint32_t x = 0, y = 0;
  uint8_t layer = 0;
  bool operator==(const TileKey &other) const {
    return zoom == other.zoom && x == other.x && y == other.y && layer == other.layer;
  }
};
struct SourceLayer {
  std::string urlTemplate; // HTTPS URL with {z}, {x}, {y}; key already escaped.
  bool overlay = false;  // Decoder must preserve alpha for an overlay.
  uint8_t opacity = 255; // Applied at render time; changing it keeps decoded tiles.
};
struct TileSource {
  std::string name, attribution;
  uint16_t tileSize = 256;
  uint8_t minZoom = 0, maxZoom = 18;
  std::vector<SourceLayer> layers;
  bool valid() const;
  std::string url(const TileKey &key) const;
  // Explicit WMTS matrix identifiers, non-square row counts and regional limits.
  std::string url(const TileKey &key, const TileGrid &grid) const;
};

// Allocation and release must be supplied together. Context must outlive images.
// Use esp32PsramAllocator() for large decoded images on ESP32.
struct Allocator {
  void *(*allocate)(size_t bytes, void *context) = nullptr;
  void (*release)(void *pointer, void *context) = nullptr;
  void *context = nullptr;
};

// Optional native RGB565 rectangle copy. Both pointers are the original full
// buffer bases; capacities and strides are measured in uint16_t pixels. Output
// dimensions describe this renderRegion's buffer, not the entire map viewport.
// A minimum-capacity strided buffer may omit its final row's padding: a DMA
// backend must reject it rather than assume stride*height pixels are available.
struct RasterBlitRequest {
  const uint16_t *source = nullptr;
  size_t sourceCapacityPixels = 0, sourceWidth = 0, sourceHeight = 0, sourceStride = 0;
  size_t sourceX = 0, sourceY = 0;
  uint16_t *destination = nullptr;
  size_t destinationCapacityPixels = 0, destinationWidth = 0, destinationHeight = 0, destinationStride = 0;
  size_t destinationX = 0, destinationY = 0, width = 0, height = 0;
};
struct RasterBlitter {
  // All pointers are borrowed only for this synchronous call. Ok means the
  // exact rectangle is complete and CPU-readable. Unsupported MUST leave it
  // untouched for CPU fallback. Every other error terminates rendering; output
  // can be partial and the caller must discard it. Never retain source pixels.
  Status (*callback)(const RasterBlitRequest &, void *context) = nullptr;
  void *context = nullptr;
};

// Move-only owned pixels. RGB565 words use native byte order, alpha is 0..255.
// A submitted image belongs to Map; do not keep writing through borrowed pointers.
class TileImage {
 public:
  TileImage() = default;
  ~TileImage();
  TileImage(TileImage &&other) noexcept;
  TileImage &operator=(TileImage &&other) noexcept;
  TileImage(const TileImage &) = delete;
  TileImage &operator=(const TileImage &) = delete;
  Status allocate(uint16_t width, uint16_t height, bool withAlpha = false,
                  Allocator allocator = {});
  void reset();
  bool valid() const;
  uint16_t width() const { return width_; }
  uint16_t height() const { return height_; }
  size_t bytes() const { return bytes_; }
  Allocator allocator() const { return allocator_; }
  uint16_t *pixels() { return static_cast<uint16_t *>(storage_); }
  const uint16_t *pixels() const { return static_cast<const uint16_t *>(storage_); }
  uint8_t *alpha();
  const uint8_t *alpha() const;
 private:
  void *storage_ = nullptr;
  uint16_t width_ = 0, height_ = 0;
  size_t bytes_ = 0;
  bool alpha_ = false;
  Allocator allocator_;
};

struct MapConfig {
  uint16_t width = 320, height = 240;
  size_t cacheBytes = 4 * 1024 * 1024;
  size_t maxCachedTiles = 64, maxInFlight = 2;
  size_t maxMarkers = 32, maxRoutePoints = 4096, maxTrackPoints = 2048;
  uint32_t retryDelayMs = 2000;
  uint16_t background = 0xe71c, missingTileColor = 0xd69a;
  size_t maxCircles = 16, maxPolygons = 16, maxPolygonPoints = 2048;
  // Optional extra budget for precomposed opaque tiles. RGB565 derived images
  // reuse the bottom tile's allocator; raw layers remain for opacity changes.
  // Zero disables this cache. Total image budget is cacheBytes + this value.
  size_t compositeCacheBytes = 0;
};
struct TilePlacement {
  TileKey key; int32_t x = 0, y = 0;
  // Display dimensions; equal tileSize at integer view zoom. Adjacent rounded
  // boundaries remain contiguous while a fractional view scales raster tiles.
  uint16_t width = 0, height = 0;
};
struct TileRequest {
  TileKey key;
  uint64_t id = 0;
  uint32_t generation = 0;
  uint16_t tileSize = 256;
  bool overlay = false;
  std::string url;
};
struct CacheStats {
  size_t readyTiles = 0, inFlight = 0, bytes = 0, budget = 0;
  uint64_t hits = 0, misses = 0, evictions = 0, failures = 0;
  size_t compositeTiles = 0, compositeBytes = 0, compositeBudget = 0;
  uint64_t compositeBuilds = 0;
};
enum class MarkerShape : uint8_t { Circle, Pin };
struct Marker {
  GeoPoint position;
  uint16_t color = 0xf800;
  uint8_t radius = 5;
  uint32_t id = 0; // Application identity; markerAt returns the array index.
  // Pin: geographic position is the bottom tip; head radius is radius pixels,
  // width 2r, total height 3r, transparent center. Hit testing includes its hole.
  MarkerShape shape = MarkerShape::Circle;
};
struct CircleOverlay {
  GeoPoint center;
  double radiusMeters = 100;
  uint16_t fillColor = 0x07e0, strokeColor = 0x03e0;
  uint8_t fillOpacity = 48, strokeWidth = 2;
};
struct PolygonOverlay {
  // One even-odd ring, shortest edges across the date line; no holes. Rings
  // winding around an entire world/pole cannot be planar and are Unsupported.
  std::vector<GeoPoint> points;
  uint16_t fillColor = 0x07e0, strokeColor = 0x03e0;
  uint8_t fillOpacity = 48, strokeWidth = 2;
};

// Single-owner task API. Network workers may own independent requests/images;
// marshal submit/fail and all Map/render calls back to the owner task.
class Map {
 public:
  explicit Map(const MapConfig &config = {});
  ~Map();
  Map(const Map &) = delete;
  Map &operator=(const Map &) = delete;
  Status setSource(const TileSource &source); // Clears cache, cancels old requests.
  Status setSource(const TileSource &source, const TileGrid &grid);
  const TileSource &source() const;
  const TileGrid &sourceGrid() const;
  // Replace/refresh one existing layer. Other layers and their requests survive;
  // the replaced layer's decoded tiles and pending request tokens are canceled.
  Status setLayer(uint8_t layer, const SourceLayer &replacement);
  Status setLayerOpacity(uint8_t layer, uint8_t opacity); // Retains cache and requests.
  Status resize(uint16_t width, uint16_t height);
  uint16_t width() const;
  uint16_t height() const;
  Status setCenter(GeoPoint center);
  GeoPoint center() const;
  Status setZoom(uint8_t zoom);
  uint8_t zoom() const;
  double viewZoom() const;
  // Continuous absolute zoom. The geographic point under this screen anchor
  // stays fixed; captured gestures may use anchors outside the viewport.
  // zoom() remains the integer tile-request level floor(viewZoom()).
  Status setViewZoomAt(double zoom, double screenX, double screenY);
  // Content follows (+dx,+dy); center moves in the opposite world direction.
  Status panBy(double dx, double dy);
  Status zoomAt(uint8_t zoom, double screenX, double screenY);
  // Maximum fitting integer zoom within the current source range. A bounds
  // wider than the viewport at minZoom returns TooLarge and changes nothing.
  Status fitBounds(GeoBounds bounds, uint16_t padding = 16);
  // 1..65536 points; chooses the minimum longitude arc across the date line.
  Status fitPoints(const GeoPoint *points, size_t count, uint16_t padding = 16);
  PixelPoint toScreen(GeoPoint point) const;
  GeoPoint fromScreen(PixelPoint point) const;
  std::vector<TilePlacement> visibleTiles() const;

  // Pull requests in visible-center priority. NotFound: no eligible work;
  // Busy: worker slots full; NoMemory: visible tiles exceed cache capacity.
  Status nextRequest(TileRequest &request, uint32_t nowMs = 0);
  Status submit(const TileRequest &request, TileImage &&image);
  Status fail(const TileRequest &request, Status reason, uint32_t nowMs = 0);
  void clearCache();
  // Owner task, outside render/blitter callbacks. Release at most one cached
  // owner: an off-screen derived composite first, then an off-screen ready raw
  // tile (needed fallback last). Current visible raw tiles and pending requests
  // are retained. Return released pixel bytes, including derived dependencies.
  // A successful reclaim increments revision/evictions; budgets and map state
  // stay intact. The caller may retry a failed allocation immediately.
  size_t reclaimCacheEntry();
  // Move fitting ready tiles from an inactive map with identical tile URLs,
  // tile dimensions and alpha layout; opacity/view/overlays may differ. Prefer
  // destination-visible tiles, preserve its existing entries/requests, then
  // clear the donor and cancel all its request tokens. No pixel copies. Call
  // from the shared owner task after stopping requests for the inactive map.
  Status takeCacheFrom(Map &donor);
  CacheStats cacheStats() const;
  uint64_t revision() const;

  Status setMarkers(const Marker *markers, size_t count);
  // Last marker is painted on top and wins hits. Invalid input returns -1.
  int32_t markerAt(PixelPoint point, double tolerance = 0) const;
  // Circles use the projection's local meter/pixel scale, not geodesic distance;
  // radius must be finite, positive and <=20037508.342789244 meters.
  // Zero strokeWidth disables the outline. Empty arrays clear overlays.
  Status setCircles(const CircleOverlay *circles, size_t count);
  Status setPolygons(const PolygonOverlay *polygons, size_t count);
  Status setRoute(const GeoPoint *points, size_t count,
                  uint16_t color = 0x001f, uint8_t width = 3);
  Status appendTrack(GeoPoint point);
  void clearTrack();
  Status setLocation(GeoPoint point, double headingDegrees = 0);
  void clearLocation();
  void setFollow(bool enabled);
  bool following() const;

  // Pure rendering: no I/O, no mutation, tightly bounded by the supplied region.
  // Output is native RGB565. A full viewport buffer is optional: render strips.
  Status renderRegion(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      uint16_t *pixels, size_t capacityPixels, size_t stride = 0) const;
  // Optional acceleration for ready, opaque 1:1 native tile rectangles only.
  // Fractional zoom, fallback sampling, alpha and geometry retain CPU raster
  // semantics. Callback/default Unsupported fallback preserves identical pixels.
  Status renderRegionWithBlitter(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      uint16_t *pixels, size_t capacityPixels, size_t stride = 0,
                      RasterBlitter blitter = {}) const;
  // Same pixels/region rules, encoded high byte first for display/UI storage.
  // Fuses byte order into raster writes; decoded tile caches remain native.
  Status renderRegionWire(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      uint16_t *pixels, size_t capacityPixels, size_t stride = 0) const;
  // Streaming renderer for boards without a viewport-sized tile cache. The
  // image is borrowed only during this call; it overrides this key without
  // changing the cache/revision. Clip output to the tile's visible placement
  // and retain previously drawn display pixels while loading the next tile.
  // Geometry overlays are rendered normally. Input/output must not overlap.
  Status renderRegionWithTile(const TileKey &key, const TileImage &image,
                      uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      uint16_t *pixels, size_t capacityPixels, size_t stride = 0) const;
 private:
  template<bool Wire>
  Status renderRegionImpl(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                      uint16_t *pixels, size_t capacityPixels, size_t stride,
                      const TileKey *borrowedKey, const TileImage *borrowedImage, RasterBlitter blitter) const;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nice_gfx_map
