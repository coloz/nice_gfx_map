/* Real Tianditu tiles + UART GNSS + SD ride CSV + imported bicycle route.
 * ESP32-S3, 8 MiB PSRAM, ILI9341, GNSS RMC/GGA and a separate SPI SD card.
 * All Map/display/SD operations belong to loop(); the network worker only owns
 * its independent request/image. No synthetic map or location fallback.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <NICE_GFX.h>
#include <NICE_GFX_MAP.h>
#include <adapters/Esp32.h>
#include <adapters/NiceGfx.h>
#include <codecs/Esp32RasterDecoder.h>
#include <demo/RideSupport.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "BikeConfig.h"
#include "RideRecorder.h"
#include <utility>

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || !CONFIG_IDF_TARGET_ESP32S3
#error "This wiring example targets ESP32-S3. Port SPI buses/pins for other chips."
#endif
using namespace nice_gfx_map;

MapConfig bikeMapConfig() {
  MapConfig config;
  config.width = 320; config.height = 160;
  config.cacheBytes = 4 * 1024 * 1024; config.maxCachedTiles = 24;
  config.maxInFlight = 1; config.maxRoutePoints = 1024; config.maxTrackPoints = 2048;
  config.retryDelayMs = 5000;
  return config;
}
ILI9341_240x320_SPI display(NICE_BIKE_TFT_CS, NICE_BIKE_TFT_DC, NICE_BIKE_TFT_RST,
                          NICE_BIKE_TFT_SCK, NICE_BIKE_TFT_MOSI, NICE_BIKE_TFT_MISO);
Map mapView(bikeMapConfig());
SPIClass sdSpi(HSPI); // SPI3; NICE_GFX's display owns SPI2.
HardwareSerial gnss(1);
NmeaParser nmea;
demo::CsvRouteReader routeReader; // 16 KiB; keep off the loop task stack.
RouteNavigator navigator;
NavigationState navigation;
demo::RideStatistics rideStats, positionGate;
bike::RideRecorder recorder;
uint16_t scratch[320 * 8];
bool ready = false, sdReady = false, routeReady = false, fixValid = false;
bool speedKnown = false, paused = false, networkConfigured = false, clockRequested = false;
GnssFix currentFix;
uint32_t lastRmcMs = 0, lastSampleMs = 0, lastMetricMs = 0, lastPaintMs = 0;
bool haveSample = false;
Status lastTileStatus = Status::NotFound;
const char *routeMessage = "NO ROUTE";

// FreeRTOS queues copy ONLY a pointer. TileRequest/string and move-only image
// stay in this slot; queue hand-offs transfer exclusive ownership of the slot.
struct TileWork {
  TileRequest request;
  TileImage image;
  Status status = Status::Busy;
  uint32_t completedMs = 0;
  int httpStatus = 0;
} tileWork;
QueueHandle_t workQueue = nullptr, resultQueue = nullptr;
bool workerBusy = false;

HttpOptions bikeHttpOptions() {
  HttpOptions options;
  options.caCertificate = bike_config::RootCa;
  options.userAgent = "nice_gfx_map-bike/0.2";
  options.timeoutMs = 7000; options.maxResponseBytes = 512 * 1024;
  options.allocator = esp32PsramAllocator();
  return options;
}
void tileWorker(void *) {
  Esp32HttpTransport transport(bikeHttpOptions());
  Esp32RasterDecoder decoder(esp32PsramAllocator());
  for (;;) {
    TileWork *work = nullptr;
    if (xQueueReceive(workQueue, &work, portMAX_DELAY) != pdTRUE || !work) continue;
    ByteBuffer bytes;
    work->image.reset();
    work->status = transport.fetch(work->request, bytes);
    work->httpStatus = transport.lastHttpStatus();
    if (work->status == Status::Ok)
      work->status = decoder.decode(work->request, bytes.data(), bytes.size(), work->image);
    work->completedMs = millis();
    // One outstanding request, one result slot, and a lifetime-long task.
    xQueueSend(resultQueue, &work, portMAX_DELAY);
  }
}
void pollTiles() {
  TileWork *completed = nullptr;
  if (resultQueue && xQueueReceive(resultQueue, &completed, 0) == pdTRUE) {
    const Status fetched = completed->status;
    lastTileStatus = fetched == Status::Ok ?
      mapView.submit(completed->request, std::move(completed->image)) :
      mapView.fail(completed->request, fetched, completed->completedMs);
    if (fetched == Status::Ok && lastTileStatus != Status::Ok && lastTileStatus != Status::Canceled)
      mapView.fail(completed->request, lastTileStatus, completed->completedMs);
    if (fetched != Status::Ok) {
      lastTileStatus = fetched;
      Serial.printf("Tile %s HTTP %d\n", statusName(fetched), completed->httpStatus);
    } // Do not print request URLs: they contain tk.
    completed->image.reset(); workerBusy = false;
  }
  if (!networkConfigured || !workQueue || workerBusy || WiFi.status() != WL_CONNECTED ||
      time(nullptr) < 1700000000) return;
  if (mapView.nextRequest(tileWork.request, millis()) != Status::Ok) return;
  TileWork *work = &tileWork;
  workerBusy = true;
  if (xQueueSend(workQueue, &work, 0) != pdTRUE) {
    mapView.fail(tileWork.request, Status::Busy, millis()); workerBusy = false;
  }
}

bool loadRoute() {
  routeReady = false; navigator.reset(); navigation = NavigationState{};
  mapView.setRoute(nullptr, 0); mapView.setMarkers(nullptr, 0); routeReader.reset();
  if (!sdReady) { routeMessage = "NO SD"; return false; }
  fs::File file = SD.open(bike_config::RoutePath, FILE_READ);
  if (!file || file.isDirectory()) { routeMessage = "NO ROUTE CSV"; return false; }
  if (file.size() > 128 * 1024) { routeMessage = "ROUTE TOO LARGE"; return false; }
  size_t bytes = 0;
  bool valid = true;
  while (file.available()) {
    const int byte = file.read();
    if (byte < 0 || ++bytes > 128 * 1024 || !routeReader.feed(char(byte))) { valid = false; break; }
  }
  if (bytes != file.size()) valid = false;
  file.close();
  if (!valid || !routeReader.finish()) {
    routeMessage = "INVALID ROUTE";
    Serial.println(demo::routeCsvStatusText(routeReader.status())); return false;
  }
  const GeoPoint *points = routeReader.points();
  const size_t count = routeReader.count();
  if (navigator.setRoute(points, count, 25, 60) != Status::Ok ||
      mapView.setRoute(points, count, 0x001f, 3) != Status::Ok) {
    routeMessage = "ROUTE SET FAILED"; routeReady = false;
    navigator.reset(); mapView.setRoute(nullptr, 0); return false;
  }
  navigation = NavigationState{}; routeReady = true; routeMessage = "START AT POINT 1";
  mapView.setMarkers(nullptr, 0);
  const Marker ends[] = {{points[0], 0x07e0, 5, 1}, {points[count-1], 0xf800, 5, 2}};
  mapView.setMarkers(ends, 2);
  Serial.printf("Route loaded: %u waypoints; no road routing/recalculation.\n", unsigned(count));
  return true;
}
void startRide() {
  if (!sdReady) { Serial.println("No SD: recording unavailable; navigation remains active."); return; }
  if (!recorder.begin(SD, millis())) { Serial.println(recorder.error()); return; }
  rideStats.reset(); paused = false; mapView.clearTrack();
  Serial.printf("Recording %s/track.csv\n", recorder.folder());
}
void loseFix() {
  fixValid = false; speedKnown = false; mapView.clearLocation();
  rideStats.breakSegment(); positionGate.breakSegment();
}
bool fieldPresent(const char *line, unsigned index) {
  const char *field = line;
  for (unsigned i = 0; i < index; ++i) {
    field = std::strchr(field, ','); if (!field) return false; ++field;
  }
  return *field && *field != ',' && *field != '*' && *field != '\r' && *field != '\n';
}
void acceptRmc(const char *sentence) {
  const uint32_t now = millis();
  GnssFix fix = nmea.fix();
  if (!fix.valid) { loseFix(); return; }
  // GGA alone never refreshes instrument speed. Missing RMC fields must not
  // keep the parser's older numeric values alive in the HUD or recorded CSV.
  const bool hasSpeed = fieldPresent(sentence, 7);
  if (!hasSpeed) fix.speedMetersPerSecond = 0;
  if (!fieldPresent(sentence, 8)) fix.headingDegrees = 0;
  lastRmcMs = now;
  if (haveSample && now - lastSampleMs < 1000) return;
  haveSample = true; lastSampleMs = now;
  if (!positionGate.accept(fix, now)) {
    // Preserve the outlier filter's previous anchor. Breaking it here would
    // let a second implausible fix become an unchecked new starting point.
    fixValid = false; speedKnown = false; mapView.clearLocation();
    rideStats.breakSegment(); return;
  }
  currentFix = fix; fixValid = true; speedKnown = hasSpeed;
  mapView.setLocation(fix.position, fix.headingDegrees);
  if (routeReady) {
    navigator.update(fix.position, navigation);
    routeMessage = navigation.arrived ? "ARRIVED" : navigation.offRoute ? "OFF ROUTE" : "ON ROUTE";
  }
  if (recorder.active() && hasSpeed) {
    if (rideStats.accept(fix, now)) {
      mapView.appendTrack(fix.position);
      if (!recorder.append(now, fix, rideStats.summary())) Serial.println(recorder.error());
    }
  } else if (recorder.active()) rideStats.breakSegment();
}
void pollGnss() {
  static char line[162] = {};
  static size_t size = 0;
  // Bounded work per pass; the UART ring retains the remaining bytes.
  for (unsigned read = 0; read < 1024 && gnss.available(); ++read) {
    const char byte = char(gnss.read());
    if (byte == '$') size = 0;
    if (size < sizeof(line) - 1) { line[size++] = byte; line[size] = 0; }
    if (nmea.feed(byte)) {
      if (size >= 7 && std::strncmp(line + 3, "RMC,", 4) == 0) acceptRmc(line);
      else if (!nmea.fix().valid) loseFix();
    }
  }
  if (fixValid && millis() - lastRmcMs > bike_config::FixTimeoutMs) loseFix();
}
void handleKeys() {
  for (unsigned count = 0; count < 32 && Serial.available(); ++count) {
    switch (Serial.read()) {
      case 'r':
        if (recorder.active()) { recorder.end(); paused = false; Serial.println("Ride stopped/flushed."); }
        else startRide();
        break;
      case 'p':
        if (recorder.active()) { paused = !paused; rideStats.setPaused(paused); recorder.flush(); }
        break;
      case 'l': if (loadRoute()) Serial.println("Route progress reset."); break;
      case 'v': if (routeReady) mapView.fitPoints(routeReader.points(), routeReader.count(), 15); break;
      case 'f': mapView.setFollow(true); break;
      case 'w': mapView.panBy(0, 32); break;
      case 's': mapView.panBy(0, -32); break;
      case 'a': mapView.panBy(32, 0); break;
      case 'd': mapView.panBy(-32, 0); break;
      case '+': if (mapView.zoom() < 18) mapView.setZoom(mapView.zoom() + 1); break;
      case '-': if (mapView.zoom() > 1) mapView.setZoom(mapView.zoom() - 1); break;
      default: break;
    }
  }
}
void paint() {
  const uint32_t now = millis();
  if (now - lastPaintMs < 250) return;
  lastPaintMs = now;
  if (NiceGfxRenderer::draw(mapView, display, 0, 40, scratch, 320 * 8) != Status::Ok) return;
  display.fillRect(0, 0, 320, 40, 0x18e3);
  display.setTextColor(0xffff, 0x18e3);
  char text[96], speed[16];
  if (fixValid && speedKnown) std::snprintf(speed, sizeof(speed), "%.1f", currentFix.speedMetersPerSecond * 3.6);
  else std::snprintf(speed, sizeof(speed), "--");
  const auto &summary = rideStats.summary();
  const unsigned long seconds = static_cast<unsigned long>(summary.durationMs / 1000);
  std::snprintf(text, sizeof(text), "%s %s km/h  %.2f km  %02lu:%02lu:%02lu",
    recorder.active() ? (paused ? "PAUSE" : "REC") : (*recorder.error() ? "SD ERR" : "STOP"),
    speed, summary.distanceMeters / 1000, seconds / 3600, seconds / 60 % 60, seconds % 60);
  display.drawString(text, 3, 3);
  std::snprintf(text, sizeof(text), "AVG %.1f MAX %.1f %s SAT %u", summary.averageKmh, summary.maxKmh,
                fixValid ? "RMC" : "NO FIX", fixValid ? unsigned(currentFix.satellites) : 0);
  display.drawString(text, 3, 22);
  display.fillRect(0, 200, 320, 40, 0xffff);
  display.setTextColor(routeReady && fixValid && navigation.offRoute ? 0xf800 : 0x0000, 0xffff);
  if (routeReady && fixValid) std::snprintf(text, sizeof(text), "%s #%u %.0fm BRG %.0f %.0fm left",
    routeMessage, unsigned(navigation.nextIndex + 1), navigation.metersToNext,
    navigation.bearingToNext, navigation.remainingMeters);
  else std::snprintf(text, sizeof(text), "%s | %s", routeMessage, fixValid ? "FIX" : "WAIT RMC");
  display.drawString(text, 3, 202);
  const char *network = !networkConfigured ? "SET WIFI/KEY/CA" : !workQueue ? "NO WORKER" :
    WiFi.status() != WL_CONNECTED ? "NO WIFI" : time(nullptr) < 1700000000 ? "WAIT CLOCK" :
    lastTileStatus == Status::NotFound ? "WAIT TILES" : statusName(lastTileStatus);
  // Persistent visible credit; complete service attribution is in README.
  std::snprintf(text, sizeof(text), "Map: Tianditu | %s", network);
  display.drawString(text, 3, 222);
}
void setup() {
  Serial.begin(115200);
  if (display.beginChecked() != nice_gfx::Status::Ok) { Serial.println("Display failed"); return; }
  display.setRotation(1); display.setFont(nice_gfx::fonts::ProFont6x12);
  display.setTextWrap(false); display.fillScreen(0xffff);
  if (!psramFound() || ESP.getPsramSize() < 8 * 1024 * 1024) {
    display.setTextColor(0xf800, 0xffff); display.drawString("Enable >=8 MiB PSRAM", 5, 5);
    Serial.println("This two-layer example requires enabled >=8 MiB PSRAM."); return;
  }
  gnss.setRxBufferSize(8192);
  gnss.begin(bike_config::GnssBaud, SERIAL_8N1, NICE_BIKE_GNSS_RX, NICE_BIKE_GNSS_TX);
  sdSpi.begin(NICE_BIKE_SD_SCK, NICE_BIKE_SD_MISO, NICE_BIKE_SD_MOSI, NICE_BIKE_SD_CS);
  sdReady = SD.begin(NICE_BIKE_SD_CS, sdSpi, 10000000) && SD.cardType() != CARD_NONE;
  if (!sdReady) Serial.println("SD unavailable: no ride file or route; live map/GNSS still work.");
  if (mapView.setSource(providers::tianditu(bike_config::TiandituKey, true)) != Status::Ok ||
      mapView.setCenter({30.6586, 104.0648}) != Status::Ok || mapView.setZoom(16) != Status::Ok) {
    Serial.println("Map configuration failed"); return;
  }
  loadRoute(); mapView.setFollow(true);
  networkConfigured = std::strcmp(bike_config::WifiSsid, "YOUR_WIFI_SSID") &&
    std::strcmp(bike_config::TiandituKey, "YOUR_TIANDITU_KEY") && *bike_config::WifiSsid &&
    *bike_config::TiandituKey && std::strstr(bike_config::RootCa, "-----BEGIN CERTIFICATE-----");
  if (networkConfigured) {
    workQueue = xQueueCreate(1, sizeof(TileWork *)); resultQueue = xQueueCreate(1, sizeof(TileWork *));
    if (!workQueue || !resultQueue || xTaskCreate(tileWorker, "tdt-tiles", 16384, nullptr, 1, nullptr) != pdPASS) {
      if (workQueue) vQueueDelete(workQueue);
      if (resultQueue) vQueueDelete(resultQueue);
      workQueue = resultQueue = nullptr; Serial.println("Tile worker allocation failed");
    }
    WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
    WiFi.begin(bike_config::WifiSsid, bike_config::WifiPassword);
  } else Serial.println("Set BikeConfig.h Wi-Fi/key/root CA for real Tianditu. No fake map fallback.");
  if (sdReady && bike_config::AutoStartRecording) startRide();
  Serial.println("r start/stop NEW ride, p pause/resume, l reload route, v route overview");
  Serial.println("w/a/s/d pan, +/- zoom, f follow; 115200 baud. No simulated GNSS.");
  Serial.println("Navigation follows supplied bicycle waypoints; no online road routing.");
  ready = true;
}
void loop() {
  if (!ready) { delay(20); return; }
  pollGnss(); handleKeys();
  const uint32_t now = millis();
  if (recorder.active() && now - lastMetricMs >= 1000) {
    lastMetricMs = now;
    if (!fixValid || !speedKnown || paused) rideStats.accept(GnssFix{}, now);
  }
  if (networkConfigured && !clockRequested && WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com"); clockRequested = true;
  }
  recorder.tick(millis()); pollTiles(); paint();
  // SD writes and strip rendering are synchronous; GNSS ring buffering reduces
  // gaps but is not a hard real-time or SD power-failure guarantee.
  delay(2);
}
