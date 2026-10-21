/*
 * INTEGRATION TEMPLATE, not a self-contained board example.
 * From your NICE_GFX Designer export copy Hardware.h, TouchAdapter.h and ui.h
 * beside this sketch. Its initial page must contain a 320x200 dynamic media:
 * <gfx-media id="map" data-media-dynamic="true"
 *            style="width:320px;height:200px"></gfx-media>
 * Replace the exported main sketch with this template; keep your hardware files.
 * This procedural background needs no account/key/PNG decoder.
 */
#include <Arduino.h>
#include <utility>
#include <NICE_GFX_MAP.h>
#include <adapters/Esp32.h>

#if __has_include("Hardware.h") && __has_include("TouchAdapter.h") && __has_include("ui.h")
#include "Hardware.h"
#include "TouchAdapter.h"
#include "ui.h"
#else
#error "Integration template: supply your Designer-exported Hardware.h, TouchAdapter.h and ui.h; read examples/03_UiMap and README."
#endif
#include <adapters/UiImage.h>

using namespace nice_gfx_map;
nice_gfx::htmlui::View ui(gfx);

MapConfig makeMapConfig() {
  MapConfig config;
  config.width = 320;
  config.height = 200;
  config.cacheBytes = 1024 * 1024;
  config.maxCachedTiles = 8;
  config.maxInFlight = 1;
  return config;
}
Map mapView(makeMapConfig());
bool ready = false;
bool pageChanged = true;
uint64_t publishedRevision = 0;
uint32_t lastPublish = 0;

void onRouteChanged(void *, nice_gfx::htmlui::Event &) { pageChanged = true; }

// Fill one visible tile per loop without doing I/O. Replace this function with
// your worker-result queue for real tiles; submit/fail stay on this UI task.
void loadProceduralTile() {
  TileRequest request;
  if (mapView.nextRequest(request, millis()) != Status::Ok) return;
  TileImage image;
  const Status allocated = image.allocate(request.tileSize, request.tileSize,
                                         false, esp32PsramAllocator());
  if (allocated != Status::Ok) { mapView.fail(request, allocated, millis()); return; }
  for (unsigned y = 0; y < request.tileSize; ++y)
    for (unsigned x = 0; x < request.tileSize; ++x)
      image.pixels()[y * request.tileSize + x] =
        (x % 64 < 8 || y % 64 < 8) ? 0xffff : 0xdedb;
  mapView.submit(request, std::move(image));
}

void setup() {
  Serial.begin(115200);
  if (!beginDisplayHardware()) return;
  if (ui_config::load(ui) != nice_gfx::Status::Ok) return;
  ui.on(0, nice_gfx::htmlui::EventType::RouteChange, onRouteChanged);
  const auto source = providers::xyz("https://example.invalid/{z}/{x}/{y}.png",
                                    "Procedural demo: no geographic map data");
  if (mapView.setSource(source) != Status::Ok ||
      mapView.setCenter({30.6586, 104.0648}) != Status::Ok || mapView.setZoom(14) != Status::Ok) return;
  mapView.setLocation({30.6586, 104.0648}, 45);
  ready = true;
}

void loop() {
  if (!ready) { delay(10); return; }
  const uint32_t now = millis();
  if (!pollUi(ui, now)) { ready = false; return; }
  loadProceduralTile();
  // Publish at most 5 images/s: UI adoption still needs a complete map surface.
  // Avoid repeatedly allocating/publishing when map content has not changed.
  if ((pageChanged || mapView.revision() != publishedRevision) && now - lastPublish >= 200) {
    lastPublish = now;
    const auto target = ui.bounds(ui_config::id("map"));
    if (target.width <= 0 || target.height <= 0) { delay(1); return; }
    const Status status = publishToUi(mapView, ui, ui_config::id("map"), esp32PsramAllocator());
    if (status == Status::Ok) {
      publishedRevision = mapView.revision();
      pageChanged = false;
    } else {
      Serial.printf("Publish: %s\n", statusName(status));
    }
    // map is absent on another route: keep application-owned Map and retry when
    // RouteChange re-enters the map page; do not tear down the entire UI.
  }
  delay(1);
}
