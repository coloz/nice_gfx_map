#pragma once
#include "Map.h"

namespace nice_gfx_map {
struct GnssFix {
  GeoPoint position;
  bool valid = false;
  double speedMetersPerSecond = 0, headingDegrees = 0, altitudeMeters = 0;
  uint8_t satellites = 0;
  uint32_t secondsOfDay = 0; // UTC 0..86399; 86400 represents 23:59:60.
};

// Bounded streaming NMEA 0183 parser. feed returns true once a checksum-valid,
// supported RMC/GGA sentence has been accepted. CR/LF ends the sentence. Invalid
// or unsupported input leaves fix unchanged. A valid no-fix sentence is accepted
// and sets valid=false; numeric fields remain their last successfully parsed
// values if absent. Feed UART bytes from the owner task, never from an ISR.
class NmeaParser {
 public:
  bool feed(char byte);
  const GnssFix &fix() const { return fix_; }
 private:
  bool parse();
  char sentence_[161] = {};
  size_t size_ = 0;
  bool receiving_ = false, overflow_ = false;
  GnssFix fix_{};
};
} // namespace nice_gfx_map
