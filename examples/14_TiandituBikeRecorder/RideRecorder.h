#pragma once
#include <FS.h>
#include <demo/RideSupport.h>
#include <cstdio>
#include <cstring>

namespace bike {
// Main-loop owner only. Never overwrites an existing ride. Files remain plain
// CSV after reset/power loss; the final unflushed rows can still be lost.
class RideRecorder {
 public:
  bool begin(fs::FS &filesystem, uint32_t now) {
    end(); error_ = ""; rows_ = 0;
    if (!filesystem.exists("/rides") && !filesystem.mkdir("/rides"))
      return fail("mkdir /rides failed");
    for (unsigned index = 1; index <= 9999; ++index) {
      std::snprintf(folder_, sizeof(folder_), "/rides/ride%04u", index);
      if (filesystem.exists(folder_)) continue;
      if (!filesystem.mkdir(folder_)) return fail("ride mkdir failed");
      char path[64];
      std::snprintf(path, sizeof(path), "%s/track.csv", folder_);
      file_ = filesystem.open(path, FILE_WRITE);
      if (!file_) return fail("track open failed");
      const char *header = nice_gfx_map::demo::RideCsvHeader;
      if (file_.write(reinterpret_cast<const uint8_t *>(header), std::strlen(header)) != std::strlen(header))
        return fail("CSV header short write");
      file_.flush(); lastFlush_ = now; rows_ = 0;
      return true;
    }
    return fail("ride numbers exhausted");
  }
  bool append(uint32_t now, const nice_gfx_map::GnssFix &fix,
              const nice_gfx_map::demo::RideSummary &summary) {
    if (!file_) return false;
    char line[320];
    const size_t count = nice_gfx_map::demo::formatRideCsv(line, sizeof(line), now, fix, summary);
    if (!count) return fail("invalid CSV record");
    if (file_.write(reinterpret_cast<const uint8_t *>(line), count) != count)
      return fail("SD short write; recording stopped");
    ++rows_;
    tick(now);
    return true;
  }
  void tick(uint32_t now) {
    if (file_ && now - lastFlush_ >= 5000) { file_.flush(); lastFlush_ = now; }
  }
  void flush() { if (file_) file_.flush(); }
  void end() { if (file_) { file_.flush(); file_.close(); } }
  bool active() const { return bool(file_); }
  const char *error() const { return error_; }
  const char *folder() const { return folder_; }
  uint32_t rows() const { return rows_; }
 private:
  bool fail(const char *message) { end(); error_ = message; return false; }
  fs::File file_;
  const char *error_ = "";
  char folder_[32] = {};
  uint32_t rows_ = 0, lastFlush_ = 0;
};
} // namespace bike
