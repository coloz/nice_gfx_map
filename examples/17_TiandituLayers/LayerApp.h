#pragma once
#if !defined(ARDUINO_ARCH_ESP32)
#error "17_TiandituLayers is an ESP32 device example."
#endif
#include <Arduino.h>
#include <WiFi.h>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <NICE_GFX.h>
#include <NICE_GFX_MAP.h>
#include <adapters/Esp32.h>
#include <adapters/NiceGfx.h>
#include <codecs/Esp32RasterDecoder.h>
#include "LayerDisplay.h"
#include "LayerSelection.h"
#if __has_include("LayerSecrets.h")
#include "LayerSecrets.h"
#else
#include "LayerSecrets.example.h"
#endif

namespace layer_app {
using namespace nice_gfx_map;
using providers::TiandituLayer;
enum class Kind : uint8_t { Profile, Tile };
struct Job { Kind kind=Kind::Profile; uint32_t version=0; Selection selection; TileRequest request; };
struct Completion { Kind kind=Kind::Profile; uint32_t version=0; Selection selection; Status status=Status::NotFound; providers::TileProfile profile; TileRequest request; TileImage image; };
inline MapConfig configuration() { MapConfig c; c.width=layer_display::Width;c.height=layer_display::Height-layer_display::Header-layer_display::Footer;
  c.cacheBytes=NICE_TDT_LAYERS_KORVO?8*1024*1024:4*1024*1024;c.compositeCacheBytes=1024*1024;c.maxCachedTiles=64;c.maxInFlight=1;return c; }
inline Map map(configuration());
inline Selection selected,applied;
inline Job job;
inline Completion completion;
inline QueueHandle_t requests=nullptr,completed=nullptr;
inline bool busy=false,dirty=true,configured=false,displayReady=false,clockStarted=false,hudDirty=true,hasProfile=false;
inline uint32_t version=1,lastPaint=0;
inline uint64_t paintedRevision=0;
inline uint64_t displayErrors=0;
inline Status lastStatus=Status::NotFound;
inline char input[128]{};
inline size_t inputSize=0;
inline bool overflow=false;
inline uint16_t scratch[layer_display::Width*8];
inline HttpOptions options() { HttpOptions o;o.caCertificate=layer_secrets::RootCa;o.timeoutMs=10000;o.maxResponseBytes=256*1024;o.allocator=esp32PsramAllocator();return o; }
inline bool networkReady() { return configured&&WiFi.status()==WL_CONNECTED&&std::time(nullptr)>=1700000000; }
inline void worker(void *) {
  Esp32HttpTransport http(options());Esp32RasterDecoder decoder(esp32PsramAllocator(),0xffff,256*1024);
  for(;;) {Job *next=nullptr;if(xQueueReceive(requests,&next,portMAX_DELAY)!=pdTRUE||!next)continue;
    completion={};completion.kind=next->kind;completion.version=next->version;completion.selection=next->selection;
    if(next->kind==Kind::Profile)completion.status=buildProfile(http,next->selection,layer_secrets::TiandituKey,completion.profile);
    else {completion.request=std::move(next->request);ByteBuffer bytes;completion.status=http.fetch(completion.request,bytes);
      if(completion.status==Status::Ok)completion.status=decoder.decode(completion.request,bytes.data(),bytes.size(),completion.image);}
    const uint8_t done=1;xQueueSend(completed,&done,portMAX_DELAY);
  }
}
inline void state() {
  const auto c=map.cacheStats();const auto center=map.center();
  const bool markerVisible=map.markerAt(map.toScreen({30.6586,104.0648}))==0;
  Serial.printf("{\"kind\":\"wmts-status\",\"configured\":%s,\"wifi\":%s,\"clock\":%s,\"worker_busy\":%s,\"profile_pending\":%s,\"has_profile\":%s,\"status\":\"%s\",\"projection\":\"%s\",\"layer\":\"%s\",\"layers\":%u,\"warnings\":%lu,\"min_zoom\":%u,\"max_zoom\":%u,\"view_zoom\":%.6f,\"latitude\":%.8f,\"longitude\":%.8f,\"ready\":%u,\"inflight\":%u,\"failures\":%llu,\"cache_bytes\":%u,\"composite_bytes\":%u,\"fixture_route_points\":3,\"fixture_markers\":1,\"fixture_marker_hit_visible\":%s,\"display_ready\":%s,\"display_errors\":%llu,\"psram_free\":%u,\"psram_largest\":%u}\n",
    configured?"true":"false",WiFi.status()==WL_CONNECTED?"true":"false",std::time(nullptr)>=1700000000?"true":"false",busy?"true":"false",(dirty||(busy&&job.kind==Kind::Profile))?"true":"false",hasProfile?"true":"false",statusName(lastStatus),map.sourceGrid().projection==Projection::Geographic?"c":"w",providers::tiandituLayerId(applied.layer),unsigned(map.source().layers.size()),static_cast<unsigned long>(map.sourceGrid().metadataWarnings),unsigned(map.source().minZoom),unsigned(map.source().maxZoom),map.viewZoom(),center.latitude,center.longitude,unsigned(c.readyTiles),unsigned(c.inFlight),static_cast<unsigned long long>(c.failures),unsigned(c.bytes),unsigned(c.compositeBytes),markerVisible?"true":"false",displayReady?"true":"false",static_cast<unsigned long long>(displayErrors),unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}
inline void menu() {
  Serial.println("WMTS live layers: layer vec|cva|img|cia|ter|cta|ibo; projection w|c");
  Serial.println("labels 0|1; borders 0|1; strict 0|1; center <lat> <lon>; zoom <level>; pan <dx> <dy>");
  Serial.println("w/a/s/d; +/-; reload; status; help. Commands require Enter; no automatic sweep.");
  Serial.println("cva_w limited to1..18; non-strict cta_w uses a reported metadata correction. Center is a preset/map center, not GNSS.");
}
inline void changed() {++version;if(!version)++version;dirty=true;hudDirty=true;}
inline void command(const char *line) {
  char name[24]{},value[24]{};double a=0,b=0;
  if(std::sscanf(line,"%23s",name)!=1)return;
  if(!std::strcmp(name,"status")){state();return;}if(!std::strcmp(name,"help")){menu();return;}
  if(!std::strcmp(name,"reload")){changed();return;}
  if(!std::strcmp(name,"layer")&&std::sscanf(line,"%*s %23s",value)==1){for(unsigned i=0;i<7;++i)if(!std::strcmp(value,providers::tiandituLayerId(TiandituLayer(i)))){selected.layer=TiandituLayer(i);changed();return;}}
  else if(!std::strcmp(name,"projection")&&std::sscanf(line,"%*s %23s",value)==1){if(!std::strcmp(value,"w")||!std::strcmp(value,"c")){selected.projection=value[0]=='c'?Projection::Geographic:Projection::WebMercator;changed();return;}}
  else if((!std::strcmp(name,"labels")||!std::strcmp(name,"borders")||!std::strcmp(name,"strict"))&&std::sscanf(line,"%*s %lf",&a)==1&&(a==0||a==1)){
    if(!std::strcmp(name,"labels"))selected.labels=a!=0;else if(!std::strcmp(name,"borders"))selected.borders=a!=0;else selected.strict=a!=0;changed();return;}
  else if(!std::strcmp(name,"center")&&std::sscanf(line,"%*s %lf %lf",&a,&b)==2){lastStatus=map.setCenter({a,b});hudDirty=true;return;}
  else if(!std::strcmp(name,"pan")&&std::sscanf(line,"%*s %lf %lf",&a,&b)==2){lastStatus=map.panBy(a,b);hudDirty=true;return;}
  else if(!std::strcmp(name,"zoom")&&std::sscanf(line,"%*s %lf",&a)==1){lastStatus=map.setViewZoomAt(a,double(map.width())/2,double(map.height())/2);hudDirty=true;return;}
  else if(std::strlen(name)==1){switch(name[0]){case 'w':lastStatus=map.panBy(0,64);return;case 's':lastStatus=map.panBy(0,-64);return;case 'a':lastStatus=map.panBy(64,0);return;case 'd':lastStatus=map.panBy(-64,0);return;
    case '+':lastStatus=map.setZoom(std::min<unsigned>(map.source().maxZoom,map.zoom()+1));return;case '-':lastStatus=map.setZoom(std::max<unsigned>(map.source().minZoom,map.zoom()?map.zoom()-1:0));return;}}
  Serial.println("ERR invalid command; use help");
}
inline void serial() {while(Serial.available()){const char c=char(Serial.read());if(c=='\r'||c=='\n'){if(!overflow&&inputSize){input[inputSize]=0;command(input);}else if(overflow)Serial.println("ERR command too long");inputSize=0;overflow=false;}else if(inputSize+1<sizeof(input))input[inputSize++]=c;else overflow=true;}}
inline void takeCompletion() {
  uint8_t done=0;if(!completed||xQueueReceive(completed,&done,0)!=pdTRUE)return;busy=false;
  if(completion.kind==Kind::Profile){if(completion.version!=version)return;lastStatus=completion.status;
    if(lastStatus==Status::Ok){lastStatus=map.setSource(completion.profile.source,completion.profile.grid);if(lastStatus==Status::Ok){applied=completion.selection;hasProfile=true;}}
    Serial.printf("{\"kind\":\"wmts-profile\",\"version\":%lu,\"status\":\"%s\",\"warnings\":%lu}\n",static_cast<unsigned long>(version),statusName(lastStatus),static_cast<unsigned long>(map.sourceGrid().metadataWarnings));state();}
  else {lastStatus=completion.status;if(lastStatus==Status::Ok)lastStatus=map.submit(completion.request,std::move(completion.image));else map.fail(completion.request,lastStatus,millis());
    if(lastStatus!=Status::Ok&&lastStatus!=Status::Canceled)Serial.printf("{\"kind\":\"wmts-tile\",\"status\":\"%s\"}\n",statusName(lastStatus));}
  hudDirty=true;
}
inline void schedule() {
  if(busy||!requests||!networkReady())return;
  job={};job.version=version;job.selection=selected;
  if(dirty){job.kind=Kind::Profile;dirty=false;}
  else {if(!hasProfile)return;job.kind=Kind::Tile;const auto status=map.nextRequest(job.request,millis());if(status!=Status::Ok){if(status!=Status::NotFound&&status!=Status::Busy){lastStatus=status;hudDirty=true;}return;}}
  Job *pointer=&job;if(xQueueSend(requests,&pointer,0)==pdTRUE)busy=true;else {if(job.kind==Kind::Profile)dirty=true;else map.fail(job.request,Status::Busy,millis());}
}
inline void paint() {
  if(!displayReady||millis()-lastPaint<100||(!hudDirty&&paintedRevision==map.revision()))return;lastPaint=millis();
  if(paintedRevision!=map.revision()){const auto status=NiceGfxRenderer::draw(map,layer_display::display(),0,layer_display::Header,scratch,sizeof(scratch)/sizeof(*scratch));if(status==Status::Ok)paintedRevision=map.revision();else {lastStatus=status;++displayErrors;}}
  char title[96],footer[128];const auto stats=map.cacheStats();
  std::snprintf(title,sizeof(title),"Tianditu %s_%c / z%.2f / %u layers",providers::tiandituLayerId(applied.layer),map.sourceGrid().projection==Projection::Geographic?'c':'w',map.viewZoom(),unsigned(map.source().layers.size()));
  std::snprintf(footer,sizeof(footer),"Map data: Tianditu | %s | ready %u | warnings %lu",statusName(lastStatus),unsigned(stats.readyTiles),static_cast<unsigned long>(map.sourceGrid().metadataWarnings));
  layer_display::bands(title,footer);hudDirty=false;
}
inline void begin() {
  Serial.begin(115200);esp_log_level_set("HTTP_CLIENT",ESP_LOG_NONE);displayReady=layer_display::begin();
  configured=std::strstr(layer_secrets::WifiSsid,"YOUR_")==nullptr&&std::strlen(layer_secrets::WifiSsid)>0&&std::strlen(layer_secrets::TiandituKey)>=16&&std::strstr(layer_secrets::TiandituKey,"YOUR_")==nullptr&&std::strstr(layer_secrets::RootCa,"-----BEGIN CERTIFICATE-----");
  map.setCenter({30.6586,104.0648});map.setZoom(14);const GeoPoint route[]={{30.656,104.061},{30.659,104.066},{30.662,104.070}};map.setRoute(route,3,0x001f,4);
  const Marker marker{{30.6586,104.0648},0xf800,10,1,MarkerShape::Pin};map.setMarkers(&marker,1);
  requests=xQueueCreate(1,sizeof(Job*));completed=xQueueCreate(1,sizeof(uint8_t));
  const BaseType_t core=0; // Keep network work off the S31 UI/SIMD core; IDF may migrate PIE users.
  if(!requests||!completed||xTaskCreatePinnedToCore(worker,"tdt-wmts",16384,nullptr,1,nullptr,core)!=pdPASS){if(requests)vQueueDelete(requests);if(completed)vQueueDelete(completed);requests=completed=nullptr;Serial.println("ERR worker allocation failed");}
  else if(configured){WiFi.mode(WIFI_STA);WiFi.begin(layer_secrets::WifiSsid,layer_secrets::WifiPassword);}else Serial.println("Configure local LayerSecrets.h; no network started.");
  menu();state();paint();
}
inline void poll() {takeCompletion();serial();if(configured&&WiFi.status()==WL_CONNECTED&&!clockStarted){configTime(0,0,"pool.ntp.org","time.cloudflare.com");clockStarted=true;}schedule();paint();delay(2);}
}
