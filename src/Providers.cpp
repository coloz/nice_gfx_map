#include "Providers.h"
#include <cstring>
#include <utility>

namespace nice_gfx_map {
namespace {
bool validTemplate(const std::string &value) {
  constexpr const char *prefix = "https://";
  if (value.compare(0, std::strlen(prefix), prefix) != 0) return false;
  const size_t authorityStart = std::strlen(prefix);
  const size_t authorityEnd = value.find_first_of("/?#", authorityStart);
  const size_t authorityLength = (authorityEnd == std::string::npos ? value.size() : authorityEnd)
                                 - authorityStart;
  if (!authorityLength) return false;
  const std::string authority = value.substr(authorityStart, authorityLength);
  if (authority.find_first_of("@{}\\") != std::string::npos) return false;
  bool z = false, x = false, y = false;
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c <= 0x20 || c == 0x7f || c == '\\' || c == '#') return false;
    if (c == '}') return false;
    if (c != '{') continue;
    if (i + 2 >= value.size() || value[i + 2] != '}') return false;
    switch (value[i + 1]) {
      case 'z': z = true; break;
      case 'x': x = true; break;
      case 'y': y = true; break;
      default: return false;
    }
    i += 2;
  }
  return z && x && y;
}

bool usableCredential(const std::string &value) {
  // Reject empty/whitespace-only input while permitting reserved bytes, which
  // urlEncode safely preserves as part of the credential rather than syntax.
  for (unsigned char c : value)
    if (c > 0x20 && c != 0x7f) return true;
  return false;
}

TileSource singleLayer(const char *name, const char *attribution,
                       std::string url, uint8_t maxZoom) {
  TileSource source;
  source.name = name;
  source.attribution = attribution;
  source.tileSize = 256;
  source.minZoom = 0;
  source.maxZoom = maxZoom;
  source.layers.push_back({std::move(url), false});
  return source;
}
} // namespace

bool TileSource::valid() const {
  if ((tileSize != 128 && tileSize != 256 && tileSize != 512) ||
      minZoom > maxZoom || maxZoom > 22 || layers.empty() || layers.size() > 256)
    return false;
  for (const auto &layer : layers)
    if (!validTemplate(layer.urlTemplate)) return false;
  return true;
}

std::string TileSource::url(const TileKey &key) const {
  if (!valid() || key.zoom < minZoom || key.zoom > maxZoom ||
      size_t(key.layer) >= layers.size()) return {};
  const uint32_t dimension = uint32_t(1) << key.zoom;
  if (key.x >= dimension || key.y >= dimension) return {};
  const std::string &pattern = layers[key.layer].urlTemplate;
  const std::string z = std::to_string(key.zoom);
  const std::string x = std::to_string(key.x);
  const std::string y = std::to_string(key.y);
  std::string result;
  result.reserve(pattern.size() + z.size() + x.size() + y.size());
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '{') { result.push_back(pattern[i]); continue; }
    switch (pattern[i + 1]) {
      case 'z': result += z; break;
      case 'x': result += x; break;
      case 'y': result += y; break;
    }
    i += 2;
  }
  return result;
}

std::string TileSource::url(const TileKey &key, const TileGrid &grid) const {
  if (grid.matrices.empty()) return grid.projection == Projection::WebMercator ? url(key) : std::string{};
  if (!valid() || !grid.valid() || key.zoom < minZoom || key.zoom > maxZoom ||
      size_t(key.layer) >= layers.size() || !grid.contains(key)) return {};
  const auto *matrix = grid.matrix(key.zoom);
  if (!matrix || matrix->tileWidth != tileSize || matrix->tileHeight != tileSize) return {};
  const std::string z = providers::urlEncode(matrix->identifier), x = std::to_string(key.x), y = std::to_string(key.y);
  const auto &pattern = layers[key.layer].urlTemplate;
  std::string result; result.reserve(pattern.size() + z.size() + x.size() + y.size());
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '{') { result.push_back(pattern[i]); continue; }
    switch (pattern[i + 1]) { case 'z': result += z; break; case 'x': result += x; break; case 'y': result += y; break; }
    i += 2;
  }
  return result;
}

namespace providers {

std::string urlEncode(const std::string &value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : value) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      result.push_back(char(c));
    } else {
      result.push_back('%');
      result.push_back(hex[c >> 4]);
      result.push_back(hex[c & 15]);
    }
  }
  return result;
}

TileSource tianditu(const std::string &key, bool labels) {
  if (!usableCredential(key)) return {};
  const std::string suffix =
      "&STYLE=default&TILEMATRIXSET=w&FORMAT=tiles&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}&tk="
      + urlEncode(key);
  auto source = singleLayer("Tianditu", "Map data: Tianditu",
      "https://t0.tianditu.gov.cn/vec_w/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=vec"
      + suffix, 18);
  source.minZoom = 1;
  if (labels) source.layers.push_back({
      "https://t0.tianditu.gov.cn/cva_w/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=cva"
      + suffix, true});
  return source;
}

TileSource mapTiler(const std::string &key, const std::string &mapId) {
  if (!usableCredential(key) || mapId.empty()) return {};
  return singleLayer("MapTiler", "Maps: MapTiler | Data: OpenStreetMap contributors",
      "https://api.maptiler.com/maps/" + urlEncode(mapId)
      + "/256/{z}/{x}/{y}.png?key=" + urlEncode(key), 22);
}

TileSource geoapify(const std::string &key, const std::string &style) {
  if (!usableCredential(key) || style.empty()) return {};
  return singleLayer("Geoapify", "Powered by Geoapify | OpenMapTiles | OpenStreetMap contributors",
      "https://maps.geoapify.com/v1/tile/" + urlEncode(style)
      + "/{z}/{x}/{y}.png?apiKey=" + urlEncode(key), 20);
}

TileSource mapbox(const std::string &token, const std::string &username,
                  const std::string &styleId) {
  if (!usableCredential(token) || username.empty() || styleId.empty()) return {};
  return singleLayer("Mapbox", "Mapbox | OpenStreetMap contributors",
      "https://api.mapbox.com/styles/v1/" + urlEncode(username) + "/" + urlEncode(styleId)
      + "/tiles/256/{z}/{x}/{y}.png?access_token=" + urlEncode(token), 22);
}

TileSource xyz(const std::string &urlTemplate, const std::string &attribution,
                uint8_t minZoom, uint8_t maxZoom, uint16_t tileSize) {
  TileSource source;
  source.name = "XYZ";
  source.attribution = attribution;
  source.tileSize = tileSize;
  source.minZoom = minZoom;
  source.maxZoom = maxZoom;
  source.layers.push_back({urlTemplate, false});
  return source;
}

} // namespace providers
} // namespace nice_gfx_map
