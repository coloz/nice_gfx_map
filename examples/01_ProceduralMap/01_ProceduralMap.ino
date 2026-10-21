/*
 * ESP32 + PSRAM + 320x240 ILI9341. No Wi-Fi, service account or API key.
 * CS=5, DC=2, RST=4, SCK=18, MOSI=23, MISO=19: change for your board.
 * Serial commands: w/a/s/d pan, +/- zoom, f follow the mock GNSS position.
 * The generated streets are a test pattern, not real geographic map data.
 */
#include <Arduino.h>
#include <cmath>
#include <utility>
#include <NICE_GFX.h>
#include <NICE_GFX_MAP.h>
#include <adapters/Esp32.h>
#include <adapters/NiceGfx.h>

using namespace nice_gfx_map;

ILI9341_240x320_SPI display(5, 2, 4, 18, 23, 19);

MapConfig makeConfig() {
  MapConfig config;
  config.width = 320;
  config.height = 240;
  config.cacheBytes = 1024 * 1024;
  config.maxCachedTiles = 8;
  config.maxInFlight = 1;
  config.maxTrackPoints = 256;
  return config;
}
Map mapView(makeConfig());

// Preserve the same fetch -> decode -> submit path as an online source.
// This transport returns no compressed image; the decoder generates one.
class ProceduralTransport final : public Transport {
 public:
  Status fetch(const TileRequest &, ByteBuffer &output) override {
    output.clear();
    return Status::Ok;
  }
};
class ProceduralDecoder final : public Decoder {
 public:
  Status decode(const TileRequest &request, const uint8_t *, size_t,
                TileImage &output) override {
    const Status status = output.allocate(request.tileSize, request.tileSize,
                                         false, esp32PsramAllocator());
    if (status != Status::Ok) return status;
    const unsigned size = request.tileSize;
    for (unsigned y = 0; y < size; ++y) {
      for (unsigned x = 0; x < size; ++x) {
        const bool street = (x % 64 < 8) || (y % 64 < 8);
        const bool park = ((x / 64 + y / 64 + request.key.x + request.key.y) % 5 == 0);
        const bool tileEdge = x == 0 || y == 0;
        output.pixels()[y * size + x] = tileEdge ? 0x8410
          : street ? 0xffff : park ? 0xbef7 : 0xe71c;
      }
    }
    return Status::Ok;
  }
};
ProceduralTransport transport;
ProceduralDecoder decoder;
TileLoader loader(transport, decoder);

uint16_t scratch[320 * 8]; // 5 KiB; no full-screen framebuffer.
bool ready = false;
uint64_t paintedRevision = 0;
uint32_t nextFix = 0;
unsigned fixIndex = 0;
const GeoPoint origin{30.6586, 104.0648};

void setup() {
  Serial.begin(115200);
  if (display.beginChecked() != nice_gfx::Status::Ok) {
    Serial.println("Display initialization failed"); return;
  }
  display.setRotation(1);
  const TileSource source = providers::xyz(
    "https://example.invalid/procedural/{z}/{x}/{y}.png",
    "Procedural demo: no geographic map data", 0, 18, 256);
  // example.invalid is never requested: ProceduralTransport performs no I/O.
  if (mapView.setSource(source) != Status::Ok ||
      mapView.setCenter(origin) != Status::Ok || mapView.setZoom(15) != Status::Ok) return;
  const GeoPoint route[] = {
    {30.6568, 104.0618}, {30.6578, 104.0634},
    {30.6586, 104.0648}, {30.6603, 104.0666}
  };
  mapView.setRoute(route, sizeof(route) / sizeof(route[0]), 0x001f, 4);
  const Marker marker{{30.6603, 104.0666}, 0xf800, 6};
  mapView.setMarkers(&marker, 1);
  mapView.setLocation(origin, 45);
  mapView.setFollow(true);
  ready = true;
  Serial.println("No network or key. w/a/s/d pan; +/- zoom; f follow mock GNSS.");
}

void loop() {
  if (!ready) { delay(10); return; }
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'w': mapView.setFollow(false); mapView.panBy(0, 32); break;
      case 's': mapView.setFollow(false); mapView.panBy(0, -32); break;
      case 'a': mapView.setFollow(false); mapView.panBy(32, 0); break;
      case 'd': mapView.setFollow(false); mapView.panBy(-32, 0); break;
      case '+': if (mapView.zoom() < mapView.source().maxZoom) mapView.setZoom(mapView.zoom() + 1); break;
      case '-': if (mapView.zoom() > mapView.source().minZoom) mapView.setZoom(mapView.zoom() - 1); break;
      case 'f': mapView.setFollow(true); break;
    }
  }
  const uint32_t now = millis();
  if (int32_t(now - nextFix) >= 0) {
    const double phase = double(fixIndex++ % 360) * 0.017453292519943295;
    const GeoPoint fix{origin.latitude + std::sin(phase) * 0.0012,
                       origin.longitude + std::cos(phase) * 0.0015};
    mapView.setLocation(fix, double(fixIndex % 360));
    mapView.appendTrack(fix);
    nextFix = now + 1000;
  }
  const Status loaded = loader.loadNext(mapView, now);
  if (loaded != Status::Ok && loaded != Status::NotFound && loaded != Status::Busy)
    Serial.printf("Tile: %s (enable PSRAM / check free memory)\n", statusName(loaded));
  if (mapView.revision() != paintedRevision) {
    const Status drawn = NiceGfxRenderer::draw(mapView, display, 0, 0,
                                             scratch, sizeof(scratch) / sizeof(scratch[0]));
    if (drawn == Status::Ok) paintedRevision = mapView.revision();
    else Serial.printf("Draw: %s\n", statusName(drawn));
  }
  delay(5);
}
