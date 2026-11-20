// The colour-costume slots. See doa4_colour_costumes.h.

#include "doa4_colour_costumes.h"

#include <algorithm>
#include <fstream>

#include "doa4_gamefiles.h"
#include "doa4_suits_manifest.h"
#include "doa4_utf8.h"

namespace doa4::colour_costumes {

namespace {

// Task 2's value table (ou_runs/colour_costumes/colours/values.txt), printed by a script from it. tpr_file = 914 + the
// avt.afs entry Task 1B proved and assigned (373-379).
constexpr SlotTable kSlots = {
    SlotDef{Slot::kAlphaRed, "alpha_red", 16, 1, 1287, 428, 427, 3758080, "caf01e0e2d9874c8f87f664ecfe3469ee864999e"},
    SlotDef{Slot::kAlphaBlue, "alpha_blue", 16, -1, 1288, 428, 427, 3758080, "c9c10d06da970a63460521eff34f0a3119b8e192"},
    SlotDef{Slot::kAlphaLilac, "alpha_lilac", 16, -1, 1289, 428, 427, 3758080, "624851c6b8ce5550fa169db4b0371e87a401f6b4"},
    SlotDef{Slot::kSpartanH3Red, "spartan_h3_red", 18, -1, 1290, 444, 443, 18741248, "068a8ba170264dcc556060ede12c2a81f7c00715"},
    SlotDef{Slot::kSpartanH3Blue, "spartan_h3_blue", 18, -1, 1291, 446, 445, 18741248, "dc13e4fd5410ec68049cbdb89aa4c5fc3abb6c9e"},
    SlotDef{Slot::kSpartanH2Orange, "spartan_h2_orange", 18, -1, 1292, 438, 437, 18741248, "f9c779965c82b84bbc8b1320469327ac53a959e2"},
    SlotDef{Slot::kSpartanH2Cyan, "spartan_h2_cyan", 18, -1, 1293, 448, 447, 18741248, "9dde70ca7fc656da0e4026e6c2b0d9d9cc7c5c50"},
};

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

bool ReadSmallFile(const std::filesystem::path& path, uint64_t limit, std::string* out) {
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(path, ec);
  if (ec || size > limit) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(size_t(size), '\0');
  return bool(in.read(out->data(), std::streamsize(size)));
}

std::string HashSource(const afs::Source& source) {
  gamefiles::Sha1 sha;
  std::vector<uint8_t> buffer(size_t(1) << 20);
  for (uint64_t at = 0; at < source.size();) {
    const size_t n = size_t(std::min<uint64_t>(buffer.size(), source.size() - at));
    if (!source.Read(at, buffer.data(), n)) return {};
    sha.Update(buffer.data(), n);
    at += n;
  }
  return sha.HexDigest();
}

std::vector<std::string> Words(std::string_view line) {
  std::vector<std::string> words;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) words.emplace_back(line.substr(start, i - start));
  }
  return words;
}

// A plain file name inside the folder: the suits' allow-list (letters, digits, '_', '.', '-'; not starting with '.'; at
// most 64 bytes), so no path, drive, stream or byte that does not convert to a path, and no ".." anywhere.
bool PlainFileName(const std::string& file) {
  return suits::SafeFileName(file) && file.find("..") == std::string::npos;
}

// The name for a note: bytes outside printable ASCII as \xNN, so a log line never carries them raw.
std::string Printable(const std::string& text) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (char c : text) {
    const uint8_t b = uint8_t(c);
    if (b >= 0x20 && b < 0x7F) {
      out += c;
    } else {
      out += "\\x";
      out += kHex[b >> 4];
      out += kHex[b & 15];
    }
  }
  return out;
}

// The manifest's file name per slot index (empty: no usable line); notes for the lines it ignores and the names it
// refuses. `refused[i]`: the slot's line named a path.
struct Parsed {
  std::vector<std::string> file;
  std::vector<bool> refused;
};
Parsed ParseManifest(const SlotTable& slots, std::string text, std::vector<std::string>* notes) {
  Parsed p{std::vector<std::string>(kSlotCount), std::vector<bool>(kSlotCount, false)};
  if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF) text.erase(0, 3);
  std::vector<size_t> seen_at(kSlotCount, 0);  // the line number that named the slot, 0 = none yet
  size_t number = 0;
  for (size_t start = 0; start < text.size();) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string_view line(text.data() + start, end - start);
    start = end + 1;
    ++number;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const std::vector<std::string> words = Words(line);
    if (words.empty() || words[0][0] == '#') continue;
    const std::string at = "manifest line " + std::to_string(number);
    if (words.size() != 2) {
      notes->push_back(at + " ignored (not \"<slot> <file>\"): " + std::string(line));
      continue;
    }
    size_t slot = kSlotCount;
    for (size_t i = 0; i < kSlotCount; ++i) {
      if (words[0] == slots[i].name) slot = i;
    }
    if (slot == kSlotCount) {
      notes->push_back(at + " ignored (no colour slot is called " + words[0] + ")");
      continue;
    }
    if (seen_at[slot]) {
      notes->push_back(at + " ignored (" + words[0] + " is already named on line " + std::to_string(seen_at[slot]) + ")");
      continue;
    }
    seen_at[slot] = number;
    if (!PlainFileName(words[1])) {
      p.refused[slot] = true;
      notes->push_back(std::string(slots[slot].name) + ": the file name " + Printable(words[1]) +
                       " is not a plain file name in the folder (letters, digits, '_', '.', '-'); stand-in");
      continue;
    }
    p.file[slot] = words[1];
  }
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (!seen_at[i]) notes->push_back(std::string(slots[i].name) + ": no manifest line; stand-in");
  }
  return p;
}

// The slot's file, checked against the compiled-in size and SHA-1; null with *why.
std::shared_ptr<const afs::Source> OpenChecked(const std::filesystem::path& folder, const SlotDef& d,
                                               const std::string& file, std::string* why) {
  std::shared_ptr<afs::FileSource> source = afs::FileSource::Open(folder / PathFromUtf8(file));
  if (!source) {
    *why = file + " is missing or in use";
    return nullptr;
  }
  if (source->size() != d.size) {
    *why = file + " is " + std::to_string(source->size()) + " bytes, not " + std::to_string(d.size);
    return nullptr;
  }
  if (HashSource(*source) != d.sha1) {
    *why = file + " is not the approved file (SHA-1)";
    return nullptr;
  }
  return source;
}

}  // namespace

const SlotTable& Slots() { return kSlots; }

bool Enabled(bool session, std::string_view np_mode) { return !session && (np_mode.empty() || np_mode == "record"); }

std::string Status::Summary() const {
  if (slots.empty()) return "colour costumes: off";
  if (!usable) return "colour costumes: not installed (see the notes)";
  size_t stand_ins = 0;
  for (const LoadedSlot& l : slots) stand_ins += l.stand_in ? 1 : 0;
  return "colour costumes: " + std::to_string(slots.size()) + " slots, " + std::to_string(slots.size() - stand_ins) +
         " from files, " + std::to_string(stand_ins) + " stand-ins";
}

Status Load(const std::filesystem::path& folder, const std::filesystem::path& game_root, const GuestBytes& guest) {
  return Load(Slots(), folder, game_root, guest);
}

Status Load(const SlotTable& slots, const std::filesystem::path& folder, const std::filesystem::path& game_root,
            const GuestBytes& guest) {
  Status status;
  for (const SlotDef& d : slots) status.slots.push_back(LoadedSlot{&d, nullptr, true});

  // Every slot's entry must have a place to be overlaid: a record naming an entry that is not overlaid would load another
  // character's file, so one without a place installs nothing.
  for (const SlotDef& d : slots) {
    if (!costumes::FileLocation(guest, d.tpr_file)) {
      status.notes.push_back(std::string(d.name) + ": its file " + std::to_string(d.tpr_file) +
                             " has no place in the game's file table; no colour slot is installed");
      return status;  // usable stays false
    }
  }

  // The files: every slot whose file passes is served from it; the others keep stand_in = true.
  std::error_code ec;
  std::string text;
  if (!std::filesystem::is_directory(folder, ec)) {
    status.notes.push_back("the colour folder " + Utf8(folder) + " is missing; every slot shows its stand-in");
  } else if (!ReadSmallFile(folder / "manifest.txt", kManifestLimit, &text)) {
    status.notes.push_back("manifest.txt in " + Utf8(folder) + " is missing, unreadable or over " +
                           std::to_string(kManifestLimit) + " bytes; every slot shows its stand-in");
  } else {
    const Parsed parsed = ParseManifest(slots, std::move(text), &status.notes);
    for (size_t i = 0; i < kSlotCount; ++i) {
      if (parsed.file[i].empty()) continue;
      std::string why;
      std::shared_ptr<const afs::Source> source = OpenChecked(folder, slots[i], parsed.file[i], &why);
      if (!source) {
        status.notes.push_back(std::string(slots[i].name) + ": " + why + "; stand-in");
        continue;
      }
      status.slots[i].tpr = std::move(source);
      status.slots[i].stand_in = false;
    }
  }

  // The stand-ins, from the game folder's archives (each opened once). One that cannot be read: nothing is installed.
  struct GameArchive {
    std::shared_ptr<afs::FileSource> source;
    std::optional<afs::Toc> toc;
    std::string why;
  };
  std::map<std::string, GameArchive> archives;
  for (LoadedSlot& l : status.slots) {
    if (!l.stand_in) continue;
    const SlotDef& d = *l.def;
    const std::optional<costumes::FileLoc> loc = costumes::FileLocation(guest, d.stand_in_file);
    std::string why;
    if (!loc) {
      why = "file " + std::to_string(d.stand_in_file) + " has no place in the game's file table";
    } else {
      auto it = archives.find(loc->archive);
      if (it == archives.end()) {
        GameArchive a;
        a.source = afs::FileSource::Open(game_root / loc->archive);
        if (!a.source) {
          a.why = "cannot open it in the game folder";
        } else {
          a.toc = afs::ReadToc(*a.source, &a.why);
        }
        it = archives.emplace(loc->archive, std::move(a)).first;
      }
      const GameArchive& a = it->second;
      if (!a.toc) {
        why = std::string(loc->archive) + ": " + a.why;
      } else if (loc->entry >= a.toc->count) {
        why = std::string(loc->archive) + " has " + std::to_string(a.toc->count) + " entries, no entry " +
              std::to_string(loc->entry);
      } else {
        l.tpr = std::make_shared<EntrySource>(a.source, a.toc->offsets[loc->entry], a.toc->sizes[loc->entry]);
      }
    }
    if (!why.empty()) {
      status.notes.push_back(std::string(d.name) + ": its stand-in (file " + std::to_string(d.stand_in_file) +
                             ") cannot be read: " + why + "; no colour slot is installed");
      status.usable = false;
      return status;
    }
  }
  status.usable = true;
  return status;
}

std::map<std::string, std::vector<afs::Replacement>> Replacements(const Status& s, const GuestBytes& guest) {
  std::map<std::string, std::vector<afs::Replacement>> out;
  if (!s.usable) return out;
  for (const LoadedSlot& l : s.slots) {
    const std::optional<costumes::FileLoc> loc = costumes::FileLocation(guest, l.def->tpr_file);
    if (!loc) return {};
    std::vector<afs::Replacement>& list = out[loc->archive];
    const bool taken = std::any_of(list.begin(), list.end(), [&](const afs::Replacement& r) { return r.entry == loc->entry; });
    if (!taken) list.push_back({loc->entry, l.tpr});
  }
  return out;
}

std::vector<costumes::Extra> TableExtras(const Status& s) {
  std::vector<costumes::Extra> out;
  if (!s.usable) return out;
  for (const LoadedSlot& l : s.slots) {
    const SlotDef& d = *l.def;
    out.push_back(costumes::Extra{d.character, d.tpr_file, d.cat_file, d.replaces, std::string("colour ") + d.name});
  }
  return out;
}

std::map<std::string, std::map<uint32_t, std::string>> EntryLabels(const Status& s, const GuestBytes& guest) {
  std::map<std::string, std::map<uint32_t, std::string>> out;
  if (!s.usable) return out;
  for (const LoadedSlot& l : s.slots) {
    if (const std::optional<costumes::FileLoc> loc = costumes::FileLocation(guest, l.def->tpr_file)) {
      out[loc->archive].emplace(loc->entry, std::string("colour ") + l.def->name);  // emplace: a shared entry keeps its first
    }
  }
  return out;
}

AlphaRow AlphaVanilla() { return {{0.9f, 1.0f, 1.0f, 0.4f}, {0.1f, 0.4f, 0.3f, 10.0f}}; }

std::optional<AlphaRow> AlphaRowFor(Slot slot) {
  // Task 2's ledger: the launchers' DOA4_ALPHA_PATCH values (RefractColor 0x8357EE30, outline tint 0x82F6A330); blue's
  // outline is the launcher's (0.04, 0.07, 0.4, 10), not the parent design's first row.
  switch (slot) {
    case Slot::kAlphaRed:
      return AlphaRow{{1.4f, 0.1f, 0.35f, 0.4f}, {0.4f, 0.03f, 0.1f, 10.0f}};
    case Slot::kAlphaBlue:
      return AlphaRow{{0.26f, 0.43f, 2.55f, 0.4f}, {0.04f, 0.07f, 0.4f, 10.0f}};
    case Slot::kAlphaLilac:
      return AlphaRow{{1.0f, 0.6f, 2.4f, 0.4f}, {0.11f, 0.065f, 0.4f, 10.0f}};
    default:
      return std::nullopt;
  }
}

}  // namespace doa4::colour_costumes
