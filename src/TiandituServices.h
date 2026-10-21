#pragma once
#include "Pipeline.h"
#include <string>
#include <utility>
#include <vector>

namespace nice_gfx_map { namespace tianditu {
// These are synchronous, portable service calls. Fetch/parse belong to a
// caller-owned worker; only publish successful results in the Map/UI owner.
enum class Kind : uint8_t { Search, Suggestion, LegacySearch, Geocode,
  ReverseGeocode, Administrative, Drive, Transit, TransitDetail, TransitReverse,
  StaticImage, StaticPixels, Raw, WfsCapabilities, WfsDescribe, WfsFeatures };
enum class Error : uint8_t { None, InvalidArgument, Network, Http, Provider,
  Parse, TooLarge, Unsupported, NoMemory };
const char *errorName(Error value);
struct Limits {
  size_t responseBytes=128*1024, items=64, routes=5, geometryPoints=4096,
         textBytes=256, jsonNodes=8192, depth=32;
  bool keepRaw=false; // Raw may contain echoed credentials (notably drive XML).
};
struct Report {
  Error error=Error::None;
  Status transportStatus=Status::Ok;
  int httpStatus=0;
  int64_t providerCode=0;
  bool hasProviderCode=false;
  std::string message; // Bounded/sanitized; never contains a request URL/key.
  std::string providerCodeText; // OGC exception codes are strings, not integers.
  bool ok() const { return error==Error::None; }
};
struct Request {
  Kind kind=Kind::Raw;
  std::string url; // Sensitive: never log Request/url or raw provider responses.
  std::string redactionKey; // Only used to remove an echoed key from messages.
  GeoPoint origin{};
  bool hasOrigin=false;
  bool valid() const;
};
enum class SearchMode : uint8_t { Normal=1, Viewport=2, Nearby=3, Suggestions=4,
  PlaceNames=7, Polygon=10, Administrative=12, Category=13, Statistics=14 };
struct SearchOptions {
  SearchMode mode=SearchMode::Normal;
  std::string keyword, specify, dataTypes;
  GeoBounds bounds{-90,-180,90,180};
  GeoPoint center{};
  std::vector<GeoPoint> polygon;
  uint16_t radiusMeters=3000, offset=0, count=3;
  uint8_t level=16;
  bool detailed=false;
};
struct DriveOptions {
  GeoPoint start{},end{};
  std::vector<GeoPoint> via;
  uint8_t style=0; // 0 fastest, 1 shortest, 2 avoid highways, 3 walking.
};
struct TransitOptions {
  GeoPoint start{},end{};
  uint8_t policies=1; // Bit mask: 1 fast, 2 fewer transfers, 4 less walk, 8 no subway.
};
struct StaticMarker { GeoPoint point{};std::string style; };
struct StaticPath { std::vector<GeoPoint> points;std::string style; };
struct StaticOptions {
  GeoPoint center{};uint16_t width=400,height=300;uint8_t zoom=14;
  std::string layers="vec_c,cva_c";
  std::vector<StaticMarker> markers;
  std::vector<StaticPath> paths;
  std::vector<GeoPoint> pixelLocations; // Nonempty => text coordinate response.
};
struct Parameter { std::string name,value; };
struct WfsOptions {
  std::string typeName;GeoBounds bounds{};uint16_t maxFeatures=2;
  std::vector<std::string> properties;
};
// Public, read-only WFS 1.0.0: verified EPSG:4326 longitude/latitude BBOX and
// application/json output. No service key is required by these public queries.
// Features require a finite nonempty BBOX <=5 degrees per axis and count1..100.
Request wfsCapabilities();
Request wfsDescribe(const std::string &typeName);
Request wfsFeatures(const WfsOptions &options);
// Invalid inputs produce an invalid/empty Request; no partial URL is returned.
Request search(const std::string &key,const SearchOptions &options);
Request legacySearch(const std::string &key,const SearchOptions &options,uint8_t legacyType);
Request geocode(const std::string &key,const std::string &address);
Request reverseGeocode(const std::string &key,GeoPoint point);
Request administrative(const std::string &key,const std::string &keyword,
                       uint8_t childLevels=0,bool boundary=false);
Request drive(const std::string &key,const DriveOptions &options);
Request transit(const std::string &key,const TransitOptions &options);
Request transitDetail(const std::string &key,const std::string &uuid);
Request transitReverse(const std::string &key,const std::string &lineUuid,const std::string &stationUuid);
Request staticMap(const std::string &key,const StaticOptions &options);
// Explicit escape hatch for documented extra parameters. Only the official
// api.tianditu.gov.cn origin is used; reserved tk/duplicate names are rejected.
Request custom(const std::string &key,Kind kind,const std::string &path,
               const std::vector<Parameter> &parameters);

struct LineReference { std::string name,id; };
struct Place {
  std::string name,address,id,type,phone;
  GeoPoint point{};bool hasPoint=false;
  double distanceMeters=0;
  std::vector<LineReference> lines;
};
struct Suggestion { std::string name,address,code; };
struct Statistic { std::string name,code;uint32_t count=0;GeoPoint center{};bool hasCenter=false; };
struct Step {
  std::string instruction,name,id;
  GeoPoint turn{};bool hasTurn=false;
  double distanceMeters=0,durationSeconds=0;
  int kind=0;
  uint16_t alternativeCount=0; // Transit geometry selects the first option.
};
struct Route {
  std::string name,id;
  double distanceMeters=0,durationSeconds=0;
  std::vector<GeoPoint> points;
  std::vector<Step> steps;
  std::vector<Place> stations;
};
struct District {
  std::string name,code;
  GeoPoint center{};bool hasCenter=false;
  int level=0,parent=-1;
  std::vector<std::vector<GeoPoint>> boundaries;
};
enum class GeometryType : uint8_t { None,Point,MultiPoint,LineString,
  MultiLineString,Polygon,MultiPolygon,Collection };
struct Feature {
  std::string id,type;GeometryType geometryType=GeometryType::None;
  std::vector<Parameter> properties;
  std::vector<GeoPoint> points;
  // Separate lines/rings; polygon holes/islands are not joined or filled.
  std::vector<std::vector<GeoPoint>> paths;
};
struct WfsLayer {
  std::string name,title,crs;GeoBounds bounds{};bool hasBounds=false;
};
struct Result {
  Kind kind=Kind::Raw;Report report;
  uint32_t total=0;int resultType=0;
  std::string summary,formattedAddress;
  GeoPoint location{};bool hasLocation=false;
  std::vector<Place> places;
  std::vector<Suggestion> suggestions;
  std::vector<Statistic> statistics;
  std::vector<Route> routes;
  std::vector<District> districts;
  std::vector<PixelPoint> pixels;
  std::vector<WfsLayer> wfsLayers;
  std::vector<std::string> formats,operations;
  std::vector<Parameter> schemaFields;
  std::vector<Feature> features;
  std::string serviceVersion;
  ByteBuffer raw; // Present for StaticImage/Raw, or explicit Limits.keepRaw.
};
class Parser {
 public:
  virtual ~Parser()=default;
  // Strong output ownership: failure preserves the caller's existing result.
  virtual Report parse(const Request &,const uint8_t *,size_t,Result &,
                       const Limits &)=0;
};
using HttpStatusReader=int (*)(void *context);
class Client {
 public:
  Client(Transport &transport,Parser &parser,Limits limits={},
         HttpStatusReader reader=nullptr,void *context=nullptr)
      :transport_(transport),parser_(parser),limits_(limits),reader_(reader),context_(context){}
  // A matching concrete HTTP transport automatically supplies its status code.
  template<class Http,class=decltype(std::declval<Http &>().lastHttpStatus())>
  Client(Http &transport,Parser &parser,Limits limits={})
      :Client(static_cast<Transport &>(transport),parser,limits,
          [](void *p){return static_cast<Http *>(p)->lastHttpStatus();},&transport){}
  // Also configure the underlying HTTP transport's response cap to at most
  // limits.responseBytes; a portable Transport controls its own fetch buffers.
  Report fetch(const Request &request,Result &output);
  const Limits &limits()const{return limits_;}
 private:
  Transport &transport_;Parser &parser_;Limits limits_;
  HttpStatusReader reader_;void *context_;
};
} } // namespace nice_gfx_map::tianditu
