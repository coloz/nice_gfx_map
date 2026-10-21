#pragma once
#include "Pipeline.h"
#include "TileGrid.h"

namespace nice_gfx_map { namespace wmts {
struct Matrix {
  std::string identifier;
  double scaleDenominator = 0, topLeftFirst = 0, topLeftSecond = 0;
  uint16_t tileWidth = 0, tileHeight = 0;
  uint32_t width = 0, height = 0;
};
struct MatrixLimits {
  std::string identifier;
  uint32_t minRow = 0, maxRow = 0, minCol = 0, maxCol = 0;
};
struct Link { std::string matrixSet; std::vector<MatrixLimits> limits; };
struct Layer {
  std::string identifier, title;
  std::vector<std::string> styles, formats;
  std::vector<Link> links;
};
struct MatrixSet { std::string identifier, supportedCrs; std::vector<Matrix> matrices; };
struct Capabilities { std::vector<Layer> layers; std::vector<MatrixSet> matrixSets; };
struct ParseLimits {
  size_t bytes = 256 * 1024, layers = 16, matrixSets = 16, matrices = 32,
    textBytes = 512, depth = 32;
};
// Bounded XML subset, no DOM or external dependency. DTD/entities are not
// loaded; comments and namespace prefixes are accepted. Error preserves output.
Status parseCapabilities(const uint8_t *bytes, size_t count, Capabilities &output,
                         const ParseLimits &limits = {});
enum Warning : uint32_t {
  None = 0, TiandituNorthEastAxes = 1, DeclaredScaleDiffers = 2,
  IncompatibleMatrix = 4, TiandituCtaWMercatorOverride = 8
};
struct GridOptions {
  uint8_t minZoom = 0, maxZoom = 22;
  bool skipIncompatible = false; // Explicitly retain only a contiguous valid subset.
  // Opt in to the documented cta_w metadata defect: geographic origin/rows
  // advertised for tiles actually using Mercator. Never applies to other layers.
  bool useTiandituCtaWMercatorOverride = false;
};
// Interpret one linked matrix set as a regular global grid. Raw metadata stays
// in Capabilities; conflicting CRS/origin/dimensions never silently change CRS.
Status tileGrid(const Capabilities &capabilities, const std::string &layer,
                const std::string &matrixSet, TileGrid &output,
                const GridOptions &options = {});
} } // namespace nice_gfx_map::wmts
