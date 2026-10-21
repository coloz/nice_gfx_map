#include <demo/ExampleSupport.h>
#include <Navigation.h>
using namespace nice_gfx_map;
demo::Example app;
Map &mapView=app.mapView;
PolygonOverlay fields[3];
GeoPoint centers[3];
double moisture[3]={28,52,76};
double areaSqMeters[3]={0,0,0};
size_t selected=0;
bool irrigation=false;
uint32_t nextSample=0;
void updateFields() {
  for(size_t i=0;i<3;++i) {
    fields[i].fillColor=moisture[i]<35?0xf800:moisture[i]<65?0xffe0:0x07e0;
    fields[i].fillOpacity=75;fields[i].strokeWidth=i==selected?4:1;
    fields[i].strokeColor=i==selected?0x001f:0x4208;
  }
  mapView.setPolygons(fields,3);
  Marker marker{centers[selected],0x001f,5,uint32_t(selected+1)};mapView.setMarkers(&marker,1);
}
void fitFields() {
  GeoPoint points[12];size_t index=0;
  for(const auto &field:fields)for(const auto point:field.points)points[index++]=point;
  mapView.fitPoints(points,index,18);
}
void setup() {
  if(!app.begin("12 Agriculture zones / SIM sensors"))return;
  for(size_t i=0;i<3;++i) {
    centers[i]=destinationPoint(demo::home(),i==0?270:90,std::abs(double(i)*160-160));
    const double bearings[]={315,45,135,225};
    for(double bearing:bearings)fields[i].points.push_back(destinationPoint(centers[i],bearing,80));
    areaSqMeters[i]=distanceMeters(fields[i].points[0],fields[i].points[1])*
                    distanceMeters(fields[i].points[1],fields[i].points[2]);
  }
  updateFields();fitFields();
  Serial.println("1/2/3: field, i: selected irrigation simulation, v: fit all, h: center-marker hit test.");
  Serial.println("Area is an approximate rectangular demo estimate, not cadastral/survey output.");
}
void loop() {
  while(Serial.available()) {
    const int key=Serial.read();if(app.handleKey(key))continue;
    if(key>='1'&&key<='3'){selected=size_t(key-'1');irrigation=false;updateFields();}
    if(key=='i')irrigation=!irrigation;
    if(key=='v')fitFields();
    if(key=='h')Serial.printf("Selected marker hit=%ld\n",long(mapView.markerAt(mapView.toScreen(centers[selected]),1)));
  }
  const uint32_t now=millis();if(now-nextSample>=1000) {
    for(size_t i=0;i<3;++i)moisture[i]=std::max(0.0,std::min(100.0,moisture[i]+(irrigation&&i==selected?1.5:-0.08)));
    updateFields();nextSample=now;
  }
  char status[100];std::snprintf(status,sizeof(status),"PLOT%u %.0f%% ~%.2fha pump%s",unsigned(selected+1),moisture[selected],areaSqMeters[selected]/10000,irrigation?"SIM ON":"OFF");
  app.tick(status);delay(2);
}
