#include "yafc/parser/zip_archive.h"

#include <unordered_map>

#include <miniz.h>

namespace yafc {

struct ZipArchive::Impl {
  mz_zip_archive zip{};
  std::unordered_map<std::string, mz_uint> index;  // entry name -> file index
};

std::shared_ptr<ZipArchive> ZipArchive::Open(const std::string& path) {
  auto impl = std::make_unique<Impl>();
  if (!mz_zip_reader_init_file(&impl->zip, path.c_str(), 0)) {
    return nullptr;
  }

  std::shared_ptr<ZipArchive> archive(new ZipArchive());
  mz_uint count = mz_zip_reader_get_num_files(&impl->zip);
  archive->entryNames_.reserve(count);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&impl->zip, i, &stat) || stat.m_is_directory) {
      continue;
    }
    archive->entryNames_.emplace_back(stat.m_filename);
    impl->index[stat.m_filename] = i;
  }
  archive->impl_ = std::move(impl);
  return archive;
}

ZipArchive::~ZipArchive() {
  if (impl_ != nullptr) mz_zip_reader_end(&impl_->zip);
}

bool ZipArchive::Exists(const std::string& entryName) const {
  return impl_->index.count(entryName) != 0;
}

std::optional<std::string> ZipArchive::Read(const std::string& entryName) const {
  auto it = impl_->index.find(entryName);
  if (it == impl_->index.end()) return std::nullopt;

  size_t size = 0;
  void* data = mz_zip_reader_extract_to_heap(
      const_cast<mz_zip_archive*>(&impl_->zip), it->second, &size, 0);
  if (data == nullptr) return std::nullopt;
  std::string result(static_cast<char*>(data), size);
  mz_free(data);
  return result;
}

}  // namespace yafc
