#pragma once
#if !defined(ARDUINO_ARCH_ESP32)
#error "16_TiandituServices is an ESP32-only device example."
#endif
#include <Arduino.h>
#include <WiFi.h>
#include <ctime>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <NICE_GFX.h>
#include <NICE_GFX_MAP.h>
#include <TiandituServices.h>
#include <adapters/Esp32.h>
#include <adapters/TiandituParser.h>
#include <adapters/NiceGfx.h>
#include <codecs/Esp32RasterDecoder.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include "ServiceDisplay.h"
#include "ServiceRequests.h"
#if __has_include("ServiceSecrets.h")
#include "ServiceSecrets.h"
#else
#include "ServiceSecrets.example.h"
#endif

namespace service_app {
using namespace nice_gfx_map;
namespace tdt=nice_gfx_map::tianditu;
constexpr size_t ResponseBytes=128*1024;
enum class WorkKind:uint8_t{Service,Tile};
struct Job : ServiceQuery {
  WorkKind kind=WorkKind::Service;
  TileRequest tile;
};
struct Completion {
  WorkKind kind=WorkKind::Service;
  char command[32]{};
  tdt::Report report;
  tdt::Result data;
  TileRequest tile;
  TileImage image;
  Status tileStatus=Status::NotFound,imageStatus=Status::NotFound;
  uint32_t elapsedMs=0;
};
// Exactly one job is outstanding. The queue handoff publishes ownership of
// these nontrivial C++ objects; no std::string/vector is copied by FreeRTOS.
inline Job job;
inline Completion completion;
inline QueueHandle_t requests=nullptr,completed=nullptr;
inline bool working=false,configured=false,displayReady=false,tilesEnabled=false;
inline bool automaticTiles=true;
inline bool clockStarted=false,hadWifi=false,hudDirty=true;
inline char statusLine[96]="Configure ServiceSecrets.h";
inline char line[320]{};
inline size_t lineSize=0;
inline bool lineOverflow=false;
inline uint32_t lastPaint=0;
inline uint64_t paintedRevision=0;
inline tdt::Result lastResult;
inline bool hasSuccessfulResult=false;
inline char lastCommand[32]{};
inline TileImage staticImage;
inline uint16_t scratch[service_display::Width*8];
inline MapConfig mapConfig(){MapConfig c;c.width=service_display::Width;c.height=service_display::Height-service_display::Header-service_display::Footer;c.cacheBytes=4*1024*1024;c.maxCachedTiles=32;c.maxInFlight=1;c.maxMarkers=24;c.maxRoutePoints=4096;c.maxPolygons=16;c.maxPolygonPoints=4096;return c;}
inline Map map(mapConfig());

inline bool networkConfigured(){return std::strlen(service_secrets::WifiSsid)>0&&std::strstr(service_secrets::WifiSsid,"YOUR_")==nullptr&&std::strstr(service_secrets::RootCa,"-----BEGIN CERTIFICATE-----");}
inline bool providerKeyConfigured(){return std::strlen(service_secrets::TiandituKey)>=16&&std::strstr(service_secrets::TiandituKey,"YOUR_")==nullptr;}
inline HttpOptions httpOptions(){HttpOptions o;o.caCertificate=service_secrets::RootCa;o.timeoutMs=10000;o.maxResponseBytes=ResponseBytes;o.allocator=esp32PsramAllocator();o.reuseConnections=false;return o;}
inline tdt::Limits serviceLimits(){tdt::Limits l;l.responseBytes=ResponseBytes;l.items=64;l.routes=8;l.geometryPoints=4096;l.textBytes=256;l.keepRaw=false;return l;}
inline void status(const char *s){std::snprintf(statusLine,sizeof(statusLine),"%s",s);hudDirty=true;}
inline std::string safeText(std::string s,size_t bytes=96){
  const std::string key=service_secrets::TiandituKey;
  if(!key.empty())for(size_t p=0;(p=s.find(key,p))!=std::string::npos;p+=10)s.replace(p,key.size(),"[redacted]");
  if(s.find("https://")!=std::string::npos||s.find("http://")!=std::string::npos)return "[URL omitted]";
  for(char &c:s)if(static_cast<unsigned char>(c)<32||c==127)c=' ';
  if(s.size()>bytes){while(bytes&&(static_cast<unsigned char>(s[bytes])&0xc0)==0x80)--bytes;s.resize(bytes);s+="...";}return s;
}
inline bool networkReady(){return configured&&WiFi.status()==WL_CONNECTED&&std::time(nullptr)>=1700000000;}


inline void worker(void *){
  Esp32HttpTransport http(httpOptions());tdt::ResponseParser parser;tdt::Client client(http,parser,serviceLimits());
  Esp32RasterDecoder decoder(esp32PsramAllocator(),0xffff,ResponseBytes);
  for(;;){Job *j=nullptr;if(xQueueReceive(requests,&j,portMAX_DELAY)!=pdTRUE||!j)continue;
    completion={};completion.kind=j->kind;std::snprintf(completion.command,sizeof(completion.command),"%s",j->command);const uint32_t started=millis();
    if(j->kind==WorkKind::Tile){completion.tile=std::move(j->tile);ByteBuffer bytes;completion.tileStatus=http.fetch(completion.tile,bytes);if(completion.tileStatus==Status::Ok)completion.tileStatus=decoder.decode(completion.tile,bytes.data(),bytes.size(),completion.image);}
    else {const auto request=buildRequest(*j,service_secrets::TiandituKey);completion.report=client.fetch(request,completion.data);
      if(completion.report.ok()&&completion.data.kind==tdt::Kind::StaticImage){TileRequest image;image.tileSize=256;completion.imageStatus=decoder.decode(image,completion.data.raw.data(),completion.data.raw.size(),completion.image);}}
    completion.elapsedMs=millis()-started;const uint8_t done=1;xQueueSend(completed,&done,portMAX_DELAY);
  }
}
inline void menu(){
  Serial.println("Tianditu LIVE ESP32 services; commands require Enter, UTF-8 arguments.");
  Serial.println("search [text] | viewport [text] | nearby [text] | suggest [prefix]");
  Serial.println("names [text] | polygon [text] | admin-search [text] | category [name/code] | statistics [text]");
  Serial.println("legacy <1|2|3|4|5|6|7|10> [text]  (historic endpoint may return 404)");
  Serial.println("geocode [address] | reverse | district [name] (children + boundary)");
  Serial.println("drive | shortest | avoid | walk   (preset destination; live service routes)");
  Serial.println("transit [1..15 policy mask] | transit-detail <uuid> | transit-reverse <lineUuid> <stationUuid>");
  Serial.println("static | pixels | map | home | tiles on/off | status | help | w/a/s/d | +/-");
  Serial.println("wfs-capabilities | wfs-describe [typeName] | wfs-features [typeName] (public read-only, no key)");
  Serial.println("Origin is the map center/preset, not a GNSS fix. No automatic request sweep.");
}
inline void showMenu(){if(!displayReady)return;service_display::clear();service_display::header("Tianditu / LIVE services / serial menu");
  service_display::text("search / nearby / viewport / suggest",0);service_display::text("names / polygon / category / statistics",1);service_display::text("geocode / reverse / district",2);service_display::text("drive / shortest / avoid / walk",3);service_display::text("transit / transit-detail / transit-reverse",4);service_display::text("static / pixels / map / status / help",5);service_display::text("wfs-capabilities / describe / features",6);service_display::footer(statusLine);}
inline void printReport(const Completion &c){
  Serial.printf("TDT service=%s ok=%u error=%s http=%d provider_present=%u provider=%lld elapsed_ms=%lu\n",c.command,unsigned(c.report.ok()),tdt::errorName(c.report.error),c.report.httpStatus,unsigned(c.report.hasProviderCode),static_cast<long long>(c.report.providerCode),static_cast<unsigned long>(c.elapsedMs));
  if(!c.report.providerCodeText.empty())Serial.printf("OGC code=%s\n",safeText(c.report.providerCodeText).c_str());
  if(!c.report.ok()&&!c.report.message.empty())Serial.printf("TDT_DETAIL message=%s\n",safeText(c.report.message).c_str());
  // Whitelisted summaries only. In particular never print drive XML/raw, which
  // may echo the service key, nor Request.url or a provider error body.
  if(!c.report.ok())return;
  const auto &r=c.data;Serial.printf("RESULT total=%lu places=%u suggestions=%u statistics=%u routes=%u districts=%u pixels=%u wfs_layers=%u fields=%u features=%u\n",static_cast<unsigned long>(r.total),unsigned(r.places.size()),unsigned(r.suggestions.size()),unsigned(r.statistics.size()),unsigned(r.routes.size()),unsigned(r.districts.size()),unsigned(r.pixels.size()),unsigned(r.wfsLayers.size()),unsigned(r.schemaFields.size()),unsigned(r.features.size()));
  if(r.hasLocation)Serial.printf("LOCATION %.7f,%.7f address=%s\n",r.location.latitude,r.location.longitude,safeText(r.formattedAddress).c_str());
  for(size_t i=0;i<std::min<size_t>(r.places.size(),3);++i){Serial.printf("POI %u id=%s name=%s address=%s distance_m=%.0f\n",unsigned(i),safeText(r.places[i].id).c_str(),safeText(r.places[i].name).c_str(),safeText(r.places[i].address).c_str(),r.places[i].distanceMeters);for(size_t n=0;n<std::min<size_t>(r.places[i].lines.size(),3);++n)Serial.printf("POI_LINE id=%s name=%s\n",safeText(r.places[i].lines[n].id).c_str(),safeText(r.places[i].lines[n].name).c_str());}
  for(size_t i=0;i<std::min<size_t>(r.suggestions.size(),3);++i)Serial.printf("SUGGESTION %u %s\n",unsigned(i),safeText(r.suggestions[i].name).c_str());
  for(size_t i=0;i<std::min<size_t>(r.statistics.size(),3);++i)Serial.printf("STATISTIC %s count=%lu\n",safeText(r.statistics[i].name).c_str(),static_cast<unsigned long>(r.statistics[i].count));
  unsigned printedIds=0,printedStations=0;for(size_t i=0;i<r.routes.size();++i){Serial.printf("ROUTE %u id=%s name=%s points=%u steps=%u meters=%.0f seconds=%.0f\n",unsigned(i),safeText(r.routes[i].id).c_str(),safeText(r.routes[i].name).c_str(),unsigned(r.routes[i].points.size()),unsigned(r.routes[i].steps.size()),r.routes[i].distanceMeters,r.routes[i].durationSeconds);for(const auto &step:r.routes[i].steps)if(!step.id.empty()&&printedIds++<8)Serial.printf("TRANSIT_LINE id=%s name=%s alternatives=%u\n",safeText(step.id).c_str(),safeText(step.name).c_str(),unsigned(step.alternativeCount));for(const auto &station:r.routes[i].stations)if(printedStations++<3)Serial.printf("TRANSIT_STATION id=%s name=%s\n",safeText(station.id).c_str(),safeText(station.name).c_str());}
  for(size_t i=0;i<std::min<size_t>(r.districts.size(),3);++i)Serial.printf("DISTRICT %s code=%s rings=%u\n",safeText(r.districts[i].name).c_str(),safeText(r.districts[i].code).c_str(),unsigned(r.districts[i].boundaries.size()));
  for(size_t i=0;i<r.pixels.size();++i)Serial.printf("PIXEL %u x=%.2f y=%.2f\n",unsigned(i),r.pixels[i].x,r.pixels[i].y);
  for(size_t i=0;i<std::min<size_t>(r.wfsLayers.size(),3);++i)Serial.printf("WFS_LAYER name=%s title=%s crs=%s\n",safeText(r.wfsLayers[i].name).c_str(),safeText(r.wfsLayers[i].title).c_str(),safeText(r.wfsLayers[i].crs).c_str());
  if(!r.serviceVersion.empty())Serial.printf("WFS_METADATA version=%s formats=%u operations=%u\n",safeText(r.serviceVersion).c_str(),unsigned(r.formats.size()),unsigned(r.operations.size()));
  for(size_t i=0;i<std::min<size_t>(r.formats.size(),3);++i)Serial.printf("WFS_FORMAT %s\n",safeText(r.formats[i]).c_str());
  for(size_t i=0;i<std::min<size_t>(r.operations.size(),3);++i)Serial.printf("WFS_OPERATION %s\n",safeText(r.operations[i]).c_str());
  for(size_t i=0;i<std::min<size_t>(r.schemaFields.size(),8);++i)Serial.printf("WFS_FIELD name=%s type=%s\n",safeText(r.schemaFields[i].name).c_str(),safeText(r.schemaFields[i].value).c_str());
  for(size_t i=0;i<r.features.size();++i){const auto &f=r.features[i];Serial.printf("WFS_FEATURE id=%s geometry=%u points=%u paths=%u properties=%u\n",safeText(f.id).c_str(),unsigned(f.geometryType),unsigned(f.points.size()),unsigned(f.paths.size()),unsigned(f.properties.size()));for(size_t p=0;p<std::min<size_t>(f.properties.size(),3);++p)Serial.printf("WFS_PROPERTY %s=%s\n",safeText(f.properties[p].name).c_str(),safeText(f.properties[p].value).c_str());}
}
inline void mapStatus(const char *feature,Status value){if(value!=Status::Ok)Serial.printf("MAP_OVERLAY feature=%s status=%s\n",feature,statusName(value));}
inline void applyResult(){
  std::vector<Marker> markers;std::vector<GeoPoint> extent;
  for(const auto &p:lastResult.places)if(p.hasPoint&&markers.size()<24){markers.push_back({p.point,0xf800,10,static_cast<uint32_t>(markers.size()+1),MarkerShape::Pin});extent.push_back(p.point);}
  if(lastResult.hasLocation&&markers.size()<24){markers.push_back({lastResult.location,0xf800,10,100,MarkerShape::Pin});extent.push_back(lastResult.location);}
  map.setRoute(nullptr,0);map.setPolygons(nullptr,0);
  if(!lastResult.routes.empty()&&lastResult.routes[0].points.size()>1){const auto &r=lastResult.routes[0];mapStatus("route",map.setRoute(r.points.data(),r.points.size(),0x33df,4));extent=r.points;}
  std::vector<PolygonOverlay> outlines;
  for(const auto &d:lastResult.districts)for(const auto &ring:d.boundaries){if(outlines.size()>=16)break;PolygonOverlay p;p.points=ring;p.fillOpacity=0;p.strokeColor=0xf800;p.strokeWidth=2;outlines.push_back(std::move(p));if(extent.empty())extent=ring;}
  bool firstLine=false;
  for(const auto &f:lastResult.features){
    if(f.geometryType==tdt::GeometryType::Point||f.geometryType==tdt::GeometryType::MultiPoint)for(auto p:f.points){if(markers.size()<24)markers.push_back({p,0xf800,10,static_cast<uint32_t>(markers.size()+1),MarkerShape::Pin});extent.push_back(p);}
    else if(f.geometryType==tdt::GeometryType::LineString||f.geometryType==tdt::GeometryType::MultiLineString){if(!firstLine){const auto &p=f.paths.empty()?f.points:f.paths[0];if(p.size()>1){mapStatus("wfs-line",map.setRoute(p.data(),p.size(),0x33df,4));extent.insert(extent.end(),p.begin(),p.end());firstLine=true;}}}
    else if(f.geometryType==tdt::GeometryType::Polygon||f.geometryType==tdt::GeometryType::MultiPolygon)for(const auto &ring:f.paths){if(outlines.size()>=16)break;PolygonOverlay p;p.points=ring;p.fillOpacity=0;p.strokeColor=0xf800;p.strokeWidth=2;outlines.push_back(std::move(p));extent.insert(extent.end(),ring.begin(),ring.end());}
    // A collection is reported with its counts/properties. Flattened parts are
    // not guessed into rings or connected across separate line segments.
  }
  mapStatus("markers",map.setMarkers(markers.data(),markers.size()));
  if(!outlines.empty())mapStatus("boundaries",map.setPolygons(outlines.data(),outlines.size()));
  if(!extent.empty())mapStatus("fit",map.fitPoints(extent.data(),extent.size(),32));
  tilesEnabled=true;paintedRevision=0;
}
inline std::string displayText(const std::string &input){const std::string safe=safeText(input,192);const size_t maximum=std::max(1,(service_display::Width-32)/16);size_t i=0,count=0;while(i<safe.size()&&count<maximum){const unsigned char c=safe[i];const size_t bytes=c<0x80?1:c<0xe0?2:c<0xf0?3:4;if(i+bytes>safe.size())break;i+=bytes;++count;}return safe.substr(0,i);}
inline void paintResultOverlay(){if(!displayReady||!hasSuccessfulResult)return;auto &d=service_display::display();const int y=service_display::Header+8;d.fillRect(8,y,service_display::Width-16,74,service_display::Paper);d.setTextColor(service_display::Ink,service_display::Paper);char counts[128];std::snprintf(counts,sizeof(counts),"Last OK: %s / total %lu / routes %u",lastCommand,static_cast<unsigned long>(lastResult.total),unsigned(lastResult.routes.size()));d.drawString(displayText(counts).c_str(),16,y+3);
  std::string first,second;if(!lastResult.places.empty()){first=lastResult.places[0].name;second=lastResult.places[0].address;}else if(!lastResult.suggestions.empty()){first=lastResult.suggestions[0].name;second=lastResult.suggestions[0].address;}else if(!lastResult.districts.empty()){first=lastResult.districts[0].name;second="Boundary outlines / rings";}else if(!lastResult.routes.empty()){first=lastResult.routes[0].name;std::snprintf(counts,sizeof(counts),"%.0f m / %.0f seconds / %u points",lastResult.routes[0].distanceMeters,lastResult.routes[0].durationSeconds,unsigned(lastResult.routes[0].points.size()));second=counts;}else if(lastResult.hasLocation){first=lastResult.formattedAddress;std::snprintf(counts,sizeof(counts),"%.5f, %.5f",lastResult.location.latitude,lastResult.location.longitude);second=counts;}else if(!lastResult.statistics.empty()){first=lastResult.statistics[0].name;std::snprintf(counts,sizeof(counts),"Count %lu",static_cast<unsigned long>(lastResult.statistics[0].count));second=counts;}else if(!lastResult.pixels.empty()){first="Static-map pixel coordinate response";std::snprintf(counts,sizeof(counts),"x %.1f / y %.1f",lastResult.pixels[0].x,lastResult.pixels[0].y);second=counts;}else if(!lastResult.features.empty()){const auto &f=lastResult.features[0];first="WFS feature "+f.id;std::snprintf(counts,sizeof(counts),"%u features / %u point(s) / %u paths",unsigned(lastResult.features.size()),unsigned(f.points.size()),unsigned(f.paths.size()));second=counts;}else if(!lastResult.wfsLayers.empty()){first=lastResult.wfsLayers[0].name;second=lastResult.wfsLayers[0].title;}else if(!lastResult.schemaFields.empty()){first=lastResult.schemaFields[0].name;second=lastResult.schemaFields[0].value;}else first="Successful empty service result";d.drawString(displayText(first).c_str(),16,y+25);d.drawString(displayText(second).c_str(),16,y+47);}
inline void renderStatic(){if(!displayReady||!staticImage.valid())return;service_display::clear();auto &d=service_display::display();const int x=std::max(0,(service_display::Width-int(staticImage.width()))/2),y=service_display::Header+std::max(0,(int(map.height())-int(staticImage.height()))/2);
  const int rows=std::min<int>(staticImage.height(),service_display::Height-service_display::Footer-y);if(rows>0){service_display::lastError=d.flush(x,y,nice_gfx::PixelView(staticImage.pixels(),staticImage.width(),rows,staticImage.width(),nice_gfx::PixelOrder::Native),true);const auto waited=d.wait();if(service_display::lastError==nice_gfx::Status::Ok)service_display::lastError=waited;if(service_display::lastError!=nice_gfx::Status::Ok)Serial.printf("STATIC display_error=%u\n",unsigned(service_display::lastError));}}
inline void takeCompletion(){uint8_t done=0;if(!working||xQueueReceive(completed,&done,0)!=pdTRUE)return;working=false;
  if(completion.kind==WorkKind::Tile){if(completion.tileStatus==Status::Ok)map.submit(completion.tile,std::move(completion.image));else{map.fail(completion.tile,completion.tileStatus,millis());Serial.printf("TILE error=%s\n",statusName(completion.tileStatus));}return;}
  printReport(completion);char summary[96];std::snprintf(summary,sizeof(summary),"%s / %s / HTTP %d",completion.command,tdt::errorName(completion.report.error),completion.report.httpStatus);status(summary);
  bool imageFailed=false;if(completion.report.ok()){lastResult=std::move(completion.data);hasSuccessfulResult=true;std::snprintf(lastCommand,sizeof(lastCommand),"%s",completion.command);if(lastResult.kind==tdt::Kind::StaticImage){tilesEnabled=false;staticImage=std::move(completion.image);if(completion.imageStatus==Status::Ok)renderStatic();else{imageFailed=true;Serial.printf("STATIC decode=%s\n",statusName(completion.imageStatus));std::snprintf(summary,sizeof(summary),"static / decode %s",statusName(completion.imageStatus));status(summary);}}else{staticImage.reset();if(providerKeyConfigured())applyResult();else{tilesEnabled=false;if(displayReady){service_display::clear();paintResultOverlay();}}}}
  if(displayReady){service_display::header(imageFailed?"Tianditu / image decode failed":"Tianditu / real service result");service_display::footer(statusLine);}
}
inline bool sendJob(){Job *pointer=&job;if(!requests||xQueueSend(requests,&pointer,0)!=pdTRUE)return false;working=true;return true;}
inline void command(char *input){
  while(*input==' ')++input;char *args=std::strchr(input,' ');if(args){*args++=0;while(*args==' ')++args;}else args=input+std::strlen(input);
  if(!*input)return;
  if(!std::strcmp(input,"help")){menu();tilesEnabled=false;showMenu();return;}
  if(!std::strcmp(input,"status")){const auto center=map.center();Serial.printf("STATE configured=%u provider_key_set=%u wifi=%u clock=%u worker_busy=%u tiles=%u automatic_tiles=%u ready_tiles=%u heap=%lu display_ready=%u display_error=%u center_lat=%.7f center_lon=%.7f zoom=%.3f psram_free=%lu psram_largest=%lu\n",unsigned(configured),unsigned(providerKeyConfigured()),unsigned(WiFi.status()==WL_CONNECTED),unsigned(std::time(nullptr)>=1700000000),unsigned(working),unsigned(tilesEnabled),unsigned(automaticTiles),unsigned(map.cacheStats().readyTiles),static_cast<unsigned long>(ESP.getFreeHeap()),unsigned(displayReady),unsigned(service_display::lastError),center.latitude,center.longitude,map.viewZoom(),static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)),static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)));return;}
  if(!std::strcmp(input,"home")){if(working){Serial.println("BUSY: home waits for the current bounded request");return;}map.setCenter(PresetOrigin);map.setZoom(15);map.setMarkers(nullptr,0);map.setRoute(nullptr,0);map.setPolygons(nullptr,0);staticImage.reset();lastResult={};hasSuccessfulResult=false;lastCommand[0]=0;tilesEnabled=providerKeyConfigured();paintedRevision=0;status("Preset map center / not a GNSS fix");if(displayReady){service_display::clear();service_display::header("Tianditu / preset map center");}Serial.printf("HOME preset=1 center_lat=%.7f center_lon=%.7f zoom=15 automatic_tiles=%u\n",PresetOrigin.latitude,PresetOrigin.longitude,unsigned(automaticTiles));return;}
  if(!std::strcmp(input,"tiles")){if(!std::strcmp(args,"off"))automaticTiles=false;else if(!std::strcmp(args,"on"))automaticTiles=true;else{Serial.println("ERR use tiles on or tiles off");return;}Serial.printf("TILES automatic=%u\n",unsigned(automaticTiles));return;}
  if(!std::strcmp(input,"map")){if(!providerKeyConfigured()){Serial.println("ERR map requires configured Tianditu key");return;}staticImage.reset();tilesEnabled=true;automaticTiles=true;paintedRevision=0;status("Tianditu / live WMTS map");return;}
  if(std::strlen(input)==1&&std::strchr("wasd+-",input[0])){if(!providerKeyConfigured()){Serial.println("ERR map requires configured Tianditu key");return;}const char c=*input;if(c=='+'||c=='-')map.setViewZoomAt(std::max(1.0,std::min(18.0,map.viewZoom()+(c=='+'?1:-1))),map.width()/2,map.height()/2);else map.panBy(c=='a'?80:c=='d'?-80:0,c=='w'?80:c=='s'?-80:0);staticImage.reset();tilesEnabled=true;return;}
  static const char *names[]={"search","viewport","nearby","suggest","names","polygon","admin-search","category","statistics","legacy","geocode","reverse","district","drive","shortest","avoid","walk","transit","transit-detail","transit-reverse","static","pixels","wfs-capabilities","wfs-describe","wfs-features"};
  bool known=false;for(const char *name:names)if(!std::strcmp(input,name))known=true;if(!known){Serial.println("ERR unknown command; use help");return;}
  if(std::strncmp(input,"wfs-",4)&&!providerKeyConfigured()){Serial.println("ERR service requires configured Tianditu key");return;}
  if(working){Serial.println("BUSY: wait for the current bounded request");return;}
  if(!networkReady()){Serial.println("ERR local configuration/Wi-Fi/secure clock not ready");return;}
  if(std::strlen(args)>=sizeof(job.arguments)){Serial.println("ERR arguments too long");return;}
  job={};job.kind=WorkKind::Service;std::snprintf(job.command,sizeof(job.command),"%s",input);std::snprintf(job.arguments,sizeof(job.arguments),"%s",args);job.origin=map.center();job.bounds={job.origin.latitude-.025,job.origin.longitude-.035,job.origin.latitude+.025,job.origin.longitude+.035};
  if(sendJob())status("Tianditu / requesting in worker");else Serial.println("ERR worker unavailable");
}
inline void serial(){while(Serial.available()){const char c=char(Serial.read());if(c=='\r'||c=='\n'){if(lineOverflow)Serial.println("ERR command exceeds 319 bytes");else if(lineSize){line[lineSize]=0;command(line);}lineSize=0;lineOverflow=false;}else if(c=='\b'||c==127){if(lineSize)--lineSize;}else if(static_cast<unsigned char>(c)>=32){if(lineSize+1<sizeof(line))line[lineSize++]=c;else lineOverflow=true;}}}
inline void begin(){Serial.begin(115200);esp_log_level_set("HTTP_CLIENT",ESP_LOG_NONE);displayReady=service_display::begin();configured=networkConfigured();map.setCenter(PresetOrigin);map.setZoom(15);if(providerKeyConfigured())map.setSource(providers::tianditu(service_secrets::TiandituKey,true));map.setFollow(false);
  requests=xQueueCreate(1,sizeof(Job *));completed=xQueueCreate(1,sizeof(uint8_t));if(!requests||!completed||xTaskCreatePinnedToCore(worker,"tdt-services",16384,nullptr,1,nullptr,0)!=pdPASS){if(requests)vQueueDelete(requests);if(completed)vQueueDelete(completed);requests=completed=nullptr;Serial.println("ERR worker allocation failed");status("Worker unavailable");}else if(configured){WiFi.mode(WIFI_STA);WiFi.begin(service_secrets::WifiSsid,service_secrets::WifiPassword);status("Connecting Wi-Fi / use status");}else Serial.println("Configure local ServiceSecrets.h; no network started.");menu();showMenu();}
inline void poll(){
  takeCompletion();serial();const bool connected=WiFi.status()==WL_CONNECTED;if(configured&&connected&&!clockStarted){configTime(0,0,"pool.ntp.org","time.cloudflare.com");clockStarted=true;status("Synchronizing verified TLS clock");}if(connected&&!hadWifi)Serial.println("Wi-Fi connected; waiting for secure clock");hadWifi=connected;
  if(clockStarted&&std::time(nullptr)>=1700000000&&std::strstr(statusLine,"Synchronizing"))status("Ready / enter a service command");
  if(!working&&tilesEnabled&&automaticTiles&&networkReady()&&requests){job={};job.kind=WorkKind::Tile;if(map.nextRequest(job.tile,millis())==Status::Ok)sendJob();}
  if(displayReady&&tilesEnabled&&map.revision()!=paintedRevision&&millis()-lastPaint>=66){lastPaint=millis();const auto drawn=NiceGfxRenderer::draw(map,service_display::display(),0,service_display::Header,scratch,sizeof(scratch)/sizeof(scratch[0]));service_display::lastError=drawn==Status::Ok?nice_gfx::Status::Ok:nice_gfx::Status::IoError;if(drawn==Status::Ok){paintedRevision=map.revision();paintResultOverlay();}hudDirty=true;}
  if(displayReady&&hudDirty){service_display::footer(statusLine);hudDirty=false;}delay(1);
}
} // namespace service_app
