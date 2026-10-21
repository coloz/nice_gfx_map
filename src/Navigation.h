#pragma once
#include "Map.h"

namespace nice_gfx_map {

// Spherical Earth estimates (mean radius 6371008.8 m), not ellipsoidal survey
// calculations. Bearings are clockwise from true north. Invalid inputs return
// NaN (both coordinates for destinationPoint); longitude wraps at the date line.
// Coincident points have bearing zero; exact antipodes have no unique bearing
// and initialBearingDegrees returns NaN. Negative destination distances fail.
double distanceMeters(GeoPoint a, GeoPoint b);
double initialBearingDegrees(GeoPoint a, GeoPoint b);
GeoPoint destinationPoint(GeoPoint start, double bearingDegrees, double meters);
// Distance to the minor great-circle arc; coincident/antipodal endpoints use
// the nearer endpoint because the antipodal arc has no unique bearing.
double distanceToSegmentMeters(GeoPoint point, GeoPoint a, GeoPoint b);

struct NavigationState {
  size_t nextIndex = 0;
  bool arrived = false, offRoute = false;
  double metersToNext = 0, bearingToNext = 0, remainingMeters = 0, crossTrackMeters = 0;
};

// Follows caller-supplied waypoints in order; does not calculate road routes.
// A waypoint advances only when a position enters its arrival radius.
// Arrival latches until setRoute/reset. Ambiguous bearings use zero in state.
class RouteNavigator {
 public:
  static constexpr size_t MaximumPoints = 4096;
  Status setRoute(const GeoPoint *points, size_t count,
                  double arrivalRadiusMeters = 20, double offRouteMeters = 60);
  Status update(GeoPoint position, NavigationState &state);
  void reset(); // Clears the route and progress.
 private:
  std::unique_ptr<GeoPoint[]> points_;
  std::unique_ptr<double[]> segmentMeters_; // Optional immutable per-edge cache.
  size_t count_ = 0, next_ = 0;
  double arrivalRadius_ = 20, offRoute_ = 60;
};

struct FenceResult {
  bool inside = false, entered = false, exited = false;
  double distanceToBoundaryMeters = 0;
};

class GeoFence {
 public:
  static constexpr size_t MaximumPoints = 1024;
  Status setCircle(GeoPoint center, double radiusMeters, double hysteresisMeters = 5);
  // Simple polygons following minor great-circle edges, including date-line
  // crossings. All vertices must project into the open hemisphere centered on
  // their mean unit vector. This conservative local-polygon restriction can
  // reject very wide polygons that fit some other hemisphere. Self-intersecting
  // and degenerate polygons are rejected.
  Status setPolygon(const GeoPoint *points, size_t count, double hysteresisMeters = 5);
  void reset(); // Clears the fence and its previous inside/outside state.
  Status update(GeoPoint position, FenceResult &result);
 private:
  enum class Kind : uint8_t { None, Circle, Polygon };
  Kind kind_ = Kind::None;
  GeoPoint center_{};
  double radius_ = 0, hysteresis_ = 5;
  std::unique_ptr<GeoPoint[]> points_;
  size_t count_ = 0;
  bool initialized_ = false, inside_ = false;
};

struct TimedPoint { uint32_t timeMs = 0; GeoPoint position; };
struct ReplaySample {
  GeoPoint position;
  double headingDegrees = 0;
  size_t segment = 0;
  bool finished = false;
};

class TrackReplay {
 public:
  static constexpr size_t MaximumPoints = 4096;
  // First timestamp must be zero; later timestamps must strictly increase.
  Status setTrack(const TimedPoint *points, size_t count);
  // Latitude and the shortest longitude difference interpolate linearly.
  // With looping, the final timestamp wraps to the first sample immediately.
  // A single point is already finished; stationary/antipodal headings use zero.
  Status sample(uint32_t elapsedMs, ReplaySample &sample, bool loop = false) const;
 private:
  std::unique_ptr<TimedPoint[]> points_;
  std::unique_ptr<double[]> segmentHeadings_; // Optional; allocation failure keeps sampling available.
  size_t count_ = 0;
};

} // namespace nice_gfx_map
