#include "TiandituServices.h"
#include "Providers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace nice_gfx_map { namespace tianditu {
namespace {
constexpr const char *Origin="https://api.tianditu.gov.cn";
constexpr const char *WfsOrigin="https://gisserver.tianditu.gov.cn";
bool point(GeoPoint p){return validCoordinate(p)&&p.longitude>=-180&&p.longitude<=180;}
bool text(const std::string &s,size_t maximum=512){return !s.empty()&&s.size()<=maximum&&s.find('\0')==std::string::npos;}
bool bounds(GeoBounds b){return point({b.south,b.west})&&point({b.north,b.east})&&b.south<b.north&&b.west<b.east;}
std::string coordinate(GeoPoint p){char out[64];std::snprintf(out,sizeof(out),"%.7f,%.7f",p.longitude,p.latitude);return out;}
std::string rectangle(GeoBounds b){return coordinate({b.south,b.west})+","+coordinate({b.north,b.east});}
std::string quote(const std::string &s){
  static const char hex[]="0123456789abcdef";std::string out="\"";out.reserve(s.size()+2);
  for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}return out+'"';
}
void field(std::string &json,const char *name,const std::string &value,bool quoted=true){if(json.size()>1)json+=',';json+=quote(name)+":"+(quoted?quote(value):value);}
Request jsonRequest(const std::string &key,Kind kind,const char *path,const std::string &json,const char *parameter="postStr",const char *type="query"){
  std::vector<Parameter> parameters{{parameter,json}};if(type&&*type)parameters.push_back({"type",type});return custom(key,kind,path,parameters);
}
std::string points(const std::vector<GeoPoint> &list,const char *separator){std::string out;for(auto p:list){if(!point(p))return {};if(!out.empty())out+=separator;out+=coordinate(p);}return out;}
Report failure(Error error,Status status=Status::Ok,int http=0){Report r;r.error=error;r.transportStatus=status;r.httpStatus=http;return r;}
bool identifier(const std::string &value){return text(value,128)&&value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_:.-")==std::string::npos;}
Request wfsRequest(Kind kind,const char *operation,const std::vector<Parameter> &parameters){Request r;r.kind=kind;r.url=std::string(WfsOrigin)+"/TDTService/wfs?service=WFS&version=1.0.0&request="+operation;for(const auto &p:parameters)r.url+='&'+providers::urlEncode(p.name)+'='+providers::urlEncode(p.value);return r.valid()?r:Request{};}
}
const char *errorName(Error error){switch(error){case Error::None:return "ok";case Error::InvalidArgument:return "invalid_argument";case Error::Network:return "network";case Error::Http:return "http";case Error::Provider:return "provider";case Error::Parse:return "parse";case Error::TooLarge:return "too_large";case Error::Unsupported:return "unsupported";case Error::NoMemory:return "no_memory";}return "unknown";}
bool Request::valid()const{if(url.size()>8192||url.find('\0')!=std::string::npos)return false;for(unsigned char c:url)if(c<=32||c==127)return false;for(const char *origin:{Origin,WfsOrigin}){const size_t size=std::strlen(origin);if(url.size()>size&&url.compare(0,size,origin)==0&&url[size]=='/')return true;}return false;}
Request wfsCapabilities(){return wfsRequest(Kind::WfsCapabilities,"GetCapabilities",{});}
Request wfsDescribe(const std::string &name){return identifier(name)?wfsRequest(Kind::WfsDescribe,"DescribeFeatureType",{{"typeName",name}}):Request{};}
Request wfsFeatures(const WfsOptions &o){if(!identifier(o.typeName)||!bounds(o.bounds)||o.bounds.east-o.bounds.west>5||o.bounds.north-o.bounds.south>5||!o.maxFeatures||o.maxFeatures>100||o.properties.size()>64)return {};std::vector<Parameter> parameters{{"typeName",o.typeName},{"outputFormat","application/json"},{"srsName","EPSG:4326"},{"bbox",rectangle(o.bounds)+",EPSG:4326"},{"maxFeatures",std::to_string(o.maxFeatures)}};if(!o.properties.empty()){std::string properties;for(const auto &name:o.properties){if(!identifier(name))return {};if(!properties.empty())properties+=',';properties+=name;}parameters.push_back({"propertyName",properties});}return wfsRequest(Kind::WfsFeatures,"GetFeature",parameters);}
Request custom(const std::string &key,Kind kind,const std::string &path,const std::vector<Parameter> &parameters){
  Request result;if(!text(key,1024)||path.empty()||path[0]!='/'||path.size()>128||path.find_first_not_of("/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-." )!=std::string::npos||parameters.size()>32)return result;
  result.kind=kind;result.url=std::string(Origin)+path+'?';result.redactionKey=key;
  for(size_t i=0;i<parameters.size();++i){const auto &p=parameters[i];if(!text(p.name,64)||p.name=="tk"||p.value.size()>6144||p.value.find('\0')!=std::string::npos)return {};
    for(size_t j=0;j<i;++j)if(parameters[j].name==p.name)return {};
    if(i)result.url+='&';result.url+=providers::urlEncode(p.name)+'='+providers::urlEncode(p.value);
  }
  if(!parameters.empty())result.url+='&';result.url+="tk="+providers::urlEncode(key);if(!result.valid())return {};return result;
}
Request search(const std::string &key,const SearchOptions &o){
  const auto mode=static_cast<unsigned>(o.mode);if(o.level<1||o.level>18||o.offset>300||o.count<1||o.count>300||o.offset+o.count>500||o.specify.size()>128||o.dataTypes.size()>512)return {};
  if(mode!=1&&mode!=2&&mode!=3&&mode!=4&&mode!=7&&mode!=10&&mode!=12&&mode!=13&&mode!=14)return {};
  if(o.mode!=SearchMode::Category&&!text(o.keyword))return {};
  if((mode==12||mode==14)&&!text(o.specify,128))return {};
  if(mode==13&&(!text(o.dataTypes)||(!text(o.specify,128)&&!bounds(o.bounds))))return {};
  if((mode==1||mode==2||mode==4||mode==7)&&!bounds(o.bounds))return {};
  if(mode==3&&(!point(o.center)||!o.radiusMeters||o.radiusMeters>10000))return {};
  if(mode==10&&(o.polygon.size()<4||o.polygon.size()>20||o.polygon.front().latitude!=o.polygon.back().latitude||o.polygon.front().longitude!=o.polygon.back().longitude))return {};
  std::string json="{";if(!o.keyword.empty())field(json,"keyWord",o.keyword);field(json,"level",std::to_string(o.level),false);field(json,"queryType",std::to_string(mode),false);
  if(mode!=14){field(json,"start",std::to_string(o.offset),false);field(json,"count",std::to_string(o.count),false);}
  if(mode==1||mode==2||mode==4||mode==7||mode==13)field(json,"mapBound",rectangle(o.bounds));
  if(!o.specify.empty())field(json,"specify",o.specify);if(!o.dataTypes.empty())field(json,"dataTypes",o.dataTypes);if(o.detailed)field(json,"show","2",false);
  if(mode==3){field(json,"queryRadius",std::to_string(o.radiusMeters),false);field(json,"pointLonlat",coordinate(o.center));}
  if(mode==10){const auto polygon=points(o.polygon,",");if(polygon.empty())return {};field(json,"polygon",polygon);}
  auto result=jsonRequest(key,mode==4?Kind::Suggestion:Kind::Search,"/v2/search",json+'}');if(mode==3){result.origin=o.center;result.hasOrigin=true;}return result;
}
Request legacySearch(const std::string &key,const SearchOptions &o,uint8_t type){
  if(type<1||type>10||type==8||type==9||!text(o.keyword)||!bounds(o.bounds)||o.level<1||o.level>20||o.offset>300||o.count<1||o.count>300||o.offset+o.count>500)return {};
  std::string json="{";field(json,"keyWord",o.keyword);field(json,"level",std::to_string(o.level));field(json,"mapBound",rectangle(o.bounds));field(json,"queryType",std::to_string(type));field(json,"start",std::to_string(o.offset));field(json,"count",std::to_string(o.count));
  if(!o.specify.empty())field(json,"specifyAdminCode",o.specify);
  if(type==3){if(!point(o.center)||!o.radiusMeters||o.radiusMeters>10000)return {};field(json,"pointLonlat",coordinate(o.center));field(json,"queryRadius",std::to_string(o.radiusMeters));}
  return jsonRequest(key,Kind::LegacySearch,"/search",json+'}');
}
Request geocode(const std::string &key,const std::string &address){if(!text(address))return {};return jsonRequest(key,Kind::Geocode,"/geocoder","{\"keyWord\":"+quote(address)+'}',"ds",nullptr);}
Request reverseGeocode(const std::string &key,GeoPoint p){if(!point(p))return {};char json[160];std::snprintf(json,sizeof(json),"{\"lon\":%.7f,\"lat\":%.7f,\"ver\":1}",p.longitude,p.latitude);return jsonRequest(key,Kind::ReverseGeocode,"/geocoder",json,"postStr","geocode");}
Request administrative(const std::string &key,const std::string &keyword,uint8_t levels,bool boundary){if(!text(keyword,128)||levels>3)return {};return custom(key,Kind::Administrative,"/v2/administrative",{{"keyword",keyword},{"childLevel",std::to_string(levels)},{"extensions",boundary?"true":"false"}});}
Request drive(const std::string &key,const DriveOptions &o){if(!point(o.start)||!point(o.end)||o.style>3||o.via.size()>16)return {};std::string json="{";field(json,"orig",coordinate(o.start));field(json,"dest",coordinate(o.end));field(json,"style",std::to_string(o.style));if(!o.via.empty()){const auto via=points(o.via,";");if(via.empty())return {};field(json,"mid",via);}return jsonRequest(key,Kind::Drive,"/drive",json+'}',"postStr","search");}
Request transit(const std::string &key,const TransitOptions &o){if(!point(o.start)||!point(o.end)||!o.policies||o.policies>15)return {};std::string json="{";field(json,"startposition",coordinate(o.start));field(json,"endposition",coordinate(o.end));field(json,"linetype",std::to_string(o.policies));return jsonRequest(key,Kind::Transit,"/transit",json+'}',"postStr","busline");}
Request transitDetail(const std::string &key,const std::string &uuid){if(!text(uuid,128))return {};return jsonRequest(key,Kind::TransitDetail,"/transit","{\"uuid\":"+quote(uuid)+'}',"postStr","busline");}
Request transitReverse(const std::string &key,const std::string &line,const std::string &station){if(!text(line,128)||!text(station,128))return {};return jsonRequest(key,Kind::TransitReverse,"/transit","{\"lineUuid\":"+quote(line)+",\"stationUuid\":"+quote(station)+'}',"postStr","busline");}
Request staticMap(const std::string &key,const StaticOptions &o){
  if(!point(o.center)||!o.width||o.width>1024||!o.height||o.height>1024||o.zoom<3||o.zoom>18||o.layers.size()>128||o.markers.size()>26||o.paths.size()>16||o.pixelLocations.size()>64)return {};
  std::vector<Parameter> parameters{{"center",coordinate(o.center)},{"width",std::to_string(o.width)},{"height",std::to_string(o.height)},{"zoom",std::to_string(o.zoom)},{"layers",o.layers}};
  if(!o.pixelLocations.empty()){const auto list=points(o.pixelLocations,"|");if(list.empty())return {};parameters.push_back({"pixLocation",list});}
  else {
    std::string markers,styles;for(const auto &m:o.markers){if(!point(m.point)||m.style.size()>256)return {};if(!markers.empty()){markers+='|';styles+='|';}markers+=coordinate(m.point);styles+=m.style;}
    if(!markers.empty()){parameters.push_back({"markers",markers});if(!styles.empty())parameters.push_back({"markerStyles",styles});}
    std::string paths,pathStyles;for(const auto &p:o.paths){if(p.points.size()<2||p.points.size()>128||p.style.size()>128)return {};const auto list=points(p.points,";");if(list.empty())return {};if(!paths.empty()){paths+='|';pathStyles+='|';}paths+=list;pathStyles+=p.style;}
    if(!paths.empty()){parameters.push_back({"paths",paths});if(!pathStyles.empty())parameters.push_back({"pathStyles",pathStyles});}
  }
  auto result=custom(key,o.pixelLocations.empty()?Kind::StaticImage:Kind::StaticPixels,"/staticimage",parameters);if(result.url.size()>2048)return {};return result;
}
Report Client::fetch(const Request &request,Result &output){
  if(!request.valid()||!limits_.responseBytes||limits_.responseBytes>8*1024*1024||!limits_.items||!limits_.routes||!limits_.geometryPoints||!limits_.textBytes||!limits_.jsonNodes||!limits_.depth)return failure(Error::InvalidArgument);
  TileRequest wire;wire.url=request.url;ByteBuffer bytes;const auto status=transport_.fetch(wire,bytes);const int http=reader_?reader_(context_):0;
  if(http&&(http<200||http>=300))return failure(Error::Http,status,http);
  if(status!=Status::Ok){const auto error=status==Status::NoMemory?Error::NoMemory:status==Status::TooLarge?Error::TooLarge:status==Status::Unsupported?Error::Unsupported:status==Status::InvalidArgument?Error::InvalidArgument:Error::Network;return failure(error,status,http);}
  if(bytes.size()>limits_.responseBytes)return failure(Error::TooLarge,status,http);
  Result next;Report report=parser_.parse(request,bytes.data(),bytes.size(),next,limits_);report.transportStatus=status;report.httpStatus=http;
  if(!report.ok())return report;
  if(limits_.keepRaw||request.kind==Kind::StaticImage||request.kind==Kind::Raw)next.raw=std::move(bytes);
  next.report=report;output=std::move(next);return report;
}
} }
