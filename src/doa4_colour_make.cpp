// Making the missing colour costume files at a lobby start. See doa4_colour_make.h.

#include "doa4_colour_make.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "doa4_colour_costumes_internal.h"
#include "doa4_colourgen.h"
#include "doa4_gamefiles.h"
#include "doa4_utf8.h"

namespace doa4::colour_costumes {

static_assert(std::size(colourgen::kSources) == kSlotCount, "one source row for each colour slot");

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kSpaceMargin = 65536;         // free bytes kept beyond the files of one call
constexpr std::chrono::hours kLeftoverAge{1};    // a temporary file older than this was left by a run that ended

enum class SlotState : uint8_t { kUndecided, kGood, kMade, kNotMade };

struct Work {
  std::array<SlotState, kSlotCount> state{};  // kUndecided until the slot's outcome is known
  std::array<std::string, kSlotCount> cause;  // of kNotMade
  std::vector<std::string> other;             // notes that are not a slot's (the sweep, the folder, the manifest)
};

void NotMade(Work& w, size_t slot, std::string cause) {
  w.state[slot] = SlotState::kNotMade;
  w.cause[slot] = std::move(cause);
}

std::string SlotFile(const SlotDef& d) { return std::string(d.name) + ".tpr"; }

std::string Sha1Of(const std::vector<uint8_t>& bytes) {
  static const uint8_t kNone = 0;
  return gamefiles::Sha1Hex(bytes.empty() ? &kNone : bytes.data(), bytes.size());
}

// Whether `file` in the folder is the slot's good file. The file is closed again before this returns.
bool Passes(const fs::path& folder, const SlotDef& d, const std::string& file) {
  std::string why;
  return OpenChecked(folder, d, file, &why) != nullptr;
}

// The manifest's usable lines, or no line for any slot when there is no readable manifest.
Parsed ReadManifest(const SlotTable& slots, const fs::path& folder) {
  std::string text;
  if (!ReadSmallFile(folder / "manifest.txt", kManifestLimit, &text)) {
    return Parsed{std::vector<std::string>(kSlotCount), std::vector<bool>(kSlotCount, false)};
  }
  std::vector<std::string> ignored;
  return ParseManifest(slots, std::move(text), &ignored);
}

// The caller's tag with everything but digits and '-' dropped, so a temporary file's name cannot leave the folder.
std::string PlainTag(const std::string& tag) {
  std::string out;
  for (char c : tag) {
    if ((c >= '0' && c <= '9') || c == '-') out += c;
  }
  return out.empty() ? std::string("0") : out;
}

// Writes the bytes as `tmp` (flushed and closed), then renames it onto `target`. False with the system's text in
// *error, the temporary removed; *rename_refused: the bytes were written and only the rename failed.
bool WriteThenRename(const fs::path& tmp, const fs::path& target, const void* data, size_t size, std::string* error,
                     bool* rename_refused) {
  *rename_refused = false;
  std::error_code ec;
  bool written = false;
  {
    errno = 0;
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (out) {
      out.write(static_cast<const char*>(data), std::streamsize(size));
      out.flush();
      written = bool(out);
      out.close();
      written = written && !out.fail();
    }
    if (!written) *error = errno ? std::generic_category().message(errno) : std::string("the file could not be written");
  }
  if (written) {
    fs::rename(tmp, target, ec);
    if (!ec) return true;
    *error = ec.message();
    *rename_refused = true;
  }
  fs::remove(tmp, ec);
  return false;
}

// "<slot>.tpr.<anything>.tmp" for one of the slots, or "manifest.txt.<anything>.tmp": the names this module's own
// temporary files have.
bool SweepForm(const SlotTable& slots, const std::string& name) {
  static const std::string kEnd = ".tmp";
  const auto has = [&name](const std::string& start) {
    return name.size() >= start.size() + kEnd.size() && name.compare(0, start.size(), start) == 0 &&
           name.compare(name.size() - kEnd.size(), kEnd.size(), kEnd) == 0;
  };
  if (has("manifest.txt.")) return true;
  for (const SlotDef& d : slots) {
    if (has(std::string(d.name) + ".tpr.")) return true;
  }
  return false;
}

// Deletes the temporary files an ended run left: files of a sweep form written more than an hour ago. No other name
// is deleted, whatever its age; one that cannot be deleted stays for a later run.
void Sweep(const SlotTable& slots, const fs::path& folder, std::vector<std::string>* notes) {
  std::error_code ec;
  const fs::file_time_type now = fs::file_time_type::clock::now();
  std::vector<fs::path> old;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::string name;
    try {
      name = Utf8(it->path().filename());
    } catch (...) {
      continue;  // a name that is not text is not one of ours
    }
    if (!SweepForm(slots, name)) continue;
    std::error_code file_ec;
    if (!it->is_regular_file(file_ec) || file_ec) continue;
    const fs::file_time_type written = fs::last_write_time(it->path(), file_ec);
    if (file_ec) continue;
    if (now - written > kLeftoverAge) old.push_back(it->path());
  }
  if (ec) notes->push_back("sweep: " + ec.message());
  for (const fs::path& p : old) fs::remove(p, ec);
}

// One slot to make: its source (checked), then the generator's result or the cause it gave.
struct Job {
  size_t slot = 0;
  std::shared_ptr<const std::vector<uint8_t>> source;
  std::optional<std::vector<uint8_t>> made;
  std::string cause;
};

// The sources of one call: each archive opened once, each distinct source file read once and shared.
class Sources {
 public:
  Sources(const fs::path& game_root, const GuestBytes& guest) : game_root_(game_root), guest_(guest) {}

  // The source file's bytes when it is there with `size` bytes; *missing or *differs otherwise.
  std::shared_ptr<const std::vector<uint8_t>> Get(uint16_t file, uint64_t size, const char* sha1, bool* missing,
                                                  bool* differs) {
    *missing = *differs = false;
    auto it = files_.find(file);
    if (it == files_.end()) it = files_.emplace(file, Find(file)).first;
    File& f = it->second;
    if (!f.archive) {
      *missing = true;
      return nullptr;
    }
    if (f.size != size) {
      *differs = true;
      return nullptr;
    }
    if (!f.bytes) {
      auto bytes = std::make_shared<std::vector<uint8_t>>(size_t(f.size));
      if (!f.archive->Read(f.offset, bytes->data(), bytes->size())) {
        f.archive = nullptr;
        *missing = true;
        return nullptr;
      }
      f.sha1 = Sha1Of(*bytes);
      f.bytes = std::move(bytes);
    }
    if (sha1 == nullptr || f.sha1 != sha1) {
      *differs = true;
      return nullptr;
    }
    return f.bytes;
  }

 private:
  struct GameArchive {
    std::shared_ptr<afs::FileSource> source;
    std::optional<afs::Toc> toc;
  };
  struct File {
    std::shared_ptr<afs::FileSource> archive;  // null: not found, cannot open, no such entry
    uint64_t offset = 0, size = 0;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::string sha1;
  };
  File Find(uint16_t file) {
    const std::optional<costumes::FileLoc> loc = costumes::FileLocation(guest_, file);
    if (!loc) return {};
    auto it = archives_.find(loc->archive);
    if (it == archives_.end()) {
      GameArchive a;
      a.source = afs::FileSource::Open(game_root_ / loc->archive);
      std::string why;
      if (a.source) a.toc = afs::ReadToc(*a.source, &why);
      it = archives_.emplace(loc->archive, std::move(a)).first;
    }
    const GameArchive& a = it->second;
    if (!a.toc || loc->entry >= a.toc->count) return {};
    return File{a.source, a.toc->offsets[loc->entry], a.toc->sizes[loc->entry], nullptr, {}};
  }

  const fs::path& game_root_;
  const GuestBytes& guest_;
  std::map<std::string, GameArchive> archives_;
  std::map<uint16_t, File> files_;
};

void Body(const SlotTable& slots, const std::array<MakeDef, kSlotCount>& defs, const fs::path& folder,
          const fs::path& game_root, const GuestBytes& guest, const MakeOptions& opt, Work& w) {
  // Which slots are good, and which of those the manifest names. Every file opened for this is closed again here.
  const Parsed first = ReadManifest(slots, folder);
  size_t to_make = 0, unnamed = 0;
  for (size_t i = 0; i < kSlotCount; ++i) {
    const bool line = !first.file[i].empty();
    const bool good = Passes(folder, slots[i], line ? first.file[i] : SlotFile(slots[i]));
    if (good) w.state[i] = SlotState::kGood;
    to_make += good ? 0 : 1;
    unnamed += (good && line) ? 0 : 1;
  }
  if (to_make == 0 && unnamed == 0) return;  // every slot good and named: nothing is touched

  // The folder, then the temporary files an ended run left.
  {
    std::error_code ec, is_ec;
    fs::create_directories(folder, ec);
    if (!fs::is_directory(folder, is_ec)) {
      const std::string text = ec ? ec.message() : is_ec ? is_ec.message() : std::string("it is not a folder");
      for (size_t i = 0; i < kSlotCount; ++i) {
        if (w.state[i] != SlotState::kGood) NotMade(w, i, "cannot write: " + text);
      }
      w.other.push_back("folder: cannot create: " + text);
      return;
    }
  }
  Sweep(slots, folder, &w.other);

  if (to_make > 0 && opt.on_start) opt.on_start(to_make);

  // On this thread, in table order: the space check, then the source and its check.
  std::vector<Job> jobs;
  if (to_make > 0) {
    const bool check_space = bool(opt.free_bytes);
    const uint64_t free_space = check_space ? opt.free_bytes(folder) : 0;  // read once a call
    uint64_t reserved = 0;  // the sizes of this call's earlier slots that passed their space check
    Sources sources(game_root, guest);
    for (size_t i = 0; i < kSlotCount; ++i) {
      if (w.state[i] == SlotState::kGood) continue;
      if (check_space && free_space < reserved + slots[i].size + kSpaceMargin) {
        NotMade(w, i, "no space");
        continue;
      }
      reserved += slots[i].size;
      bool missing = false, differs = false;
      std::shared_ptr<const std::vector<uint8_t>> source =
          sources.Get(defs[i].source_file, defs[i].source_size, defs[i].source_sha1, &missing, &differs);
      if (!source) {
        NotMade(w, i, missing ? "source missing" : "source differs");  // never passed to the generator
        continue;
      }
      jobs.push_back(Job{i, std::move(source), std::nullopt, {}});
    }
  }

  // The generator, on up to opt.threads workers (this thread is one of them). Nothing else runs on the others.
  if (!jobs.empty()) {
    const auto run = [&slots, &opt](Job& job) {
      try {
        std::string why;
        std::optional<std::vector<uint8_t>> made =
            opt.generate(slots[job.slot].name, std::span<const uint8_t>(*job.source), &why);
        if (made) {
          job.made = std::move(made);
        } else {
          job.cause = "generator: " + why;
        }
      } catch (const std::exception& e) {
        job.cause = std::string("generator: ") + e.what();
      } catch (...) {
        job.cause = "generator: unknown exception";
      }
    };
    std::atomic<size_t> next{0};
    const auto worker = [&jobs, &next, &run] {
      for (size_t j; (j = next.fetch_add(1)) < jobs.size();) {
        try {
          run(jobs[j]);
        } catch (...) {  // what run's own handlers throw (no memory for a cause): the job is left without a result
        }
      }
    };
    struct Joined {
      std::vector<std::thread> threads;
      ~Joined() {
        for (std::thread& t : threads) t.join();
      }
    } joined;
    const size_t workers = std::min<size_t>(opt.threads, jobs.size());
    for (size_t t = 1; t < workers; ++t) {
      try {
        joined.threads.emplace_back(worker);
      } catch (...) {
        break;  // no more threads: the ones there are make the rest
      }
    }
    worker();
  }

  // Back on this thread, in table order: each result's size and SHA-1, then the write.
  const std::string tag = PlainTag(opt.temp_tag);
  for (Job& job : jobs) {
    const size_t i = job.slot;
    const SlotDef& d = slots[i];
    job.source.reset();
    if (!job.made) {
      NotMade(w, i, job.cause.empty() ? std::string("generator: unknown exception") : job.cause);
      continue;
    }
    if (job.made->size() != d.size) {
      NotMade(w, i, "made file fails its check");
      continue;
    }
    if (Sha1Of(*job.made) != d.sha1) {
      NotMade(w, i, "made file fails its check");
      continue;
    }
    std::string error;
    bool rename_refused = false;
    const fs::path target = folder / PathFromUtf8(SlotFile(d));
    const fs::path tmp = folder / PathFromUtf8(SlotFile(d) + "." + tag + ".tmp");
    if (WriteThenRename(tmp, target, job.made->data(), job.made->size(), &error, &rename_refused)) {
      w.state[i] = SlotState::kMade;
    } else if (rename_refused && Passes(folder, d, SlotFile(d))) {
      w.state[i] = SlotState::kGood;  // another process made it meanwhile
    } else {
      NotMade(w, i, "cannot write: " + error);
    }
    job.made.reset();
  }

  // The manifest, read again just before it is replaced: one line per slot whose file is good on disk now. A usable
  // line that names a good file is kept (another process may have written it); any other slot's line names <slot>.tpr.
  try {
    const Parsed now = ReadManifest(slots, folder);
    const Parsed& lines = now;
    std::string text = "# colour costume files, one \"<slot> <file>\" line each; the game makes a missing file again\n";
    for (size_t i = 0; i < kSlotCount; ++i) {
      std::string file;
      if (!lines.file[i].empty() && Passes(folder, slots[i], lines.file[i])) {
        file = lines.file[i];
      } else if (Passes(folder, slots[i], SlotFile(slots[i]))) {
        file = SlotFile(slots[i]);
      }
      if (!file.empty()) text += std::string(slots[i].name) + " " + file + "\n";
    }
    std::string error;
    bool rename_refused = false;
    if (!WriteThenRename(folder / PathFromUtf8("manifest.txt." + tag + ".tmp"), folder / "manifest.txt", text.data(),
                         text.size(), &error, &rename_refused)) {
      w.other.push_back("manifest: cannot write: " + error);
    }
  } catch (const std::exception& e) {
    w.other.push_back(std::string("manifest: cannot write: ") + e.what());
  } catch (...) {
    w.other.push_back("manifest: cannot write: unknown exception");
  }
}

}  // namespace

ProcessKind KindOf(bool session, std::string_view np_mode) {
  return (!session && np_mode.empty()) ? ProcessKind::kLobby : ProcessKind::kMatch;
}

bool ShouldMake(ProcessKind kind, bool launch_refused, bool folder_overridden) {
  return kind == ProcessKind::kLobby && !launch_refused && !folder_overridden;
}

const std::array<MakeDef, kSlotCount>& MakeDefs() {
  static const std::array<MakeDef, kSlotCount> kDefs = [] {
    std::array<MakeDef, kSlotCount> defs{};
    for (size_t i = 0; i < kSlotCount; ++i) {
      const colourgen::SourceDef& s = colourgen::kSources[i];
      defs[i] = MakeDef{s.slot, s.file, s.size, s.sha1};
    }
    return defs;
  }();
  return kDefs;
}

MakeReport MakeMissing(const SlotTable& slots, const std::array<MakeDef, kSlotCount>& defs,
                       const std::filesystem::path& folder, const std::filesystem::path& game_root,
                       const GuestBytes& guest, const MakeOptions& opt) {
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  MakeReport report;
  Work w;
  // Nothing escapes: whatever is thrown outside a slot's own work ends the call with the report so far.
  try {
    Body(slots, defs, folder, game_root, guest, opt, w);
  } catch (const std::exception& e) {
    try {
      w.other.push_back(std::string("stopped: ") + e.what());
    } catch (...) {
    }
  } catch (...) {
    try {
      w.other.push_back("stopped: unknown exception");
    } catch (...) {
    }
  }
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (w.state[i] == SlotState::kGood) {
      ++report.good;
    } else if (w.state[i] == SlotState::kMade) {
      ++report.made;
    } else {
      ++report.not_made;
    }
  }
  try {
    // One note per slot not made, in the table's order whatever the workers' order was; then the others. Text from
    // outside the program (system text, the generator's reason, an exception's) goes through Printable.
    for (size_t i = 0; i < kSlotCount; ++i) {
      if (w.state[i] == SlotState::kGood || w.state[i] == SlotState::kMade) continue;
      const std::string cause = w.cause[i].empty() ? std::string("cannot write: the call stopped first") : w.cause[i];
      report.notes.push_back(Printable(std::string(slots[i].name) + ": " + cause));
    }
    for (const std::string& note : w.other) report.notes.push_back(Printable(note));
  } catch (...) {
  }
  report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return report;
}

std::string MakeTempTag() {
  static std::atomic<long long> last{0};
  long long ticks = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
  long long seen = last.load();
  do {
    if (ticks <= seen) ticks = seen + 1;  // two calls inside one clock tick still differ
  } while (!last.compare_exchange_weak(seen, ticks));
#if defined(_WIN32)
  const long long pid = _getpid();
#else
  const long long pid = getpid();
#endif
  return std::to_string(pid) + "-" + std::to_string(ticks);
}

}  // namespace doa4::colour_costumes
