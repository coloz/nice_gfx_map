// Guidance along a supplied route. This does not plan roads or turns.
#include <demo/ExampleSupport.h>
#include <Navigation.h>
using namespace nice_gfx_map;
demo::Example app;
Map &mapView = app.mapView;
RouteNavigator navigator;
TrackReplay ride;
const GeoPoint route[] = {{30.6575,104.0624},{30.6581,104.0640},
                         {30.6591,104.0648},{30.6600,104.0662}};
const TimedPoint samples[] = {{0,route[0]},{10000,route[1]},
                             {20000,route[2]},{35000,route[3]}};
uint32_t started = 0, nextUpdate = 0;
bool detour = false;
NavigationState navigation;
void resetRide() {
  navigator.setRoute(route, 4, 25, 55);
  mapView.clearTrack();detour=false;started = millis(); nextUpdate = 0;
}
void setup() {
  if (!app.begin("05 Cycle: supplied route / SIM")) return;
  mapView.setRoute(route, 4, 0x001f, 4); mapView.fitPoints(route, 4, 20);
  ride.setTrack(samples, 4); resetRide();
  Serial.println("o: 120m detour toggle, r: restart. OFF means off supplied route, ARR=arrival.");
}
void loop() {
  while (Serial.available()) {
    const int key = Serial.read(); if (app.handleKey(key)) continue;
    if (key == 'o') detour = !detour;
    if (key == 'r') resetRide();
  }
  const uint32_t now = millis();
  if (int32_t(now - nextUpdate) >= 0) {
    ReplaySample sample;
    if (ride.sample(now - started, sample, false) == Status::Ok) {
      GeoPoint position = detour ? destinationPoint(sample.position, 90, 120) : sample.position;
      navigator.update(position, navigation);
      mapView.setLocation(position, sample.headingDegrees); mapView.appendTrack(position);
    }
    nextUpdate = now + 250;
  }
  char status[100];
  std::snprintf(status, sizeof(status), "%s next%u %.0fm remain%.0fm %s", navigation.arrived ? "ARR" : "RIDE",
                unsigned(std::min(size_t(4),navigation.nextIndex + 1)), navigation.metersToNext,
                navigation.remainingMeters, navigation.offRoute ? "OFF!" : "ON");
  app.tick(status); delay(2);
}
