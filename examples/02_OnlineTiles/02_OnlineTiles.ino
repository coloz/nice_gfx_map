/*
 * ESP32 + PSRAM + ILI9341. Install NICE_GFX_MAP, NICE_GFX and PNGdec.
 * Fill Wi-Fi credentials, provider key and a current trusted root CA below.
 * This intentionally synchronous example loads at most one tile per loop.
 * timeoutMs is a best-effort total deadline applied to blocking I/O, not a
 * strict real-time bound. Use a worker to keep a product UI responsive.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <cstring>
#include <NICE_GFX.h>
#include <NICE_GFX_MAP.h>
#include <adapters/Esp32.h>
#include <adapters/NiceGfx.h>
#include <codecs/PngDecoder.h>

using namespace nice_gfx_map;

const char *kWifiSsid = "YOUR_WIFI_SSID";
const char *kWifiPassword = "YOUR_WIFI_PASSWORD";
const char *kProviderKey = "YOUR_PROVIDER_KEY";
// Obtain the trusted root CA for the selected HTTPS service from its actual
// certificate chain, using a trusted source. Keep it current in your product.
const char kCaCertificate[] = R"PEM(
REPLACE_WITH_THE_TRUSTED_ROOT_CA_CERTIFICATE_IN_PEM_FORMAT
)PEM";

enum class Provider { Tianditu, Geoapify, MapTiler };
constexpr Provider kProvider = Provider::Geoapify;

ILI9341_240x320_SPI display(5, 2, 4, 18, 23, 19);
Map mapView;
uint16_t scratch[320 * 8];
bool ready = false;
uint64_t paintedRevision = 0;

HttpOptions makeHttpOptions() {
  HttpOptions options;
  options.caCertificate = kCaCertificate;
  options.timeoutMs = 10000;
  options.maxResponseBytes = 512 * 1024;
  options.userAgent = "nice_gfx_map-example/0.1";
  options.allocator = esp32PsramAllocator();
  return options;
}
Esp32HttpTransport transport(makeHttpOptions());
PngDecoder decoder(esp32PsramAllocator());
TileLoader loader(transport, decoder, []() -> uint32_t { return millis(); });

TileSource selectedSource() {
  switch (kProvider) {
    case Provider::Tianditu: return providers::tianditu(kProviderKey, true);
    case Provider::MapTiler: return providers::mapTiler(kProviderKey, "streets-v4");
    default: return providers::geoapify(kProviderKey, "osm-bright");
  }
}

void setup() {
  Serial.begin(115200);
  if (std::strcmp(kWifiSsid, "YOUR_WIFI_SSID") == 0 ||
      std::strcmp(kProviderKey, "YOUR_PROVIDER_KEY") == 0 ||
      !std::strstr(kCaCertificate, "-----BEGIN CERTIFICATE-----")) {
    Serial.println("Set Wi-Fi, your service key and a trusted PEM root CA first.");
    return;
  }
  if (display.beginChecked() != nice_gfx::Status::Ok) {
    Serial.println("Display initialization failed"); return;
  }
  display.setRotation(1);
  display.setFont(nice_gfx::fonts::TomThumb4x6);
  WiFi.begin(kWifiSsid, kWifiPassword);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) delay(100);
  if (WiFi.status() != WL_CONNECTED) { Serial.println("Wi-Fi timeout"); return; }
  // Certificate verification also needs a valid clock; no insecure TLS fallback.
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  const uint32_t clockStarted = millis();
  while (time(nullptr) < 1700000000 && millis() - clockStarted < 15000) delay(100);
  if (time(nullptr) < 1700000000) { Serial.println("Clock synchronization failed"); return; }
  if (mapView.setSource(selectedSource()) != Status::Ok ||
      mapView.setCenter({30.6586, 104.0648}) != Status::Ok ||
      mapView.setZoom(14) != Status::Ok) { Serial.println("Map configuration failed"); return; }
  Serial.println(mapView.source().attribution.c_str());
  Serial.println("Provide the full required attribution in your product UI.");
  ready = true;
}

void loop() {
  if (!ready) { delay(10); return; }
  // This is bounded by one request, not by frame time. A TLS/network request can
  // block for seconds; no promise of uninterrupted touch sampling is made here.
  const Status loaded = loader.loadNext(mapView, millis());
  if (loaded != Status::Ok && loaded != Status::NotFound && loaded != Status::Busy)
    Serial.printf("Tile: %s\n", statusName(loaded)); // Never log URLs containing keys.
  if (mapView.revision() != paintedRevision) {
    const Status status = NiceGfxRenderer::draw(mapView, display, 0, 0,
                                             scratch, sizeof(scratch) / sizeof(scratch[0]));
    if (status == Status::Ok) {
      // Visible text credits. Add the required provider logo and an accessible
      // attribution/link view for the actual service/plan before distribution.
      display.fillRect(0, 224, 320, 16, 0xffff);
      display.setTextColor(0x0000, 0xffff);
      display.drawString(mapView.source().attribution.c_str(), 2, 225);
      paintedRevision = mapView.revision();
    } else Serial.printf("Draw: %s\n", statusName(status));
  }
  delay(10);
}
