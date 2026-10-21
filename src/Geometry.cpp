#include "Map.h"
#include "TileGrid.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nice_gfx_map {
namespace {
constexpr double Pi = 3.1415926535897932384626433832795;

bool validProjection(uint8_t zoom, uint16_t tileSize) {
  return zoom <= 22 && (tileSize == 128 || tileSize == 256 || tileSize == 512);
}

double invalidNumber() { return std::numeric_limits<double>::quiet_NaN(); }
} // namespace

const char *statusName(Status status) {
  switch (status) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::NoMemory: return "NoMemory";
    case Status::NotFound: return "NotFound";
    case Status::Busy: return "Busy";
    case Status::NetworkError: return "NetworkError";
    case Status::DecodeError: return "DecodeError";
    case Status::Unsupported: return "Unsupported";
    case Status::TooLarge: return "TooLarge";
    case Status::Canceled: return "Canceled";
    case Status::IoError: return "IoError";
  }
  return "Unknown";
}

bool validCoordinate(GeoPoint point) {
  // Longitudes may cross the antimeridian during panning; project normalizes
  // them. Latitudes outside the globe indicate an invalid input, not a pole.
  return std::isfinite(point.latitude) && std::isfinite(point.longitude) &&
         point.latitude >= -90.0 && point.latitude <= 90.0;
}

double normalizeLongitude(double longitude) {
  if (!std::isfinite(longitude)) return invalidNumber();
  // Reduce before adding 180 so even very large finite inputs remain finite.
  double result = std::fmod(longitude, 360.0);
  if (result >= 180.0) result -= 360.0;
  if (result < -180.0) result += 360.0;
  return result == 0.0 ? 0.0 : result;
}

PixelPoint project(GeoPoint point, uint8_t zoom, uint16_t tileSize) {
  if (!validCoordinate(point) || !validProjection(zoom, tileSize))
    return {invalidNumber(), invalidNumber()};
  const double world = std::ldexp(double(tileSize), zoom);
  const double latitude = std::max(-MercatorLatitudeLimit,
                                   std::min(MercatorLatitudeLimit, point.latitude));
  const double longitude = normalizeLongitude(point.longitude);
  const double latitudeRadians = latitude * Pi / 180.0;
  const double y = (1.0 - std::asinh(std::tan(latitudeRadians)) / Pi) * 0.5;
  return {(longitude + 180.0) / 360.0 * world,
          std::max(0.0, std::min(1.0, y)) * world};
}

GeoPoint unproject(PixelPoint point, uint8_t zoom, uint16_t tileSize) {
  if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
      !validProjection(zoom, tileSize))
    return {invalidNumber(), invalidNumber()};
  const double world = std::ldexp(double(tileSize), zoom);
  // Wrap x before converting to degrees to avoid overflow for large finite x.
  double x = std::fmod(point.x, world);
  if (x < 0.0) x += world;
  const double y = std::max(0.0, std::min(world, point.y));
  const double latitude = std::atan(std::sinh(Pi * (1.0 - 2.0 * y / world))) * 180.0 / Pi;
  return {latitude, normalizeLongitude(x / world * 360.0 - 180.0)};
}

PixelPoint project(GeoPoint point, uint8_t zoom, uint16_t tileSize, Projection projection) {
  if (projection == Projection::WebMercator) return project(point, zoom, tileSize);
  if (projection != Projection::Geographic || !validCoordinate(point) || !validProjection(zoom, tileSize))
    return {invalidNumber(), invalidNumber()};
  const double world = std::ldexp(double(tileSize), zoom);
  return {(normalizeLongitude(point.longitude) + 180.0) / 360.0 * world,
          (90.0 - point.latitude) / 360.0 * world};
}
GeoPoint unproject(PixelPoint point, uint8_t zoom, uint16_t tileSize, Projection projection) {
  if (projection == Projection::WebMercator) return unproject(point, zoom, tileSize);
  if (projection != Projection::Geographic || !std::isfinite(point.x) || !std::isfinite(point.y) ||
      !validProjection(zoom, tileSize)) return {invalidNumber(), invalidNumber()};
  const double world = std::ldexp(double(tileSize), zoom);
  double x = std::fmod(point.x, world); if (x < 0) x += world;
  const double y = std::max(0.0, std::min(world / 2, point.y));
  return {90.0 - y / world * 360.0, normalizeLongitude(x / world * 360.0 - 180.0)};
}

} // namespace nice_gfx_map
