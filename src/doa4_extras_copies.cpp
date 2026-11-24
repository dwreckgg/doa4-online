// Extras copies: the start-up builder (extras-copies plan, Task 8). See doa4_extras_copies.h.

#include "doa4_extras_copies.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "doa4_gamefiles.h"
#include "doa4_suitconv_slim.h"

namespace doa4::extras_copies {

namespace {

constexpr std::string_view kOperaBox = "doa4_helena_opera";
constexpr std::string_view kKimonosBox = "doa4_kokoro_kimonos";

// G0's g0_sha1.txt (ou_runs/extras_refl/g0): the retail files' SHA-1s. Limits: the plan's Global Constraints.
constexpr Costume kCostumes[] = {
    {"doa4_helena_opera", "hel03 (extras copy)", "hel03", 16, 17, "46aa6d42e34d093e1cdc198b9e78669a65bb0f5c",
     "6a7135bff35e25da97a47b77f83d7f26ddcc9a54", 0xFA0000, 0x200000},
    {"doa4_kokoro_kimonos", "hak03 (extras copy)", "hak03", 46, 47, "00408e2162b8c3dcc6e6bb85a3e22e0ef312ea65",
     "6b91dc27df6b8190b92cbed6c08a822b1e0300b2", 0xCB0000, 0x300000},
    {"doa4_kokoro_kimonos", "hak08 (extras copy)", "hak08", 56, 57, "e81b060734d8e908e19dec828c9f9d319f384e71",
     "de0335bd97423465f078421bab0f489642e195ed", 0xCB0000, 0x300000},
};

// The callback's answer, with a throw counted as false; a std::exception's text goes to *why.
bool ReadOne(const ReadEntry& read_entry, uint32_t entry, std::vector<uint8_t>* out, std::string* why) {
  try {
    return read_entry && read_entry(entry, out);
  } catch (const std::exception& e) {
    try {
      *why = e.what();
    } catch (...) {
    }
    return false;
  } catch (...) {
    return false;
  }
}

std::string CouldNotRead(uint32_t entry, const std::string& why) {
  std::string s = "could not read chrtwo.afs entry " + std::to_string(entry);
  if (!why.empty()) s += ": " + why;
  return s;
}

std::string NotRetail(const Costume& c, const char* ext, uint32_t entry) {
  return std::string(c.name) + "." + ext + " (chrtwo.afs entry " + std::to_string(entry) + ") is not the retail file";
}

std::string OverLimit(const Costume& c, const char* ext, size_t size, uint32_t limit) {
  return "the built " + std::string(c.name) + "." + ext + " is " + std::to_string(size) + " bytes, over its limit of " +
         std::to_string(limit);
}

// One costume: true with *out filled, or false with *reason. Throws only what the allocator or the string code throws.
bool BuildOne(const Costume& c, const ReadEntry& read_entry, suitconv::slim::Result* out, std::string* reason) {
  std::vector<uint8_t> tpr, cat;
  std::string why;
  if (!ReadOne(read_entry, c.tpr_entry, &tpr, &why)) return *reason = CouldNotRead(c.tpr_entry, why), false;
  if (!ReadOne(read_entry, c.cat_entry, &cat, &why)) return *reason = CouldNotRead(c.cat_entry, why), false;
  if (gamefiles::Sha1Hex(tpr.data(), tpr.size()) != c.tpr_sha1) return *reason = NotRetail(c, "tpr", c.tpr_entry), false;
  if (gamefiles::Sha1Hex(cat.data(), cat.size()) != c.cat_sha1) return *reason = NotRetail(c, "cat", c.cat_entry), false;
  suitconv::slim::Result built;
  std::string error;
  bool ok = false;
  try {
    ok = suitconv::slim::SlimCopies(c.name, tpr, cat, tpr, cat, suitconv::slim::CopyMode::kSlim, &built, &error);
  } catch (const std::exception& e) {  // SlimCopies promises not to throw; this is the game's start-up (Task 6 M7)
    ok = false;
    error = std::string("unexpected error: ") + e.what();
  } catch (...) {
    ok = false;
    error = "unexpected non-standard exception";
  }
  if (!ok) return *reason = error, false;
  if (built.tpr.size() > c.tpr_limit) return *reason = OverLimit(c, "tpr", built.tpr.size(), c.tpr_limit), false;
  if (built.cat.size() > c.cat_limit) return *reason = OverLimit(c, "cat", built.cat.size(), c.cat_limit), false;
  *out = std::move(built);
  return true;
}

// SHA-1 over every installed replacement's bytes, in order, read back from its source in fixed-size pieces.
std::string HashInstalled(const std::vector<afs::Replacement>& chrtwo) {
  gamefiles::Sha1 h;
  uint8_t buf[1 << 14];
  for (const afs::Replacement& r : chrtwo) {
    const uint64_t size = r.data ? r.data->size() : 0;
    for (uint64_t at = 0; at < size;) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(sizeof buf, size - at));
      if (!r.data->Read(at, buf, n)) throw std::runtime_error("an installed replacement could not be read back");
      h.Update(buf, n);
      at += n;
    }
  }
  return h.HexDigest();
}

// 40 lower-case hex chars: what HexDigest gives on success.
bool IsSha1Hex(const std::string& s) {
  return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// Runs f, and once more if it threw (a transient allocation failure); false when both attempts threw.
template <class F>
bool TwoTries(F&& f) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      f();
      return true;
    } catch (...) {
    }
  }
  return false;
}

}  // namespace

std::span<const Costume> Costumes() { return kCostumes; }

Built Build(bool opera_on, bool kimonos_on, const ReadEntry& read_entry, std::span<const Costume> table,
            const Hasher& hash) {
  const auto start = std::chrono::steady_clock::now();
  Built result;
  result.boxes_at_start = (opera_on ? 1u : 0u) | (kimonos_on ? 2u : 0u);
  // Room for every replacement and line up front, so committing a costume below allocates nothing.
  TwoTries([&] {
    result.chrtwo.reserve(2 * table.size());
    result.log.reserve(table.size() + 1);
  });
  int selected = 0, built_count = 0;
  for (const Costume& c : table) {
    const size_t mark = result.chrtwo.size();  // a failure while committing rolls the replacements back to here
    try {
      const std::string_view box = c.tweak_cvar ? c.tweak_cvar : "";
      if (box != kOperaBox && box != kKimonosBox) {
        result.log.push_back(std::string(c.name ? c.name : "?") + ": never built: unknown box " + std::string(box));
        continue;
      }
      if (!(box == kOperaBox ? opera_on : kimonos_on)) continue;
      ++selected;
      suitconv::slim::Result out;
      std::string reason;
      if (!BuildOne(c, read_entry, &out, &reason)) {
        result.log.push_back(std::string(c.name) + ": kept the stand-in: " + reason);
        continue;
      }
      // Everything that allocates first: the line and both sources.
      std::string line = std::string(c.name) + ": built (tpr " + std::to_string(out.tpr.size()) + " bytes, cat " +
                         std::to_string(out.cat.size()) + " bytes)";
      std::shared_ptr<const afs::Source> tpr = std::make_shared<afs::MemorySource>(std::move(out.tpr));
      std::shared_ptr<const afs::Source> cat = std::make_shared<afs::MemorySource>(std::move(out.cat));
      // Then the commit: within the reserved room these pushes only move.
      result.chrtwo.push_back({c.tpr_entry, std::move(tpr)});
      result.chrtwo.push_back({c.cat_entry, std::move(cat)});
      result.log.push_back(std::move(line));
      ++built_count;
    } catch (const std::exception& e) {
      result.chrtwo.erase(result.chrtwo.begin() + static_cast<ptrdiff_t>(mark), result.chrtwo.end());
      try {
        result.log.push_back(std::string(c.name ? c.name : "?") + ": kept the stand-in: unexpected error: " + e.what());
      } catch (...) {
      }
    } catch (...) {
      result.chrtwo.erase(result.chrtwo.begin() + static_cast<ptrdiff_t>(mark), result.chrtwo.end());
      try {
        result.log.push_back(std::string(c.name ? c.name : "?") +
                             ": kept the stand-in: unexpected non-standard exception");
      } catch (...) {
      }
    }
  }
  // sha1 from what was installed, in order; "" when nothing was.
  if (!result.chrtwo.empty() &&
      !TwoTries([&] { result.sha1 = hash ? hash(result.chrtwo) : HashInstalled(result.chrtwo); }))
    result.sha1.clear();
  // Keyed on the value, whatever produced it (two throws, or a hasher that answered "" or junk): copies with no real
  // digest are not installed, since Task 9 writes sha1 as the extras_copies meta key (online, record and replay), and
  // installed copies with no key would let two PCs or a replay differ unseen. Nothing here throws.
  const bool unhashed = !result.chrtwo.empty() && !IsSha1Hex(result.sha1);
  if (unhashed) {
    result.chrtwo.clear();
    result.sha1.clear();
    built_count = 0;
  }
  TwoTries([&] {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    result.log.push_back("extras copies: " + std::to_string(built_count) + " built of " + std::to_string(selected) +
                         " selected" + (unhashed ? " (the copies could not be hashed; none installed)" : "") + " in " +
                         std::to_string(ms.count()) + " ms");
  });
  return result;
}

ReadEntry ReadEntryFromSource(std::shared_ptr<const afs::Source> archive) {
  struct State {
    std::shared_ptr<const afs::Source> source;
    bool tried = false;
    std::optional<afs::Toc> toc;
    std::string toc_error;
  };
  auto state = std::make_shared<State>();
  state->source = std::move(archive);
  return [state](uint32_t entry, std::vector<uint8_t>* out) -> bool {
    if (!state->tried) {  // once; a throw (allocation) leaves it untried for the next call
      if (state->source) state->toc = afs::ReadToc(*state->source, &state->toc_error);
      state->tried = true;
    }
    if (!state->source || !out) return false;
    if (!state->toc) throw std::runtime_error(state->toc_error);  // Build puts the TOC's reason in its line
    if (entry >= state->toc->count) return false;
    out->resize(state->toc->sizes[entry]);
    return state->source->Read(state->toc->offsets[entry], out->data(), out->size());
  };
}

ReadEntry ReadEntryFrom(const std::filesystem::path& archive, Opener open) {
  struct State {
    std::filesystem::path path;
    Opener open;
    bool opened = false;
    ReadEntry inner;
  };
  auto state = std::make_shared<State>();
  state->path = archive;
  state->open = std::move(open);
  return [state](uint32_t entry, std::vector<uint8_t>* out) -> bool {
    if (!state->opened) {  // on the first call, not before: a Build with both boxes off never opens the file
      state->inner = ReadEntryFromSource(state->open ? state->open(state->path) : afs::FileSource::Open(state->path));
      state->opened = true;
    }
    return state->inner(entry, out);
  };
}

}  // namespace doa4::extras_copies
