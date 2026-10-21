#pragma once
#include "Map.h"
#include "TiandituWmts.h"

namespace nice_gfx_map { namespace providers {
// Legacy factories use Web Mercator raster tiles. Credentials are caller supplied;
// no keys, blanket offline permissions, or account quotas are bundled.
TileSource tianditu(const std::string &key, bool labels = true);
enum class TiandituLayer : uint8_t { Vector, VectorLabels, Imagery, ImageryLabels,
                                   Terrain, TerrainLabels, Boundaries };
struct TiandituOptions {
  TiandituLayer layer = TiandituLayer::Vector;
  Projection projection = Projection::WebMercator;
  uint8_t server = 0; // Stable t0..t7; do not create eight parallel TLS clients.
  wmts::GridOptions grid;
};
struct TileProfile { TileSource source; TileGrid grid; };
const char *tiandituLayerId(TiandituLayer layer);
bool tiandituOverlay(TiandituLayer layer);
// Independent metadata request, fetched/parsed by a worker. URL is sensitive.
TileRequest tiandituCapabilities(const std::string &key, const TiandituOptions &options);
// All geometry/ranges come from the supplied capabilities, not a universal max.
// Conflicting matrices are rejected unless explicitly skipped in options.grid.
Status tiandituProfile(const std::string &key, const wmts::Capabilities &capabilities,
                       const TiandituOptions &options, TileProfile &output);
// Conservative common levels/coverage; preserves each layer's alpha role.
Status combineProfiles(const TileProfile *profiles, size_t count, TileProfile &output);
TileSource mapTiler(const std::string &key, const std::string &mapId = "streets-v4");
TileSource geoapify(const std::string &key, const std::string &style = "osm-bright");
TileSource mapbox(const std::string &token, const std::string &username = "mapbox",
                  const std::string &styleId = "streets-v12");
TileSource xyz(const std::string &urlTemplate, const std::string &attribution,
               uint8_t minZoom = 0, uint8_t maxZoom = 18, uint16_t tileSize = 256);
std::string urlEncode(const std::string &value);
} } // namespace nice_gfx_map::providers
