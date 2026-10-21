#include "TiandituWmts.h"
#include "Providers.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#define NICE_WMTS_TRY try
#define NICE_WMTS_CATCH catch (const std::bad_alloc &) { return Status::NoMemory; }
#else
#define NICE_WMTS_TRY
#define NICE_WMTS_CATCH
#endif

namespace nice_gfx_map { namespace wmts {
namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
std::string trim(const std::string &value) {
  size_t a = 0, b = value.size();
  while (a < b && space(value[a])) ++a;
  while (b > a && space(value[b - 1])) --b;
  return value.substr(a, b - a);
}
std::string local(const std::string &name) {
  const auto colon = name.find(':');
  return colon == std::string::npos ? name : name.substr(colon + 1);
}
bool nameCharacter(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
}
bool number(const std::string &text, double &value) {
  if (text.empty()) return false;
  char *end = nullptr; value = std::strtod(text.c_str(), &end);
  return end == text.c_str() + text.size() && std::isfinite(value);
}
bool integer(const std::string &text, uint32_t &value) {
  if (text.empty()) return false;
  uint64_t result = 0;
  for (const char c : text) {
    if (c < '0' || c > '9' || result > (UINT32_MAX - unsigned(c - '0')) / 10) return false;
    result = result * 10 + unsigned(c - '0');
  }
  value = uint32_t(result); return true;
}
bool pair(const std::string &text, double &a, double &b) {
  const auto split = text.find_first_of(" \t\r\n");
  return split != std::string::npos && number(text.substr(0, split), a) && number(trim(text.substr(split)), b);
}
bool appendCodepoint(std::string &text, uint32_t value) {
  if (!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
      (value < 32 && value != 9 && value != 10 && value != 13)) return false;
  if (value < 128) text.push_back(char(value));
  else if (value < 2048) { text.push_back(char(0xc0 | value >> 6)); text.push_back(char(0x80 | (value & 63))); }
  else if (value < 65536) { text.push_back(char(0xe0 | value >> 12)); text.push_back(char(0x80 | ((value >> 6) & 63))); text.push_back(char(0x80 | (value & 63))); }
  else { text.push_back(char(0xf0 | value >> 18)); text.push_back(char(0x80 | ((value >> 12) & 63))); text.push_back(char(0x80 | ((value >> 6) & 63))); text.push_back(char(0x80 | (value & 63))); }
  return true;
}
bool decoded(const std::string &input, std::string &output) {
  output.clear();
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] != '&') { output.push_back(input[i]); continue; }
    const auto end = input.find(';', i + 1);
    if (end == std::string::npos || end - i > 12) return false;
    const auto entity = input.substr(i + 1, end - i - 1);
    if (entity == "amp") output.push_back('&');
    else if (entity == "lt") output.push_back('<');
    else if (entity == "gt") output.push_back('>');
    else if (entity == "apos") output.push_back('\'');
    else if (entity == "quot") output.push_back('"');
    else if (!entity.empty() && entity[0] == '#') {
      const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
      const size_t start = hex ? 2 : 1;
      if (start == entity.size()) return false;
      uint32_t value = 0;
      for (size_t j = start; j < entity.size(); ++j) {
        const char c = entity[j]; int digit = c >= '0' && c <= '9' ? c - '0' :
          hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        const unsigned base = hex ? 16 : 10;
        if (digit < 0 || value > (0x10ffffu - unsigned(digit)) / base) return false;
        value = value * base + unsigned(digit);
      }
      if (!appendCodepoint(output, value)) return false;
    } else return false;
    i = end;
  }
  return true;
}
template<class Values> bool uniqueIds(const Values &values) {
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i].identifier.empty()) return false;
    for (size_t j = 0; j < i; ++j) if (values[i].identifier == values[j].identifier) return false;
  }
  return true;
}
struct Xml {
  const ParseLimits &limits;
  explicit Xml(const ParseLimits &value) : limits(value) {}
  Capabilities result;
  std::vector<std::string> stack;
  std::string text;
  uint8_t matrixFields = 0, limitFields = 0;
  bool closed = false;
  bool path(std::initializer_list<const char *> names) const {
    if (stack.size() != names.size()) return false;
    size_t i = 0; for (const char *name : names) if (local(stack[i++]) != name) return false;
    return true;
  }
  Status open(const std::string &name) {
    if (closed || stack.size() >= limits.depth) return closed ? Status::DecodeError : Status::TooLarge;
    if (stack.empty() && local(name) != "Capabilities") return Status::DecodeError;
    stack.push_back(name); text.clear();
    if (path({"Capabilities", "Contents", "Layer"})) {
      if (result.layers.size() == limits.layers) return Status::TooLarge;
      result.layers.emplace_back();
    } else if (path({"Capabilities", "Contents", "TileMatrixSet"})) {
      if (result.matrixSets.size() == limits.matrixSets) return Status::TooLarge;
      result.matrixSets.emplace_back();
    } else if (path({"Capabilities", "Contents", "TileMatrixSet", "TileMatrix"})) {
      auto &set = result.matrixSets.back();
      if (set.matrices.size() == limits.matrices) return Status::TooLarge;
      set.matrices.emplace_back(); matrixFields = 0;
    } else if (path({"Capabilities", "Contents", "Layer", "TileMatrixSetLink"})) {
      auto &layer = result.layers.back();
      if (layer.links.size() == limits.matrixSets) return Status::TooLarge;
      layer.links.emplace_back();
    } else if (path({"Capabilities", "Contents", "Layer", "TileMatrixSetLink", "TileMatrixSetLimits", "TileMatrixLimits"})) {
      auto &link = result.layers.back().links.back();
      if (link.limits.size() == limits.matrices) return Status::TooLarge;
      link.limits.emplace_back(); limitFields = 0;
    }
    return Status::Ok;
  }
  Status close(const std::string &name) {
    if (stack.empty() || stack.back() != name) return Status::DecodeError;
    const auto value = trim(text); std::string content;
    if (!decoded(value, content)) return Status::DecodeError;
    const std::string tag = local(name);
    if (stack.size() == 4 && path({"Capabilities", "Contents", "Layer", tag.c_str()})) {
      auto &layer = result.layers.back();
      if (tag == "Identifier") { if (!layer.identifier.empty()) return Status::DecodeError; layer.identifier = content; }
      else if (tag == "Title") layer.title = content;
      else if (tag == "Format") { if (layer.formats.size() >= 16) return Status::TooLarge; layer.formats.push_back(content); }
    } else if (path({"Capabilities", "Contents", "Layer", "Style", "Identifier"})) {
      auto &styles = result.layers.back().styles;
      if (styles.size() >= 16) return Status::TooLarge; styles.push_back(content);
    } else if (path({"Capabilities", "Contents", "Layer", "TileMatrixSetLink", "TileMatrixSet"})) {
      auto &id = result.layers.back().links.back().matrixSet;
      if (!id.empty()) return Status::DecodeError; id = content;
    } else if (stack.size() == 4 && path({"Capabilities", "Contents", "TileMatrixSet", tag.c_str()})) {
      auto &set = result.matrixSets.back();
      if (tag == "Identifier") { if (!set.identifier.empty()) return Status::DecodeError; set.identifier = content; }
      else if (tag == "SupportedCRS") { if (!set.supportedCrs.empty()) return Status::DecodeError; set.supportedCrs = content; }
    } else if (stack.size() == 5 && path({"Capabilities", "Contents", "TileMatrixSet", "TileMatrix", tag.c_str()})) {
      auto &m = result.matrixSets.back().matrices.back(); uint8_t flag = 0; bool okay = true; uint32_t numeric = 0;
      if (tag == "Identifier") { flag = 1; m.identifier = content; okay = !content.empty(); }
      else if (tag == "ScaleDenominator") { flag = 2; okay = number(content, m.scaleDenominator) && m.scaleDenominator > 0; }
      else if (tag == "TopLeftCorner") { flag = 4; okay = pair(content, m.topLeftFirst, m.topLeftSecond); }
      else if (tag == "TileWidth" || tag == "TileHeight") {
        flag = tag == "TileWidth" ? 8 : 16; okay = integer(content, numeric) && numeric && numeric <= 512;
        if (flag == 8) m.tileWidth = uint16_t(numeric); else m.tileHeight = uint16_t(numeric);
      } else if (tag == "MatrixWidth" || tag == "MatrixHeight") {
        flag = tag == "MatrixWidth" ? 32 : 64; okay = integer(content, numeric) && numeric;
        if (flag == 32) m.width = numeric; else m.height = numeric;
      }
      if (!okay || (matrixFields & flag)) return Status::DecodeError; matrixFields |= flag;
    } else if (stack.size() == 7 && path({"Capabilities", "Contents", "Layer", "TileMatrixSetLink", "TileMatrixSetLimits", "TileMatrixLimits", tag.c_str()})) {
      auto &m = result.layers.back().links.back().limits.back(); uint8_t flag = 0; uint32_t numeric = 0;
      if (tag == "TileMatrix") { flag = 1; m.identifier = content; if (content.empty()) return Status::DecodeError; }
      else if (tag == "MinTileRow" || tag == "MaxTileRow" || tag == "MinTileCol" || tag == "MaxTileCol") {
        if (!integer(content, numeric)) return Status::DecodeError;
        if (tag == "MinTileRow") { flag = 2; m.minRow = numeric; }
        else if (tag == "MaxTileRow") { flag = 4; m.maxRow = numeric; }
        else if (tag == "MinTileCol") { flag = 8; m.minCol = numeric; }
        else { flag = 16; m.maxCol = numeric; }
      }
      if (limitFields & flag) return Status::DecodeError; limitFields |= flag;
    }
    if (path({"Capabilities", "Contents", "TileMatrixSet", "TileMatrix"}) && matrixFields != 127) return Status::DecodeError;
    if (path({"Capabilities", "Contents", "Layer", "TileMatrixSetLink", "TileMatrixSetLimits", "TileMatrixLimits"}) && limitFields != 31) return Status::DecodeError;
    stack.pop_back(); text.clear(); if (stack.empty()) closed = true;
    return Status::Ok;
  }
};
bool near(double a, double b) { return std::abs(a - b) <= std::max(1e-7, std::abs(b) * 1e-8); }
} // namespace

Status parseCapabilities(const uint8_t *bytes, size_t count, Capabilities &output, const ParseLimits &limits) {
  if (!bytes || !count || !limits.bytes || !limits.layers || !limits.matrixSets || !limits.matrices ||
      !limits.textBytes || limits.depth < 4 || limits.depth > 64 || limits.layers > 256 ||
      limits.matrixSets > 256 || limits.matrices > 64 || limits.textBytes > 65536) return Status::InvalidArgument;
  if (count > limits.bytes) return Status::TooLarge;
  for (size_t i = 0; i < count; ++i) if (bytes[i] < 32 && !space(char(bytes[i]))) return Status::DecodeError;
  NICE_WMTS_TRY {
    Xml xml{limits}; size_t i = count >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    const auto starts = [&](size_t at, const char *text) { const size_t n = std::strlen(text); return n <= count - at && std::memcmp(bytes + at, text, n) == 0; };
    const auto find = [&](size_t at, const char *text) { for (size_t p = at; p < count; ++p) if (starts(p, text)) return p; return count; };
    while (i < count) {
      if (bytes[i] != '<') {
        const size_t first = i; while (i < count && bytes[i] != '<') ++i;
        if (xml.stack.empty()) { for (size_t p = first; p < i; ++p) if (!space(char(bytes[p]))) return Status::DecodeError; }
        else {
          size_t a = first, b = i; while (a < b && space(char(bytes[a]))) ++a; while (b > a && space(char(bytes[b - 1]))) --b;
          if (b - a > limits.textBytes - std::min(limits.textBytes, xml.text.size())) return Status::TooLarge;
          xml.text.append(reinterpret_cast<const char *>(bytes + a), b - a);
        }
        continue;
      }
      if (starts(i, "<!--")) { const size_t end = find(i + 4, "-->"); if (end == count) return Status::DecodeError; i = end + 3; continue; }
      if (starts(i, "<?")) { const size_t end = find(i + 2, "?>"); if (end == count) return Status::DecodeError; i = end + 2; continue; }
      if (starts(i, "<!")) return Status::Unsupported;
      ++i; const bool closing = i < count && bytes[i] == '/'; if (closing) ++i;
      const size_t first = i; while (i < count && nameCharacter(bytes[i])) ++i;
      if (i == first || i - first > 128) return Status::DecodeError;
      const std::string name(reinterpret_cast<const char *>(bytes + first), i - first);
      bool selfClosing = false; char quote = 0;
      while (i < count) {
        const char c = char(bytes[i]);
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        else if (c == '>') break;
        else if (c == '<' || (closing && !space(c))) return Status::DecodeError;
        ++i;
      }
      if (i == count || quote) return Status::DecodeError;
      if (i && bytes[i - 1] == '/') selfClosing = true;
      ++i;
      Status status = closing ? xml.close(name) : xml.open(name);
      if (status != Status::Ok) return status;
      if (selfClosing) { status = xml.close(name); if (status != Status::Ok) return status; }
    }
    if (!xml.closed || !xml.stack.empty() || xml.result.layers.empty() || xml.result.matrixSets.empty() ||
        !uniqueIds(xml.result.layers) || !uniqueIds(xml.result.matrixSets)) return Status::DecodeError;
    for (const auto &set : xml.result.matrixSets) if (set.supportedCrs.empty() || set.matrices.empty() || !uniqueIds(set.matrices)) return Status::DecodeError;
    for (const auto &layer : xml.result.layers) {
      if (layer.links.empty() || layer.styles.empty() || layer.formats.empty()) return Status::DecodeError;
      for (const auto &link : layer.links) if (link.matrixSet.empty() || !uniqueIds(link.limits)) return Status::DecodeError;
    }
    output = std::move(xml.result); return Status::Ok;
  } NICE_WMTS_CATCH
}

Status tileGrid(const Capabilities &capabilities, const std::string &layerId, const std::string &setId,
                TileGrid &output, const GridOptions &options) {
  if (options.minZoom > options.maxZoom || options.maxZoom > 22) return Status::InvalidArgument;
  const Layer *layer = nullptr; const MatrixSet *set = nullptr; const Link *link = nullptr;
  for (const auto &value : capabilities.layers) if (value.identifier == layerId) layer = &value;
  for (const auto &value : capabilities.matrixSets) if (value.identifier == setId) set = &value;
  if (!layer || !set) return Status::NotFound;
  for (const auto &value : layer->links) if (value.matrixSet == setId) link = &value;
  if (!link) return Status::NotFound;
  const bool geographic = set->supportedCrs == "urn:ogc:def:crs:EPSG::4490" || set->supportedCrs == "urn:ogc:def:crs:EPSG::4326" || set->supportedCrs == "EPSG:4490" || set->supportedCrs == "EPSG:4326";
  const bool mercator = set->supportedCrs == "urn:ogc:def:crs:EPSG::900913" || set->supportedCrs == "urn:ogc:def:crs:EPSG::3857" || set->supportedCrs == "EPSG:3857" || set->supportedCrs == "EPSG:900913";
  if (!geographic && !mercator) return Status::Unsupported;
  NICE_WMTS_TRY {
    TileGrid grid; grid.projection = geographic ? Projection::Geographic : Projection::WebMercator;
    grid.identifier = setId; grid.supportedCrs = set->supportedCrs;
    constexpr double halfWorld = 20037508.342789244, circumference = halfWorld * 2;
    for (const auto &raw : set->matrices) {
      uint8_t zoom = 0; uint32_t width = 1;
      while (zoom < 22 && width < raw.width) { ++zoom; width <<= 1; }
      if (zoom < options.minZoom || zoom > options.maxZoom) continue;
      bool compatible = raw.width == width && raw.height == (geographic ? width / 2 : width) &&
        raw.height && raw.tileWidth == raw.tileHeight && (raw.tileWidth == 128 || raw.tileWidth == 256 || raw.tileWidth == 512);
      const double west = geographic ? -180 : -halfWorld, north = geographic ? 90 : halfWorld;
      const bool xy = near(raw.topLeftFirst, west) && near(raw.topLeftSecond, north);
      const bool yx = near(raw.topLeftFirst, north) && near(raw.topLeftSecond, west);
      compatible = compatible && (xy || yx);
      const bool ctaOverride = !compatible && options.useTiandituCtaWMercatorOverride &&
        layerId == "cta" && setId == "w" && mercator && zoom >= 1 && zoom <= 18 &&
        raw.identifier == std::to_string(zoom) && raw.width == width && raw.height == width / 2 &&
        raw.tileWidth == 256 && raw.tileHeight == 256 &&
        near(raw.topLeftFirst, 90) && near(raw.topLeftSecond, -180);
      if (ctaOverride) {
        compatible = true;
        grid.metadataWarnings |= IncompatibleMatrix | TiandituCtaWMercatorOverride | TiandituNorthEastAxes;
      }
      if (!compatible) { if (!options.skipIncompatible) return Status::Unsupported; grid.metadataWarnings |= IncompatibleMatrix; continue; }
      if (yx) grid.metadataWarnings |= TiandituNorthEastAxes;
      const double metersPerPixel = circumference / (double(raw.width) * raw.tileWidth);
      if (!near(raw.scaleDenominator * 0.00028, metersPerPixel)) grid.metadataWarnings |= DeclaredScaleDiffers;
      TileMatrix matrix; matrix.identifier = raw.identifier; matrix.zoom = zoom;
      matrix.width = raw.width; matrix.height = ctaOverride ? width : raw.height;
      matrix.tileWidth = raw.tileWidth; matrix.tileHeight = raw.tileHeight;
      matrix.scaleDenominator = raw.scaleDenominator; matrix.topLeftFirst = raw.topLeftFirst; matrix.topLeftSecond = raw.topLeftSecond;
      if (!link->limits.empty()) {
        const MatrixLimits *limit = nullptr;
        for (const auto &value : link->limits) if (value.identifier == raw.identifier) limit = &value;
        if (!limit) continue;
        matrix.limited = true; matrix.minRow = limit->minRow; matrix.maxRow = limit->maxRow;
        matrix.minCol = limit->minCol; matrix.maxCol = limit->maxCol;
      }
      grid.matrices.push_back(std::move(matrix));
    }
    if (grid.matrices.empty()) return Status::Unsupported;
    std::sort(grid.matrices.begin(), grid.matrices.end(), [](const TileMatrix &a, const TileMatrix &b) { return a.zoom < b.zoom; });
    if (!grid.valid()) return Status::Unsupported;
    output = std::move(grid); return Status::Ok;
  } NICE_WMTS_CATCH
}
} } // namespace nice_gfx_map::wmts

namespace nice_gfx_map { namespace providers {
namespace {
bool validOptions(const std::string &key, const TiandituOptions &options) {
  bool credential = false;
  for (const unsigned char c : key) if (c > 32 && c != 127) credential = true;
  return credential && options.server < 8 && tiandituLayerId(options.layer) &&
    (options.projection == Projection::WebMercator || options.projection == Projection::Geographic) &&
    options.grid.minZoom <= options.grid.maxZoom && options.grid.maxZoom <= 22;
}
std::string endpoint(const TiandituOptions &options) {
  return std::string("https://t") + char('0' + options.server) + ".tianditu.gov.cn/" +
    tiandituLayerId(options.layer) + (options.projection == Projection::Geographic ? "_c/wmts" : "_w/wmts");
}
}
const char *tiandituLayerId(TiandituLayer layer) {
  static const char *const names[] = {"vec", "cva", "img", "cia", "ter", "cta", "ibo"};
  return size_t(layer) < sizeof(names) / sizeof(names[0]) ? names[size_t(layer)] : nullptr;
}
bool tiandituOverlay(TiandituLayer layer) {
  return layer == TiandituLayer::VectorLabels || layer == TiandituLayer::ImageryLabels ||
    layer == TiandituLayer::TerrainLabels || layer == TiandituLayer::Boundaries;
}
TileRequest tiandituCapabilities(const std::string &key, const TiandituOptions &options) {
  TileRequest request;
  if (!validOptions(key, options)) return request;
  request.url = endpoint(options) + "?SERVICE=WMTS&REQUEST=GetCapabilities&VERSION=1.0.0&tk=" + urlEncode(key);
  return request;
}
Status tiandituProfile(const std::string &key, const wmts::Capabilities &capabilities,
                       const TiandituOptions &options, TileProfile &output) {
  if (!validOptions(key, options)) return Status::InvalidArgument;
  NICE_WMTS_TRY {
    const std::string layer = tiandituLayerId(options.layer);
    const std::string set = options.projection == Projection::Geographic ? "c" : "w";
    const wmts::Layer *metadata = nullptr;
    for (const auto &value : capabilities.layers) if (value.identifier == layer) metadata = &value;
    if (!metadata) return Status::NotFound;
    if (std::find(metadata->styles.begin(), metadata->styles.end(), "default") == metadata->styles.end() ||
        std::find(metadata->formats.begin(), metadata->formats.end(), "tiles") == metadata->formats.end()) return Status::Unsupported;
    TileProfile profile;
    Status status = wmts::tileGrid(capabilities, layer, set, profile.grid, options.grid);
    if (status != Status::Ok) return status;
    if (profile.grid.projection != options.projection) return Status::Unsupported;
    profile.source.name = "Tianditu " + layer + "_" + set;
    profile.source.attribution = "Map data: Tianditu";
    profile.source.tileSize = profile.grid.matrices.front().tileWidth;
    profile.source.minZoom = profile.grid.matrices.front().zoom;
    profile.source.maxZoom = profile.grid.matrices.back().zoom;
    profile.source.layers.push_back({endpoint(options) + "?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=" + layer +
      "&STYLE=default&TILEMATRIXSET=" + set + "&FORMAT=tiles&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}&tk=" + urlEncode(key), tiandituOverlay(options.layer)});
    if (!profile.source.valid()) return Status::InvalidArgument;
    output = std::move(profile); return Status::Ok;
  } NICE_WMTS_CATCH
}
Status combineProfiles(const TileProfile *profiles, size_t count, TileProfile &output) {
  if (!profiles || !count || count > 256) return Status::InvalidArgument;
  NICE_WMTS_TRY {
    TileProfile combined = profiles[0];
    if (!combined.source.valid() || !combined.grid.valid() || combined.grid.matrices.empty()) return Status::InvalidArgument;
    for (size_t i = 1; i < count; ++i) {
      const auto &next = profiles[i];
      if (!next.source.valid() || !next.grid.valid() || next.grid.matrices.empty() ||
          next.grid.projection != combined.grid.projection || next.grid.identifier != combined.grid.identifier ||
          next.grid.supportedCrs != combined.grid.supportedCrs || next.source.tileSize != combined.source.tileSize ||
          next.source.layers.size() > 256 - combined.source.layers.size()) return Status::InvalidArgument;
      combined.source.minZoom = std::max(combined.source.minZoom, next.source.minZoom);
      combined.source.maxZoom = std::min(combined.source.maxZoom, next.source.maxZoom);
      if (combined.source.minZoom > combined.source.maxZoom) return Status::Unsupported;
      combined.grid.matrices.erase(std::remove_if(combined.grid.matrices.begin(), combined.grid.matrices.end(),
        [&](const TileMatrix &m) { return m.zoom < combined.source.minZoom || m.zoom > combined.source.maxZoom; }), combined.grid.matrices.end());
      for (auto &matrix : combined.grid.matrices) {
        const auto *other = next.grid.matrix(matrix.zoom);
        if (!other || matrix.identifier != other->identifier || matrix.width != other->width || matrix.height != other->height) return Status::Unsupported;
        if (matrix.limited || other->limited) {
          const uint32_t minRow = std::max(matrix.limited ? matrix.minRow : 0, other->limited ? other->minRow : 0);
          const uint32_t maxRow = std::min(matrix.limited ? matrix.maxRow : matrix.height - 1, other->limited ? other->maxRow : matrix.height - 1);
          const uint32_t minCol = std::max(matrix.limited ? matrix.minCol : 0, other->limited ? other->minCol : 0);
          const uint32_t maxCol = std::min(matrix.limited ? matrix.maxCol : matrix.width - 1, other->limited ? other->maxCol : matrix.width - 1);
          if (minRow > maxRow || minCol > maxCol) return Status::Unsupported;
          matrix.limited = true; matrix.minRow = minRow; matrix.maxRow = maxRow; matrix.minCol = minCol; matrix.maxCol = maxCol;
        }
      }
      combined.grid.metadataWarnings |= next.grid.metadataWarnings;
      combined.source.layers.insert(combined.source.layers.end(), next.source.layers.begin(), next.source.layers.end());
    }
    if (!combined.source.valid() || !combined.grid.valid()) return Status::Unsupported;
    output = std::move(combined); return Status::Ok;
  } NICE_WMTS_CATCH
}
} } // namespace nice_gfx_map::providers
