#pragma once
#include "../TiandituServices.h"
namespace nice_gfx_map { namespace tianditu {
// cJSON is already linked by ESP32 Arduino/IDF. Other platforms may supply it;
// without cJSON JSON services report Unsupported, while XML/pixels still work.
// Parsing enforces input/node/depth/item/geometry/text limits and never logs.
class ResponseParser final : public Parser {
 public:
  Report parse(const Request &,const uint8_t *,size_t,Result &,
               const Limits &limits={}) override;
};
} }
