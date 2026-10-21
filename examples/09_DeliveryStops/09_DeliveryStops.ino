#include <demo/ExampleSupport.h>
#include <Navigation.h>
using namespace nice_gfx_map;
demo::Example app;
Map &mapView = app.mapView;
const GeoPoint stops[]={{30.6589,104.0634},{30.6597,104.0647},{30.6587,104.0660},{30.6576,104.0650}};
GeoPoint courier=demo::home();
size_t completed=0;
bool driving=false,denied=false;
uint32_t nextStep=0;
void updateStops() {
  Marker markers[4];
  for(size_t i=0;i<4;++i)markers[i]={stops[i],uint16_t(i<completed?0x07e0:i==completed?0xf800:0x001f),6,uint32_t(i+1)};
  mapView.setMarkers(markers,4);
  if(completed<4)mapView.setRoute(stops+completed,4-completed,0x001f,3);
  else mapView.setRoute(nullptr,0);
}
void setup() {
  if(!app.begin("09 Delivery stop workflow / SIM"))return;
  updateStops();mapView.fitPoints(stops,4,24);mapView.setLocation(courier);
  Serial.println("t: drive/pause toward next stop; c: confirm ONLY within 25m; r: restart. No optimization/planning.");
}
void loop() {
  while(Serial.available()) {
    const int key=Serial.read();if(app.handleKey(key))continue;
    if(key=='t'){driving=!driving;denied=false;}
    if(key=='r'){completed=0;courier=demo::home();driving=false;denied=false;updateStops();mapView.setLocation(courier);mapView.clearTrack();}
    if(key=='c'&&completed<4) {
      if(distanceMeters(courier,stops[completed])<=25){++completed;driving=false;denied=false;updateStops();Serial.println("Delivery confirmed");}
      else {denied=true;Serial.println("Rejected: next stop is more than 25m away");}
    }
  }
  const uint32_t now=millis();
  double distance=completed<4?distanceMeters(courier,stops[completed]):0;
  if(driving&&completed<4&&now-nextStep>=400) {
    const double bearing=initialBearingDegrees(courier,stops[completed]);
    courier=destinationPoint(courier,bearing,std::min(8.0,distance));
    mapView.setLocation(courier,bearing);mapView.appendTrack(courier);nextStep=now;
    if(distance<=15)driving=false;
    distance=distanceMeters(courier,stops[completed]);
  }
  if(distance<=25)denied=false;
  char status[100];std::snprintf(status,sizeof(status),"DONE %u/4 next%.0fm %s",unsigned(completed),distance,completed==4?"ALL DONE":denied?"TOO FAR":distance<=25?"PRESS c":driving?"DRIVING":"PAUSED");
  app.tick(status);delay(2);
}
