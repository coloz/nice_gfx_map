#include <demo/ExampleSupport.h>
#include <Navigation.h>
using namespace nice_gfx_map;
demo::Example app;
Map &mapView = app.mapView;
Marker vehicles[5];
int selected = -1;
uint32_t nextUpdate = 0;
double phase = 0;
void selectVehicle(int index) {
  selected=index;
  if(index>=0) {mapView.setLocation(vehicles[index].position);mapView.setFollow(true);}
  else {mapView.clearLocation();mapView.setFollow(false);}
}
void fitFleet() {
  GeoPoint points[5];for(size_t i=0;i<5;++i)points[i]=vehicles[i].position;
  selectVehicle(-1);mapView.fitPoints(points,5,20);
}
void updateFleet() {
  for(size_t i=0;i<5;++i) {
    const GeoPoint position=destinationPoint(demo::home(),phase+i*72,100+i*45);
    vehicles[i]={position,uint16_t(int(i)==selected?0xf800:0x001f),uint8_t(int(i)==selected?8:5),uint32_t(101+i)};
  }
  mapView.setMarkers(vehicles,5);
  if(selected>=0)mapView.setLocation(vehicles[selected].position,std::fmod(phase+selected*72+90,360.0));
}
void setup() {
  if(!app.begin("07 Fleet: five moving units / SIM"))return;
  updateFleet();fitFleet();
  Serial.println("1..5: select/follow unit, v: fit fleet, h: hit-test map center and select. IDs 101..105.");
}
void loop() {
  while(Serial.available()) {
    const int key=Serial.read();if(app.handleKey(key))continue;
    if(key>='1'&&key<='5'){selectVehicle(key-'1');updateFleet();}
    if(key=='v')fitFleet();
    if(key=='h') {
      const int32_t hit=mapView.markerAt({160,100},12);
      if(hit>=0){selectVehicle(hit);updateFleet();}
      Serial.printf("Center hit index=%ld (pan a marker to center first)\n",long(hit));
    }
  }
  const uint32_t now=millis();if(int32_t(now-nextUpdate)>=0){phase=std::fmod(phase+4,360.0);updateFleet();nextUpdate=now+1000;}
  char status[100];std::snprintf(status,sizeof(status),"5 units %s ID%u cache%u",selected<0?"OVERVIEW":mapView.following()?"FOLLOW":"SELECTED",selected<0?0:vehicles[selected].id,unsigned(mapView.cacheStats().readyTiles));
  app.tick(status);delay(2);
}
