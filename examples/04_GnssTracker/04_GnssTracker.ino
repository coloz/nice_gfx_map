// SIM by default; 'm' switches to checksum-validated NMEA on UART1.
// Connect a 3.3 V GNSS TX to configured RX, GND to GND; UART TX is optional.
#include <demo/ExampleSupport.h>
#include <Navigation.h>
#include <Nmea.h>
#ifndef NICE_MAP_GNSS_RX
#if defined(CONFIG_IDF_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32
#define NICE_MAP_GNSS_RX 32
#else
#define NICE_MAP_GNSS_RX 16
#endif
#endif
#ifndef NICE_MAP_GNSS_TX
#if defined(CONFIG_IDF_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32
#define NICE_MAP_GNSS_TX 33
#else
#define NICE_MAP_GNSS_TX 17
#endif
#endif
#ifndef NICE_MAP_GNSS_BAUD
#define NICE_MAP_GNSS_BAUD 9600
#endif
#ifndef NICE_MAP_REAL_GNSS
#define NICE_MAP_REAL_GNSS 0
#endif
using namespace nice_gfx_map;
demo::Example app;
Map &mapView = app.mapView;
HardwareSerial gnssUart(1);
NmeaParser parser;
bool realGnss = NICE_MAP_REAL_GNSS != 0;
uint32_t nextSample = 0, lastValidFix = 0;
unsigned sampleIndex = 0;
double speedMps = 0;
bool validFix = false;

void useFix(const GnssFix &fix) {
  validFix = fix.valid;
  if (!fix.valid) { speedMps=0; mapView.clearLocation(); return; }
  lastValidFix = millis(); speedMps = fix.speedMetersPerSecond;
  mapView.setLocation(fix.position, fix.headingDegrees);
  mapView.appendTrack(fix.position);
}
void setup() {
  if (!app.begin("04 GNSS tracker: SIM / UART")) return;
  gnssUart.begin(NICE_MAP_GNSS_BAUD, SERIAL_8N1, NICE_MAP_GNSS_RX, NICE_MAP_GNSS_TX);
  mapView.setFollow(true);
  Serial.println("m: SIM/UART, c: clear trail. UART uses real NMEA RMC/GGA, not simulated fallback.");
}
void loop() {
  while (Serial.available()) {
    const int key = Serial.read();
    if (app.handleKey(key)) continue;
    if (key == 'm') {
      realGnss = !realGnss; validFix = false; speedMps=0;
      mapView.clearLocation();mapView.clearTrack();parser = NmeaParser{};nextSample=0;
    }
    if (key == 'c') mapView.clearTrack();
  }
  const uint32_t now = millis();
  if (realGnss) {
    while (gnssUart.available()) if (parser.feed(char(gnssUart.read()))) useFix(parser.fix());
    // Read time AFTER accepting UART fixes: an older 'now' can unsigned-wrap
    // when useFix recorded a timestamp from the following millisecond.
    if (validFix && millis() - lastValidFix > 3000) {
      validFix = false;speedMps=0;mapView.clearLocation();
    }
  } else if (int32_t(now - nextSample) >= 0) {
    const double bearing = double((sampleIndex++ * 9) % 360);
    GnssFix fix; fix.valid = true;
    fix.position = destinationPoint(demo::home(), bearing, 120);
    fix.headingDegrees = std::fmod(bearing + 90, 360.0); fix.speedMetersPerSecond = 3.1;
    useFix(fix); nextSample = now + 1000;
  }
  char status[100];
  std::snprintf(status, sizeof(status), "%s %s %.1fm/s trail%c", realGnss ? "UART" : "SIM",
                validFix ? "FIX" : "NO FIX", speedMps, mapView.following() ? 'F' : '-');
  app.tick(status); delay(2);
}
