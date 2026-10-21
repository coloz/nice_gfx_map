#include <demo/ExampleSupport.h>
#include <Navigation.h>
using namespace nice_gfx_map;
demo::Example app;
Map &mapView=app.mapView;
TrackReplay replay;
const TimedPoint track[]={{0,{30.6576,104.0628}},{15000,{30.6582,104.0642}},
                         {35000,{30.6594,104.0650}},{50000,{30.6585,104.0665}},
                         {60000,{30.6576,104.0628}}};
constexpr uint32_t duration=60000;
double playhead=0,speed=1;
bool paused=false,looping=false;
uint32_t previousTime=0,nextSample=0;
ReplaySample current;
void fitTrack() {
  GeoPoint points[5];for(size_t i=0;i<5;++i)points[i]=track[i].position;
  mapView.setFollow(false);mapView.fitPoints(points,5,20);
}
void setup() {
  if(!app.begin("13 Timestamped track replay / SIM"))return;
  replay.setTrack(track,5);
  GeoPoint points[5];for(size_t i=0;i<5;++i)points[i]=track[i].position;
  mapView.setRoute(points,5,0x001f,2);fitTrack();previousTime=millis();
  Serial.println("SPACE pause; </> speed 0.25..8x; [/] seek -/+5s; r restart; l loop; v fit.");
}
void loop() {
  const uint32_t now=millis(),delta=now-previousTime;previousTime=now;
  if(!paused)playhead+=double(delta)*speed;
  if(looping&&playhead>=duration){playhead=std::fmod(playhead,double(duration));mapView.clearTrack();}
  else playhead=std::min(double(duration),playhead);
  while(Serial.available()) {
    const int key=Serial.read();if(app.handleKey(key))continue;
    if(key==' ')paused=!paused;
    if(key=='<')speed=std::max(0.25,speed/2);
    if(key=='>')speed=std::min(8.0,speed*2);
    if(key=='['){playhead=std::max(0.0,playhead-5000);mapView.clearTrack();nextSample=0;}
    if(key==']'){playhead=std::min(double(duration),playhead+5000);mapView.clearTrack();nextSample=0;}
    if(key=='r'){playhead=0;paused=false;mapView.clearTrack();nextSample=0;}
    if(key=='l')looping=!looping;
    if(key=='v')fitTrack();
  }
  if(int32_t(now-nextSample)>=0&&replay.sample(uint32_t(playhead),current,false)==Status::Ok) {
    mapView.setLocation(current.position,current.headingDegrees);
    if(!paused&&!current.finished)mapView.appendTrack(current.position);
    nextSample=now+200;
  }
  char status[100];std::snprintf(status,sizeof(status),"%s %.0f/60s %.2fx %s",paused?"PAUSE":current.finished?"DONE":"PLAY",playhead/1000,speed,looping?"LOOP":"ONCE");
  app.tick(status);delay(2);
}
