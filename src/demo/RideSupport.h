#pragma once
#include "../Navigation.h"
#include "../Nmea.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nice_gfx_map { namespace demo {

enum class RouteCsvStatus : uint8_t {
  Reading, Complete, InvalidHeader, InvalidData, LineTooLong,
  TooManyPoints, TooFewPoints
};

inline const char *routeCsvStatusText(RouteCsvStatus status) {
  switch(status) {
    case RouteCsvStatus::Reading: return "reading";
    case RouteCsvStatus::Complete: return "complete";
    case RouteCsvStatus::InvalidHeader: return "expected latitude,longitude header";
    case RouteCsvStatus::InvalidData: return "invalid latitude/longitude row";
    case RouteCsvStatus::LineTooLong: return "route line exceeds 127 bytes";
    case RouteCsvStatus::TooManyPoints: return "route exceeds 1024 points";
    case RouteCsvStatus::TooFewPoints: return "route needs at least two points";
  }
  return "unknown route error";
}

// Bounded, allocation-free CSV reader. Format: latitude,longitude header, then
// decimal degree pairs. Leading/trailing spaces, blank lines, and whole-line #
// comments are allowed; quoted fields/extra columns/BOM are not. CR, LF and CRLF
// are accepted. Call finish() at EOF, including after a trailing newline. A route
// is exposed only when the ENTIRE file is valid and contains 2..1024 points.
// This object owns 16 KiB of point storage: keep it global, not on a task stack.
class CsvRouteReader {
 public:
  static constexpr size_t MaximumPoints = 1024;
  static constexpr size_t MaximumLineBytes = 127;
  bool feed(char byte) {
    if(status_ != RouteCsvStatus::Reading) return false;
    if(byte == '\n' && afterCr_) { afterCr_ = false; return true; }
    afterCr_ = false;
    if(byte == '\r' || byte == '\n') {
      afterCr_ = byte == '\r';
      return consumeLine();
    }
    const auto value = static_cast<unsigned char>(byte);
    if(value == 0 || (value < 32 && byte != '\t') || value == 127) {
      status_ = RouteCsvStatus::InvalidData; return false;
    }
    if(lineSize_ == MaximumLineBytes) {
      status_ = RouteCsvStatus::LineTooLong; return false;
    }
    line_[lineSize_++] = byte;
    return true;
  }
  bool finish() {
    if(status_ == RouteCsvStatus::Complete) return true;
    if(status_ != RouteCsvStatus::Reading) return false;
    if(lineSize_ && !consumeLine()) return false;
    if(!headerSeen_) status_ = RouteCsvStatus::InvalidHeader;
    else if(count_ < 2) status_ = RouteCsvStatus::TooFewPoints;
    else status_ = RouteCsvStatus::Complete;
    return status_ == RouteCsvStatus::Complete;
  }
  void reset() {
    status_ = RouteCsvStatus::Reading; count_ = lineSize_ = 0;
    headerSeen_ = afterCr_ = false;
  }
  RouteCsvStatus status() const { return status_; }
  const GeoPoint *points() const {
    return status_ == RouteCsvStatus::Complete ? points_ : nullptr;
  }
  size_t count() const { return status_ == RouteCsvStatus::Complete ? count_ : 0; }
 private:
  static char *trim(char *text) {
    while(*text == ' ' || *text == '\t') ++text;
    size_t size = std::strlen(text);
    while(size && (text[size-1] == ' ' || text[size-1] == '\t')) text[--size] = 0;
    return text;
  }
  static bool number(char *text, double &value) {
    text = trim(text);
    const char *scan = text;
    if(*scan == '+' || *scan == '-') ++scan;
    size_t digits = 0;
    while(*scan >= '0' && *scan <= '9') { ++scan; ++digits; }
    if(*scan == '.') {
      ++scan;
      while(*scan >= '0' && *scan <= '9') { ++scan; ++digits; }
    }
    if(!digits) return false;
    if(*scan == 'e' || *scan == 'E') {
      ++scan; if(*scan == '+' || *scan == '-') ++scan;
      size_t exponentDigits = 0;
      while(*scan >= '0' && *scan <= '9') { ++scan; ++exponentDigits; }
      if(!exponentDigits) return false;
    }
    if(*scan) return false;
    errno = 0; char *end = nullptr;
    value = std::strtod(text, &end);
    return end == scan && errno != ERANGE && std::isfinite(value);
  }
  bool consumeLine() {
    line_[lineSize_] = 0; lineSize_ = 0;
    char *text = trim(line_);
    if(!*text || *text == '#') return true;
    char *comma = std::strchr(text, ',');
    if(!comma || std::strchr(comma + 1, ',')) {
      status_ = headerSeen_ ? RouteCsvStatus::InvalidData : RouteCsvStatus::InvalidHeader;
      return false;
    }
    *comma++ = 0;
    if(!headerSeen_) {
      if(std::strcmp(trim(text), "latitude") || std::strcmp(trim(comma), "longitude")) {
        status_ = RouteCsvStatus::InvalidHeader; return false;
      }
      headerSeen_ = true; return true;
    }
    GeoPoint point;
    if(!number(text, point.latitude) || !number(comma, point.longitude) ||
       point.latitude < -90 || point.latitude > 90 ||
       point.longitude < -180 || point.longitude > 180) {
      status_ = RouteCsvStatus::InvalidData; return false;
    }
    if(count_ == MaximumPoints) { status_ = RouteCsvStatus::TooManyPoints; return false; }
    points_[count_++] = point;
    return true;
  }
  GeoPoint points_[MaximumPoints]{};
  char line_[MaximumLineBytes + 1]{};
  size_t count_ = 0, lineSize_ = 0;
  RouteCsvStatus status_ = RouteCsvStatus::Reading;
  bool headerSeen_ = false, afterCr_ = false;
};

struct RideSummary {
  double distanceMeters = 0;
  double averageKmh = 0, movingAverageKmh = 0, maxKmh = 0;
  uint64_t durationMs = 0, movingTimeMs = 0;
  uint32_t acceptedPoints = 0, rejectedPoints = 0;
};

// Bicycle estimates from accepted fixes, not a wheel sensor. Duration includes
// stops, pauses and no-fix time, measured between observed timestamps after the
// first accepted fix. Average uses that duration. Distance/moving time never
// bridge pause, no-fix, explicit breakSegment(), or >5 s gaps. A segment moves
// when its spatial or reported speed is >=0.8 m/s. GNSS drift can still inflate
// distance. Call at least once per half millis() wrap (~24 days); uint32 wrap is
// handled. Repeated/out-of-order timestamps are rejected. No-fix/paused input
// does not increment rejectedPoints. A rejected outlier preserves the previous
// anchor until a valid point or a gap; accept() false must not be logged as a fix.
class RideStatistics {
 public:
  static constexpr uint32_t MaximumGapMs = 5000;
  static constexpr double MaximumSpeedMetersPerSecond = 35; // 126 km/h
  static constexpr double MinimumMovingMetersPerSecond = 0.8;
  static constexpr double JumpAllowanceMeters = 8;
  bool accept(const GnssFix &fix, uint32_t uptimeMs) {
    if(clockStarted_) {
      const uint32_t elapsed = uptimeMs - lastObservedMs_;
      if(elapsed > 0x7fffffffu) { ++summary_.rejectedPoints; return false; }
      summary_.durationMs += elapsed;
      lastObservedMs_ = uptimeMs;
      updateAverages();
    }
    if(paused_ || !fix.valid) { breakSegment(); return false; }
    if(!validFix(fix)) { ++summary_.rejectedPoints; return false; }
    if(!clockStarted_) { clockStarted_ = true; lastObservedMs_ = uptimeMs; }
    if(anchorValid_) {
      const uint32_t elapsed = uptimeMs - lastAcceptedMs_;
      if(!elapsed) { ++summary_.rejectedPoints; return false; }
      if(elapsed <= MaximumGapMs) {
        const double seconds = double(elapsed) / 1000;
        const double distance = distanceMeters(lastPosition_, fix.position);
        if(!std::isfinite(distance) ||
           distance > MaximumSpeedMetersPerSecond * seconds + JumpAllowanceMeters) {
          ++summary_.rejectedPoints; return false;
        }
        summary_.distanceMeters += distance;
        const double spatialSpeed = distance / seconds;
        if(spatialSpeed >= MinimumMovingMetersPerSecond ||
           fix.speedMetersPerSecond >= MinimumMovingMetersPerSecond) {
          summary_.movingTimeMs += elapsed;
        }
        // A small positional tolerance admits GNSS jitter in short samples; it
        // must not turn into a reported speed above the bicycle plausibility cap.
        const double maximum = std::fmin(MaximumSpeedMetersPerSecond,
          std::fmax(spatialSpeed, fix.speedMetersPerSecond)) * 3.6;
        summary_.maxKmh = std::fmax(summary_.maxKmh, maximum);
      }
    }
    summary_.maxKmh = std::fmax(summary_.maxKmh, fix.speedMetersPerSecond * 3.6);
    lastPosition_ = fix.position; lastAcceptedMs_ = uptimeMs; anchorValid_ = true;
    ++summary_.acceptedPoints; updateAverages(); return true;
  }
  void breakSegment() { anchorValid_ = false; }
  void setPaused(bool paused) { if(paused_ != paused) breakSegment(); paused_ = paused; }
  bool paused() const { return paused_; }
  void reset() { *this = RideStatistics{}; }
  const RideSummary &summary() const { return summary_; }
 private:
  static bool validFix(const GnssFix &fix) {
    return std::isfinite(fix.position.latitude) && std::isfinite(fix.position.longitude) &&
      fix.position.latitude >= -90 && fix.position.latitude <= 90 &&
      fix.position.longitude >= -180 && fix.position.longitude <= 180 &&
      std::isfinite(fix.speedMetersPerSecond) && fix.speedMetersPerSecond >= 0 &&
      fix.speedMetersPerSecond <= MaximumSpeedMetersPerSecond &&
      std::isfinite(fix.altitudeMeters) && fix.secondsOfDay <= 86400;
  }
  void updateAverages() {
    summary_.averageKmh = summary_.durationMs ?
      summary_.distanceMeters * 3600 / double(summary_.durationMs) : 0;
    summary_.movingAverageKmh = summary_.movingTimeMs ?
      summary_.distanceMeters * 3600 / double(summary_.movingTimeMs) : 0;
  }
  RideSummary summary_{};
  GeoPoint lastPosition_{};
  uint32_t lastObservedMs_ = 0, lastAcceptedMs_ = 0;
  bool clockStarted_ = false, anchorValid_ = false, paused_ = false;
};

inline constexpr char RideCsvHeader[] =
  "uptime_ms,utc_seconds,latitude,longitude,speed_kmh,altitude_m,satellites,distance_m,moving_ms,duration_ms\n";

// Fixed numeric columns, no dynamic allocation. Use the C numeric locale (the
// Arduino default). Returns bytes excluding NUL; zero and an empty destination
// mean invalid data or insufficient space. Call only for a fix accepted by the
// statistics object. UTC is GNSS seconds-of-day, not an inferred calendar date.
inline size_t formatRideCsv(char *destination, size_t capacity, uint32_t uptimeMs,
                           const GnssFix &fix, const RideSummary &summary) {
  if(!destination || !capacity) return 0;
  destination[0] = 0;
  if(!fix.valid || !std::isfinite(fix.position.latitude) ||
     !std::isfinite(fix.position.longitude) || fix.position.latitude < -90 ||
     fix.position.latitude > 90 || fix.position.longitude < -180 ||
     fix.position.longitude > 180 || !std::isfinite(fix.speedMetersPerSecond) ||
     fix.speedMetersPerSecond < 0 ||
     fix.speedMetersPerSecond > RideStatistics::MaximumSpeedMetersPerSecond ||
     !std::isfinite(fix.altitudeMeters) || fix.secondsOfDay > 86400 ||
     !std::isfinite(summary.distanceMeters) || summary.distanceMeters < 0) return 0;
  const int length = std::snprintf(destination, capacity,
    "%lu,%lu,%.7f,%.7f,%.2f,%.2f,%u,%.2f,%llu,%llu\n",
    static_cast<unsigned long>(uptimeMs), static_cast<unsigned long>(fix.secondsOfDay),
    fix.position.latitude, fix.position.longitude, fix.speedMetersPerSecond * 3.6,
    fix.altitudeMeters, static_cast<unsigned>(fix.satellites), summary.distanceMeters,
    static_cast<unsigned long long>(summary.movingTimeMs),
    static_cast<unsigned long long>(summary.durationMs));
  if(length <= 0 || static_cast<size_t>(length) >= capacity) { destination[0] = 0; return 0; }
  return static_cast<size_t>(length);
}

}} // namespace nice_gfx_map::demo
