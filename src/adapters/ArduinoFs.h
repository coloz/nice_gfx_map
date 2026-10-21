#pragma once
#include "../Pipeline.h"
#include <FS.h>
#include <algorithm>
#include <string>
#include <utility>

namespace nice_gfx_map {
// Read-only offline transport for SD, SD_MMC and LittleFS. The caller mounts the
// filesystem and chooses a namespace containing one projection/style/version.
// Paths: <root>/<sourceId>/<layer>/<z>/<x>/<y>.png . No URL/key is used as a path.
class FsTileTransport : public Transport {
 public:
  FsTileTransport(fs::FS &filesystem, std::string root = "/tiles",
                  std::string sourceId = "demo-v1", size_t maxResponseBytes = 512 * 1024,
                  Allocator allocator = {})
      : filesystem_(filesystem), root_(std::move(root)), sourceId_(std::move(sourceId)),
        limit_(maxResponseBytes), allocator_(allocator) {}
  std::string pathFor(const TileRequest &request) const {
    if (!validSettings() || request.key.zoom > 22 ||
        (request.tileSize != 128 && request.tileSize != 256 && request.tileSize != 512)) return {};
    const uint32_t dimension = uint32_t(1) << request.key.zoom;
    if (request.key.x >= dimension || request.key.y >= dimension) return {};
    std::string path = root_;
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path + "/" + sourceId_ + "/" + std::to_string(request.key.layer) + "/" +
      std::to_string(request.key.zoom) + "/" + std::to_string(request.key.x) + "/" +
      std::to_string(request.key.y) + ".png";
  }
  Status fetch(const TileRequest &request, ByteBuffer &output) override {
    const std::string path = pathFor(request);
    if (path.empty()) return Status::InvalidArgument;
    auto file = filesystem_.open(path.c_str(), "r");
    if (!file) return Status::NotFound;
    struct Close { fs::File &file; ~Close() { file.close(); } } close{file};
    if (file.isDirectory()) return Status::InvalidArgument;
    const size_t length = file.size();
    if (!length) return Status::DecodeError;
    if (length > limit_) return Status::TooLarge;
    ByteBuffer input;
    Status status = input.reserve(length, allocator_);
    if (status != Status::Ok) return status;
    uint8_t block[1024];
    while (input.size() < length) {
      const size_t wanted = std::min(sizeof(block), length - input.size());
      const size_t count = file.read(block, wanted);
      if (!count || count > wanted) return Status::DecodeError;
      status = input.append(block, count);
      if (status != Status::Ok) return status;
    }
    uint8_t extra;
    if (file.read(&extra, 1)) return Status::DecodeError; // Changed while reading.
    output = std::move(input);
    return Status::Ok;
  }
 private:
  bool validSettings() const {
    if (root_.empty() || root_[0] != '/' || root_.size() > 256 || sourceId_.empty() ||
        sourceId_.size() > 64 || !limit_ || limit_ > 8 * 1024 * 1024 ||
        bool(allocator_.allocate) != bool(allocator_.release)) return false;
    for (unsigned char c : sourceId_)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    for (size_t offset = 0; offset < root_.size();) {
      if (root_[offset] == '/') { ++offset; continue; }
      const size_t next = root_.find('/', offset);
      const size_t end = next == std::string::npos ? root_.size() : next;
      const std::string component = root_.substr(offset, end - offset);
      if (component == "." || component == "..") return false;
      for (unsigned char c : component)
        if (c <= 0x20 || c >= 0x7f || c == '\\' || c == ':' || c == '?' || c == '#') return false;
      offset = end;
    }
    return true;
  }
  fs::FS &filesystem_;
  std::string root_, sourceId_;
  size_t limit_;
  Allocator allocator_;
};
} // namespace nice_gfx_map
