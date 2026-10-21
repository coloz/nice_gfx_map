/*
 * Wio Terminal / real Tianditu map, without an SD card or PSRAM.
 * Install NICE_GFX, NICE_GFX_MAP, rpcWiFi, PNGdec and JPEGDEC; see README.
 * Native 256px service tiles are loaded one at a time and drawn by visible row.
 * No complete decoded tile, map framebuffer or tile cache is allocated.
 */
#include <NICE_GFX.h>
#include <NICE_GFX_MAP.h>
#include <rpcWiFi.h>
#include <erpc/erpc_port.h>
#include <erpc/rpc_system.h>
#include <algorithm>
#include <cstdio>
#include <unistd.h>
#include <vector>
#include "WioHttps.h"
#include "WioNativeTileRenderer.h"
#include "TiandituRootCa.h"
#include "WioWirelessTls.h"
#include "WioTlsVerify.h"
#if __has_include("WioSecrets.local.h")
#include "WioSecrets.local.h"
#else
#include "WioSecrets.example.h"
#endif

#if !defined(SEEED_WIO_TERMINAL)
#error "Select Seeeduino:samd:seeed_wio_terminal; this example uses onboard hardware"
#endif

using namespace nice_gfx_map;
extern "C" char __end__;
namespace {
constexpr uint16_t MapWidth = 320, MapHeight = 192, MapTop = 24;
constexpr uint16_t TileSize = 256;
constexpr size_t MaximumEncodedBytes = 48 * 1024;
constexpr size_t MaximumLabelEncodedBytes = 24 * 1024;
constexpr uint8_t ButtonPins[] = {31, 34, 32, 33, 35, 28, 29, 30};
// UP, DOWN, LEFT, RIGHT, five-way PRESS, A/right, B/middle, C/left.
WioTerminalDisplay display;
MapConfig makeConfig() {
  MapConfig config;
  config.width = MapWidth; config.height = MapHeight;
  config.cacheBytes = 0; config.maxCachedTiles = 1; config.maxInFlight = 1;
  config.maxMarkers = 0; config.maxRoutePoints = 0; config.maxTrackPoints = 0;
  config.maxCircles = 0; config.maxPolygons = 0; config.maxPolygonPoints = 0;
  config.background = 0xdedb; config.missingTileColor = 0xdedb;
  return config;
}
Map mapView(makeConfig());
TileSource service;
std::vector<TilePlacement> placements;
ByteBuffer encoded; // One bounded buffer reused for every base and label response.
size_t nextTile = 0, failedTiles = 0, loadedTiles = 0;
uint32_t viewId = 0, viewStarted = 0;
bool viewReported = false;
bool ready = false, labels = wio_map_config::Labels, viewDirty = true;
bool fatalServiceError = false;
uint32_t lastWifiAttempt = 0, nextRetry = 0;
uint16_t scratch[512]; // One tile row, or one screen capture row expanded to RGB888.
char stateText[64] = "Starting";

void hud(const char *state) {
  if (std::strcmp(state, stateText) != 0) {
    Serial.print("WIO_MAP_HUD "); Serial.println(state);
  }
  if (state != stateText) std::snprintf(stateText, sizeof(stateText), "%s", state);
  display.fillRect(0, 0, 320, 24, 0x1106);
  display.setTextColor(0xffff, 0x1106);
  display.setTextSize(1);
  char line[56];
  std::snprintf(line, sizeof(line), "Tianditu / Wio / no SD    z%u %s", unsigned(mapView.zoom()), labels ? "+labels" : "base");
  display.drawString(line, 4, 5);
  display.fillRect(0, 216, 320, 24, 0x1106);
  display.drawString("Map data: Tianditu", 4, 216);
  display.drawString(stateText, 4, 228);
}

void reportView() {
  const GeoPoint center = mapView.center();
  Serial.print("WIO_MAP_VIEW id="); Serial.print(viewId);
  Serial.print(" lat="); Serial.print(center.latitude, 7);
  Serial.print(" lon="); Serial.print(center.longitude, 7);
  Serial.print(" zoom="); Serial.print(mapView.zoom());
  Serial.print(" labels="); Serial.print(labels ? 1 : 0);
  Serial.print(" wifi="); Serial.print(int(WiFi.status()));
  Serial.print(" loaded="); Serial.print(loadedTiles);
  Serial.print(" failed="); Serial.print(failedTiles);
  Serial.print(" attempted="); Serial.print(nextTile);
  Serial.print(" total="); Serial.print(placements.size());
  Serial.print(" fatal="); Serial.print(fatalServiceError ? 1 : 0);
  // These measure two different allocators. The newlib break's growth is
  // allocated address space, NOT available heap or a stack-gap estimate.
  const uintptr_t heapBreak = reinterpret_cast<uintptr_t>(sbrk(0));
  Serial.print(" newlib_break=0x"); Serial.print(heapBreak, HEX);
  Serial.print(" newlib_growth=");
  Serial.print(heapBreak - reinterpret_cast<uintptr_t>(&__end__));
  Serial.print(" rtos_free="); Serial.print(xPortGetFreeHeapSize());
  Serial.print(" task_stack_min_free=");
  Serial.print(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
  Serial.print(" elapsed_ms="); Serial.println(millis() - viewStarted);
}

void reportViewDone() {
  if (viewReported || (!fatalServiceError && nextTile < placements.size())) return;
  viewReported = true;
  Serial.print("WIO_MAP_DONE id="); Serial.print(viewId);
  Serial.print(" loaded="); Serial.print(loadedTiles);
  Serial.print(" failed="); Serial.print(failedTiles);
  Serial.print(" total="); Serial.print(placements.size());
  Serial.print(" fatal="); Serial.print(fatalServiceError ? 1 : 0);
  Serial.print(" elapsed_ms="); Serial.println(millis() - viewStarted);
  reportView();
}

void tileLog(const char *stage, bool annotation, Status status, int httpStatus = 0, size_t bytes = 0,
             const char *transportPhase = nullptr, int networkCode = 0) {
  Serial.print("WIO_MAP_TILE view="); Serial.print(viewId);
  Serial.print(" index="); Serial.print(nextTile + 1);
  Serial.print(" layer="); Serial.print(annotation ? "labels" : "base");
  Serial.print(" stage="); Serial.print(stage);
  Serial.print(" status="); Serial.print(statusName(status));
  Serial.print(" http="); Serial.print(httpStatus);
  Serial.print(" bytes="); Serial.print(bytes);
  if (transportPhase) { Serial.print(" transport="); Serial.print(transportPhase); Serial.print(" net_code="); Serial.print(networkCode); }
  Serial.println();
}

bool writeCaptureBytes(const uint8_t *bytes, size_t count) {
  size_t offset = 0; uint32_t stalledAt = millis();
  while (offset < count) {
    if (!Serial || uint32_t(millis() - stalledAt) > 2000) return false;
    const size_t requested = std::min<size_t>(64, count - offset);
    const size_t sent = Serial.write(bytes + offset, requested);
    // This USB core can return unsigned -1 on timeout, not a byte count.
    if (sent && sent <= requested) { offset += sent; stalledAt = millis(); }
    else delay(1);
    yield();
  }
  return true;
}

void captureScreen() {
  // Read real ILI9341 GRAM a row at a time; reuse the existing 1 KiB scratch.
  // Protocol: one ASCII header, then exactly 320*240*3 raw RGB888 bytes.
  // No other logging runs during the synchronous snapshot. A failed read or
  // disconnected host aborts the payload; the host must reject short frames.
  auto *panel = nice_gfx::detail::DisplayAccess::panel(display);
  if (!panel || display.wait() != nice_gfx::Status::Ok ||
      panel->readRect(0, 0, 320, 1, scratch) != nice_gfx::Status::Ok) {
    Serial.println("WIO_MAP_CAPTURE_ERROR readback"); return;
  }
  Serial.println("WIO_MAP_RGB 320 240");
  auto *rgb = reinterpret_cast<uint8_t *>(scratch);
  for (unsigned y = 0; y < 240; ++y) {
    if (y && panel->readRect(0, y, 320, 1, scratch) != nice_gfx::Status::Ok) return;
    // Expand backwards so the output never overwrites unread RGB565 pixels.
    for (int x = 319; x >= 0; --x) {
      const uint16_t pixel = scratch[x];
      const uint8_t r = pixel >> 11, g = (pixel >> 5) & 63, b = pixel & 31;
      rgb[x * 3] = (r << 3) | (r >> 2);
      rgb[x * 3 + 1] = (g << 2) | (g >> 4);
      rgb[x * 3 + 2] = (b << 3) | (b >> 2);
    }
    if (!writeCaptureBytes(rgb, 320 * 3)) return;
  }
  // A short USB packet releases the final full-size packets on Windows CDC.
  // The host checks this trailer after reading the exact binary frame length.
  Serial.println("WIO_MAP_RGB_END");
}

void probeNetwork() {
  // Pause the map between synchronous requests. Press C/send r to reload it.
  fatalServiceError = true;
  viewDirty = false;
  hud("TLS diagnostics / r: resume");
  char *version = rpc_system_version();
  Serial.print("WIO_MAP_RADIO version="); Serial.println(version ? version : "unavailable");
  if (version) erpc_free(version);
  Serial.print("WIO_MAP_NET wifi="); Serial.print(int(WiFi.status()));
  Serial.print(" ip="); Serial.print(WiFi.localIP());
  Serial.print(" gateway="); Serial.print(WiFi.gatewayIP());
  Serial.print(" dns="); Serial.println(WiFi.dnsIP());
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WIO_MAP_NET_DONE disconnected"); return;
  }
  if (!wio_map::ensureWirelessTls()) {
    Serial.println("WIO_MAP_NET_DONE tls_bootstrap_failed"); return;
  }
  constexpr const char *hosts[] = {"t0.tianditu.gov.cn", "t1.tianditu.gov.cn", "www.baidu.com"};
  for (const char *host : hosts) {
    IPAddress address;
    const uint32_t started = millis();
    const int resolved = WiFi.hostByName(host, address);
    Serial.print("WIO_MAP_DNS host="); Serial.print(host);
    Serial.print(" ok="); Serial.print(resolved);
    Serial.print(" ip="); Serial.print(address);
    Serial.print(" elapsed_ms="); Serial.println(millis() - started);
  }
  // Two certificate-verified handshakes only: no requests, tokens or URLs.
  for (const unsigned index : {0u, 2u}) {
    WiFiClientSecure client;
    client.setCACert(wio_map_config::RootCa);
    Serial.print("WIO_MAP_TLS_BEGIN host="); Serial.println(hosts[index]);
    const uint32_t started = millis();
    const int connected = client.connect(hosts[index], 443, 15000);
    char error[100] = {};
    const int errorCode = client.lastError(error, sizeof(error));
    for (char *c = error; *c; ++c) if (*c == '\r' || *c == '\n') *c = ' ';
    Serial.print("WIO_MAP_TLS host="); Serial.print(hosts[index]);
    Serial.print(" ok="); Serial.print(connected);
    Serial.print(" elapsed_ms="); Serial.print(millis() - started);
    Serial.print(" error_code="); Serial.print(errorCode);
    Serial.print(" error="); Serial.println(error);
    client.stop();
  }
  Serial.println("WIO_MAP_NET_DONE paused / r: resume");
  reportView();
}

void resetView() {
  mapView.setCenter({wio_map_config::InitialLatitude, wio_map_config::InitialLongitude});
  mapView.setZoom(uint8_t(wio_map_config::InitialZoom));
  viewDirty = true;
}

void beginView() {
  placements = mapView.visibleTiles();
  nextTile = failedTiles = loadedTiles = 0;
  ++viewId; viewStarted = millis(); viewReported = false;
  fatalServiceError = false;
  nextRetry = 0;
  display.fillRect(0, MapTop, MapWidth, MapHeight, 0xdedb);
  viewDirty = false;
  hud("Loading real map...");
  reportView();
}

void clearTile(const TilePlacement &placement) {
  const int left = std::max<int>(0, placement.x), top = std::max<int>(0, placement.y);
  const int right = std::min<int>(MapWidth, placement.x + TileSize);
  const int bottom = std::min<int>(MapHeight, placement.y + TileSize);
  if (right > left && bottom > top)
    display.fillRect(left, MapTop + top, right - left, bottom - top, 0xdedb);
}

Status readMapRow(void *, int x, int y, uint16_t *pixels, size_t width) {
  if (x < 0 || y < 0 || y >= MapHeight || !width || width > MapWidth || size_t(x) + width > MapWidth)
    return Status::InvalidArgument;
  auto *panel = nice_gfx::detail::DisplayAccess::panel(display);
  if (!panel || panel->readRect(x, MapTop + y, width, 1, pixels) != nice_gfx::Status::Ok)
    return Status::Unsupported;
  return Status::Ok;
}

Status writeMapRow(void *, int x, int y, const uint16_t *pixels, size_t width) {
  if (x < 0 || y < 0 || y >= MapHeight || !width || width > MapWidth || size_t(x) + width > MapWidth)
    return Status::InvalidArgument;
  const auto sent = display.flush(x, MapTop + y,
    nice_gfx::PixelView(pixels, width, 1, width, nice_gfx::PixelOrder::Native), false);
  return sent == nice_gfx::Status::Ok && display.wait() == nice_gfx::Status::Ok ? Status::Ok : Status::Busy;
}

wio_map::NativeTileTarget tileTarget(const TilePlacement &placement) {
  wio_map::NativeTileTarget target;
  target.tileX = placement.x; target.tileY = placement.y;
  target.viewportWidth = MapWidth; target.viewportHeight = MapHeight;
  target.background = 0xdedb;
  target.row = scratch; target.rowCapacity = sizeof(scratch) / sizeof(*scratch);
  target.context = nullptr; target.readRow = readMapRow; target.writeRow = writeMapRow;
  return target;
}

void report(Status status, int httpStatus, bool annotation) {
  char line[64];
  if (httpStatus) std::snprintf(line, sizeof(line), "%s HTTP %d / %s", annotation ? "Labels" : "Tile", httpStatus, statusName(status));
  else std::snprintf(line, sizeof(line), "%s: %s", annotation ? "Labels" : "Tile", statusName(status));
  hud(line);
  // Never print a request URL, SSID, password or service key.
  Serial.println(line);
}

void loadOneTile() {
  if (fatalServiceError || nextTile >= placements.size() || int32_t(millis() - nextRetry) < 0) return;
  const TilePlacement placement = placements[nextTile];
  const auto target = tileTarget(placement);
  auto downloaded = wio_map::downloadToBuffer(service.url(placement.key), encoded,
    wio_map_config::RootCa, MaximumEncodedBytes, true);
  Status status = downloaded.status;
  tileLog("download", false, status, downloaded.httpStatus, downloaded.bytes, downloaded.phase, downloaded.networkCode);
  if (status == Status::Ok) status = wio_map::drawNativePng(encoded.data(), encoded.size(), target, false);
  tileLog("decode_draw", false, status);
  encoded.clear();
  if (status != Status::Ok) {
    clearTile(placement);
    ++failedTiles; ++nextTile;
    report(status, downloaded.httpStatus, false);
    // Authentication/rate failures stop this view; C/r explicitly retries.
    fatalServiceError = downloaded.httpStatus == 401 || downloaded.httpStatus == 403 || downloaded.httpStatus == 429;
    nextRetry = millis() + 1000;
    reportViewDone();
    return;
  }
  if (labels) {
    TileKey key = placement.key; key.layer = 1;
    downloaded = wio_map::downloadToBuffer(service.url(key), encoded,
      wio_map_config::RootCa, MaximumLabelEncodedBytes, true);
    status = downloaded.status;
    tileLog("download", true, status, downloaded.httpStatus, downloaded.bytes, downloaded.phase, downloaded.networkCode);
    if (status == Status::Ok) status = wio_map::drawNativePng(encoded.data(), encoded.size(), target, true);
    tileLog("decode_draw", true, status);
    encoded.clear();
    if (status != Status::Ok) {
      // Remove the affected screen region if a late streaming decode failed.
      clearTile(placement);
      ++failedTiles; ++nextTile;
      report(status, downloaded.httpStatus, true);
      fatalServiceError = downloaded.httpStatus == 401 || downloaded.httpStatus == 403 || downloaded.httpStatus == 429;
      nextRetry = millis() + 1000;
      reportViewDone();
      return;
    }
  }
  ++loadedTiles;
  ++nextTile;
  if (status == Status::Ok) {
    char line[64];
    if (nextTile == placements.size()) {
      if (failedTiles) std::snprintf(line, sizeof(line), "%u failed / C: retry / l: labels", unsigned(failedTiles));
      else std::snprintf(line, sizeof(line), "Ready / arrows: pan / A+: B-: C:reload");
    } else std::snprintf(line, sizeof(line), "Loading %u/%u", unsigned(nextTile), unsigned(placements.size()));
    hud(line);
  }
  reportViewDone();
}

void command(char key) {
  switch (key) {
    case 'v': reportView(); return;
    case 'x': captureScreen(); return;
    case 't': probeNetwork(); return;
    case 'q':
      fatalServiceError = true; viewDirty = false;
      hud("TLS verification / r: resume");
      if (wio_map::ensureWirelessTls())
        wio_tls_bootstrap::verifyConnections(wio_map_config::RootCa, wio_map::tlsEpoch());
      reportView(); return;
    case 'w': mapView.panBy(0, 64); break;
    case 's': mapView.panBy(0, -64); break;
    case 'a': mapView.panBy(64, 0); break;
    case 'd': mapView.panBy(-64, 0); break;
    case '+': if (mapView.zoom() < 18) mapView.setZoom(mapView.zoom() + 1); else return; break;
    case '-': if (mapView.zoom() > 1) mapView.setZoom(mapView.zoom() - 1); else return; break;
    case 'h': resetView(); break;
    case 'r': break;
    case 'l': labels = !labels; break;
    default: return;
  }
  Serial.print("WIO_MAP_COMMAND "); Serial.println(key);
  viewDirty = true;
}

void pollButtons() {
  static uint8_t last = 0, stable = 0;
  static uint32_t changed = 0;
  uint8_t current = 0;
  for (unsigned i = 0; i < sizeof(ButtonPins); ++i) if (!digitalRead(ButtonPins[i])) current |= uint8_t(1u << i);
  const uint32_t now = millis();
  if (current != last) { last = current; changed = now; }
  if (now - changed < 30 || current == stable) return;
  const uint8_t pressed = current & ~stable; stable = current;
  constexpr char commands[] = {'w', 's', 'a', 'd', 'h', '+', '-', 'r'};
  for (unsigned i = 0; i < sizeof(ButtonPins); ++i) if (pressed & (1u << i)) command(commands[i]);
}
} // namespace

void setup() {
  Serial.begin(115200); // USB is optional; never wait for a serial monitor.
  pinMode(LCD_BACKLIGHT, OUTPUT); digitalWrite(LCD_BACKLIGHT, HIGH);
  if (!display.begin()) return;
  display.setRotation(3); display.fillScreen(0x1106);
  display.setFont(nice_gfx::fonts::ProFont6x12);
  for (const uint8_t pin : ButtonPins) pinMode(pin, INPUT_PULLUP);
  service = providers::tianditu(wio_map_config::TiandituKey, true);
  service.tileSize = TileSize; // Native 256px density; only visible placements are fetched.
  TileSource baseOnly = service; baseOnly.layers.resize(1);
  if (!service.valid() || mapView.setSource(baseOnly) != Status::Ok ||
      wio_map_config::InitialZoom < 1 || wio_map_config::InitialZoom > 18 ||
      !validCoordinate({wio_map_config::InitialLatitude, wio_map_config::InitialLongitude}) ||
      wio_map_config::InitialLatitude < -MercatorLatitudeLimit ||
      wio_map_config::InitialLatitude > MercatorLatitudeLimit) {
    hud("Invalid Tianditu configuration"); return;
  }
  resetView();
  if (!*wio_map_config::WifiSsid || !*wio_map_config::TiandituKey ||
      std::strcmp(wio_map_config::WifiSsid, "YOUR_WIFI_SSID") == 0 ||
      std::strcmp(wio_map_config::TiandituKey, "YOUR_TIANDITU_KEY") == 0) {
    hud("Set WioSecrets.local.h first"); return;
  }
  // Allocate once before network traffic can fragment the SAMD51 heap.
  if (encoded.reserve(MaximumEncodedBytes) != Status::Ok) {
    hud("No RAM for map download buffer"); return;
  }
  ready = true;
  hud("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wio_map_config::WifiSsid, wio_map_config::WifiPassword);
  lastWifiAttempt = millis();
}

void loop() {
  if (!ready) { delay(10); return; }
  pollButtons();
  while (Serial.available()) command(char(Serial.read()));
  if (WiFi.status() != WL_CONNECTED) {
    wio_map::invalidateWirelessTls();
    if (uint32_t(millis() - lastWifiAttempt) >= 20000) {
      hud("WiFi reconnecting...");
      WiFi.begin(wio_map_config::WifiSsid, wio_map_config::WifiPassword);
      lastWifiAttempt = millis();
    }
    delay(10); return;
  }
  if (!wio_map::ensureWirelessTls()) {
    hud("TLS setup failed / see serial");
    delay(1000); return;
  }
  if (viewDirty) beginView();
  loadOneTile();
  delay(2);
}
