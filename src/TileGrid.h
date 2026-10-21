#pragma once
#include "Map.h"
#include <cmath>
#include <limits>

namespace nice_gfx_map {
// Projection is independent of the geographic datum. Geographic WMTS grids
// use longitude/latitude degrees; no WGS84/CGCS2000/GCJ-02 conversion is implied.
enum class Projection : uint8_t { WebMercator, Geographic };

struct TileMatrix {
  std::string identifier;
  uint8_t zoom = 0; // Internal power-of-two column level, not an assumed WMTS ID.
  uint32_t width = 1, height = 1;
  uint16_t tileWidth = 256, tileHeight = 256;
  double scaleDenominator = 0; // Original metadata; never drives raster sampling.
  double topLeftFirst = 0, topLeftSecond = 0; // Preserve advertised axis order.
  bool limited = false;
  uint32_t minRow = 0, maxRow = 0, minCol = 0, maxCol = 0;
};

// The renderer supports regular global quad grids only. Explicit matrices keep
// identifiers, row counts and regional limits from capabilities. Empty matrices
// mean the legacy global Web Mercator grid, retaining the old source contract.
struct TileGrid {
  Projection projection = Projection::WebMercator;
  std::string identifier, supportedCrs;
  std::vector<TileMatrix> matrices;
  uint32_t metadataWarnings = 0;

  const TileMatrix *matrix(uint8_t zoom) const {
    for (const auto &value : matrices) if (value.zoom == zoom) return &value;
    return nullptr;
  }
  uint32_t columns(uint8_t zoom) const {
    if (zoom > 22) return 0;
    const auto *value = matrix(zoom);
    return value ? value->width : matrices.empty() ? uint32_t(1) << zoom : 0;
  }
  uint32_t rows(uint8_t zoom) const {
    if (zoom > 22) return 0;
    const auto *value = matrix(zoom);
    if (value) return value->height;
    return matrices.empty() && projection == Projection::WebMercator ? uint32_t(1) << zoom : 0;
  }
  bool contains(const TileKey &key) const {
    if (key.x >= columns(key.zoom) || key.y >= rows(key.zoom)) return false;
    const auto *value = matrix(key.zoom);
    return !value || !value->limited || (key.x >= value->minCol && key.x <= value->maxCol &&
      key.y >= value->minRow && key.y <= value->maxRow);
  }
  bool valid() const {
    if (projection != Projection::WebMercator && projection != Projection::Geographic) return false;
    if (identifier.size() > 128 || supportedCrs.size() > 256 || matrices.size() > 23) return false;
    if (matrices.empty()) return projection == Projection::WebMercator;
    uint8_t previous = 0; bool first = true;
    uint16_t size = matrices.front().tileWidth;
    if (size != 128 && size != 256 && size != 512) return false;
    for (const auto &value : matrices) {
      if (value.identifier.empty() || value.identifier.size() > 128 || value.zoom > 22 ||
          (!first && value.zoom != previous + 1) || value.tileWidth != size || value.tileHeight != size ||
          value.width != (uint32_t(1) << value.zoom) ||
          value.height != (projection == Projection::WebMercator ? value.width : value.width / 2) ||
          !value.height || !std::isfinite(value.scaleDenominator) || value.scaleDenominator < 0 ||
          !std::isfinite(value.topLeftFirst) || !std::isfinite(value.topLeftSecond)) return false;
      if (value.limited && (value.minCol > value.maxCol || value.maxCol >= value.width ||
          value.minRow > value.maxRow || value.maxRow >= value.height)) return false;
      previous = value.zoom; first = false;
    }
    return true;
  }
};
} // namespace nice_gfx_map
