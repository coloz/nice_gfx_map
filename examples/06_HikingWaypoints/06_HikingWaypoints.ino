#include <demo/ExampleSupport.h>
#include <Navigation.h>
using namespace nice_gfx_map;
demo::Example app;
Map &mapView = app.mapView;
GeoPoint hiker = demo::home();
const GeoPoint waypoints[] = {{30.6596,104.0635},{30.6610,104.0656},
                             {30.6581,104.0680},{30.6568,104.0645}};
size_t selected = 0;
bool walking = false;
uint32_t nextStep = 0;
void updateWaypoints() {
  Marker markers[4];
  for (size_t i = 0; i < 4; ++i) markers[i] = {waypoints[i], uint16_t(i == selected ? 0xf800 : 0x001f), 6, uint32_t(i+1)};
  mapView.setMarkers(markers, 4);
  const GeoPoint line[] = {hiker, waypoints[selected]}; mapView.setRoute(line, 2, 0xf800, 2);
}
void fitAll() {
  GeoPoint points[5] = {hiker,waypoints[0],waypoints[1],waypoints[2],waypoints[3]};
  mapView.fitPoints(points, 5, 16);
}
void setup() {
  if (!app.begin("06 Hike: waypoint bearing / SIM")) return;
  updateWaypoints(); fitAll(); mapView.setLocation(hiker);
  Serial.println("n/b: select waypoint, t: walk/pause, v: fit all. Bearing is true north, not compass calibration.");
}
void loop() {
  while (Serial.available()) {
    const int key=Serial.read(); if(app.handleKey(key)) continue;
    if(key=='n') { selected=(selected+1)%4; updateWaypoints(); }
    if(key=='b') { selected=(selected+3)%4; updateWaypoints(); }
    if(key=='t') walking=!walking;
    if(key=='v') fitAll();
  }
  const uint32_t now=millis();
  const double distance=distanceMeters(hiker,waypoints[selected]);
  const double bearing=initialBearingDegrees(hiker,waypoints[selected]);
  if(distance<=3)walking=false;
  if(walking && now-nextStep>=500 && distance>3) {
    hiker=destinationPoint(hiker,bearing,std::min(4.0,distance));
    mapView.setLocation(hiker,bearing);mapView.appendTrack(hiker);updateWaypoints();nextStep=now;
  }
  char status[100];std::snprintf(status,sizeof(status),"WP%u %.0fm bearing%03.0f %s",unsigned(selected+1),distance,bearing,distance<=3?"ARR":walking?"WALK":"PAUSE");
  app.tick(status);delay(2);
}
