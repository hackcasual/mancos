// Read-only zip access for zipped Factorio mods (upstream: SharpCompress).
// Backed by miniz; entry names use forward slashes, matched exactly.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yafc {

class ZipArchive {
 public:
  // Returns nullptr if the file cannot be opened as a zip.
  static std::shared_ptr<ZipArchive> Open(const std::string& path);
  ~ZipArchive();
  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;

  bool Exists(const std::string& entryName) const;
  std::optional<std::string> Read(const std::string& entryName) const;
  const std::vector<std::string>& EntryNames() const { return entryNames_; }

 private:
  ZipArchive() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<std::string> entryNames_;
};

}  // namespace yafc
