// Parts of doa4_colour_costumes.cpp that doa4_colour_make.cpp also uses: the manifest parser, the checked open of a
// slot's file, the hash of a source, note text, and the archive-entry source. Defined in doa4_colour_costumes.cpp.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "doa4_colour_costumes.h"

namespace doa4::colour_costumes {

// One entry of an open archive as a byte source (the stand-in): reads [offset, offset + size) of the archive.
class EntrySource final : public afs::Source {
 public:
  EntrySource(std::shared_ptr<const afs::Source> archive, uint64_t offset, uint64_t size)
      : archive_(std::move(archive)), offset_(offset), size_(size) {}
  uint64_t size() const override { return size_; }
  bool Read(uint64_t offset, void* dst, size_t n) const override {
    if (offset > size_ || n > size_ - offset) return false;
    return archive_->Read(offset_ + offset, dst, n);
  }

 private:
  std::shared_ptr<const afs::Source> archive_;
  uint64_t offset_, size_;
};

// The whole of a file of at most `limit` bytes; false when it is missing, unreadable or larger.
bool ReadSmallFile(const std::filesystem::path& path, uint64_t limit, std::string* out);

// The SHA-1 of a source, lower-case hex; empty when a read fails.
std::string HashSource(const afs::Source& source);

// Text for a note: bytes outside printable ASCII as \xNN, so a log line never carries them raw. Every note that holds
// text from outside the program (a manifest line, a word of it, a file name, the folder's path) holds it through this.
std::string Printable(const std::string& text);

// The manifest's file name per slot index (empty: no usable line); notes for the lines it ignores and the names it
// refuses. `refused[i]`: the slot's line named a path.
struct Parsed {
  std::vector<std::string> file;
  std::vector<bool> refused;
};
Parsed ParseManifest(const SlotTable& slots, std::string text, std::vector<std::string>* notes);

// The slot's file, checked against the compiled-in size and SHA-1; null with *why. The returned source holds the file
// open without write or delete sharing until it is released.
std::shared_ptr<const afs::Source> OpenChecked(const std::filesystem::path& folder, const SlotDef& d,
                                               const std::string& file, std::string* why);

}  // namespace doa4::colour_costumes
