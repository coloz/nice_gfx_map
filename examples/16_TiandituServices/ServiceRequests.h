#pragma once
// Portable command-to-request mapping shared by the real device worker and
// host contract tests. No I/O, platform headers or credentials live here.
#include <TiandituServices.h>
#include <cstdlib>
#include <cstring>
namespace service_app {
using namespace nice_gfx_map;
namespace tdt=nice_gfx_map::tianditu;
constexpr GeoPoint PresetOrigin{30.658637,104.064873};
constexpr GeoPoint PresetDestination{30.66200,104.05360};
// This endpoint produced an actual transit plan/line-detail/reverse sequence;
// the returned identifiers remain runtime data rather than fixed fake IDs.
constexpr GeoPoint PresetTransitDestination{30.6595,104.0708};
struct ServiceQuery {
  char command[32]{},arguments[240]{};
  GeoPoint origin{};
  GeoBounds bounds{};
};
inline tdt::Request buildRequest(const ServiceQuery &j,const std::string &key){
  const std::string arg=j.arguments;
  const auto is=[&](const char *name){return std::strcmp(j.command,name)==0;};
  if(is("wfs-capabilities"))return tdt::wfsCapabilities();
  if(is("wfs-describe"))return tdt::wfsDescribe(arg.empty()?"TDTService:RESP":arg);
  if(is("wfs-features")){tdt::WfsOptions o;o.typeName=arg.empty()?"TDTService:RESP":arg;o.bounds={30.63,104.03,30.69,104.09};o.maxFeatures=2;return tdt::wfsFeatures(o);}
  tdt::SearchOptions s;s.center=j.origin;s.bounds=j.bounds;s.keyword=arg.empty()?"公园":arg;s.count=3;s.level=16;
  if(is("search"))return tdt::search(key,s);
  if(is("viewport")){s.mode=tdt::SearchMode::Viewport;return tdt::search(key,s);}
  if(is("nearby")){s.mode=tdt::SearchMode::Nearby;s.keyword=arg.empty()?"公共厕所":arg;s.radiusMeters=3000;return tdt::search(key,s);}
  if(is("suggest")){s.mode=tdt::SearchMode::Suggestions;return tdt::search(key,s);}
  if(is("names")){s.mode=tdt::SearchMode::PlaceNames;return tdt::search(key,s);}
  if(is("polygon")){s.mode=tdt::SearchMode::Polygon;s.polygon={{j.origin.latitude-.02,j.origin.longitude-.02},{j.origin.latitude-.02,j.origin.longitude+.02},{j.origin.latitude+.02,j.origin.longitude+.02},{j.origin.latitude+.02,j.origin.longitude-.02},{j.origin.latitude-.02,j.origin.longitude-.02}};return tdt::search(key,s);}
  if(is("admin-search")){s.mode=tdt::SearchMode::Administrative;s.specify="156510100";return tdt::search(key,s);}
  if(is("category")){s.mode=tdt::SearchMode::Category;s.specify="156510100";s.dataTypes=arg.empty()?"公园":arg;s.keyword="";s.detailed=true;return tdt::search(key,s);}
  if(is("statistics")){s.mode=tdt::SearchMode::Statistics;s.specify="156510100";return tdt::search(key,s);}
  if(is("legacy")){char *end=nullptr;const long type=std::strtol(j.arguments,&end,10);if(end==j.arguments||(*end&&*end!=' ')||type<0||type>255)return {};while(*end==' ')++end;s.keyword=*end?end:"公园";return tdt::legacySearch(key,s,static_cast<uint8_t>(type));}
  if(is("geocode"))return tdt::geocode(key,arg.empty()?"成都市天府广场":arg);
  if(is("reverse"))return tdt::reverseGeocode(key,j.origin);
  if(is("district"))return tdt::administrative(key,arg.empty()?"成都市":arg,1,true);
  if(is("drive")||is("shortest")||is("avoid")||is("walk")){tdt::DriveOptions d;d.start=j.origin;d.end=PresetDestination;d.style=is("shortest")?1:is("avoid")?2:is("walk")?3:0;return tdt::drive(key,d);}
  if(is("transit")){tdt::TransitOptions t;t.start=j.origin;t.end=PresetTransitDestination;if(!arg.empty()){char *end=nullptr;const long v=std::strtol(arg.c_str(),&end,10);if(!end||*end||v<1||v>15)return {};t.policies=static_cast<uint8_t>(v);}return tdt::transit(key,t);}
  if(is("transit-detail"))return tdt::transitDetail(key,arg);
  if(is("transit-reverse")){const auto space=arg.find(' ');if(space==std::string::npos)return {};return tdt::transitReverse(key,arg.substr(0,space),arg.substr(space+1));}
  if(is("static")||is("pixels")){tdt::StaticOptions image;image.center=j.origin;image.width=256;image.height=256;image.zoom=15;image.markers.push_back({j.origin,"red1"});image.paths.push_back({{j.origin,PresetDestination},"0x3178F6,4,0.8"});if(is("pixels"))image.pixelLocations={j.origin,PresetDestination};return tdt::staticMap(key,image);}
  return {};
}
} // namespace service_app
