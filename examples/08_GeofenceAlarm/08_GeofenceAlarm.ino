#include <demo/ExampleSupport.h>
#include <Navigation.h>
using namespace nice_gfx_map;
demo::Example app;
Map &mapView = app.mapView;
GeoFence fence;
FenceResult fenceState;
bool polygonMode=false;
double hysteresis=8;
unsigned entered=0,exited=0;
uint32_t nextUpdate=0;
double phase=0;
GeoPoint corners[4];
void configureFence() {
  fence.reset();
  if(polygonMode) {
    fence.setPolygon(corners,4,hysteresis);
    PolygonOverlay polygon;polygon.points.assign(corners,corners+4);
    polygon.fillColor=0x07e0;polygon.fillOpacity=45;polygon.strokeColor=0x03e0;
    mapView.setPolygons(&polygon,1);mapView.setCircles(nullptr,0);
  } else {
    fence.setCircle(demo::home(),100,hysteresis);
    CircleOverlay circle;circle.center=demo::home();circle.radiusMeters=100;
    mapView.setCircles(&circle,1);mapView.setPolygons(nullptr,0);
  }
  entered=exited=0;
}
void setup() {
  if(!app.begin("08 Geofence transitions / SIM"))return;
  const double bearings[]={315,45,135,225};
  for(size_t i=0;i<4;++i)corners[i]=destinationPoint(demo::home(),bearings[i],145);
  configureFence();mapView.fitPoints(corners,4,32);
  Serial.println("p: circle/polygon, h: hysteresis 8/25m. ENTER/EXIT are state transitions, not every position.");
}
void loop() {
  while(Serial.available()) {
    const int key=Serial.read();if(app.handleKey(key))continue;
    if(key=='p'){polygonMode=!polygonMode;configureFence();}
    if(key=='h'){hysteresis=hysteresis<10?25:8;configureFence();}
  }
  const uint32_t now=millis();if(int32_t(now-nextUpdate)>=0) {
    phase+=0.09;
    // A slow 65..135m boundary crossing with small synthetic GNSS jitter.
    const double radius=100+35*std::sin(phase)+3*std::sin(phase*19);
    const GeoPoint position=destinationPoint(demo::home(),90,radius);
    if(fence.update(position,fenceState)==Status::Ok) {
      if(fenceState.entered){++entered;Serial.println("ENTER");}
      if(fenceState.exited){++exited;Serial.println("EXIT / ALARM");}
    }
    mapView.setLocation(position,90);nextUpdate=now+250;
  }
  char status[100];std::snprintf(status,sizeof(status),"%s %s H%.0fm E%u X%u",polygonMode?"POLY":"CIRCLE",fenceState.inside?"IN":"OUT!",hysteresis,entered,exited);
  app.tick(status);delay(2);
}
