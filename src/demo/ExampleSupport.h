#pragma once
// Explicitly included Arduino demo support; not part of the portable map core.
#include <Arduino.h>
#include <NICE_GFX.h>
#include "../Map.h"
#include "../Providers.h"
#include "../Pipeline.h"
#include "../adapters/Esp32.h"
#include "../adapters/NiceGfx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef NICE_MAP_DEMO_CS
#define NICE_MAP_DEMO_CS 5
#endif
#ifndef NICE_MAP_DEMO_DC
#define NICE_MAP_DEMO_DC 2
#endif
#ifndef NICE_MAP_DEMO_RST
#define NICE_MAP_DEMO_RST 4
#endif
#ifndef NICE_MAP_DEMO_SCK
#define NICE_MAP_DEMO_SCK 18
#endif
#ifndef NICE_MAP_DEMO_MOSI
#define NICE_MAP_DEMO_MOSI 23
#endif
#ifndef NICE_MAP_DEMO_MISO
#define NICE_MAP_DEMO_MISO 19
#endif

namespace nice_gfx_map { namespace demo {

inline MapConfig configuration() {
  MapConfig config;
  config.width = 320; config.height = 200;
  config.cacheBytes = 1536 * 1024;
  config.maxCachedTiles = 40; config.maxInFlight = 1;
  config.maxTrackPoints = 1024;
  return config;
}
inline GeoPoint home() { return {30.6586, 104.0648}; }

class ProceduralTransport final : public Transport {
 public:
  Status fetch(const TileRequest &, ByteBuffer &bytes) override {
    bytes.clear(); return Status::Ok; // Deliberately no HTTP or file I/O.
  }
};
class ProceduralDecoder final : public Decoder {
 public:
  unsigned phase = 0;
  Status decode(const TileRequest &request, const uint8_t *, size_t, TileImage &output) override {
    const Status status = output.allocate(request.tileSize, request.tileSize,
                                         request.overlay, esp32PsramAllocator());
    if (status != Status::Ok) return status;
    const uint32_t size = request.tileSize;
    for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
      // World-pixel coordinates keep the pattern aligned across tile boundaries.
      const uint32_t wx = request.key.x * size + x, wy = request.key.y * size + y;
      const size_t index = size_t(y) * size + x;
      if (request.overlay) {
        const double dx = double((wx + phase * 23u) % 384u) - 192;
        const double dy = double(wy % 384u) - 192;
        const double radius = std::sqrt(dx * dx + dy * dy);
        output.pixels()[index] = radius < 65 ? 0xf800 : radius < 105 ? 0xffe0 : 0x07ff;
        output.alpha()[index] = radius < 145 ? uint8_t(std::max(0.0, 180 - radius)) : 0;
      } else {
        const bool street = wx % 96 < 5 || wy % 96 < 5;
        const bool park = (wx / 96 + wy / 96) % 7 == 0;
        output.pixels()[index] = street ? 0xffff : park ? 0xbef7 : 0xe71c;
      }
    }
    return Status::Ok;
  }
};

class Example {
 public:
  ILI9341_240x320_SPI display;
  Map mapView;
  ProceduralTransport transport;
  ProceduralDecoder decoder;
  TileLoader loader;
  bool automaticTiles = true;
  Example()
      : display(NICE_MAP_DEMO_CS, NICE_MAP_DEMO_DC, NICE_MAP_DEMO_RST,
                NICE_MAP_DEMO_SCK, NICE_MAP_DEMO_MOSI, NICE_MAP_DEMO_MISO),
        mapView(configuration()), loader(transport, decoder) {}

  bool begin(const char *title, GeoPoint center = home(), uint8_t zoom = 16) {
    Serial.begin(115200);
    title_ = title ? title : "NICE_GFX_MAP";
    if (display.beginChecked() != nice_gfx::Status::Ok) {
      Serial.println("Display initialization failed"); return false;
    }
    display.setRotation(1);
    display.setFont(nice_gfx::fonts::ProFont6x12);
    display.setTextWrap(false);
    display.fillScreen(0xffff);
    Status status = mapView.setSource(proceduralSource(false));
    if (status == Status::Ok) status = mapView.setCenter(center);
    if (status == Status::Ok) status = mapView.setZoom(zoom);
    ready_ = status == Status::Ok;
    Serial.println("Procedural geography is SYNTHETIC, not real roads.");
    Serial.println("Common commands: w/a/s/d pan, +/- zoom, f follow.");
    if (!ready_) Serial.println(statusName(status));
    return ready_;
  }
  TileSource proceduralSource(bool weather) const {
    auto source = providers::xyz("https://example.invalid/demo/{z}/{x}/{y}.png",
                                 "Synthetic test geography - no real map data", 0, 18, 128);
    if (weather) source.layers.push_back({
      "https://example.invalid/weather/{z}/{x}/{y}.png", true, 180});
    return source;
  }
  Status useProceduralOverlay(bool enabled) {
    return mapView.setSource(proceduralSource(enabled));
  }
  Status loadOne() {
    const Status status = loader.loadNext(mapView, millis());
    if (status != Status::Ok && status != Status::NotFound && status != Status::Busy)
      lastTileStatus_ = status;
    else if (status == Status::Ok) lastTileStatus_ = Status::Ok;
    return status;
  }
  // Return true when a common map command was consumed.
  bool handleKey(int key) {
    switch (key) {
      case 'w': mapView.panBy(0, 28); return true;
      case 's': mapView.panBy(0, -28); return true;
      case 'a': mapView.panBy(28, 0); return true;
      case 'd': mapView.panBy(-28, 0); return true;
      case '+': if (mapView.zoom() < mapView.source().maxZoom) mapView.setZoom(mapView.zoom() + 1); return true;
      case '-': if (mapView.zoom() > mapView.source().minZoom) mapView.setZoom(mapView.zoom() - 1); return true;
      case 'f': mapView.setFollow(true); return true;
      default: return false;
    }
  }
  void tick(const char *status) {
    if (!ready_) { delay(10); return; }
    if (automaticTiles) loadOne();
    const uint32_t now = millis();
    if (now - lastPaint_ < 100) return; // At most 10 complete demo frames/s.
    const std::string message = status ? status : "";
    if (mapView.revision() == paintedRevision_ && message == message_ &&
        lastTileStatus_ == paintedTileStatus_) return;
    const Status drawn = NiceGfxRenderer::draw(mapView, display, 0, 20,
                                              scratch_, sizeof(scratch_) / sizeof(scratch_[0]));
    if (drawn != Status::Ok) { Serial.println(statusName(drawn)); lastPaint_ = now; return; }
    display.fillRect(0, 0, 320, 20, 0x18e3);
    display.setTextColor(0xffff, 0x18e3);
    display.drawString(title_.c_str(), 4, 4);
    display.fillRect(0, 220, 320, 20, 0xffff);
    display.setTextColor(lastTileStatus_ == Status::Ok ? 0x0000 : 0xf800, 0xffff);
    char text[54];
    std::snprintf(text, sizeof(text), "%.52s", message.c_str());
    display.drawString(text, 4, 224);
    message_ = message; paintedRevision_ = mapView.revision();
    paintedTileStatus_ = lastTileStatus_; lastPaint_ = now;
  }
 private:
  bool ready_ = false;
  std::string title_, message_;
  uint16_t scratch_[320 * 8] = {};
  uint64_t paintedRevision_ = 0;
  uint32_t lastPaint_ = 0;
  Status lastTileStatus_ = Status::Ok, paintedTileStatus_ = Status::Ok;
};

} } // namespace nice_gfx_map::demo
