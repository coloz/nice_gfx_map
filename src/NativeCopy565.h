#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace nice_gfx_map { namespace detail {
// Native RGB565 spans are halfword aligned. Stream aligned destination words
// even when clipping puts the source on the other halfword. Every read stays
// inside the supplied span, including a final row at an allocation boundary.
inline void nativeCopy565(uint16_t *destination,const uint16_t *source,size_t count) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  if(!count)return;
  typedef uint32_t CopyWord __attribute__((__may_alias__));
  if(reinterpret_cast<uintptr_t>(destination)&2){*destination++=*source++;if(!--count)return;}
  auto *output=reinterpret_cast<CopyWord *>(destination);
  if(!(reinterpret_cast<uintptr_t>(source)&2)){
    const auto *input=reinterpret_cast<const CopyWord *>(source);
    while(count>=8){output[0]=input[0];output[1]=input[1];output[2]=input[2];output[3]=input[3];output+=4;input+=4;count-=8;}
    while(count>=2){*output++=*input++;count-=2;}
    if(count){uint16_t tail;__builtin_memcpy(&tail,input,2);*reinterpret_cast<uint16_t *>(output)=tail;}
    return;
  }
  uint16_t carry=*source;
  const auto *input=reinterpret_cast<const CopyWord *>(source+1);
  while(count>=9){
    const uint32_t a=input[0],b=input[1],c=input[2],d=input[3];
    output[0]=uint32_t(carry)|(a<<16);output[1]=(a>>16)|(b<<16);
    output[2]=(b>>16)|(c<<16);output[3]=(c>>16)|(d<<16);
    carry=uint16_t(d>>16);output+=4;input+=4;count-=8;
  }
  while(count>=3){const uint32_t word=*input++;*output++=uint32_t(carry)|(word<<16);carry=uint16_t(word>>16);count-=2;}
  auto *tail=reinterpret_cast<uint16_t *>(output);tail[0]=carry;
  if(count==2){uint16_t last;__builtin_memcpy(&last,input,2);tail[1]=last;}
#else
  memcpy(destination,source,count*2);
#endif
}
}}
