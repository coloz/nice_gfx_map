#pragma once
#include "Map.h"
#include <algorithm>
#include <cmath>

namespace nice_gfx_map {

// One owner task feeds a captured pair; driver contact IDs/reordering remain
// the application's responsibility. Distances and centroids are swap invariant.
class PinchGesture {
 public:
  explicit PinchGesture(double minimumSpacing = 12) : minimumSpacing_(minimumSpacing) {}
  bool begin(Map &map, PixelPoint a, PixelPoint b) {
    if (active_ || !finite(a) || !finite(b) || !std::isfinite(minimumSpacing_) || minimumSpacing_ <= 0 ||
        a.x < 0 || a.y < 0 || b.x < 0 || b.y < 0 ||
        a.x >= map.width() || b.x >= map.width() || a.y >= map.height() || b.y >= map.height()) return false;
    const double distance = std::hypot(a.x - b.x, a.y - b.y);
    if (!std::isfinite(distance) || distance < minimumSpacing_) return false;
    const PixelPoint middle{a.x / 2 + b.x / 2, a.y / 2 + b.y / 2};
    const GeoPoint anchor = map.fromScreen(middle);
    if (!std::isfinite(anchor.latitude) || !std::isfinite(anchor.longitude)) return false;
    anchor_ = anchor; lastMiddle_ = middle; distance_ = distance; zoom_ = map.viewZoom();
    active_ = true; changed_ = false;
    return true;
  }
  Status move(Map &map, PixelPoint a, PixelPoint b) {
    if (!active_) return Status::NotFound;
    if (!finite(a) || !finite(b)) return Status::InvalidArgument;
    const double distance = std::hypot(a.x - b.x, a.y - b.y);
    if (!std::isfinite(distance)) return Status::InvalidArgument;
    const PixelPoint middle{a.x / 2 + b.x / 2, a.y / 2 + b.y / 2};
    const double minimum = map.source().valid() ? map.source().minZoom : 0;
    const double maximum = map.source().valid() ? map.source().maxZoom : 22;
    const double target = std::max(minimum, std::min(maximum,
      zoom_ + std::log2(std::max(distance, minimumSpacing_) / distance_)));
    // Tiny spacing/contact jitter does not force a complete map publication.
    if (std::abs(target - map.viewZoom()) < 0.0025 &&
        std::hypot(middle.x - lastMiddle_.x, middle.y - lastMiddle_.y) < 0.25) return Status::Ok;
    Status status = map.setViewZoomAt(target, double(map.width()) / 2, double(map.height()) / 2);
    if (status != Status::Ok) return status;
    const auto projected = map.toScreen(anchor_);
    status = map.panBy(middle.x - projected.x, middle.y - projected.y);
    if (status == Status::Ok) { lastMiddle_ = middle; changed_ = true; }
    return status;
  }
  bool end() { const bool changed = changed_; cancel(); return changed; }
  void cancel() { active_ = changed_ = false; }
  bool active() const { return active_; }
  bool changed() const { return changed_; }
 private:
  static bool finite(PixelPoint point) { return std::isfinite(point.x) && std::isfinite(point.y); }
  GeoPoint anchor_{};
  PixelPoint lastMiddle_{};
  double minimumSpacing_ = 12, distance_ = 0, zoom_ = 0;
  bool active_ = false, changed_ = false;
};

} // namespace nice_gfx_map
