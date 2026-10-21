#pragma once
#include "Map.h"
#include <cmath>

namespace nice_gfx_map {

// Allocation-free, single-pointer map gesture. Feed viewport-local positions
// from the Map owner's task. Begin only after UI hit testing has accepted this
// map; a captured pointer may move outside it until end/cancel. UI controls
// layered over the map should receive their own pointer presses first.
class TouchPan {
 public:
  explicit TouchPan(uint16_t thresholdPixels = 4) : threshold_(thresholdPixels) {}

  bool begin(const Map &map, PixelPoint point) {
    if (active_ || !finite(point) || point.x < 0 || point.y < 0 ||
        point.x >= map.width() || point.y >= map.height()) return false;
    origin_ = last_ = point;
    active_ = true;
    dragging_ = false;
    return true;
  }

  Status move(Map &map, PixelPoint point) {
    if (!finite(point)) return Status::InvalidArgument;
    if (!active_) return Status::NotFound;
    const double dx = point.x - last_.x, dy = point.y - last_.y;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return Status::InvalidArgument;
    if (!dragging_) {
      const double totalX = point.x - origin_.x, totalY = point.y - origin_.y;
      if (std::hypot(totalX, totalY) < threshold_ || (!dx && !dy)) return Status::Ok;
    } else if (!dx && !dy) return Status::Ok;
    const Status result = map.panBy(dx, dy);
    if (result == Status::Ok) { last_ = point; dragging_ = true; }
    return result;
  }

  // True means the gesture moved the map; suppress the corresponding click.
  bool end() { const bool consumed = dragging_; cancel(); return consumed; }
  void cancel() { active_ = dragging_ = false; }
  bool active() const { return active_; }
  bool dragging() const { return dragging_; }

 private:
  static bool finite(PixelPoint point) { return std::isfinite(point.x) && std::isfinite(point.y); }
  PixelPoint origin_, last_;
  uint16_t threshold_;
  bool active_ = false, dragging_ = false;
};

} // namespace nice_gfx_map
