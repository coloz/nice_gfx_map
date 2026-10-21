#include "TiandituParser.h"
#include "../Navigation.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#if defined(__has_include) && !defined(NICE_GFX_TDT_DISABLE_CJSON)
#if __has_include(<cJSON.h>)
#include <cJSON.h>
#define NICE_GFX_TDT_HAS_CJSON 1
#endif
#endif

namespace nice_gfx_map { namespace tianditu {
namespace {
struct Context {
  const Request &request;const Limits &limits;Report report{};size_t points=0;
  bool fail(Error e,const char *message){if(report.ok()){report.error=e;report.message=message;}return false;}
  bool item(size_t count){return count<limits.items||fail(Error::TooLarge,"Service item limit exceeded");}
  bool point(std::vector<GeoPoint> &out,GeoPoint p){if(!validCoordinate(p)||p.longitude< -180||p.longitude>180)return fail(Error::Parse,"Invalid provider coordinate");if(points>=limits.geometryPoints)return fail(Error::TooLarge,"Service geometry limit exceeded");++points;out.push_back(p);return true;}
  std::string text(const char *input,bool redact=false){
    std::string out;if(!input)return out;
    // Check the full decoded message before truncating, so a key crossing the
    // text limit cannot leak a prefix through an otherwise bounded message.
    if(redact&&((!request.redactionKey.empty()&&std::strstr(input,request.redactionKey.c_str()))||std::strstr(input,"http:")||std::strstr(input,"https:")||std::strstr(input,"tk=")||std::strstr(input,"sk=")))return "Provider message redacted";
    const auto *s=reinterpret_cast<const unsigned char *>(input);bool full=false;
    while(*s){const unsigned n=*s<128?1:*s>=0xc2&&*s<=0xdf?2:*s>=0xe0&&*s<=0xef?3:*s>=0xf0&&*s<=0xf4?4:0;if(!n){fail(Error::Parse,"Invalid UTF-8 response");return {};}
      for(unsigned i=1;i<n;++i)if(!s[i]||(s[i]&0xc0)!=0x80){fail(Error::Parse,"Invalid UTF-8 response");return {};}
      if((n==3&&((*s==0xe0&&s[1]<0xa0)||(*s==0xed&&s[1]>=0xa0)))||(n==4&&((*s==0xf0&&s[1]<0x90)||(*s==0xf4&&s[1]>=0x90)))){fail(Error::Parse,"Invalid UTF-8 response");return {};}
      if(!full&&out.size()+n<=limits.textBytes){if(n==1&&(*s<32||*s==127))out+=' ';else out.append(reinterpret_cast<const char *>(s),n);}else full=true;s+=n;
    }
    if(redact){if((!request.redactionKey.empty()&&out.find(request.redactionKey)!=std::string::npos)||out.find("http:")!=std::string::npos||out.find("https:")!=std::string::npos||out.find("tk=")!=std::string::npos||out.find("sk=")!=std::string::npos)return "Provider message redacted";}
    return out;
  }
};
bool numeric(const char *text,double &out){if(!text||!*text)return false;char *end=nullptr;out=std::strtod(text,&end);if(end==text||!std::isfinite(out))return false;while(*end&&std::isspace(static_cast<unsigned char>(*end)))++end;return !*end;}
bool metric(Context &c,double value,double scale,double &out){
  if(!std::isfinite(value)||value<0||!std::isfinite(value*scale))return c.fail(Error::Parse,"Invalid provider distance or time");
  out=value*scale;return true;
}
bool addMetric(Context &c,double &total,double value){
  if(!std::isfinite(total+value))return c.fail(Error::Parse,"Provider distance or time overflow");
  total+=value;return true;
}
bool coordinate(const char *s,GeoPoint &point){if(!s)return false;char *end=nullptr;const double lon=std::strtod(s,&end);if(end==s)return false;while(*end&&std::isspace(static_cast<unsigned char>(*end)))++end;if(*end!=',')return false;const char *latStart=end+1;const double lat=std::strtod(latStart,&end);if(end==latStart)return false;while(*end&&std::isspace(static_cast<unsigned char>(*end)))++end;point={lat,lon};return !*end&&validCoordinate(point)&&lon>=-180&&lon<=180;}
bool coordinates(Context &c,const char *s,std::vector<GeoPoint> &out){
  if(!s)return true;while(*s){while(*s&&(std::isspace(static_cast<unsigned char>(*s))||*s==';'||*s=='|'||*s==','))++s;if(!*s)break;
    char *end=nullptr;const double lon=std::strtod(s,&end);if(end==s)return c.fail(Error::Parse,"Invalid provider geometry");s=end;while(*s&&std::isspace(static_cast<unsigned char>(*s)))++s;if(*s!=',')return c.fail(Error::Parse,"Invalid provider geometry");++s;
    const double lat=std::strtod(s,&end);if(end==s)return c.fail(Error::Parse,"Invalid provider geometry");s=end;if(*s&&!std::isspace(static_cast<unsigned char>(*s))&&*s!=','&&*s!=';'&&*s!='|')return c.fail(Error::Parse,"Invalid provider geometry");
    if(!c.point(out,{lat,lon}))return false;
  }return true;
}
bool wkt(Context &c,const char *text,std::vector<std::vector<GeoPoint>> &rings){
  if(!text||!*text)return true;const char *s=text;unsigned ringDepth=0;
  if(std::strncmp(s,"MULTIPOLYGON",12)==0){s+=12;ringDepth=3;}else if(std::strncmp(s,"POLYGON",7)==0){s+=7;ringDepth=2;}else return c.fail(Error::Unsupported,"Unsupported administrative boundary format");
  while(std::isspace(static_cast<unsigned char>(*s)))++s;if(std::strcmp(s,"EMPTY")==0)return true;
  unsigned depth=0;std::vector<GeoPoint> ring;bool saw=false;
  while(*s){if(std::isspace(static_cast<unsigned char>(*s))||*s==','){++s;continue;}if(*s=='('){if(++depth>ringDepth)return c.fail(Error::Parse,"Invalid WKT nesting");++s;continue;}
    if(*s==')'){if(!depth)return c.fail(Error::Parse,"Invalid WKT nesting");if(depth==ringDepth){if(ring.size()<4||ring.front().latitude!=ring.back().latitude||ring.front().longitude!=ring.back().longitude)return c.fail(Error::Parse,"Invalid WKT ring");if(!c.item(rings.size()))return false;rings.push_back(std::move(ring));ring={};saw=true;}--depth;++s;continue;}
    if(depth!=ringDepth)return c.fail(Error::Parse,"Invalid WKT coordinate depth");char *end=nullptr;const double lon=std::strtod(s,&end);if(end==s||!std::isspace(static_cast<unsigned char>(*end)))return c.fail(Error::Parse,"Invalid WKT coordinate");s=end;const double lat=std::strtod(s,&end);if(end==s)return c.fail(Error::Parse,"Invalid WKT coordinate");s=end;if(!c.point(ring,{lat,lon}))return false;
  }return !depth&&saw||c.fail(Error::Parse,"Incomplete WKT boundary");
}
// Small non-expanding XML reader: no DTD/external entities, bounded nodes/depth,
// matched tags, and offsets into the original bounded body rather than copies.
struct XmlNode {uint32_t name=0,nameSize=0,start=0,end=0;int parent=-1,first=-1,last=-1,next=-1;};
struct Xml {
  Context &c;const char *data;size_t size;std::vector<XmlNode> nodes;
  bool equal(int n,const char *name)const{const auto &v=nodes[n];size_t begin=v.name;for(size_t i=0;i<v.nameSize;++i)if(data[v.name+i]==':')begin=v.name+i+1;return std::strlen(name)==v.name+v.nameSize-begin&&std::memcmp(data+begin,name,v.name+v.nameSize-begin)==0;}
  int child(int parent,const char *name)const{if(parent<0)return -1;for(int n=nodes[parent].first;n>=0;n=nodes[n].next)if(equal(n,name))return n;return -1;}
  std::string name(int n)const{const auto &v=nodes[n];size_t begin=v.name;for(size_t i=0;i<v.nameSize;++i)if(data[v.name+i]==':')begin=v.name+i+1;return std::string(data+begin,v.name+v.nameSize-begin);}
  std::string attribute(int n,const char *name){const auto &v=nodes[n];size_t i=v.name+v.nameSize,finish=v.start-1;while(i<finish){while(i<finish&&std::isspace(static_cast<unsigned char>(data[i])))++i;if(i>=finish||data[i]=='/')break;const size_t begin=i;while(i<finish&&!std::isspace(static_cast<unsigned char>(data[i]))&&data[i]!='=')++i;const std::string key(data+begin,i-begin);while(i<finish&&std::isspace(static_cast<unsigned char>(data[i])))++i;if(i>=finish||data[i++]!='=')return {};while(i<finish&&std::isspace(static_cast<unsigned char>(data[i])))++i;if(i>=finish||(data[i]!='\''&&data[i]!='"'))return {};const char quote=data[i++];const size_t value=i;while(i<finish&&data[i]!=quote)++i;if(i>=finish)return {};if(key==name){const std::string out(data+value,i-value);return c.text(out.c_str());}++i;}return {};}
  std::string value(int n){if(n<0)return {};const auto &v=nodes[n];std::string out;const char *s=data+v.start,*end=data+v.end;
    while(s<end){if(end-s>=9&&std::memcmp(s,"<![CDATA[",9)==0){const char *finish=std::search(s+9,end,"]]>","]]>"+3);if(finish==end){c.fail(Error::Parse,"Incomplete XML CDATA");return {};}out.append(s+9,size_t(finish-s-9));s=finish+3;continue;}
      if(*s!='&'){out+=*s++;continue;}const char *finish=std::find(s,end,';');if(finish==end||finish-s>12){c.fail(Error::Parse,"Invalid XML entity");return {};}
      const std::string entity(s+1,size_t(finish-s-1));if(entity=="amp")out+='&';else if(entity=="lt")out+='<';else if(entity=="gt")out+='>';else if(entity=="quot")out+='"';else if(entity=="apos")out+='\'';else if(entity.size()>1&&entity[0]=='#'){
        char *last=nullptr;const bool hex=entity.size()>2&&(entity[1]=='x'||entity[1]=='X');const auto code=std::strtoul(entity.c_str()+(hex?2:1),&last,hex?16:10);if(!last||*last||!code||code>0x10ffff||(code>=0xd800&&code<=0xdfff)){c.fail(Error::Parse,"Invalid XML code point");return {};}
        if(code<0x80)out+=char(code);else if(code<0x800){out+=char(0xc0|(code>>6));out+=char(0x80|(code&63));}else if(code<0x10000){out+=char(0xe0|(code>>12));out+=char(0x80|((code>>6)&63));out+=char(0x80|(code&63));}else{out+=char(0xf0|(code>>18));out+=char(0x80|((code>>12)&63));out+=char(0x80|((code>>6)&63));out+=char(0x80|(code&63));}
      }else{c.fail(Error::Parse,"Unknown XML entity");return {};}s=finish+1;
    }return out;
  }
  std::string string(int parent,const char *name){const auto v=value(child(parent,name));return c.text(v.c_str());}
  double number(int parent,const char *name){const auto v=value(child(parent,name));double out=0;if(!v.empty()&&!numeric(v.c_str(),out))c.fail(Error::Parse,"Invalid XML number");return out;}
  bool parse(){std::vector<int> stack;size_t i=0;unsigned roots=0;
    while(i<size){if(data[i]!='<'){if(stack.empty()&&!std::isspace(static_cast<unsigned char>(data[i])))return c.fail(Error::Parse,"Text outside XML root");++i;continue;}
      const auto starts=[&](const char *s){const size_t n=std::strlen(s);return i+n<=size&&std::memcmp(data+i,s,n)==0;};
      if(starts("<?")||starts("<!--")||starts("<![CDATA[")){const char *marker=starts("<?")?"?>":starts("<!--")?"-->":"]]>";if(starts("<![CDATA[")&&stack.empty())return c.fail(Error::Parse,"CDATA outside XML root");const auto begin=data+i+(starts("<?")?2:starts("<!--")?4:9);const auto finish=std::search(begin,data+size,marker,marker+std::strlen(marker));if(finish==data+size)return c.fail(Error::Parse,"Incomplete XML special section");i=size_t(finish-data)+std::strlen(marker);continue;}
      if(starts("<!"))return c.fail(Error::Unsupported,"XML declarations/entities are disabled");
      const bool closing=i+1<size&&data[i+1]=='/';size_t begin=i+(closing?2:1),nameEnd=begin;while(nameEnd<size&&(std::isalnum(static_cast<unsigned char>(data[nameEnd]))||data[nameEnd]=='_'||data[nameEnd]==':'||data[nameEnd]=='-'||data[nameEnd]=='.'))++nameEnd;if(nameEnd==begin)return c.fail(Error::Parse,"Invalid XML tag");
      size_t finish=nameEnd;char quoted=0;for(;finish<size;++finish){const char ch=data[finish];if(quoted){if(ch==quoted)quoted=0;}else if(ch=='\''||ch=='"')quoted=ch;else if(ch=='>')break;}if(finish==size)return c.fail(Error::Parse,"Incomplete XML tag");
      if(closing){for(size_t p=nameEnd;p<finish;++p)if(!std::isspace(static_cast<unsigned char>(data[p])))return c.fail(Error::Parse,"Invalid XML closing tag");if(stack.empty())return c.fail(Error::Parse,"Unexpected XML closing tag");const auto &v=nodes[stack.back()];if(v.nameSize!=nameEnd-begin||std::memcmp(data+v.name,data+begin,v.nameSize)!=0)return c.fail(Error::Parse,"Mismatched XML closing tag");nodes[stack.back()].end=uint32_t(i);stack.pop_back();}
      else {if(nodes.size()>=c.limits.jsonNodes||stack.size()>=c.limits.depth)return c.fail(Error::TooLarge,"XML node/depth limit exceeded");const int index=int(nodes.size()),parent=stack.empty()?-1:stack.back();XmlNode node;node.name=uint32_t(begin);node.nameSize=uint32_t(nameEnd-begin);node.start=node.end=uint32_t(finish+1);node.parent=parent;nodes.push_back(node);
        if(parent<0){if(++roots>1)return c.fail(Error::Parse,"Multiple XML roots");}else{auto &p=nodes[parent];if(p.last>=0)nodes[p.last].next=index;else p.first=index;p.last=index;}
        if(finish==begin||data[finish-1]!='/')stack.push_back(index);
      }i=finish+1;
    }return stack.empty()&&roots==1||c.fail(Error::Parse,"Incomplete XML root");
  }
};
bool driveXml(Context &c,const uint8_t *bytes,size_t size,Result &out){
  Xml xml{c,reinterpret_cast<const char *>(bytes),size,{}};if(!xml.parse())return false;
  if(!xml.equal(0,"result"))return c.fail(Error::Parse,"Unexpected drive XML root");
  Route route;if(!metric(c,xml.number(0,"distance"),1000,route.distanceMeters)||!metric(c,xml.number(0,"duration"),1,route.durationSeconds))return false;
  // Official RoutePlan documents kilometres and seconds. Transit plans use
  // metres/minutes (the official BusLine5 example labels getDuration as 分).
  const auto geometry=xml.value(xml.child(0,"routelatlon"));if(geometry.empty())return c.fail(Error::Provider,"Drive service returned no route");if(!coordinates(c,geometry.c_str(),route.points))return false;
  const int routes=xml.child(0,"routes");for(int n=routes<0?-1:xml.nodes[routes].first;n>=0;n=xml.nodes[n].next){if(!xml.equal(n,"item"))continue;if(!c.item(route.steps.size()))return false;Step step;step.instruction=xml.string(n,"strguide");step.name=xml.string(n,"streetName");if(!metric(c,xml.number(n,"streetDistance"),1,step.distanceMeters))return false;const auto turn=xml.value(xml.child(n,"turnlatlon"));if(!turn.empty()){if(!coordinate(turn.c_str(),step.turn))return c.fail(Error::Parse,"Invalid drive turn coordinate");step.hasTurn=true;}route.steps.push_back(std::move(step));}
  const int simple=xml.child(0,"simple");if(simple>=0&&route.steps.empty())for(int n=xml.nodes[simple].first;n>=0;n=xml.nodes[n].next){if(!xml.equal(n,"item"))continue;if(!c.item(route.steps.size()))return false;Step step;step.name=xml.string(n,"streetNames");if(!metric(c,xml.number(n,"streetDistance"),1,step.distanceMeters))return false;route.steps.push_back(std::move(step));}
  out.routes.push_back(std::move(route));out.total=1;out.summary="Driving/walking route";return c.report.ok();
}
bool wfsXml(Context &c,const uint8_t *bytes,size_t size,Result &out){
  Xml xml{c,reinterpret_cast<const char *>(bytes),size,{}};if(!xml.parse())return false;
  if(xml.equal(0,"ExceptionReport")||xml.equal(0,"ServiceExceptionReport")){
    c.report.error=Error::Provider;for(size_t i=0;i<xml.nodes.size();++i)if(xml.equal(int(i),"Exception")||xml.equal(int(i),"ServiceException")){c.report.providerCodeText=xml.attribute(int(i),"exceptionCode");if(c.report.providerCodeText.empty())c.report.providerCodeText=xml.attribute(int(i),"code");const int child=xml.child(int(i),"ExceptionText");const auto value=xml.value(child>=0?child:int(i));c.report.message=c.text(value.c_str(),true);break;}if(c.report.message.empty())c.report.message="WFS service rejected request";return false;
  }
  if(c.request.kind==Kind::WfsDescribe){
    if(!xml.equal(0,"schema"))return c.fail(Error::Parse,"Expected WFS XML schema");
    for(size_t i=0;i<xml.nodes.size();++i)if(xml.equal(int(i),"element")&&xml.nodes[i].parent>=0&&xml.equal(xml.nodes[i].parent,"sequence")){if(!c.item(out.schemaFields.size()))return false;Parameter p{xml.attribute(int(i),"name"),xml.attribute(int(i),"type")};if(p.name.empty()||p.value.empty())return c.fail(Error::Parse,"Invalid WFS schema element");out.schemaFields.push_back(std::move(p));}
    if(out.schemaFields.empty())return c.fail(Error::Parse,"WFS schema has no fields");out.total=uint32_t(out.schemaFields.size());out.summary="WFS feature schema";return c.report.ok();
  }
  if(!xml.equal(0,"WFS_Capabilities"))return c.fail(Error::Parse,"Expected WFS capabilities");out.serviceVersion=xml.attribute(0,"version");
  for(size_t i=0;i<xml.nodes.size();++i){const int n=int(i);
    if(xml.equal(n,"FeatureType")){if(!c.item(out.wfsLayers.size()))return false;WfsLayer layer;layer.name=xml.string(n,"Name");layer.title=xml.string(n,"Title");int crs=xml.child(n,"DefaultCRS");if(crs<0)crs=xml.child(n,"DefaultSRS");if(crs<0)crs=xml.child(n,"SRS");const auto coordinateSystem=xml.value(crs);layer.crs=c.text(coordinateSystem.c_str());
      const int box=xml.child(n,"LatLongBoundingBox");if(box>=0){const auto west=xml.attribute(box,"minx"),south=xml.attribute(box,"miny"),east=xml.attribute(box,"maxx"),north=xml.attribute(box,"maxy");layer.hasBounds=numeric(west.c_str(),layer.bounds.west)&&numeric(south.c_str(),layer.bounds.south)&&numeric(east.c_str(),layer.bounds.east)&&numeric(north.c_str(),layer.bounds.north);}
      if(layer.name.empty())return c.fail(Error::Parse,"WFS layer missing name");out.wfsLayers.push_back(std::move(layer));
    }
    if(xml.equal(n,"Operation")){const auto name=xml.attribute(n,"name");if(!name.empty()){if(!c.item(out.operations.size()))return false;out.operations.push_back(name);}}
    else if(xml.nodes[i].parent>=0&&xml.equal(xml.nodes[i].parent,"Request")){if(!c.item(out.operations.size()))return false;out.operations.push_back(xml.name(n));}
    if(xml.nodes[i].parent>=0&&(xml.equal(xml.nodes[i].parent,"ResultFormat")||xml.equal(xml.nodes[i].parent,"OutputFormats"))){if(!c.item(out.formats.size()))return false;const auto v=xml.value(n);out.formats.push_back(v.empty()?xml.name(n):c.text(v.c_str()));}
  }
  if(out.wfsLayers.empty())return c.fail(Error::Parse,"WFS capabilities has no layers");out.total=uint32_t(out.wfsLayers.size());out.summary="WFS capabilities (advertised operations are not write permission)";return c.report.ok();
}

#if defined(NICE_GFX_TDT_HAS_CJSON)
using J=cJSON;
const J *get(const J *value,const char *name){const auto *exact=cJSON_GetObjectItemCaseSensitive(value,name);return exact?exact:cJSON_GetObjectItem(value,name);}
const J *either(const J *value,const char *a,const char *b){const auto *n=get(value,a);return n?n:get(value,b);}
const char *str(const J *v){return cJSON_IsString(v)?v->valuestring:nullptr;}
bool number(const J *v,double &out){if(cJSON_IsNumber(v)){out=v->valuedouble;return std::isfinite(out);}return numeric(str(v),out);}
bool integer(const J *v,int64_t &out){double n=0;if(!number(v,n)||n< -1e12||n>1e12||std::floor(n)!=n)return false;out=int64_t(n);return true;}
bool optionalInteger(Context &c,const J *v,int &out){
  if(!v){out=0;return true;}int64_t value=0;
  if(!integer(v,value)||value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())return c.fail(Error::Parse,"Invalid provider integer");
  out=static_cast<int>(value);return true;
}
bool optionalMetric(Context &c,const J *v,double scale,double &out){
  if(!v){out=0;return true;}double value=0;
  if(!number(v,value))return c.fail(Error::Parse,"Invalid provider distance or time");
  return metric(c,value,scale,out);
}
std::string string(Context &c,const J *v){if(!v)return {};if(const auto *s=str(v))return c.text(s);if(cJSON_IsNumber(v)){if(!std::isfinite(v->valuedouble)){c.fail(Error::Parse,"Invalid provider number");return {};}char out[40];std::snprintf(out,sizeof(out),"%.17g",v->valuedouble);return c.text(out);}c.fail(Error::Parse,"Expected provider string");return {};}
bool geo(const J *v,GeoPoint &out){
  if(const char *s=str(v))return coordinate(s,out);double lon=0,lat=0;
  if(cJSON_IsArray(v)){const int size=cJSON_GetArraySize(v);double altitude=0;if((size!=2&&size!=3)||!number(cJSON_GetArrayItem(v,0),lon)||!number(cJSON_GetArrayItem(v,1),lat)||(size==3&&!number(cJSON_GetArrayItem(v,2),altitude)))return false;}
  else if(cJSON_IsObject(v)){if(!number(either(v,"lon","lng"),lon)||!number(get(v,"lat"),lat))return false;}else return false;
  out={lat,lon};return validCoordinate(out)&&lon>=-180&&lon<=180;
}
bool boundedJson(Context &c,const uint8_t *bytes,size_t count){bool quoted=false,escaped=false;size_t depth=0,tokens=0;
  for(size_t i=0;i<count;++i){const auto ch=bytes[i];if(!ch)return c.fail(Error::Parse,"NUL in JSON response");if(quoted){if(escaped){if(ch=='u'&&i+4<count&&std::memcmp(bytes+i+1,"0000",4)==0)return c.fail(Error::Parse,"NUL in JSON string");escaped=false;}else if(ch=='\\')escaped=true;else if(ch=='"')quoted=false;continue;}
    if(ch=='"'){quoted=true;if(++tokens>c.limits.jsonNodes)return c.fail(Error::TooLarge,"JSON node limit exceeded");}else if(ch=='{'||ch=='['){if(++depth>c.limits.depth||++tokens>c.limits.jsonNodes)return c.fail(Error::TooLarge,"JSON node/depth limit exceeded");}else if(ch=='}'||ch==']'){if(!depth)return c.fail(Error::Parse,"JSON nesting error");--depth;}else if(ch==','||ch==':'){if(++tokens>c.limits.jsonNodes)return c.fail(Error::TooLarge,"JSON node limit exceeded");}
  }return !quoted&&!depth||c.fail(Error::Parse,"Incomplete JSON response");
}
struct Json {J *value=nullptr;~Json(){cJSON_Delete(value);}};
bool place(Context &c,const J *item,Place &p){
  if(!cJSON_IsObject(item))return c.fail(Error::Parse,"Invalid POI object");p.name=string(c,get(item,"name"));p.address=string(c,get(item,"address"));p.id=string(c,either(item,"hotPointID","uuid"));p.type=string(c,get(item,"poiType"));p.phone=string(c,get(item,"phone"));
  if(const auto *position=either(item,"lonlat","location")){if(!geo(position,p.point))return c.fail(Error::Parse,"Invalid POI coordinate");p.hasPoint=true;}
  if(const auto *lines=either(item,"linedata","stationData")){if(!cJSON_IsArray(lines))return c.fail(Error::Parse,"Invalid station lines");for(const auto *line=lines->child;line;line=line->next){if(!c.item(p.lines.size()))return false;LineReference ref;ref.name=string(c,either(line,"name","lineName"));ref.id=string(c,get(line,"uuid"));p.lines.push_back(std::move(ref));}}
  if(c.request.hasOrigin&&p.hasPoint)p.distanceMeters=distanceMeters(c.request.origin,p.point);return c.report.ok();
}
bool places(Context &c,const J *array,std::vector<Place> &out){if(!array)return true;if(!cJSON_IsArray(array))return c.fail(Error::Parse,"Expected POI array");for(const J *n=array->child;n;n=n->next){if(!c.item(out.size()))return false;Place p;if(!place(c,n,p))return false;out.push_back(std::move(p));}return true;}
bool suggestion(Context &c,const J *array,std::vector<Suggestion> &out){if(!array)return true;if(!cJSON_IsArray(array))return c.fail(Error::Parse,"Expected suggestions array");for(const auto *n=array->child;n;n=n->next){if(!c.item(out.size()))return false;Suggestion s;if(cJSON_IsString(n))s.name=string(c,n);else if(cJSON_IsObject(n)){s.name=string(c,get(n,"name"));s.address=string(c,get(n,"address"));s.code=string(c,either(n,"gbCode","adminCode"));}else return c.fail(Error::Parse,"Invalid suggestion");out.push_back(std::move(s));}return true;}
bool geometry(Context &c,const J *value,std::vector<GeoPoint> &out){if(!value)return true;if(const auto *s=str(value))return coordinates(c,s,out);if(!cJSON_IsArray(value))return c.fail(Error::Parse,"Invalid geometry array");for(const J *n=value->child;n;n=n->next){GeoPoint p;if(!geo(n,p))return c.fail(Error::Parse,"Invalid geometry point");if(!c.point(out,p))return false;}return true;}
bool geoJsonGeometry(Context &c,const J *object,Feature &feature,size_t depth=0){
  if(cJSON_IsNull(object))return true;if(depth>=c.limits.depth)return c.fail(Error::TooLarge,"GeoJSON depth limit exceeded");if(!cJSON_IsObject(object))return c.fail(Error::Parse,"Invalid GeoJSON geometry");const auto *type=str(get(object,"type"));if(!type)return c.fail(Error::Parse,"GeoJSON missing geometry type");if(depth==0)feature.type=c.text(type);
  const J *coordinates=get(object,"coordinates");const auto path=[&](const J *array,bool ring=false){if(!c.item(feature.paths.size()))return false;std::vector<GeoPoint> points;if(!geometry(c,array,points))return false;if(points.size()<(ring?4u:2u)||(ring&&(points.front().latitude!=points.back().latitude||points.front().longitude!=points.back().longitude)))return c.fail(Error::Parse,"Invalid GeoJSON line/ring");feature.paths.push_back(std::move(points));return true;};
  if(std::strcmp(type,"Point")==0){if(depth==0)feature.geometryType=GeometryType::Point;GeoPoint point;if(!geo(coordinates,point))return c.fail(Error::Parse,"Invalid GeoJSON point");return c.point(feature.points,point);}
  if(std::strcmp(type,"MultiPoint")==0){if(depth==0)feature.geometryType=GeometryType::MultiPoint;return geometry(c,coordinates,feature.points);}
  if(std::strcmp(type,"LineString")==0){if(depth==0)feature.geometryType=GeometryType::LineString;return path(coordinates);}
  if(std::strcmp(type,"MultiLineString")==0||std::strcmp(type,"Polygon")==0){const bool ring=std::strcmp(type,"Polygon")==0;if(depth==0)feature.geometryType=ring?GeometryType::Polygon:GeometryType::MultiLineString;if(!cJSON_IsArray(coordinates))return c.fail(Error::Parse,"Invalid GeoJSON paths");for(const J *n=coordinates->child;n;n=n->next)if(!path(n,ring))return false;return true;}
  if(std::strcmp(type,"MultiPolygon")==0){if(depth==0)feature.geometryType=GeometryType::MultiPolygon;if(!cJSON_IsArray(coordinates))return c.fail(Error::Parse,"Invalid GeoJSON polygons");for(const J *polygon=coordinates->child;polygon;polygon=polygon->next){if(!cJSON_IsArray(polygon))return c.fail(Error::Parse,"Invalid GeoJSON polygon rings");for(const J *ring=polygon->child;ring;ring=ring->next)if(!path(ring,true))return false;}return true;}
  if(std::strcmp(type,"GeometryCollection")==0){if(depth==0)feature.geometryType=GeometryType::Collection;const auto *array=get(object,"geometries");if(!cJSON_IsArray(array))return c.fail(Error::Parse,"Invalid GeoJSON collection");for(const J *n=array->child;n;n=n->next)if(!geoJsonGeometry(c,n,feature,depth+1))return false;return true;}
  return c.fail(Error::Unsupported,"Unsupported GeoJSON geometry type");
}
bool wfsJson(Context &c,const J *root,Result &out){
  const char *type=str(get(root,"type"));if(!type||std::strcmp(type,"FeatureCollection")!=0)return c.fail(Error::Parse,"Expected WFS GeoJSON FeatureCollection");const auto *features=get(root,"features");if(!cJSON_IsArray(features))return c.fail(Error::Parse,"Missing WFS features");
  if(const auto *crs=get(root,"crs")){const char *name=str(get(get(crs,"properties"),"name"));bool accepted=false;for(const char *known:{"EPSG:4326","urn:ogc:def:crs:EPSG::4326","urn:ogc:def:crs:EPSG:6.6:4326","http://www.opengis.net/def/crs/EPSG/0/4326","https://www.opengis.net/def/crs/EPSG/0/4326","CRS:84","urn:ogc:def:crs:OGC::CRS84","urn:ogc:def:crs:OGC:1.3:CRS84"})if(name&&std::strcmp(name,known)==0){accepted=true;break;}if(!accepted)return c.fail(Error::Unsupported,"WFS GeoJSON CRS is not longitude/latitude");}
  int64_t total=0;if(integer(either(root,"totalFeatures","numberMatched"),total)&&total>=0&&total<=UINT32_MAX)out.total=uint32_t(total);
  for(const J *n=features->child;n;n=n->next){if(!c.item(out.features.size())||!cJSON_IsObject(n))return c.fail(Error::Parse,"Invalid WFS feature");const char *featureType=str(get(n,"type"));if(!featureType||std::strcmp(featureType,"Feature")!=0)return c.fail(Error::Parse,"Invalid GeoJSON feature type");Feature feature;feature.id=string(c,get(n,"id"));const auto *properties=get(n,"properties");if(properties&&!cJSON_IsObject(properties)&&!cJSON_IsNull(properties))return c.fail(Error::Parse,"Invalid WFS properties");
    for(const auto *p=properties?properties->child:nullptr;p;p=p->next){if(!c.item(feature.properties.size()))return false;Parameter field;field.name=c.text(p->string);if(cJSON_IsBool(p))field.value=cJSON_IsTrue(p)?"true":"false";else if(cJSON_IsNull(p))field.value="null";else if(cJSON_IsObject(p)||cJSON_IsArray(p))return c.fail(Error::Unsupported,"Nested WFS property requires custom parsing");else field.value=string(c,p);feature.properties.push_back(std::move(field));}
    if(!geoJsonGeometry(c,get(n,"geometry"),feature))return false;out.features.push_back(std::move(feature));
  }if(!out.total)out.total=uint32_t(out.features.size());out.summary="WFS GeoJSON features";return c.report.ok();
}
bool provider(Context &c,const J *root,int64_t success,bool statusRequired=true){
  const auto *status=get(root,"status");if(cJSON_IsObject(status))status=get(status,"infocode");if(!status)status=get(root,"resultCode");
  if(!status)return !statusRequired||c.fail(Error::Parse,"Missing provider status");int64_t code=0;if(!integer(status,code))return c.fail(Error::Parse,"Invalid provider status");c.report.providerCode=code;c.report.hasProviderCode=true;
  if(code!=success){c.report.error=Error::Provider;const auto *message=either(root,"msg","message");if(!message&&cJSON_IsObject(get(root,"status")))message=get(get(root,"status"),"cndesc");c.report.message=c.text(str(message),true);if(c.report.message.empty())c.report.message="Provider rejected request";return false;}return true;
}
bool statistics(Context &c,const J *value,std::vector<Statistic> &out){
  if(!value)return true;const J *array=cJSON_IsArray(value)?value:either(value,"allAdmins","priorityCitys");if(!array)return true;if(!cJSON_IsArray(array))return c.fail(Error::Parse,"Invalid statistics array");
  for(const J *n=array->child;n;n=n->next){if(!c.item(out.size()))return false;Statistic s;s.name=string(c,get(n,"name"));s.code=string(c,either(n,"adminCode","gbCode"));int64_t count=0;if(get(n,"count")&&(!integer(get(n,"count"),count)||count<0||count>UINT32_MAX))return c.fail(Error::Parse,"Invalid statistics count");s.count=uint32_t(count);if(const auto *p=get(n,"lonlat")){if(!geo(p,s.center))return c.fail(Error::Parse,"Invalid statistics coordinate");s.hasCenter=true;}out.push_back(std::move(s));}
  return true;
}
bool searchJson(Context &c,const J *root,Result &out){
  const auto *status=get(root,"status");if(status){int64_t code=0;const auto *v=cJSON_IsObject(status)?get(status,"infocode"):status;if(integer(v,code)&&code==3001){c.report.hasProviderCode=true;c.report.providerCode=code;out.total=0;out.summary="No search results";return true;}if(!provider(c,root,1000))return false;}
  else if(c.request.kind!=Kind::Suggestion&&c.request.kind!=Kind::LegacySearch)return c.fail(Error::Parse,"Missing search status");
  int64_t total=0,type=0;if(get(root,"count")&&(!integer(get(root,"count"),total)||total<0||total>UINT32_MAX))return c.fail(Error::Parse,"Invalid search count");if(!integer(get(root,"resultType"),type)||type<1||type>5)return c.fail(Error::Parse,"Invalid search result type");out.total=uint32_t(total);out.resultType=int(type);
  if(type==1){if(total&&!cJSON_IsArray(get(root,"pois")))return c.fail(Error::Parse,"Missing POI results");if(!places(c,get(root,"pois"),out.places))return false;for(const auto &p:out.places)if(p.name.empty()||!p.hasPoint)return c.fail(Error::Parse,"Incomplete POI result");}
  else if(type==2){if(!statistics(c,get(root,"statistics"),out.statistics))return false;}
  else if(type==3){const J *areas=get(root,"area");if(!cJSON_IsArray(areas))return c.fail(Error::Parse,"Missing search areas");for(const J *n=areas->child;n;n=n->next){if(!c.item(out.districts.size()))return false;District d;d.name=string(c,get(n,"name"));d.code=string(c,get(n,"adminCode"));if(const auto *p=get(n,"lonlat")){if(!geo(p,d.center))return c.fail(Error::Parse,"Invalid area center");d.hasCenter=true;}out.districts.push_back(std::move(d));}}
  else if(type==4){if(!suggestion(c,get(root,"suggests"),out.suggestions))return false;}
  else {const auto *lines=get(root,"lineData");if(!cJSON_IsArray(lines))return c.fail(Error::Parse,"Missing transit search lines");for(const auto *n=lines->child;n;n=n->next){if(!c.item(out.places.size()))return false;Place p;p.name=string(c,get(n,"name"));p.id=string(c,get(n,"uuid"));p.type="103";out.places.push_back(std::move(p));}}
  out.summary="Search results";return c.report.ok();
}
bool district(Context &c,const J *array,Result &out,int parent= -1,size_t depth=0){
  if(!array)return true;if(depth>=c.limits.depth)return c.fail(Error::TooLarge,"Administrative depth limit exceeded");if(!cJSON_IsArray(array))return c.fail(Error::Parse,"Invalid district array");
  for(const auto *n=array->child;n;n=n->next){if(!c.item(out.districts.size())||!cJSON_IsObject(n))return c.fail(Error::Parse,"Invalid district");District d;d.parent=parent;d.name=string(c,get(n,"name"));d.code=string(c,get(n,"gb"));if(!optionalInteger(c,get(n,"level"),d.level))return false;if(const auto *p=get(n,"center")){if(!geo(p,d.center))return c.fail(Error::Parse,"Invalid district center");d.hasCenter=true;}
    if(const auto *b=get(n,"boundary")){if(!cJSON_IsString(b)||!wkt(c,str(b),d.boundaries))return c.fail(Error::Parse,"Invalid administrative boundary");}
    const int index=int(out.districts.size());out.districts.push_back(std::move(d));if(!district(c,get(n,"children"),out,index,depth+1))return false;
  }return c.report.ok();
}
bool station(Context &c,const J *n,std::vector<Place> &out){if(!n)return true;if(!c.item(out.size()))return false;Place p;if(!place(c,n,p))return false;out.push_back(std::move(p));return true;}
bool transitJson(Context &c,const J *root,Result &out){
  if(get(root,"resultCode")){int64_t code=0;if(!integer(get(root,"resultCode"),code))return c.fail(Error::Parse,"Invalid transit result code");if(!provider(c,root,code==5?5:0))return false;}
  if(c.request.kind==Kind::Transit){
    const auto *results=get(root,"results");if(!cJSON_IsArray(results))return c.fail(Error::Parse,"Missing transit results");
    for(const auto *type=results->child;type;type=type->next){const auto *lines=get(type,"lines");if(!cJSON_IsArray(lines))return c.fail(Error::Parse,"Invalid transit lines");for(const auto *line=lines->child;line;line=line->next){if(out.routes.size()>=c.limits.routes)return c.fail(Error::TooLarge,"Route limit exceeded");Route route;route.name=string(c,get(line,"lineName"));route.id=string(c,get(line,"uuid"));const auto *segments=get(line,"segments");if(!cJSON_IsArray(segments))return c.fail(Error::Parse,"Missing transit segments");
      for(const auto *segment=segments->child;segment;segment=segment->next){if(!c.item(route.steps.size()))return false;Step step;if(!optionalInteger(c,get(segment,"segmentType"),step.kind))return false;const auto *details=get(segment,"segmentLine");if(cJSON_IsArray(details)){const auto options=cJSON_GetArraySize(details);if(options<1||size_t(options)>c.limits.items||options>UINT16_MAX)return c.fail(Error::TooLarge,"Transit alternative limit exceeded");step.alternativeCount=uint16_t(options);details=details->child;}else step.alternativeCount=1;if(!cJSON_IsObject(details))return c.fail(Error::Parse,"Missing transit segment");step.name=string(c,either(details,"segmentName","lineName"));step.id=string(c,get(details,"byuuid"));step.instruction=string(c,get(details,"direction"));if(!optionalMetric(c,get(details,"segmentDistance"),1,step.distanceMeters)||!optionalMetric(c,get(details,"segmentTime"),60,step.durationSeconds)||!addMetric(c,route.distanceMeters,step.distanceMeters)||!addMetric(c,route.durationSeconds,step.durationSeconds))return false;
        if(!geometry(c,get(details,"linePoint"),route.points)||!station(c,get(segment,"stationStart"),route.stations)||!station(c,get(segment,"stationEnd"),route.stations))return false;route.steps.push_back(std::move(step));}
      out.routes.push_back(std::move(route));
    }}out.total=uint32_t(out.routes.size());out.summary="Transit plans";return c.report.ok();
  }
  const J *line=either(root,"lineinfo","lineInfo");if(cJSON_IsArray(line))line=line->child;if(!line&&get(root,"lineName"))line=root;
  if(line){Route route;route.name=string(c,get(line,"lineName"));route.id=string(c,either(line,"uuid","byuuid"));if(!optionalMetric(c,get(line,"length"),1,route.distanceMeters)||!optionalMetric(c,get(line,"totalTime"),60,route.durationSeconds)||!geometry(c,get(line,"linePoint"),route.points)||!places(c,get(line,"station"),route.stations))return false;out.routes.push_back(std::move(route));out.total=1;out.summary="Transit line details";return c.report.ok();}
  if(get(root,"lonlat")&&get(root,"uuid")&&get(root,"name")){Place p;if(!place(c,root,p))return false;out.places.push_back(std::move(p));out.total=1;out.summary="Transit station details";return c.report.ok();}
  const J *stations=either(root,"stationdata","stationData");if(cJSON_IsArray(stations)){if(!places(c,stations,out.places))return false;out.total=uint32_t(out.places.size());out.summary="Transit station details";return c.report.ok();}
  if(cJSON_IsObject(stations)){Place p;if(!place(c,stations,p))return false;out.places.push_back(std::move(p));out.total=1;out.summary="Transit station details";return c.report.ok();}
  // Return-line queries have varied object names in historical documentation;
  // preserve the explicit response state and recognizable line/station objects.
  if(c.request.kind==Kind::TransitReverse){if(get(root,"status")&&!provider(c,root,0))return false;const J *array=either(root,"lines","lineData");if(array){if(!places(c,array,out.places))return false;out.total=uint32_t(out.places.size());out.summary="Reverse transit lines";return c.report.ok();}return c.fail(Error::Unsupported,"Unrecognized reverse transit response");}
  return c.fail(Error::Parse,"Unrecognized transit detail response");
}
bool json(Context &c,const uint8_t *bytes,size_t count,Result &out){
  if(!boundedJson(c,bytes,count))return false;const char *end=nullptr;Json root;root.value=cJSON_ParseWithLengthOpts(reinterpret_cast<const char *>(bytes),count,&end,false);if(!root.value||!cJSON_IsObject(root.value))return c.fail(Error::Parse,"Invalid service JSON");const char *finish=reinterpret_cast<const char *>(bytes)+count;if(!end||end>finish)return c.fail(Error::Parse,"Invalid JSON boundary");for(;end<finish;++end)if(!std::isspace(static_cast<unsigned char>(*end)))return c.fail(Error::Parse,"Trailing service JSON data");
  const J *r=root.value;switch(c.request.kind){
    case Kind::Search:case Kind::Suggestion:case Kind::LegacySearch:return searchJson(c,r,out);
    case Kind::Geocode:if(!provider(c,r,0))return false;if(!geo(get(r,"location"),out.location))return c.fail(Error::Parse,"Missing geocoded location");out.hasLocation=true;out.total=1;out.summary="Geocoded location";return true;
    case Kind::ReverseGeocode:{if(!provider(c,r,0))return false;const auto *result=get(r,"result");if(!cJSON_IsObject(result))return c.fail(Error::Parse,"Missing reverse geocode result");out.formattedAddress=string(c,get(result,"formatted_address"));if(!geo(get(result,"location"),out.location))return c.fail(Error::Parse,"Missing reverse geocode location");out.hasLocation=true;out.total=1;out.summary="Reverse geocoded address";return c.report.ok();}
    case Kind::Administrative:{if(!provider(c,r,200))return false;const auto *data=get(r,"data");if(!cJSON_IsObject(data))return c.fail(Error::Parse,"Missing administrative data");if(!suggestion(c,get(data,"suggestion"),out.suggestions)||!district(c,get(data,"district"),out))return false;out.total=uint32_t(out.districts.size());out.summary="Administrative divisions";return c.report.ok();}
    case Kind::Transit:case Kind::TransitDetail:case Kind::TransitReverse:return transitJson(c,r,out);
    case Kind::WfsFeatures:return wfsJson(c,r,out);
    case Kind::Drive:{if(get(r,"status"))return provider(c,r,0)&&c.fail(Error::Parse,"Expected drive XML geometry");c.report.error=Error::Provider;c.report.message="Drive service returned no XML route";return false;}
    default:return c.fail(Error::Unsupported,"JSON is not valid for this response kind");
  }
}
#endif
} // namespace

Report ResponseParser::parse(const Request &request,const uint8_t *bytes,size_t count,Result &output,const Limits &limits){
  Context c{request,limits};Result out;out.kind=request.kind;
  if(!request.valid()||!bytes||!count||!limits.responseBytes||!limits.items||!limits.routes||!limits.geometryPoints||!limits.textBytes||!limits.jsonNodes||!limits.depth){c.fail(Error::InvalidArgument,"Invalid service parse arguments");return c.report;}
  if(count>limits.responseBytes){c.fail(Error::TooLarge,"Service response byte limit exceeded");return c.report;}
  if(request.kind!=Kind::Raw&&request.kind!=Kind::StaticImage&&std::memchr(bytes,0,count)){c.fail(Error::Parse,"NUL in text response");return c.report;}
  bool okay=false;
  if(request.kind==Kind::Raw){okay=true;out.summary="Raw service response";}
  else if(request.kind==Kind::StaticImage){const uint8_t signature[]={137,80,78,71,13,10,26,10};if(count>=24&&std::memcmp(bytes,signature,8)==0&&std::memcmp(bytes+12,"IHDR",4)==0){const uint32_t width=(uint32_t(bytes[16])<<24)|(uint32_t(bytes[17])<<16)|(uint32_t(bytes[18])<<8)|bytes[19],height=(uint32_t(bytes[20])<<24)|(uint32_t(bytes[21])<<16)|(uint32_t(bytes[22])<<8)|bytes[23];okay=width&&width<=1024&&height&&height<=1024;if(okay)out.summary="Static PNG map";else c.fail(Error::TooLarge,"Static PNG dimensions exceed service limits");}else c.fail(Error::Parse,"Expected static PNG response");}
  else if(request.kind==Kind::StaticPixels){
    std::string text(reinterpret_cast<const char *>(bytes),count);const char *s=text.c_str();while(*s&&c.report.ok()){while(std::isspace(static_cast<unsigned char>(*s)))++s;if(!*s)break;char *end=nullptr;const double x=std::strtod(s,&end);if(end==s||*end!=','){c.fail(Error::Parse,"Invalid static pixel pair");break;}s=end+1;const double y=std::strtod(s,&end);if(end==s||!std::isfinite(x)||!std::isfinite(y)){c.fail(Error::Parse,"Invalid static pixel pair");break;}if(!c.item(out.pixels.size()))break;out.pixels.push_back({x,y});s=end;while(std::isspace(static_cast<unsigned char>(*s)))++s;if(*s){if(s[0]!='|'||s[1]!='|'){c.fail(Error::Parse,"Invalid static pixel separator");break;}s+=2;if(!*s){c.fail(Error::Parse,"Incomplete static pixel pair");break;}}}okay=c.report.ok()&&!out.pixels.empty();if(okay){out.total=uint32_t(out.pixels.size());out.summary="Static map pixel coordinates";}}
  else {size_t begin=0;while(begin<count&&std::isspace(bytes[begin]))++begin;if((request.kind==Kind::WfsCapabilities||request.kind==Kind::WfsDescribe||request.kind==Kind::WfsFeatures)&&begin<count&&bytes[begin]=='<')okay=wfsXml(c,bytes,count,out);else if(request.kind==Kind::Drive&&begin<count&&bytes[begin]=='<')okay=driveXml(c,bytes,count,out);else {
#if defined(NICE_GFX_TDT_HAS_CJSON)
    okay=json(c,bytes,count,out);
#else
    c.fail(Error::Unsupported,"cJSON is required for JSON service responses");
#endif
  }}
  if(!okay&&c.report.ok())c.fail(Error::Parse,"Unrecognized service response");
  if(okay&&c.report.ok()){out.report=c.report;output=std::move(out);}return c.report;
}
} }
