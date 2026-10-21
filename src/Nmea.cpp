#include "Nmea.h"
#include <cmath>
#include <cstring>

namespace nice_gfx_map {
namespace {
int hex(char c) {
  if(c>='0'&&c<='9') return c-'0';
  if(c>='A'&&c<='F') return c-'A'+10;
  if(c>='a'&&c<='f') return c-'a'+10;
  return -1;
}
bool decimal(const char *text,double &value) {
  if(!text||!*text) return false;
  bool negative=false;if(*text=='-'||*text=='+') { negative=*text=='-';++text; }
  double result=0,factor=0.1;bool point=false,digit=false;
  for(;*text;++text) {
    if(*text=='.'&&!point) { point=true;continue; }
    if(*text<'0'||*text>'9') return false;
    digit=true;
    if(point) { result+=(*text-'0')*factor;factor*=0.1; }
    else result=result*10+(*text-'0');
    if(!std::isfinite(result)) return false;
  }
  if(!digit) return false;value=negative?-result:result;return true;
}
bool integer(const char *text,unsigned maximum,unsigned &value) {
  if(!text||!*text) return false;
  unsigned result=0;
  for(;*text;++text) {
    if(*text<'0'||*text>'9') return false;
    const unsigned digit=unsigned(*text-'0');
    if(digit>maximum||result>(maximum-digit)/10) return false;
    result=result*10+digit;
    if(result>maximum) return false;
  }
  value=result;return true;
}
bool time(const char *text,uint32_t &seconds) {
  const size_t size=std::strlen(text);
  if(size<6) return false;
  for(size_t i=0;i<6;++i) if(text[i]<'0'||text[i]>'9') return false;
  if(size>6) {
    if(text[6]!='.'||size==7) return false;
    for(size_t i=7;i<size;++i) if(text[i]<'0'||text[i]>'9') return false;
  }
  const unsigned hour=unsigned(text[0]-'0')*10+unsigned(text[1]-'0');
  const unsigned minute=unsigned(text[2]-'0')*10+unsigned(text[3]-'0');
  const unsigned second=unsigned(text[4]-'0')*10+unsigned(text[5]-'0');
  if(hour>23||minute>59||second>60||(second==60&&(hour!=23||minute!=59))) return false;
  seconds=hour*3600+minute*60+second;return true;
}
bool coordinate(const char *number,const char *hemisphere,bool latitude,double &value) {
  const size_t degrees=latitude?2:3,length=std::strlen(number);
  if(length<degrees+2||std::strlen(hemisphere)!=1) return false;
  for(size_t i=0;i<degrees+2;++i) if(number[i]<'0'||number[i]>'9') return false;
  if(length>degrees+2&&number[degrees+2]!='.') return false;
  double minutes=0;if(!decimal(number+degrees,minutes)||minutes<0||minutes>=60) return false;
  unsigned deg=0;for(size_t i=0;i<degrees;++i) deg=deg*10+unsigned(number[i]-'0');
  const unsigned maximum=latitude?90:180;
  if(deg>maximum||(deg==maximum&&minutes!=0)) return false;
  const char positive=latitude?'N':'E',negative=latitude?'S':'W';
  if(*hemisphere!=positive&&*hemisphere!=negative) return false;
  value=(double(deg)+minutes/60)*(*hemisphere==negative?-1:1);return true;
}
bool position(char **fields,size_t lat,size_t lon,bool required,GeoPoint &point) {
  const bool present=*fields[lat]||*fields[lat+1]||*fields[lon]||*fields[lon+1];
  if(!present) return !required;
  return coordinate(fields[lat],fields[lat+1],true,point.latitude)&&
      coordinate(fields[lon],fields[lon+1],false,point.longitude);
}
bool date(const char *text) {
  if(!*text) return true;
  if(std::strlen(text)!=6) return false;
  for(size_t i=0;i<6;++i) if(text[i]<'0'||text[i]>'9') return false;
  const unsigned day=unsigned(text[0]-'0')*10+unsigned(text[1]-'0');
  const unsigned month=unsigned(text[2]-'0')*10+unsigned(text[3]-'0');
  const unsigned year=unsigned(text[4]-'0')*10+unsigned(text[5]-'0');
  if(!month||month>12||!day) return false;
  const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  return day<=days[month-1]+unsigned(month==2&&year%4==0);
}
} // namespace

bool NmeaParser::feed(char byte) {
  if(byte=='$') { size_=0;receiving_=true;overflow_=false;return false; }
  if(!receiving_) return false;
  if(byte=='\r'||byte=='\n') {
    receiving_=false;
    if(overflow_||!size_) return false;
    sentence_[size_]='\0';return parse();
  }
  const unsigned char c=static_cast<unsigned char>(byte);
  if(c<0x20||c>0x7e) { receiving_=false;return false; }
  if(size_>=160) { overflow_=true;return false; }
  sentence_[size_++]=byte;return false;
}

bool NmeaParser::parse() {
  char *star=std::strchr(sentence_,'*');
  if(!star||std::strlen(star)!=3||hex(star[1])<0||hex(star[2])<0) return false;
  unsigned checksum=0;for(const char *p=sentence_;p<star;++p) checksum^=static_cast<unsigned char>(*p);
  if(checksum!=unsigned(hex(star[1])*16+hex(star[2]))) return false;
  *star='\0';
  char *fields[32]={sentence_};size_t count=1;
  for(char *p=sentence_;p<star;++p) if(*p==',') {
    if(count==32) return false;
    *p='\0';fields[count++]=p+1;
  }
  const bool rmc=std::strcmp(fields[0],"GPRMC")==0||std::strcmp(fields[0],"GNRMC")==0;
  const bool gga=std::strcmp(fields[0],"GPGGA")==0||std::strcmp(fields[0],"GNGGA")==0;
  if(!rmc&&!gga) return false;
  GnssFix candidate=fix_;
  if(rmc) {
    if(count<10||std::strlen(fields[2])!=1||(*fields[2]!='A'&&*fields[2]!='V')) return false;
    candidate.valid=*fields[2]=='A';
    if(count>12&&*fields[12]) {
      if(std::strlen(fields[12])!=1||!std::strchr("ADEFMNPRS",*fields[12])) return false;
      if(*fields[12]=='N') candidate.valid=false;
    }
    if((*fields[1]&&!time(fields[1],candidate.secondsOfDay))||(!*fields[1]&&candidate.valid)||
       !position(fields,3,5,candidate.valid,candidate.position)||!date(fields[9])) return false;
    double number=0;
    if(*fields[7]) {
      if(!decimal(fields[7],number)||number<0) return false;
      candidate.speedMetersPerSecond=number*(1852.0/3600.0);
      if(!std::isfinite(candidate.speedMetersPerSecond)) return false;
    }
    if(*fields[8]) {
      if(!decimal(fields[8],number)||number<0||number>360) return false;
      candidate.headingDegrees=number==360?0:number;
    }
    if(count>10&&*fields[10]) {
      if(!decimal(fields[10],number)||number<0||number>180||count<12||
         std::strlen(fields[11])!=1||(*fields[11]!='E'&&*fields[11]!='W')) return false;
    } else if(count>11&&*fields[11]) return false;
  } else {
    if(count<10) return false;
    unsigned quality=0,satellites=0;
    if(!integer(fields[6],8,quality)) return false;
    candidate.valid=quality!=0;
    if((*fields[1]&&!time(fields[1],candidate.secondsOfDay))||(!*fields[1]&&candidate.valid)||
       !position(fields,2,4,candidate.valid,candidate.position)) return false;
    if(*fields[7]) { if(!integer(fields[7],255,satellites)) return false;candidate.satellites=uint8_t(satellites); }
    double number=0;
    if(*fields[8]&&(!decimal(fields[8],number)||number<0)) return false;
    if(*fields[9]) {
      if(!decimal(fields[9],number)||count<11||std::strcmp(fields[10],"M")!=0) return false;
      candidate.altitudeMeters=number;
    }
    if(count>11&&*fields[11])
      if(!decimal(fields[11],number)||count<13||std::strcmp(fields[12],"M")!=0) return false;
  }
  fix_=candidate;return true;
}
} // namespace nice_gfx_map
