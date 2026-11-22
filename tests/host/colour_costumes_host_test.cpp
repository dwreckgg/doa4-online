// Host test for src/doa4_colour_costumes.cpp: the slot table, Load's degrade grains against a synthetic game folder
// (a chrthree-shaped archive holding the stand-in entries, an avt-shaped archive with entry 0 at 0x1800) and synthetic
// colour files, the replacements, table rows, labels and Alpha's rows. The synthetic checks plant the file-table words of
// the files they use in a fake guest, so they run without the image. Optional tiers:
//   DOA4_TEST_IMAGE (doa4_test_image.h): the real file table and costume records for the slot table's file numbers;
//   DOA4_SUITS_GAME_DIR=<the DOA4 folder> (read-only): with the image, the real stand-ins and avt.afs entries;
//   DOA4_COLOURS_DIR=<the private colour folder> (read-only): with the image and the game folder, the approved files load.
// Also src/doa4_colour_make.cpp (making the missing files at a lobby start): KindOf, ShouldMake, MakeDefs, MakeTempTag
// and MakeMissing against the same synthetic folders with a test generator; each of those tests prints "-- <its name>"
// when it starts. With the image, the source files of colourgen::kSources are looked up in its file table. And the
// decisions the app's wiring reads (same header): RowsOn, MustRefuse, FreeBytesFrom, IsPassingNote, RefusalMessage.
// Set but unusable, a tier FAILS. No game byte and no colour file is in this repository. Built and run by
// run_colour_costumes_test.bat. Prints "colour_costumes: N checks run" before its verdict line.

#include "../../src/doa4_colour_costumes.h"
#include "../../src/doa4_colour_make.h"
#include "../../src/doa4_colourgen.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "../../src/doa4_costume_lists.h"
#include "../../src/doa4_costumes.h"
#include "../../src/doa4_gamefiles.h"
#include "../../src/doa4_tweaks.h"
#include "../../src/doa4_utf8.h"
#include "doa4_test_image.h"

#include <exception>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int g_failures = 0;
static int g_checks = 0;  // every CHECK that ran: a mutant that skips or ends a section shows here
#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::fflush(stdout);                                        \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace doa4::colour_costumes;
namespace fs = std::filesystem;
namespace costumes = doa4::costumes;
namespace afs = doa4::afs;

static const doa4_test::CostumeImage* g_image = nullptr;

// ---- fixtures ---------------------------------------------------------------------------------------------------------

// The ledger (Task 2's value table, printed by a script from it): name, character, replaces, tpr_file, cat_file,
// stand_in_file, size, SHA-1. Written here on its own, so a changed number in the module is a failure.
struct LedgerRow {
  const char* name;
  int character, replaces, tpr_file, cat_file, stand_in_file;
  uint64_t size;
  const char* sha1;
};
static const LedgerRow kLedger[kSlotCount] = {
    {"alpha_red", 16, 1, 1287, 428, 427, 3758080ull, "caf01e0e2d9874c8f87f664ecfe3469ee864999e"},
    {"alpha_blue", 16, -1, 1288, 428, 427, 3758080ull, "c9c10d06da970a63460521eff34f0a3119b8e192"},
    {"alpha_lilac", 16, -1, 1289, 428, 427, 3758080ull, "624851c6b8ce5550fa169db4b0371e87a401f6b4"},
    {"spartan_h3_pink", 18, -1, 1290, 440, 439, 18741248ull, "21f76e0b9f91bf5bfa9b1a1bdaee328b36e77a45"},
    {"spartan_h3_orchid", 18, -1, 1291, 446, 445, 18741248ull, "40b71311f008d6cd7b773502777033ad70c51f4c"},
    {"spartan_h2_orange", 18, -1, 1292, 438, 437, 18741248ull, "f9c779965c82b84bbc8b1320469327ac53a959e2"},
    {"spartan_h2_cyan", 18, -1, 1293, 448, 447, 18741248ull, "9dde70ca7fc656da0e4026e6c2b0d9d9cc7c5c50"},
};
// The retail stand-in entries (chrthree.afs of the game folder), from the same ledger: stand-in file, size, SHA-1.
struct StandInRow {
  int file;
  uint64_t size;
  const char* sha1;
};
static const StandInRow kStandInLedger[kSlotCount] = {
    {427, 3758080ull, "b2d94ab7b1da6d4ccdf0e136140d58216639a209"},
    {427, 3758080ull, "b2d94ab7b1da6d4ccdf0e136140d58216639a209"},
    {427, 3758080ull, "b2d94ab7b1da6d4ccdf0e136140d58216639a209"},
    {439, 18741248ull, "2f8b20c08c23ec96635bbc10d042c77260e27062"},
    {445, 18741248ull, "7467381f8b20fa3a0ab739e3f4486ddba81b3fe2"},
    {437, 18741248ull, "05274f0370b48e8306580d3585ce7f6de655e098"},
    {447, 18741248ull, "9c4fc2f8019a2b12a1c5bfd9374a4d2d8f1b40ea"},
};
// Task 1B's proven avt.afs entries (files 914 + entry), the ones assigned to the slots.
static constexpr uint32_t kProvenFirst = 373, kProvenLast = 379;
static constexpr uint32_t kAvtFirstFile = 914;
// Where Task 2 mapped the chrthree files: (file, entry); the pink/orchid plan's Task 3 put pink on COS. 3 (439/440 =
// entries 34/35) in place of h3_red's COS. 5 (443/444).
static const std::map<int, uint32_t> kChrthreeEntry = {{427, 22}, {428, 23}, {437, 32}, {438, 33}, {439, 34},
                                                       {440, 35}, {445, 40}, {446, 41}, {447, 42}, {448, 43}};

struct FakeGuest {
  std::map<uint32_t, uint8_t> bytes;
  doa4::GuestBytes guest{[this](uint32_t a) {
                           auto it = bytes.find(a);
                           return it == bytes.end() ? uint8_t(0) : it->second;
                         },
                         [this](uint32_t a, uint8_t v) { bytes[a] = v; }};
  // A heap for costumes::Apply's moved blocks (as costumes_host_test.cpp's): outside the image, 32-byte steps.
  uint32_t next_block = 0x40000000;
  costumes::AllocFn alloc = [this](uint32_t size) -> uint32_t {
    const uint32_t block = next_block;
    next_block += (size + 31) / 32 * 32;
    return block;
  };
  void Load(uint32_t start, const std::vector<uint8_t>& data) {
    for (size_t i = 0; i < data.size(); ++i) bytes[start + uint32_t(i)] = data[i];
  }
  // A file-table word: {archive, 0, be16 entry} at kFileTable + 4 * file.
  void PlantFile(uint32_t file, uint8_t archive, uint32_t entry) {
    const uint32_t at = costumes::kFileTable + 4 * file;
    bytes[at] = archive;
    bytes[at + 1] = 0;
    bytes[at + 2] = uint8_t(entry >> 8);
    bytes[at + 3] = uint8_t(entry);
  }
};

// The synthetic guest: the file-table words of every file the slot table names (archive 5 = chrthree, 0 = avt).
static FakeGuest PlantedGuest() {
  FakeGuest g;
  for (const auto& [file, entry] : kChrthreeEntry) g.PlantFile(uint32_t(file), 5, entry);
  for (uint32_t e = kProvenFirst; e <= kProvenLast; ++e) g.PlantFile(kAvtFirstFile + e, 0, e);
  return g;
}
static FakeGuest ImageGuest() {
  FakeGuest g;
  if (!g_image) return g;
  g.Load(doa4_test::kCostumeTablesRange.address, g_image->tables);
  g.Load(doa4_test::kCostumeRecordsRange.address, g_image->records);
  g.Load(doa4_test::kCostumeFilesRange.address, g_image->files);
  return g;
}

static void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}
static uint32_t Get32(const std::vector<uint8_t>& b, size_t at) {
  if (at + 4 > b.size()) return 0;
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

// An AFS archive of `count` entries: the entries in `data` hold those bytes, every other one a 16-byte placeholder
// (16 x 0x77). Entry 0 starts after the TOC, rounded up to 2048 (count 700: 0x1800, as avt.afs).
static std::vector<uint8_t> Archive(uint32_t count, const std::map<uint32_t, std::vector<uint8_t>>& data) {
  const uint64_t first = (8 + 8ull * count + 8 + 2047) / 2048 * 2048;
  std::vector<uint64_t> offsets, sizes;
  uint64_t pos = first;
  for (uint32_t i = 0; i < count; ++i) {
    const auto it = data.find(i);
    const uint64_t size = it == data.end() ? 16 : it->second.size();
    offsets.push_back(pos);
    sizes.push_back(size);
    pos = (pos + size + 2047) / 2048 * 2048;
  }
  std::vector<uint8_t> a(size_t((pos + 48ull * count + 2047) / 2048 * 2048), 0);
  std::memcpy(a.data(), "AFS\0", 4);
  Put32(a, 4, count);
  for (uint32_t i = 0; i < count; ++i) {
    Put32(a, 8 + 8 * i, uint32_t(offsets[i]));
    Put32(a, 12 + 8 * i, uint32_t(sizes[i]));
    const auto it = data.find(i);
    if (it == data.end()) {
      std::memset(a.data() + offsets[i], 0x77, 16);
    } else {
      std::memcpy(a.data() + offsets[i], it->second.data(), it->second.size());
    }
  }
  Put32(a, 8 + 8 * count, uint32_t(pos));
  Put32(a, 12 + 8 * count, 48 * count);
  return a;
}

static std::vector<uint8_t> Content(int seed, size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed * 31 + i * 7 + (i >> 8));
  return v;
}
static std::string Sha(const std::vector<uint8_t>& v) { return doa4::gamefiles::Sha1Hex(v.data(), v.size()); }
static void Write(const fs::path& p, const std::vector<uint8_t>& bytes) {
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
static void WriteText(const fs::path& p, const std::string& text) {
  std::ofstream(p, std::ios::binary).write(text.data(), std::streamsize(text.size()));
}
static std::vector<uint8_t> ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Whether `source` holds exactly `want`.
static bool Holds(const std::shared_ptr<const afs::Source>& source, const std::vector<uint8_t>& want) {
  if (!source || source->size() != want.size()) return false;
  std::vector<uint8_t> got(want.size());
  return source->Read(0, got.data(), got.size()) && got == want;
}
static std::string HashOf(const afs::Source& source) {
  doa4::gamefiles::Sha1 sha;
  std::vector<uint8_t> buffer(size_t(1) << 20);
  for (uint64_t at = 0; at < source.size();) {
    const size_t n = size_t(std::min<uint64_t>(buffer.size(), source.size() - at));
    if (!source.Read(at, buffer.data(), n)) return {};
    sha.Update(buffer.data(), n);
    at += n;
  }
  return sha.HexDigest();
}

static bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
static std::string NoteAt(const Status& s, size_t i) { return i < s.notes.size() ? s.notes[i] : std::string(); }
static size_t NotesNaming(const Status& s, const std::string& part) {
  size_t n = 0;
  for (const std::string& note : s.notes) n += Contains(note, part) ? 1 : 0;
  return n;
}
static void PrintNotes(const char* test, const Status& s) {
  for (const std::string& note : s.notes) std::printf("  %s note: %s\n", test, note.c_str());
}

// The synthetic stand-in entries' bytes (distinct per entry), and each slot's synthetic colour file (distinct per slot).
static std::vector<uint8_t> StandInBytes(uint32_t entry) { return Content(int(100 + entry), 3000 + 11 * entry); }
// A colour file has the size of its slot's stand-in entry (a stand-in of another size is not usable: the rule in Load);
// the seed differs, so its bytes differ from the stand-in's and from every other slot's.
static std::vector<uint8_t> StandInFor(size_t slot);
static std::vector<uint8_t> ColourBytes(size_t slot) { return Content(int(1 + slot), StandInFor(slot).size()); }

// Slots() with each size and SHA-1 replaced by the synthetic file's (the file numbers stay the real ones).
static const SlotTable& TestSlots() {
  static const SlotTable table = [] {
    static std::vector<std::string> shas;
    SlotTable t = Slots();
    shas.reserve(kSlotCount);
    for (size_t i = 0; i < kSlotCount; ++i) {
      const std::vector<uint8_t> bytes = ColourBytes(i);
      shas.push_back(Sha(bytes));
      t[i].size = bytes.size();
      t[i].sha1 = shas.back().c_str();
    }
    return t;
  }();
  return table;
}

// One synthetic game folder and colour folder per World (per process and counter, under the run's TEMP).
struct World {
  fs::path root, game, colours;
  explicit World(uint32_t chrthree_count = 48, bool with_chrthree = true) {
    static int counter = 0;
    root = fs::temp_directory_path() /
           ("doa4_colour_costumes_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(root, ec);
    game = root / "game";
    colours = root / "colour_costumes";
    fs::create_directories(game);
    fs::create_directories(colours);
    std::map<uint32_t, std::vector<uint8_t>> chr;
    for (const auto& [file, entry] : kChrthreeEntry) {
      if (entry < chrthree_count) chr[entry] = StandInBytes(entry);
    }
    if (with_chrthree) Write(game / "chrthree.afs", Archive(chrthree_count, chr));
    std::map<uint32_t, std::vector<uint8_t>> avt;
    for (uint32_t e = kProvenFirst; e <= kProvenLast; ++e) avt[e] = Content(int(500 + e), 26624);
    Write(game / "avt.afs", Archive(700, avt));
    std::string manifest;
    for (size_t i = 0; i < kSlotCount; ++i) {
      const std::string name = Slots()[i].name;
      Write(colours / (name + ".tpr"), ColourBytes(i));
      manifest += name + " " + name + ".tpr\n";
    }
    WriteText(colours / "manifest.txt", manifest);
  }
  ~World() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  fs::path File(size_t slot) const { return colours / (std::string(Slots()[slot].name) + ".tpr"); }
};

// The stand-in bytes slot i must be served (its stand-in file's synthetic entry).
static std::vector<uint8_t> StandInFor(size_t slot) {
  const auto it = kChrthreeEntry.find(Slots()[slot].stand_in_file);
  return it == kChrthreeEntry.end() ? std::vector<uint8_t>() : StandInBytes(it->second);
}

// Every slot: listed in order, from its file (stand_in false, the colour bytes) unless `stand_ins` names it.
static void CheckSlots(const Status& s, const std::set<size_t>& stand_ins) {
  CHECK(s.slots.size() == kSlotCount);
  for (size_t i = 0; i < kSlotCount && i < s.slots.size(); ++i) {
    const LoadedSlot& l = s.slots[i];
    CHECK(l.def == &TestSlots()[i]);
    const bool want_stand_in = stand_ins.count(i) != 0;
    CHECK(l.stand_in == want_stand_in);
    if (want_stand_in) {
      CHECK(Holds(l.tpr, StandInFor(i)));
      CHECK(!Holds(l.tpr, ColourBytes(i)));
    } else {
      CHECK(Holds(l.tpr, ColourBytes(i)));
    }
  }
}

// ---- tests ------------------------------------------------------------------------------------------------------------

static void TestLoadAllGood() {
  World w;
  FakeGuest g = PlantedGuest();
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  PrintNotes("TestLoadAllGood", s);
  CHECK(s.usable);
  CHECK(s.notes.empty());
  CheckSlots(s, {});
  CHECK(s.Summary() == "colour costumes: 7 slots, 7 from files, 0 stand-ins");
  // Every slot is a table row, in order.
  const std::vector<costumes::Extra> extras = TableExtras(s);
  CHECK(extras.size() == kSlotCount);
  for (size_t i = 0; i < kSlotCount && i < extras.size(); ++i) {
    const SlotDef& d = Slots()[i];
    CHECK(extras[i].character == d.character);
    CHECK(extras[i].body_tpr == d.tpr_file);
    CHECK(extras[i].body_cat == d.cat_file);
    CHECK(extras[i].replaces == d.replaces);
    CHECK(extras[i].label == std::string("colour ") + d.name);
  }
  // The stand-in archive is not needed when every file is good: a game folder without chrthree.afs still loads.
  World nochr(48, false);
  const Status s2 = Load(TestSlots(), nochr.colours, nochr.game, g.guest);
  CHECK(s2.usable);
  CHECK(s2.notes.empty());
}

static void TestLoadNoFolder() {
  World w;
  FakeGuest g = PlantedGuest();
  const Status s = Load(TestSlots(), w.root / "no_such_folder", w.game, g.guest);
  PrintNotes("TestLoadNoFolder", s);
  CHECK(s.usable);
  CHECK(s.notes.size() == 1);
  CheckSlots(s, {0, 1, 2, 3, 4, 5, 6});
  CHECK(s.Summary() == "colour costumes: 7 slots, 0 from files, 7 stand-ins");
  // Every slot is still listed, each overlaid with its stand-in.
  CHECK(TableExtras(s).size() == kSlotCount);
  const auto r = Replacements(s, g.guest);
  const auto it = r.find("avt.afs");
  CHECK(r.size() == 1 && it != r.end() && it->second.size() == kSlotCount);
}

static void TestLoadNoManifest() {
  World w;
  fs::remove(w.colours / "manifest.txt");
  FakeGuest g = PlantedGuest();
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  PrintNotes("TestLoadNoManifest", s);
  CHECK(s.usable);
  CHECK(s.notes.size() == 1);
  CHECK(Contains(NoteAt(s, 0), "manifest.txt"));
  CheckSlots(s, {0, 1, 2, 3, 4, 5, 6});
  // A manifest past the size limit is unreadable too.
  WriteText(w.colours / "manifest.txt", std::string(size_t(kManifestLimit) + 1, '#'));
  const Status big = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(big.usable && big.notes.size() == 1);
  CheckSlots(big, {0, 1, 2, 3, 4, 5, 6});
}

static void TestLoadWrongSize() {
  World w;
  std::vector<uint8_t> longer = ColourBytes(4);
  longer.push_back(0);
  Write(w.File(4), longer);
  FakeGuest g = PlantedGuest();
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  PrintNotes("TestLoadWrongSize", s);
  CHECK(s.usable);
  CHECK(s.notes.size() == 1);
  CHECK(NotesNaming(s, "spartan_h3_orchid") == 1);
  CheckSlots(s, {4});
  // A missing file: the same grain (another folder: `s` holds this one's files open, so they cannot be deleted).
  World w2;
  Write(w2.File(4), longer);
  CHECK(fs::remove(w2.File(0)));
  const Status m = Load(TestSlots(), w2.colours, w2.game, g.guest);
  CHECK(m.usable && m.notes.size() == 2 && NotesNaming(m, "alpha_red") == 1);
  CheckSlots(m, {0, 4});
}

// Degenerate fixture: the right size, one byte changed. A size-only check passes it.
static void TestLoadSameSizeOtherBytes() {
  World w;
  std::vector<uint8_t> bytes = ColourBytes(2);
  bytes[bytes.size() / 2] ^= 0x5A;
  Write(w.File(2), bytes);
  CHECK(bytes.size() == TestSlots()[2].size);  // precondition: only the hash can tell
  FakeGuest g = PlantedGuest();
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  PrintNotes("TestLoadSameSizeOtherBytes", s);
  CHECK(s.usable);
  CHECK(s.notes.size() == 1);
  CHECK(NotesNaming(s, "alpha_lilac") == 1);
  CheckSlots(s, {2});
}

static void TestStandInUnreadable() {
  FakeGuest g = PlantedGuest();
  {
    // No colour folder and no chrthree.afs: no slot has bytes.
    World w(48, false);
    const Status s = Load(TestSlots(), w.root / "none", w.game, g.guest);
    PrintNotes("TestStandInUnreadable (a)", s);
    CHECK(!s.usable);
    CHECK(s.notes.size() == 2);  // the folder, then the stand-in
    CHECK(Contains(NoteAt(s, 1), "chrthree.afs"));
    CHECK(TableExtras(s).empty());
    CHECK(Replacements(s, g.guest).empty());
    CHECK(EntryLabels(s, g.guest).empty());
    CHECK(Contains(s.Summary(), "not installed"));
  }
  {
    // One slot needs its stand-in, and its entry (40, spartan_h3_orchid's) is past the archive's end; the others are good.
    World w(40);
    fs::remove(w.File(4));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    PrintNotes("TestStandInUnreadable (b)", s);
    CHECK(!s.usable);
    CHECK(s.notes.size() == 2);  // the missing file, then the stand-in
    CHECK(NotesNaming(s, "spartan_h3_orchid") == 2);
    CHECK(TableExtras(s).empty());
    CHECK(Replacements(s, g.guest).empty());
    CHECK(EntryLabels(s, g.guest).empty());
  }
  {
    // The same archive with every file good: the stand-in is not needed, so the slots install.
    World w(40);
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable && s.notes.empty() && TableExtras(s).size() == kSlotCount);
  }
  {
    // A stand-in file number with no location (past the file table): that slot has no bytes.
    World w;
    fs::remove(w.File(6));
    SlotTable t = TestSlots();
    t[6].stand_in_file = 1700;
    const Status s = Load(t, w.colours, w.game, g.guest);
    PrintNotes("TestStandInUnreadable (d)", s);
    CHECK(!s.usable);
    CHECK(NotesNaming(s, "spartan_h2_cyan") == 2);
  }
}

static void TestSharedEntry() {
  World w;
  FakeGuest g = PlantedGuest();
  SlotTable t = TestSlots();
  t[2].tpr_file = t[1].tpr_file;  // lilac shares blue's entry (Task 1's fallback)
  const Status s = Load(t, w.colours, w.game, g.guest);
  CHECK(s.usable && s.notes.empty());
  const auto r = Replacements(s, g.guest);
  const auto it = r.find("avt.afs");
  CHECK(r.size() == 1 && it != r.end());
  if (it == r.end()) return;
  CHECK(it->second.size() == 6);
  size_t holders = 0;
  for (const afs::Replacement& rep : it->second) {
    if (rep.entry != 374) continue;
    ++holders;
    CHECK(Holds(rep.data, ColourBytes(1)));   // blue's bytes
    CHECK(!Holds(rep.data, ColourBytes(2)));  // NOT lilac's
  }
  CHECK(holders == 1);
  const std::vector<costumes::Extra> extras = TableExtras(s);
  CHECK(extras.size() == kSlotCount);
  CHECK(extras.size() > 2 && extras[2].body_tpr == 1288);
  const auto labels = EntryLabels(s, g.guest);
  const auto l = labels.find("avt.afs");
  CHECK(labels.size() == 1 && l != labels.end());
  if (l == labels.end()) return;
  CHECK(l->second.size() == 6);
  CHECK(l->second.count(374) && l->second.at(374) == "colour alpha_blue");
  CHECK(!l->second.count(375));
}

// Each slot's archive and entry come from FileLocation(tpr_file); built over the avt-shaped archive, each entry reads back
// its slot's bytes and every other entry is unchanged.
static void TestReplacementsByArchive() {
  World w;
  fs::remove(w.File(5));  // one stand-in among them
  FakeGuest g = PlantedGuest();
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(s.usable && s.notes.size() == 1);
  const auto r = Replacements(s, g.guest);
  const auto it = r.find("avt.afs");
  CHECK(r.size() == 1 && it != r.end());
  if (it == r.end()) return;
  CHECK(it->second.size() == kSlotCount);
  std::map<uint32_t, std::shared_ptr<const afs::Source>> by_entry;
  for (const afs::Replacement& rep : it->second) by_entry[rep.entry] = rep.data;
  for (size_t i = 0; i < kSlotCount; ++i) {
    const auto loc = costumes::FileLocation(g.guest, Slots()[i].tpr_file);
    CHECK(loc && std::string(loc->archive) == "avt.afs" && loc->entry == kProvenFirst + i);
    const auto e = by_entry.find(kProvenFirst + uint32_t(i));
    CHECK(e != by_entry.end() && Holds(e->second, i == 5 ? StandInFor(i) : ColourBytes(i)));
  }
  // The overlay over the avt-shaped archive (entry 0 at 0x1800).
  const std::vector<uint8_t> original = ReadAll(w.game / "avt.afs");
  CHECK(Get32(original, 8) == 0x1800);  // precondition
  std::string why;
  auto source = std::make_shared<afs::MemorySource>(original);
  const auto overlay = afs::Overlay::Build(source, it->second, &why);
  CHECK(overlay != nullptr);
  if (!overlay) {
    std::printf("  overlay: %s\n", why.c_str());
    return;
  }
  CHECK(overlay->entry_offset(0) == 0x1800);
  for (size_t i = 0; i < kSlotCount; ++i) {
    const std::vector<uint8_t> want = i == 5 ? StandInFor(i) : ColourBytes(i);
    const uint32_t e = kProvenFirst + uint32_t(i);
    std::vector<uint8_t> got(size_t(overlay->entry_size(e)));
    CHECK(overlay->entry_size(e) == want.size() && overlay->Read(overlay->entry_offset(e), got.data(), got.size()) &&
          got == want);
  }
  for (uint32_t e : {0u, 372u, 380u, 699u}) {
    const uint32_t off = Get32(original, 8 + 8 * size_t(e)), size = Get32(original, 12 + 8 * size_t(e));
    std::vector<uint8_t> got(size);
    CHECK(overlay->entry_size(e) == size && overlay->Read(overlay->entry_offset(e), got.data(), got.size()) &&
          std::vector<uint8_t>(original.begin() + off, original.begin() + off + size) == got);
  }
}

static void TestEntryLabels() {
  World w;
  FakeGuest g = PlantedGuest();
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  const auto labels = EntryLabels(s, g.guest);
  const auto l = labels.find("avt.afs");
  CHECK(labels.size() == 1 && l != labels.end());
  CHECK(l != labels.end() && l->second.size() == kSlotCount);
  for (size_t i = 0; i < kSlotCount; ++i) {
    const bool found = l != labels.end() && l->second.count(kProvenFirst + uint32_t(i)) == 1;
    CHECK(found && l->second.at(kProvenFirst + uint32_t(i)) == std::string("colour ") + kLedger[i].name);
  }
  // A stand-in slot keeps its label (another folder: `s` holds this one's files open).
  World w2;
  CHECK(fs::remove(w2.File(3)));
  const Status s2 = Load(TestSlots(), w2.colours, w2.game, g.guest);
  CHECK(s2.usable && s2.slots.size() == kSlotCount && s2.slots[3].stand_in);
  const auto labels2 = EntryLabels(s2, g.guest);
  const auto l2 = labels2.find("avt.afs");
  CHECK(l2 != labels2.end() && l2->second.count(376) && l2->second.at(376) == "colour spartan_h3_pink");
}

static uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
static bool RowIs(const float (&row)[4], const uint32_t (&want)[4]) {
  for (int i = 0; i < 4; ++i) {
    if (Bits(row[i]) != want[i]) return false;
  }
  return true;
}

// Task 2's ledgered launcher values, as big-endian float32 words (the ledger's hex column).
static void TestAlphaRows() {
  const AlphaRow v = AlphaVanilla();
  CHECK(RowIs(v.refract, {0x3F666666u, 0x3F800000u, 0x3F800000u, 0x3ECCCCCDu}));  // (0.9, 1, 1, 0.4)
  CHECK(RowIs(v.outline, {0x3DCCCCCDu, 0x3ECCCCCDu, 0x3E99999Au, 0x41200000u}));  // (0.1, 0.4, 0.3, 10)
  const auto red = AlphaRowFor(Slot::kAlphaRed);
  const auto blue = AlphaRowFor(Slot::kAlphaBlue);
  const auto lilac = AlphaRowFor(Slot::kAlphaLilac);
  CHECK(red && RowIs(red->refract, {0x3FB33333u, 0x3DCCCCCDu, 0x3EB33333u, 0x3ECCCCCDu}));
  CHECK(red && RowIs(red->outline, {0x3ECCCCCDu, 0x3CF5C28Fu, 0x3DCCCCCDu, 0x41200000u}));
  CHECK(blue && RowIs(blue->refract, {0x3E851EB8u, 0x3EDC28F6u, 0x40233333u, 0x3ECCCCCDu}));
  CHECK(blue && RowIs(blue->outline, {0x3D23D70Au, 0x3D8F5C29u, 0x3ECCCCCDu, 0x41200000u}));
  CHECK(lilac && RowIs(lilac->refract, {0x3F800000u, 0x3F19999Au, 0x4019999Au, 0x3ECCCCCDu}));
  CHECK(lilac && RowIs(lilac->outline, {0x3DE147AEu, 0x3D851EB8u, 0x3ECCCCCDu, 0x41200000u}));
  for (Slot sp : {Slot::kSpartanH3Pink, Slot::kSpartanH3Orchid, Slot::kSpartanH2Orange, Slot::kSpartanH2Cyan}) {
    CHECK(!AlphaRowFor(sp).has_value());
  }
}

static void TestSlotTableValues() {
  const SlotTable& t = Slots();
  std::set<std::string> names;
  for (size_t i = 0; i < kSlotCount; ++i) {
    const SlotDef& d = t[i];
    const LedgerRow& want = kLedger[i];
    CHECK(size_t(d.slot) == i);
    CHECK(d.name != nullptr && std::string(d.name) == want.name);
    CHECK(d.character == want.character);
    CHECK(d.replaces == want.replaces);
    CHECK(d.tpr_file == want.tpr_file);
    CHECK(d.cat_file == want.cat_file);
    CHECK(d.stand_in_file == want.stand_in_file);
    CHECK(d.size == want.size);
    CHECK(d.sha1 != nullptr && std::string(d.sha1) == want.sha1);
    // The controller's guard for costumes::Apply (which does no fit check on extras): Alpha or Spartan, and a costume
    // number or -1.
    CHECK(d.character == 16 || d.character == 18);
    CHECK(d.replaces >= -1);
    // A file number in Task 1B's proven set (files 1287-1293).
    CHECK(d.tpr_file >= kAvtFirstFile + kProvenFirst && d.tpr_file <= kAvtFirstFile + kProvenLast);
    if (d.name) names.insert(d.name);
  }
  CHECK(names.size() == kSlotCount);
  // F19: no slot's character has a costume box (CarrySuits matches a box by its list-pointer patch kCostumeList + 4c).
  std::set<int> box_characters;
  for (const doa4::tweaks::Tweak& tw : doa4::tweaks::AllTweaks()) {
    for (const doa4::tweaks::CostumeAddition& a : tw.additions) {
      box_characters.insert(a.character);
      CHECK(a.character != 16 && a.character != 18);
    }
    for (const doa4::tweaks::Patch& p : tw.patches) {
      CHECK(p.address != doa4::tweaks::kCostumeList + 4 * 16 && p.address != doa4::tweaks::kCostumeList + 4 * 18);
    }
  }
  // Positive control: the walk sees the three boxes (Helena, Kokoro, Christie), so an empty list cannot pass it.
  std::printf("  costume boxes for %zu characters\n", box_characters.size());
  CHECK(box_characters.size() == 3);
}

// On the image: where each file number lives, and that no record any character's costume list reaches names a slot's
// tpr file (a typo such as 427, Alpha's own entry 22, would be one).
static void TestSlotTableOnImage() {
  FakeGuest g = ImageGuest();
  const SlotTable& t = Slots();
  static const uint32_t kCatEntry[kSlotCount] = {23, 23, 23, 35, 41, 33, 43};
  static const uint32_t kStandInEntry[kSlotCount] = {22, 22, 22, 34, 40, 32, 42};
  for (size_t i = 0; i < kSlotCount; ++i) {
    const auto tpr = costumes::FileLocation(g.guest, t[i].tpr_file);
    const auto cat = costumes::FileLocation(g.guest, t[i].cat_file);
    const auto si = costumes::FileLocation(g.guest, t[i].stand_in_file);
    CHECK(tpr && std::string(tpr->archive) == "avt.afs" && tpr->entry >= kProvenFirst && tpr->entry <= kProvenLast);
    CHECK(tpr && tpr->entry == kProvenFirst + i);
    CHECK(cat && std::string(cat->archive) == "chrthree.afs" && cat->entry == kCatEntry[i]);
    CHECK(si && std::string(si->archive) == "chrthree.afs" && si->entry == kStandInEntry[i]);
  }
  // Every (character, list entry) -> model -> record; the four file words of each record reached.
  std::set<uint32_t> reached;
  size_t records = 0;
  for (uint32_t c = 0; c < costumes::kCharCount; ++c) {
    const uint32_t list = g.guest.ReadBe32(costumes::kListTable + 4 * c);
    const uint32_t len = g.guest.Read(costumes::kLenTable + c);
    const uint32_t block = g.guest.ReadBe32(costumes::kModelTable + 8 * c + 4);
    if (!list || !block) continue;
    for (uint32_t i = 0; i < len; ++i) {
      const uint32_t model = g.guest.Read(list + i);
      const uint32_t at = block + 8 * model;
      for (uint32_t w = 0; w < 4; ++w) {
        reached.insert(uint32_t(g.guest.Read(at + 2 * w)) << 8 | g.guest.Read(at + 2 * w + 1));
      }
      ++records;
    }
  }
  std::printf("  image: %zu list entries walked, %zu file numbers reached\n", records, reached.size());
  // Positive control: the walk reaches Alpha's own body tpr and each stand-in (so it can see a typo such as 427).
  CHECK(records > 100);
  for (size_t i = 0; i < kSlotCount; ++i) CHECK(reached.count(t[i].stand_in_file) == 1);
  for (size_t i = 0; i < kSlotCount; ++i) CHECK(reached.count(t[i].tpr_file) == 0);
}

static std::string ManifestOf(const std::vector<std::string>& lines, const char* eol) {
  std::string text;
  for (const std::string& l : lines) text += l + eol;
  return text;
}
static std::vector<std::string> GoodLines() {
  std::vector<std::string> lines;
  for (size_t i = 0; i < kSlotCount; ++i) lines.push_back(std::string(Slots()[i].name) + " " + Slots()[i].name + ".tpr");
  return lines;
}

// F13: every grain the manifest can be partial at, and the CRLF and BOM forms.
static void TestManifestGrains() {
  FakeGuest g = PlantedGuest();
  {
    World w;
    WriteText(w.colours / "manifest.txt", ManifestOf(GoodLines(), "\r\n"));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    PrintNotes("grains CRLF", s);
    CHECK(s.usable && s.notes.empty());
    CheckSlots(s, {});
  }
  {
    World w;
    WriteText(w.colours / "manifest.txt", "\xEF\xBB\xBF" + ManifestOf(GoodLines(), "\n"));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    PrintNotes("grains BOM", s);
    CHECK(s.usable && s.notes.empty());
    CheckSlots(s, {});
  }
  {
    // A BOM and CRLF together, a blank line and a comment.
    World w;
    std::vector<std::string> lines = GoodLines();
    lines.insert(lines.begin() + 3, "");
    lines.insert(lines.begin(), "# colour costumes");
    WriteText(w.colours / "manifest.txt", "\xEF\xBB\xBF" + ManifestOf(lines, "\r\n"));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable && s.notes.empty());
    CheckSlots(s, {});
  }
  {
    // A slot with no line.
    World w;
    std::vector<std::string> lines = GoodLines();
    lines.erase(lines.begin() + 5);
    WriteText(w.colours / "manifest.txt", ManifestOf(lines, "\n"));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    PrintNotes("grains no line", s);
    CHECK(s.usable && s.notes.size() == 1 && NotesNaming(s, "spartan_h2_orange") == 1);
    CheckSlots(s, {5});
  }
  {
    // A duplicate keeps the first: first good, second wrong -> from its file; first wrong, second good -> stand-in.
    World w;
    Write(w.colours / "other.tpr", ColourBytes(6));
    std::vector<std::string> lines = GoodLines();
    lines.push_back("alpha_red other.tpr");
    lines.insert(lines.begin(), "alpha_blue other.tpr");
    WriteText(w.colours / "manifest.txt", ManifestOf(lines, "\n"));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    PrintNotes("grains duplicate", s);
    // Notes: alpha_blue's own line repeated (ignored), alpha_red's second line (ignored), alpha_blue's file wrong.
    CHECK(s.usable && s.notes.size() == 3);
    CHECK(NotesNaming(s, "alpha_red") == 1);
    CHECK(NotesNaming(s, "alpha_blue") == 2);
    CheckSlots(s, {1});
  }
  {
    // An unknown slot, a malformed line (one word, three words): each ignored with one note; the slots load.
    World w;
    std::vector<std::string> lines = GoodLines();
    lines.push_back("kasumi_red kasumi.tpr");
    lines.push_back("lonely");
    lines.push_back("alpha_red alpha_red.tpr extra");
    WriteText(w.colours / "manifest.txt", ManifestOf(lines, "\n"));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    PrintNotes("grains unknown/malformed", s);
    CHECK(s.usable && s.notes.size() == 3);
    CHECK(NotesNaming(s, "kasumi_red") == 1);
    CHECK(NotesNaming(s, "lonely") == 1);
    CHECK(NotesNaming(s, "extra") == 1);
    CheckSlots(s, {});
  }
  {
    // File names holding a path: the slot gets its stand-in even where the path reaches the right file.
    World w;
    fs::create_directories(w.colours / "sub");
    Write(w.colours / "sub" / "alpha_red.tpr", ColourBytes(0));
    std::vector<std::string> lines = GoodLines();
    lines[0] = "alpha_red sub/alpha_red.tpr";
    lines[1] = "alpha_blue ../colour_costumes/alpha_blue.tpr";
    lines[2] = "alpha_lilac sub\\alpha_lilac.tpr";
    Write(w.colours / "sub" / "alpha_lilac.tpr", ColourBytes(2));
    lines[3] = "spartan_h3_pink c:spartan_h3_pink.tpr";
    lines[4] = "spartan_h3_orchid ..spartan_h3_orchid.tpr";
    Write(w.colours / "..spartan_h3_orchid.tpr", ColourBytes(4));
    WriteText(w.colours / "manifest.txt", ManifestOf(lines, "\n"));
    // Precondition: each path names a real file of the right bytes, so only the name rule can refuse it.
    CHECK(ReadAll(w.colours / "sub/alpha_red.tpr") == ColourBytes(0));
    CHECK(ReadAll(w.colours / "../colour_costumes/alpha_blue.tpr") == ColourBytes(1));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    PrintNotes("grains paths", s);
    CHECK(s.usable && s.notes.size() == 5);
    for (size_t i = 0; i < 5; ++i) CHECK(NotesNaming(s, Slots()[i].name) == 1);
    CheckSlots(s, {0, 1, 2, 3, 4});
  }
  {
    // Names that are not plain file names (letters, digits, '_', '.', '-'): a byte that is not UTF-8 (Load must not throw
    // on it), a UTF-8 letter outside ASCII (its file exists with the right bytes, so only the name rule refuses it), and
    // ".." inside a name the character rule alone would pass (its file exists too). Each slot: stand-in, one note.
    World w;
    const std::string e9(1, char(0xE9));
    const std::string c3a9 = std::string(1, char(0xC3)) + std::string(1, char(0xA9));
    std::vector<std::string> lines = GoodLines();
    lines[0] = "alpha_red caf" + e9 + ".tpr";
    lines[1] = "alpha_blue caf" + c3a9 + ".tpr";
    Write(w.colours / doa4::PathFromUtf8("caf" + c3a9 + ".tpr"), ColourBytes(1));
    lines[5] = "spartan_h2_orange spartan..h2_orange.tpr";
    Write(w.colours / "spartan..h2_orange.tpr", ColourBytes(5));
    WriteText(w.colours / "manifest.txt", ManifestOf(lines, "\n"));
    CHECK(ReadAll(w.colours / doa4::PathFromUtf8("caf" + c3a9 + ".tpr")) == ColourBytes(1));  // precondition
    CHECK(ReadAll(w.colours / "spartan..h2_orange.tpr") == ColourBytes(5));                     // precondition
    Status s;
    std::string threw;
    try {
      s = Load(TestSlots(), w.colours, w.game, g.guest);
    } catch (const std::exception& e) {
      threw = e.what();
    } catch (...) {
      threw = "(not a std::exception)";
    }
    if (!threw.empty()) std::printf("  grains names: Load threw: %s\n", threw.c_str());
    CHECK(threw.empty());
    PrintNotes("grains names", s);
    CHECK(s.usable && s.notes.size() == 3);
    CHECK(NotesNaming(s, "alpha_red") == 1);
    CHECK(NotesNaming(s, "alpha_blue") == 1);
    CHECK(NotesNaming(s, "spartan_h2_orange") == 1);
    CheckSlots(s, {0, 1, 5});
  }
}

// Spartan pink/orchid plan, Review Focus 1: a colour folder made for the OLD table (its manifest names spartan_h3_red and
// spartan_h3_blue, the two slots Pink and Orchid replaced) beside the new table. Those two lines are unknown slots (one
// note each, ignored), the two new slots have no line and show their stand-ins (the new bases' retail entries 34 and 40),
// the other five load. The old files hold bytes the new slots WOULD accept, so only the name rule refuses them; the
// folder holds no file under a new name (as the play copy's old folder). Mutants (task-3-report.md): an unknown
// "spartan_h3_" name falling to the first such slot, and a slot with no line taking the file on its own line number.
static void TestOldManifestBesideNewTable() {
  World w;
  FakeGuest g = PlantedGuest();
  CHECK(fs::remove(w.File(3)) && fs::remove(w.File(4)));  // no spartan_h3_pink.tpr / spartan_h3_orchid.tpr
  Write(w.colours / "spartan_h3_red.tpr", ColourBytes(3));
  Write(w.colours / "spartan_h3_blue.tpr", ColourBytes(4));
  // The old extract's manifest (ou_runs/colour_costumes/colours/manifest.txt), line for line.
  WriteText(w.colours / "manifest.txt",
            "alpha_red alpha_red.tpr\n"
            "alpha_blue alpha_blue.tpr\n"
            "alpha_lilac alpha_lilac.tpr\n"
            "spartan_h3_red spartan_h3_red.tpr\n"
            "spartan_h3_blue spartan_h3_blue.tpr\n"
            "spartan_h2_orange spartan_h2_orange.tpr\n"
            "spartan_h2_cyan spartan_h2_cyan.tpr\n");
  // Preconditions: the old files pass the new slots' size and SHA-1 (only the name can refuse them); the table's names.
  CHECK(Sha(ReadAll(w.colours / "spartan_h3_red.tpr")) == TestSlots()[3].sha1);
  CHECK(Sha(ReadAll(w.colours / "spartan_h3_blue.tpr")) == TestSlots()[4].sha1);
  CHECK(std::string(Slots()[3].name) == "spartan_h3_pink" && std::string(Slots()[4].name) == "spartan_h3_orchid");
  CHECK(!fs::exists(w.File(3)) && !fs::exists(w.File(4)));
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  PrintNotes("old manifest", s);
  CHECK(s.usable);
  CHECK(s.notes.size() == 4);
  CHECK(NoteAt(s, 0) == "manifest line 4 ignored (no colour slot is called spartan_h3_red)");
  CHECK(NoteAt(s, 1) == "manifest line 5 ignored (no colour slot is called spartan_h3_blue)");
  CHECK(NoteAt(s, 2) == "spartan_h3_pink: no manifest line; stand-in");
  CHECK(NoteAt(s, 3) == "spartan_h3_orchid: no manifest line; stand-in");
  CHECK(s.Summary() == "colour costumes: 7 slots, 5 from files, 2 stand-ins");
  CheckSlots(s, {3, 4});
  // The stand-ins are the new bases' retail entries (pink: COS. 3 = entry 34, orchid: COS. 6 = entry 40), not h3_red's
  // COS. 5 (entry 38).
  CHECK(s.slots.size() == kSlotCount && Holds(s.slots[3].tpr, StandInBytes(34)) && Holds(s.slots[4].tpr, StandInBytes(40)));
  CHECK(s.slots.size() == kSlotCount && !Holds(s.slots[3].tpr, StandInBytes(38)));
  // Every slot still gets its row and label: COS. 8 and 9 exist and draw their stand-ins (the new bases' cats).
  const std::vector<costumes::Extra> extras = TableExtras(s);
  CHECK(extras.size() == kSlotCount);
  CHECK(extras.size() == kSlotCount && extras[3].body_tpr == 1290 && extras[3].body_cat == 440 &&
        extras[3].label == "colour spartan_h3_pink");
  CHECK(extras.size() == kSlotCount && extras[4].body_tpr == 1291 && extras[4].body_cat == 446 &&
        extras[4].label == "colour spartan_h3_orchid");
  const auto labels = EntryLabels(s, g.guest);
  const auto l = labels.find("avt.afs");
  CHECK(l != labels.end() && l->second.count(376) && l->second.at(376) == "colour spartan_h3_pink");
  CHECK(l != labels.end() && l->second.count(377) && l->second.at(377) == "colour spartan_h3_orchid");
}

// A slot whose tpr_file has no place in the file table: its entry could never be overlaid, so nothing is installed (no
// record may name an entry that is not overlaid).
static void TestTprWithoutPlace() {
  World w;
  FakeGuest g = PlantedGuest();
  SlotTable t = TestSlots();
  t[3].tpr_file = 1700;  // past the file table (kFileCount 1640): FileLocation gives nothing
  CHECK(!costumes::FileLocation(g.guest, 1700).has_value());  // precondition
  const Status s = Load(t, w.colours, w.game, g.guest);
  PrintNotes("TestTprWithoutPlace", s);
  CHECK(!s.usable);
  CHECK(s.notes.size() == 1);
  CHECK(NotesNaming(s, "spartan_h3_pink") == 1);
  CHECK(TableExtras(s).empty());
  CHECK(Replacements(s, g.guest).empty());
  CHECK(EntryLabels(s, g.guest).empty());
  CHECK(Contains(s.Summary(), "not installed"));
  // Control: the same folder with the real file number installs.
  const Status ok = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(ok.usable && ok.notes.empty() && TableExtras(ok).size() == kSlotCount);
}

// ---- Task 5: start-up wiring ------------------------------------------------------------------------------------------

// F10: what SetUpSuits does with the overlays' outcome. Decide(usable, build_ok, install_ok, retry_ok): `usable` = the colour
// entries went into the first build; build_ok / install_ok = that build and its install; retry_ok = the build and install
// again without the colours (read only when the colours were in a first try that failed).
static void TestDecideRows() {
  using O = InstallOutcome;
  // F10's five rows.
  CHECK(Decide(true, true, true, false) == O::kInstalled);   // (usable, ok, ok): installed, extras passed
  CHECK(Decide(true, false, false, true) == O::kDropped);    // (usable, build fails): again without colours, suits kept
  CHECK(Decide(true, true, false, true) == O::kDropped);     // (usable, build ok, install fails): the same
  CHECK(Decide(true, false, false, false) == O::kFailed);    // (usable, fails, retry fails): today's suits_status_.Fail
  CHECK(Decide(false, true, true, false) == O::kNotUsable);  // (!usable): no colour entries, no extras, suits as today
  // Fields read only under a condition hold, with the condition off, the value that would change the outcome (#0759):
  // install_ok after a failed build, retry_ok after a first try that worked, retry_ok without colour entries.
  CHECK(Decide(true, false, true, true) == O::kDropped);
  CHECK(Decide(true, false, true, false) == O::kFailed);
  CHECK(Decide(true, true, true, true) == O::kInstalled);
  CHECK(Decide(true, true, false, false) == O::kFailed);
  CHECK(Decide(false, false, false, true) == O::kFailed);  // nothing to drop: a failure is today's Fail, whatever retry_ok
  CHECK(Decide(false, true, false, true) == O::kFailed);
  CHECK(Decide(false, false, true, true) == O::kFailed);
  CHECK(Decide(false, true, true, true) == O::kNotUsable);
  // All sixteen inputs: the colours count as installed only after a first try with them that worked.
  for (int bits = 0; bits < 16; ++bits) {
    const bool u = (bits & 8) != 0, b = (bits & 4) != 0, i = (bits & 2) != 0, r = (bits & 1) != 0;
    CHECK(Installed(Decide(u, b, i, r)) == (u && b && i));
  }
  CHECK(Installed(O::kInstalled) && !Installed(O::kDropped) && !Installed(O::kFailed) && !Installed(O::kNotUsable));
}

// "Colours not installed means no records" (Review Focus 2), first half: SetUpSuits' step 4 as F10 writes it
// (`std::vector<costumes::Extra> extras; if (installed) extras = TableExtras(colours);`), for every Decide input, on a
// usable Status whose rows exist: the rows go to Apply only after an install with them.
static void TestNotInstalledNoExtras() {
  World w;
  FakeGuest g = PlantedGuest();
  const Status colours = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(colours.usable && TableExtras(colours).size() == kSlotCount);  // precondition: there are rows to withhold
  size_t with = 0, without = 0;
  for (int bits = 0; bits < 16; ++bits) {
    const InstallOutcome o = Decide((bits & 8) != 0, (bits & 4) != 0, (bits & 2) != 0, (bits & 1) != 0);
    const bool installed = Installed(o);
    std::vector<costumes::Extra> extras;
    if (installed) extras = TableExtras(colours);
    CHECK(extras.size() == (o == InstallOutcome::kInstalled ? kSlotCount : 0));
    (extras.empty() ? without : with) += 1;
  }
  CHECK(with == 2 && without == 14);  // (u, b, i) all true: two inputs (retry_ok either way)
}

// Second half, on the image: Apply as SetUpSuits calls it, with one swimsuit (Kasumi, appended, so Apply runs whatever
// the colours do). Not installed: no record, list entry or Applied row for Alpha (16) or Spartan (18); installed: all seven.
static void TestNotInstalledNoRecords() {
  World w;
  FakeGuest planted = PlantedGuest();
  const Status colours = Load(TestSlots(), w.colours, w.game, planted.guest);
  CHECK(colours.usable);
  doa4::suits::Suit kasumi;
  kasumi.character = 5;
  kasumi.body_tpr = 271;
  kasumi.body_cat = 272;
  kasumi.design = "vks_test";
  for (const bool installed : {false, true}) {
    FakeGuest g = ImageGuest();
    const costumes::GuardState before = costumes::ReadGuardState(g.guest);
    CHECK(before.records[16] == 2 && before.records[18] == 8);  // precondition (the image)
    std::vector<costumes::Extra> extras;
    if (installed) extras = TableExtras(colours);
    const costumes::ApplyResult r = costumes::Apply(g.guest, g.alloc, extras, {kasumi});
    size_t rows_16_18 = 0, kasumi_rows = 0;
    for (const costumes::Applied& a : r.applied) {
      rows_16_18 += (a.character == 16 || a.character == 18) ? 1 : 0;
      kasumi_rows += a.character == 5 ? 1 : 0;
    }
    CHECK(kasumi_rows == 1);  // Apply ran
    CHECK(rows_16_18 == (installed ? kSlotCount : 0));
    CHECK(r.guard.records[16] == (installed ? 5u : 2u));
    CHECK(r.guard.records[18] == (installed ? 12u : 8u));
    // Not installed: Alpha's and Spartan's list words, counts and record blocks are the image's; installed: all moved.
    const FakeGuest image = ImageGuest();
    for (const uint32_t c : {16u, 18u}) {
      const doa4::costume_lists::Tables was = doa4::costume_lists::Read(image.guest, c);
      const doa4::costume_lists::Tables now = doa4::costume_lists::Read(g.guest, c);
      const uint32_t records_was = image.guest.ReadBe32(costumes::kModelTable + 8 * c + 4);
      const uint32_t records_now = g.guest.ReadBe32(costumes::kModelTable + 8 * c + 4);
      const bool same = was.list == now.list && was.variants == now.variants && was.length == now.length &&
                        was.maximum == now.maximum && records_was == records_now;
      CHECK(same == !installed);
    }
    CHECK(doa4::costume_lists::Read(g.guest, 5).maximum == doa4::costume_lists::Read(image.guest, 5).maximum + 1);
    CHECK(costumes::CountSuits(r.applied)[16] == (installed ? 2 : 0));
    CHECK(costumes::CountSuits(r.applied)[18] == (installed ? 4 : 0));
  }
}

// F19: the models CarrySuits gets, per character, in Apply's order: appended ones only (a replacing extra adds no costume,
// and passing it makes CarrySuits' "box state plus k" one too many).
static void TestAppendedModels() {
  std::vector<costumes::Applied> applied;
  const auto add = [&](uint8_t c, uint32_t model, uint32_t costume, const char* label, bool replaced) {
    costumes::Applied a;
    a.character = c;
    a.model = model;
    a.costume = costume;
    a.label = label;
    a.replaced = replaced;
    applied.push_back(a);
  };
  add(5, 7, 4, "vks_test c0", false);
  add(16, 2, 1, "colour alpha_red", true);
  add(16, 3, 2, "colour alpha_blue", false);
  add(16, 4, 3, "colour alpha_lilac", false);
  add(18, 8, 7, "colour spartan_h3_pink", false);
  add(18, 9, 8, "colour spartan_h3_orchid", false);
  const std::map<uint32_t, std::vector<uint8_t>> want = {{5, {7}}, {16, {3, 4}}, {18, {8, 9}}};
  CHECK(costumes::AppendedModels(applied) == want);
  // A character whose only extra replaces: no entry at all (CarrySuits is not called for her).
  CHECK(costumes::AppendedModels({applied[1]}).empty());
  CHECK(costumes::AppendedModels({}).empty());
  // The same count CountSuits gives the save (one rule for both).
  const costumes::SuitCounts counts = costumes::CountSuits(applied);
  for (const auto& [c, models] : costumes::AppendedModels(applied)) CHECK(models.size() == counts[c]);
}

// The same on the image, after Apply with the real slot table's rows: Alpha 3, 4 (red's model 2 replaced COS. 2), Spartan
// 8-11.
static void TestAppendedModelsOnImage() {
  World w;
  FakeGuest planted = PlantedGuest();
  const Status colours = Load(TestSlots(), w.colours, w.game, planted.guest);
  FakeGuest g = ImageGuest();
  const costumes::ApplyResult r = costumes::Apply(g.guest, g.alloc, TableExtras(colours), {});
  CHECK(r.applied.size() == kSlotCount && r.refused.empty());
  const std::map<uint32_t, std::vector<uint8_t>> want = {{16, {3, 4}}, {18, {8, 9, 10, 11}}};
  CHECK(costumes::AppendedModels(r.applied) == want);
}

static bool PrintableAscii(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return uint8_t(c) >= 0x20 && uint8_t(c) < 0x7F; });
}

// M3-5: SetUpSuits logs every note, one line each, so no note carries raw bytes: a manifest's unknown slot name and
// malformed line holding control bytes (ESC, BEL, a CR inside the line, NUL, DEL) and a byte past ASCII come out as \xNN;
// so does a folder name past ASCII.
static void TestNotesPrintable() {
  FakeGuest g = PlantedGuest();
  const std::string esc(1, char(0x1B)), bel(1, char(0x07)), cr(1, char(0x0D)), nul(1, char(0x00)), del(1, char(0x7F)),
      e9(1, char(0xE9));
  {
    World w;
    std::vector<std::string> lines = GoodLines();
    lines.push_back("kasumi" + bel + e9 + "_red kasumi.tpr");             // two words, unknown slot
    lines.push_back("alpha" + cr + "x alpha_red.tpr");                     // a CR inside the line: unknown slot
    lines.push_back("bad" + esc + "[31m line" + nul + " three" + del);  // three words: malformed
    WriteText(w.colours / "manifest.txt", ManifestOf(lines, "\n"));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable);
    CheckSlots(s, {});
    CHECK(s.notes.size() == 3);
    size_t printable = 0;
    for (const std::string& note : s.notes) printable += PrintableAscii(note) ? 1 : 0;
    CHECK(printable == s.notes.size());
    // Each byte is named, not dropped: the reader of the log can still see what the line held.
    CHECK(NotesNaming(s, "kasumi\\x07\\xE9_red") == 1);
    CHECK(NotesNaming(s, "alpha\\x0Dx") == 1);
    CHECK(NotesNaming(s, "bad\\x1B[31m line\\x00 three\\x7F") == 1);
  }
  {
    World w;
    const fs::path folder = w.root / doa4::PathFromUtf8("caf" + std::string(1, char(0xC3)) + std::string(1, char(0xA9)));
    const Status s = Load(TestSlots(), folder, w.game, g.guest);
    CHECK(s.usable && s.notes.size() == 1);
    CHECK(PrintableAscii(NoteAt(s, 0)) && Contains(NoteAt(s, 0), "caf\\xC3\\xA9"));
  }
}

// D1: the colour folder is <user data>/colour_costumes unless doa4_colour_costumes_dir names one; always absolute (the
// [COLOURS] line prints it for the run's first check, F17).
static void TestColourFolder() {
  const fs::path user = fs::path(L"C:\\Users\\someone\\Documents\\dead_or_alive_4");
  CHECK(ColourFolder(user, "") == user / "colour_costumes");
  CHECK(ColourFolder(user, "D:/colours") == fs::path("D:/colours"));
  CHECK(ColourFolder(user, "relative_here") == fs::absolute(fs::path("relative_here")));
  CHECK(ColourFolder(fs::path("rel_user"), "") == fs::absolute(fs::path("rel_user") / "colour_costumes"));
  CHECK(ColourFolder(user, "D:/colours").is_absolute() && ColourFolder(user, "").is_absolute());
  // UTF-8 in the override (a toml string) is read as UTF-8.
  const std::string utf8 = "D:/caf" + std::string(1, char(0xC3)) + std::string(1, char(0xA9));
  CHECK(ColourFolder(user, utf8) == doa4::PathFromUtf8(utf8));
  // An override that is not UTF-8 (a lone 0xE9) cannot name a folder: the default, and no throw (a throw here ends start-up).
  std::string threw;
  fs::path got;
  try {
    got = ColourFolder(user, "D:/caf" + std::string(1, char(0xE9)));
  } catch (...) {
    threw = "threw";
  }
  CHECK(threw.empty() && got == user / "colour_costumes");
}

static void TestSummaryOff() {
  const Status off;
  CHECK(off.Summary() == "colour costumes: off");
  CHECK(TableExtras(off).empty());
  FakeGuest g = PlantedGuest();
  CHECK(Replacements(off, g.guest).empty());
  CHECK(EntryLabels(off, g.guest).empty());
}

// ---- the stand-in size rule in Load -------------------------------------------------------------------------------------

// The synthetic game folder's chrthree.afs again, with some entries replaced.
static void WriteChrthree(const World& w, uint32_t count, const std::map<uint32_t, std::vector<uint8_t>>& overrides) {
  std::map<uint32_t, std::vector<uint8_t>> chr;
  for (const auto& [file, entry] : kChrthreeEntry) {
    if (entry < count) chr[entry] = StandInBytes(entry);
  }
  for (const auto& [entry, bytes] : overrides) chr[entry] = bytes;
  Write(w.game / "chrthree.afs", Archive(count, chr));
}

// A stand-in entry of another size than the slot's file is not usable (a slot record sized for the file would read past
// it or short of it). The wrong version this excludes: Load serving whatever the archive holds at that entry.
static void TestStandInWrongSizeNotUsable() {
  std::printf("-- TestStandInWrongSizeNotUsable\n");
  FakeGuest g = PlantedGuest();
  const uint64_t size = TestSlots()[3].size;  // spartan_h3_pink: stand-in file 439, entry 34
  std::vector<uint8_t> shorter = StandInBytes(34);
  shorter.pop_back();
  CHECK(shorter.size() + 1 == size);  // precondition: one byte short of the slot's size
  {
    World w;
    WriteChrthree(w, 48, {{34, shorter}});
    CHECK(fs::remove(w.File(3)));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    PrintNotes("TestStandInWrongSizeNotUsable", s);
    const std::string want = "its stand-in is " + std::to_string(size - 1) + " bytes, not " + std::to_string(size);
    CHECK(!s.usable);
    CHECK(NotesNaming(s, want) == 1);
    CHECK(s.notes.size() == 2 && Contains(NoteAt(s, 1), "spartan_h3_pink") && Contains(NoteAt(s, 1), "file 439") &&
          Contains(NoteAt(s, 1), want));
    CHECK(TableExtras(s).empty());
  }
  {
    // One byte longer: refused the same way (the rule is "differs", not "shorter").
    std::vector<uint8_t> longer = StandInBytes(34);
    longer.push_back(0);
    World w;
    WriteChrthree(w, 48, {{34, longer}});
    CHECK(fs::remove(w.File(3)));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(!s.usable && NotesNaming(s, "its stand-in is " + std::to_string(size + 1) + " bytes, not " + std::to_string(size)) == 1);
  }
  {
    // Control: the same archive with the slot's good file present: the stand-in is never read.
    World w;
    WriteChrthree(w, 48, {{34, shorter}});
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable && s.notes.empty());
    CheckSlots(s, {});
  }
  {
    // Control: the entry at the right size with other bytes: usable, the slot shows that stand-in.
    std::vector<uint8_t> other = StandInBytes(34);
    other[5] ^= 0x33;
    World w;
    WriteChrthree(w, 48, {{34, other}});
    CHECK(fs::remove(w.File(3)));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable);
    CHECK(s.slots.size() == kSlotCount && s.slots[3].stand_in && Holds(s.slots[3].tpr, other));
  }
}

// ---- making the missing files (doa4_colour_make.cpp) --------------------------------------------------------------------
// A slot's synthetic source is its stand-in entry (StandInFor). The generator and free_bytes below are fakes. The real
// generator (colourgen::Make) refuses a source it cannot read with a reason and is not expected to throw; the fake can
// refuse, throw, return wrong bytes or a short result, sleep, and act as another process between a slot's making and its
// write (`post`). The real free_bytes is std::filesystem::space; the fake returns what a test sets, or throws.

namespace cg = doa4::colourgen;
using Defs = std::array<MakeDef, kSlotCount>;

static const Defs& TestDefs() {
  static const Defs defs = [] {
    static std::vector<std::string> shas;
    shas.reserve(kSlotCount);
    Defs d{};
    for (size_t i = 0; i < kSlotCount; ++i) {
      const std::vector<uint8_t> source = StandInFor(i);
      shas.push_back(Sha(source));
      d[i] = MakeDef{Slots()[i].name, Slots()[i].stand_in_file, source.size(), shas.back().c_str()};
    }
    return d;
  }();
  return defs;
}

static size_t SlotIndex(std::string_view name) {
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (name == Slots()[i].name) return i;
  }
  return kSlotCount;
}
static std::string SlotName(size_t i) { return Slots()[i].name; }

using Made = std::optional<std::vector<uint8_t>>;
struct Gen {
  std::array<std::atomic<int>, kSlotCount> calls{};
  std::atomic<int> unknown{0};
  // Runs after the slot's bytes were computed (result set) or its source was refused (result empty, *why set).
  std::function<void(size_t slot, Made& result, std::string* why)> post;
  Made Run(std::string_view slot, std::span<const uint8_t> source, std::string* why) {
    const size_t i = SlotIndex(slot);
    if (i == kSlotCount) {
      unknown.fetch_add(1);
      if (why) *why = "unknown slot";
      return std::nullopt;
    }
    calls[i].fetch_add(1);
    const std::vector<uint8_t> want = StandInFor(i);
    Made result;
    if (source.size() == want.size() && std::equal(source.begin(), source.end(), want.begin())) {
      result = ColourBytes(i);
    } else if (why) {
      *why = "not this slot's source";
    }
    if (post) post(i, result, why);
    return result;
  }
  Generator Fn() {
    return [this](std::string_view slot, std::span<const uint8_t> source, std::string* why) { return Run(slot, source, why); };
  }
  int Calls(size_t i) const { return i < kSlotCount ? calls[i].load() : -1; }
  int Total() const {
    int n = unknown.load();
    for (const std::atomic<int>& c : calls) n += c.load();
    return n;
  }
};

static constexpr uint64_t kPlenty = uint64_t(1) << 40;
static MakeOptions Opt(Gen& gen, std::vector<size_t>* starts = nullptr) {
  static int counter = 0;
  MakeOptions o;
  o.generate = gen.Fn();
  o.free_bytes = [](const fs::path&) { return kPlenty; };
  o.temp_tag = "4242-" + std::to_string(++counter);
  if (starts) o.on_start = [starts](size_t n) { starts->push_back(n); };
  return o;
}
static size_t SlotNotes(const MakeReport& r);
// MakeMissing in a statement of its own. An exception that escapes it is a failed check, and so is a report whose
// notes do not hold exactly one note per slot not made (two checks per call).
static MakeReport RunMake(const World& w, FakeGuest& g, const MakeOptions& o, const SlotTable& t = TestSlots(),
                          const Defs& d = TestDefs()) {
  MakeReport r;
  bool escaped = false;
  try {
    r = MakeMissing(t, d, w.colours, w.game, g.guest, o);
  } catch (...) {
    escaped = true;
  }
  CHECK(!escaped);
  CHECK(SlotNotes(r) == r.not_made);  // one note names each slot that was not made, and no other slot
  return r;
}
static void PrintMake(const char* test, const MakeReport& r) {
  std::printf("  %s: good %zu made %zu not made %zu\n", test, r.good, r.made, r.not_made);
  for (const std::string& note : r.notes) std::printf("  %s note: %s\n", test, note.c_str());
}
static std::string RNote(const MakeReport& r, size_t i) { return i < r.notes.size() ? r.notes[i] : std::string(); }
static bool StartsWith(const std::string& text, const std::string& start) { return text.rfind(start, 0) == 0; }
// The cause of a slot's note (the text after "<slot>: "), or "" when no note starts with the slot's name.
static std::string Cause(const MakeReport& r, size_t slot) {
  const std::string start = SlotName(slot) + ": ";
  for (const std::string& note : r.notes) {
    if (StartsWith(note, start)) return note.substr(start.size());
  }
  return {};
}
static size_t SlotNotes(const MakeReport& r) {
  size_t n = 0;
  for (const std::string& note : r.notes) {
    for (size_t i = 0; i < kSlotCount; ++i) n += StartsWith(note, SlotName(i) + ": ") ? 1 : 0;
  }
  return n;
}

struct FileState {
  std::vector<uint8_t> bytes;
  fs::file_time_type time;
  bool operator==(const FileState&) const = default;
};
static std::map<std::string, FileState> Snapshot(const fs::path& dir) {
  std::map<std::string, FileState> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path p = it->path();
    std::error_code tec;
    out[doa4::Utf8(p.filename())] = FileState{ReadAll(p), fs::last_write_time(p, tec)};
  }
  return out;
}
static bool Kept(const std::map<std::string, FileState>& before, const std::map<std::string, FileState>& after,
                 const std::string& name) {
  const auto b = before.find(name), a = after.find(name);
  return b != before.end() && a != after.end() && b->second == a->second;
}
static size_t TmpCount(const fs::path& dir) {
  size_t n = 0;
  for (const auto& [name, state] : Snapshot(dir)) n += name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0 ? 1 : 0;
  return n;
}
static size_t GoodFiles(const World& w) {
  size_t n = 0;
  for (size_t i = 0; i < kSlotCount; ++i) n += ReadAll(w.File(i)) == ColourBytes(i) ? 1 : 0;
  return n;
}
static void SetAge(const fs::path& p, int minutes) {
  std::error_code ec;
  fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::minutes(minutes), ec);
  CHECK(!ec);
}
static std::string Text(const fs::path& p) {
  const std::vector<uint8_t> b = ReadAll(p);
  return std::string(b.begin(), b.end());
}
// manifest.txt as the writer must make it (*shape): one '#' line first, LF line ends, no CR, no other comment or empty
// line. Returns the lines after the comment.
static std::vector<std::string> ManifestBody(const fs::path& dir, bool* shape) {
  const std::string text = Text(dir / "manifest.txt");
  std::vector<std::string> lines;
  for (size_t start = 0; start < text.size();) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  *shape = !text.empty() && text.back() == '\n' && text.find('\r') == std::string::npos && !lines.empty() &&
           !lines[0].empty() && lines[0][0] == '#';
  if (!lines.empty()) lines.erase(lines.begin());
  for (const std::string& line : lines) {
    if (line.empty() || line[0] == '#') *shape = false;
  }
  return lines;
}
static std::vector<std::string> StdLines() {
  std::vector<std::string> lines;
  for (size_t i = 0; i < kSlotCount; ++i) lines.push_back(SlotName(i) + " " + SlotName(i) + ".tpr");
  return lines;
}
static void RemoveFiles(const World& w, std::initializer_list<size_t> slots) {
  size_t removed = 0;
  for (size_t i : slots) removed += fs::remove(w.File(i)) ? 1 : 0;
  CHECK(removed == slots.size());
}
static void RemoveAllFiles(const World& w) { RemoveFiles(w, {0, 1, 2, 3, 4, 5, 6}); }
static HANDLE Hold(const fs::path& p, DWORD share) {
  const HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(h != INVALID_HANDLE_VALUE);
  return h;
}
static void Release(HANDLE h) {
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void TestKindOf() {
  std::printf("-- TestKindOf\n");
  CHECK(KindOf(false, "") == ProcessKind::kLobby);
  CHECK(KindOf(false, "record") == ProcessKind::kMatch);
  CHECK(KindOf(false, "replay") == ProcessKind::kMatch);
  CHECK(KindOf(false, "hash") == ProcessKind::kMatch);
  // Any other DOA4_NP value is a match process too: it makes nothing and ignores vanilla mode, and np::RefuseRun does
  // nothing in it (np::Start starts no session for such a value: it is a plain offline game).
  CHECK(KindOf(false, "x") == ProcessKind::kMatch);
  CHECK(KindOf(true, "") == ProcessKind::kMatch);
  CHECK(KindOf(true, "record") == ProcessKind::kMatch);
  CHECK(KindOf(true, "replay") == ProcessKind::kMatch);
  CHECK(KindOf(true, "hash") == ProcessKind::kMatch);
  CHECK(KindOf(true, "x") == ProcessKind::kMatch);
}

static void TestShouldMake() {
  std::printf("-- TestShouldMake\n");
  CHECK(ShouldMake(ProcessKind::kLobby, false, false) == true);
  CHECK(ShouldMake(ProcessKind::kLobby, true, false) == false);
  CHECK(ShouldMake(ProcessKind::kLobby, false, true) == false);
  CHECK(ShouldMake(ProcessKind::kLobby, true, true) == false);
  CHECK(ShouldMake(ProcessKind::kMatch, false, false) == false);
  CHECK(ShouldMake(ProcessKind::kMatch, true, false) == false);
  CHECK(ShouldMake(ProcessKind::kMatch, false, true) == false);
  CHECK(ShouldMake(ProcessKind::kMatch, true, true) == false);
}

// ---- the decisions the app's wiring reads (colour generator plan Task 6) ------------------------------------------------
// Each call of the function under test is a statement of its own; each row's comment names the wrong version it excludes.

// The colour rows are in the costume tables of every process, except a lobby process in vanilla mode.
static void TestRowsOn() {
  std::printf("-- TestRowsOn\n");
  // Excludes: rows only in a match process (the kind read without the vanilla flag).
  { const bool on = RowsOn(ProcessKind::kLobby, false); CHECK(on == true); }
  // The one false row. Excludes: always on (vanilla mode ignored).
  { const bool on = RowsOn(ProcessKind::kLobby, true); CHECK(on == false); }
  // Excludes: off in a match process (the offline-only rule this replaces).
  { const bool on = RowsOn(ProcessKind::kMatch, false); CHECK(on == true); }
  // Excludes: vanilla mode alone switches the rows off (the kind not read): a vanilla-mode player's match process
  // would build other tables than the other PC's.
  { const bool on = RowsOn(ProcessKind::kMatch, true); CHECK(on == true); }
}

// A match process that did not install the colour entries must not run; a lobby process is never refused.
static void TestMustRefuse() {
  std::printf("-- TestMustRefuse\n");
  using O = InstallOutcome;
  // A lobby process, each outcome. Excludes: the outcome read without the kind (three rows), and "always".
  { const bool refuse = MustRefuse(ProcessKind::kLobby, O::kInstalled); CHECK(refuse == false); }
  { const bool refuse = MustRefuse(ProcessKind::kLobby, O::kDropped); CHECK(refuse == false); }
  { const bool refuse = MustRefuse(ProcessKind::kLobby, O::kNotUsable); CHECK(refuse == false); }
  { const bool refuse = MustRefuse(ProcessKind::kLobby, O::kFailed); CHECK(refuse == false); }
  // A match process. Excludes: the kind read without the outcome (every match process refused).
  { const bool refuse = MustRefuse(ProcessKind::kMatch, O::kInstalled); CHECK(refuse == false); }
  // Excludes: kDropped taken for installed (the swimsuits went in, the colour entries did not), and "never".
  { const bool refuse = MustRefuse(ProcessKind::kMatch, O::kDropped); CHECK(refuse == true); }
  // Excludes: only a failed install refuses (a Load that was not usable passes).
  { const bool refuse = MustRefuse(ProcessKind::kMatch, O::kNotUsable); CHECK(refuse == true); }
  // Excludes: only "the colours are missing" refuses (a failed install of everything passes).
  { const bool refuse = MustRefuse(ProcessKind::kMatch, O::kFailed); CHECK(refuse == true); }
}

// std::filesystem::space answers an error with every member at the largest value, not 0: a query that cannot be
// answered must not refuse the making ("no space"), and a full disk must.
static_assert(sizeof(std::uintmax_t) == sizeof(uint64_t) &&
                  static_cast<uint64_t>(static_cast<std::uintmax_t>(-1)) == UINT64_MAX,
              "here the sentinel uintmax_t(-1) IS UINT64_MAX: the sentinel row below cannot tell a version without the "
              "sentinel test from the right one");
static void TestFreeBytesFrom() {
  std::printf("-- TestFreeBytesFrom\n");
  const auto space = [](std::uintmax_t available) {
    fs::space_info info{};
    info.capacity = 5000;
    info.free = 4000;
    info.available = available;
    return info;
  };
  const std::error_code none;
  const std::error_code failed = std::make_error_code(std::errc::no_such_file_or_directory);
  CHECK(!none && bool(failed));  // precondition: the two codes are an answer and an error
  // An error with a number in the member. Excludes: the error code not read (1000 would be compared with the sizes).
  { const uint64_t got = FreeBytesFrom(space(1000), failed); CHECK(got == UINT64_MAX); }
  // An error with 0 in the member. Excludes: "0 free on error", which would report "no space" for every slot.
  { const uint64_t got = FreeBytesFrom(space(0), failed); CHECK(got == UINT64_MAX); }
  // No error, the sentinel. (See the static_assert: equal with or without the sentinel test on this platform.)
  { const uint64_t got = FreeBytesFrom(space(static_cast<std::uintmax_t>(-1)), none); CHECK(got == UINT64_MAX); }
  // No error, a full disk. Excludes: 0 taken for "unknown" (a full disk must refuse).
  { const uint64_t got = FreeBytesFrom(space(0), none); CHECK(got == 0); }
  // No error, a number. Excludes: `free` (4000) or `capacity` (5000) read in place of `available`.
  { const uint64_t got = FreeBytesFrom(space(123456), none); CHECK(got == 123456); }
}

// The one note that can appear for a moment when two copies of the game start together.
static void TestIsPassingNote() {
  std::printf("-- TestIsPassingNote\n");
  // The text, written here on its own (MakeMissing's note, pinned in TestManifestRereadFails too).
  CHECK(kManifestUnreadableNote == "manifest: cannot write: the current one cannot be read");
  // Excludes: never (every note a warning).
  { const bool passing = IsPassingNote(kManifestUnreadableNote); CHECK(passing == true); }
  // A refused write shares the first 24 bytes. Excludes: a match on "manifest: " or on "manifest: cannot write: ".
  { const bool passing = IsPassingNote("manifest: cannot write: access denied"); CHECK(passing == false); }
  // Excludes: a match on the start of the text (starts_with).
  { const bool passing = IsPassingNote(std::string(kManifestUnreadableNote) + " "); CHECK(passing == false); }
  // Excludes: a match on the end of the text.
  { const bool passing = IsPassingNote("x" + std::string(kManifestUnreadableNote)); CHECK(passing == false); }
  // The other manifest note, and a slot's note. Excludes: a match on "manifest: ", and "always".
  { const bool passing = IsPassingNote("manifest: alpha_lilac not listed"); CHECK(passing == false); }
  { const bool passing = IsPassingNote("alpha_red: source differs"); CHECK(passing == false); }
  { const bool passing = IsPassingNote(""); CHECK(passing == false); }
}

// What a refused match process says: the cause by outcome, then what repairs it.
static void TestRefusalMessage() {
  std::printf("-- TestRefusalMessage\n");
  using O = InstallOutcome;
  const std::string head = "colour costumes: this PC cannot build the 0.3.8 costume tables: ";
  const std::string tail = "; check the game files (chrthree.afs) and restart";
  // The remedy follows the outcome: a build or install that failed (kDropped, kFailed) can come from a swimsuit
  // archive as well as from a game file, so its remedy names both and no one file.
  const std::string tail_install = "; check the game files and the swimsuit import, then restart";
  // Load's notes in the order Load pushes them: why a stand-in was needed first, the cause of "not usable" last.
  const std::vector<std::string> notes = {"the colour folder is missing; every slot shows its stand-in",
                                          "alpha_red: its stand-in cannot be read; no colour slot is installed"};
  const std::vector<std::string> no_notes;
  // The whole text, literally. kNotUsable names the LAST note. Excludes: the first note (it says why a stand-in was
  // needed, not why none could be read), every note joined, the install error in the note's place, another wording
  // of the start or the end.
  {
    const std::string got = RefusalMessage(O::kNotUsable, false, notes, "an install error", "a retry error", false);
    CHECK(got == "colour costumes: this PC cannot build the 0.3.8 costume tables: alpha_red: its stand-in cannot be read; "
                 "no colour slot is installed; check the game files (chrthree.afs) and restart");
  }
  // kNotUsable with no note. Excludes: an empty cause, and a read of the last note of an empty list.
  {
    const std::string got = RefusalMessage(O::kNotUsable, false, no_notes, "an install error", "a retry error", false);
    CHECK(got == head + "the colour slots were not loaded" + tail);
  }
  // kDropped names the install error. Excludes: the note (kNotUsable's source), and the retry's error (the retry worked).
  // The whole text of the second remedy, literally. Excludes: one remedy for every outcome (chrthree.afs named for a
  // failure a swimsuit archive can cause).
  {
    const std::string got = RefusalMessage(O::kDropped, true, notes, "an install error", "a retry error", false);
    CHECK(got == head + "an install error" + tail_install);
    CHECK(got == "colour costumes: this PC cannot build the 0.3.8 costume tables: an install error; check the game files "
                 "and the swimsuit import, then restart");
  }
  // kDropped with no error text. Excludes: an empty cause.
  {
    const std::string got = RefusalMessage(O::kDropped, true, notes, "", "a retry error", false);
    CHECK(got == head + "the overlay install failed" + tail_install);
  }
  // kDropped with the usable flag off: the flag is read for kFailed only. Excludes: the colour cause put in front
  // whenever the flag is off, whatever the outcome.
  {
    const std::string got = RefusalMessage(O::kDropped, false, notes, "an install error", "a retry error", false);
    CHECK(got == head + "an install error" + tail_install);
  }
  // kFailed with the colour slots usable names both tries. Excludes: the first error alone, the retry's alone, the
  // two in the other order, and the colour cause put in front although the slots were usable.
  {
    const std::string got = RefusalMessage(O::kFailed, true, notes, "an install error", "a retry error", false);
    CHECK(got == head + "an install error / a retry error" + tail_install);
  }
  // kFailed, usable, with no first error text. Excludes: an empty first half (" / a retry error").
  {
    const std::string got = RefusalMessage(O::kFailed, true, notes, "", "a retry error", false);
    CHECK(got == head + "the overlay install failed / a retry error" + tail_install);
  }
  // kFailed, usable, with no retry text. Excludes: a cause that ends in " / " with nothing after it.
  {
    const std::string got = RefusalMessage(O::kFailed, true, notes, "an install error", "", false);
    CHECK(got == head + "an install error" + tail_install);
  }
  // kFailed with the colour slots NOT usable (Load was not usable and the swimsuits' install failed: there was no
  // second try): the colour cause as for kNotUsable, then the install error. A retry text is given here although
  // none exists then. Excludes: the usable flag not read ("an install error / a retry error", which never says the
  // colour slots could not be loaded), and the retry text appended.
  {
    const std::string got = RefusalMessage(O::kFailed, false, notes, "an install error", "a retry error", false);
    CHECK(got == head + "alpha_red: its stand-in cannot be read; no colour slot is installed / an install error" + tail_install);
  }
  // The same with no note, and with no error text. Excludes: an empty half on either side of " / ".
  {
    const std::string got = RefusalMessage(O::kFailed, false, no_notes, "an install error", "a retry error", false);
    CHECK(got == head + "the colour slots were not loaded / an install error" + tail_install);
  }
  {
    const std::string got = RefusalMessage(O::kFailed, false, notes, "", "", false);
    CHECK(got == head + "alpha_red: its stand-in cannot be read; no colour slot is installed / the overlay install failed" + tail_install);
  }
  // The test switch beats a real note, and every outcome's own cause, and keeps the first remedy whatever the
  // outcome. Excludes: the switch not read, and (the kFailed row) the outcome's remedy under the switch.
  {
    const std::string got = RefusalMessage(O::kNotUsable, false, notes, "an install error", "a retry error", true);
    CHECK(got == head + "test switch DOA4_COLOURS_TEST_UNUSABLE" + tail);
  }
  {
    const std::string got = RefusalMessage(O::kFailed, false, no_notes, "an install error", "a retry error", true);
    CHECK(got == head + "test switch DOA4_COLOURS_TEST_UNUSABLE" + tail);
  }
  // kInstalled has no cause (the app never asks for it). Excludes: another outcome's cause borrowed for it.
  {
    const std::string got = RefusalMessage(O::kInstalled, true, notes, "an install error", "a retry error", false);
    CHECK(got == head + tail);
  }
}

// The refusal's cause when the notes come from their producer: Load on a folder set that is not usable for one reason
// (a stand-in of the wrong size) and degraded for another (no colour folder at all). Load says the benign thing
// first and the cause last; the message must name the cause. Excludes: the first note taken for the cause.
static void TestRefusalNamesLoadsCause() {
  std::printf("-- TestRefusalNamesLoadsCause\n");
  FakeGuest g = PlantedGuest();
  const uint64_t size = TestSlots()[3].size;  // spartan_h3_pink: stand-in file 439, entry 34
  std::vector<uint8_t> shorter = StandInBytes(34);
  shorter.pop_back();
  World w;
  WriteChrthree(w, 48, {{34, shorter}});
  std::error_code ec;
  fs::remove_all(w.colours, ec);
  CHECK(!ec && !fs::exists(w.colours));  // precondition: there is no colour folder
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  PrintNotes("TestRefusalNamesLoadsCause", s);
  const std::string cause = "its stand-in is " + std::to_string(size - 1) + " bytes, not " + std::to_string(size);
  // preconditions: not usable; at least two notes; the first is the folder's, the last the stand-in's
  CHECK(!s.usable);
  CHECK(s.notes.size() >= 2 && Contains(NoteAt(s, 0), "is missing; every slot shows its stand-in") &&
        !Contains(NoteAt(s, 0), cause));
  CHECK(!s.notes.empty() && Contains(s.notes.back(), cause) && Contains(s.notes.back(), "no colour slot is installed"));
  const std::string got = RefusalMessage(InstallOutcome::kNotUsable, s.usable, s.notes, "", "", false);
  CHECK(Contains(got, cause));
  CHECK(!Contains(got, "is missing; every slot shows its stand-in"));
  CHECK(!s.notes.empty() &&
        got == "colour costumes: this PC cannot build the 0.3.8 costume tables: " + s.notes.back() +
                   "; check the game files (chrthree.afs) and restart");
}

// MakeDefs() against the real slot table and colourgen::kSources, and against this file's own ledger rows (kStandInLedger:
// written on their own, so a row copied wrongly into kSources and from there into MakeDefs shows).
static void TestMakeDefsCoverSlots() {
  std::printf("-- TestMakeDefsCoverSlots\n");
  const Defs& d = MakeDefs();
  const SlotTable& t = Slots();
  CHECK(std::size(cg::kSources) == kSlotCount);
  for (size_t i = 0; i < kSlotCount; ++i) {
    const cg::SourceDef& s = cg::kSources[i];
    CHECK(d[i].slot && std::string_view(d[i].slot) == t[i].name && std::string_view(d[i].slot) == s.slot);
    CHECK(d[i].source_file == t[i].stand_in_file && d[i].source_file == s.file);
    CHECK(d[i].source_size == s.size);
    CHECK(d[i].source_sha1 && std::string_view(d[i].source_sha1) == s.sha1);
    CHECK(d[i].source_file == kStandInLedger[i].file && d[i].source_size == kStandInLedger[i].size && d[i].source_sha1 &&
          std::string_view(d[i].source_sha1) == kStandInLedger[i].sha1);
  }
}

// Image tier: the five distinct source files of kSources are where kSources says, in the image's own file table.
static void TestSourcesOnImage() {
  std::printf("-- TestSourcesOnImage\n");
  FakeGuest g = ImageGuest();
  std::set<uint16_t> files;
  for (const cg::SourceDef& s : cg::kSources) {
    if (!files.insert(s.file).second) continue;
    const auto loc = costumes::FileLocation(g.guest, s.file);
    CHECK(loc && std::string(loc->archive) == "chrthree.afs" && loc->entry == s.afs_entry);
  }
  CHECK(files.size() == 5);
}

// M2: every slot good and named: nothing is touched. The start manifest is one the writer never produces (CR LF ends, no
// comment line, the slots in reverse order), so a rewrite cannot leave the same bytes.
static void TestAllGoodTouchesNothing() {
  std::printf("-- TestAllGoodTouchesNothing\n");
  World w;
  FakeGuest g = PlantedGuest();
  std::string manifest;
  for (size_t i = kSlotCount; i-- > 0;) manifest += SlotName(i) + " " + SlotName(i) + ".tpr\r\n";
  WriteText(w.colours / "manifest.txt", manifest);
  Write(w.colours / "alpha_red.tpr.x.tmp", Content(9, 40));
  SetAge(w.colours / "alpha_red.tpr.x.tmp", 120);  // old enough for the sweep, if the sweep ran
  {
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);  // precondition: this manifest names seven good files
    CHECK(s.usable && s.notes.empty());
    CheckSlots(s, {});
  }
  const std::map<std::string, FileState> before = Snapshot(w.colours);
  CHECK(before.size() == 9);
  Gen gen;
  std::vector<size_t> starts;
  const MakeOptions o = Opt(gen, &starts);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestAllGoodTouchesNothing", r);
  const std::map<std::string, FileState> after = Snapshot(w.colours);
  CHECK(r.good == 7 && r.made == 0 && r.not_made == 0);  // it looked: seven good
  CHECK(r.notes.empty());
  CHECK(after == before);  // the file list, every file's bytes, every write time
  CHECK(after.count("alpha_red.tpr.x.tmp") == 1);
  CHECK(Text(w.colours / "manifest.txt") == manifest);
  CHECK(gen.Total() == 0);
  CHECK(starts.empty());
}

static void TestMakesAllFromEmpty() {
  std::printf("-- TestMakesAllFromEmpty\n");
  FakeGuest g = PlantedGuest();
  {
    World w;
    fs::remove_all(w.colours);
    CHECK(!fs::exists(w.colours));
    Gen gen;
    std::vector<size_t> starts;
    const MakeOptions o = Opt(gen, &starts);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestMakesAllFromEmpty", r);
    CHECK(r.good == 0 && r.made == 7 && r.not_made == 0);
    CHECK(r.notes.empty());
    CHECK(starts.size() == 1 && starts[0] == 7);
    for (size_t i = 0; i < kSlotCount; ++i) {
      CHECK(ReadAll(w.File(i)) == ColourBytes(i));
      CHECK(gen.Calls(i) == 1);
    }
    CHECK(Snapshot(w.colours).size() == 8);  // seven files and the manifest
    CHECK(TmpCount(w.colours) == 0);
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape);
    CHECK(lines == StdLines());
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);  // the production reader of the manifest
    CHECK(s.usable && s.notes.empty());
    CheckSlots(s, {});
    CHECK(s.Summary() == "colour costumes: 7 slots, 7 from files, 0 stand-ins");
  }
  {
    // A temp_tag that is not "digits and '-' only" cannot take a write out of the folder.
    World w;
    fs::remove_all(w.colours);
    const std::map<std::string, FileState> game_before = Snapshot(w.game);
    Gen gen;
    MakeOptions o = Opt(gen);
    o.temp_tag = "..\\..\\x/9-1";
    const MakeReport r = RunMake(w, g, o);
    CHECK(r.good == 0 && r.made == 7 && r.not_made == 0);
    CHECK(Snapshot(w.root).size() == 2);  // game and colour_costumes: nothing beside them
    CHECK(Snapshot(w.game) == game_before);
    CHECK(Snapshot(w.colours).size() == 8 && TmpCount(w.colours) == 0);
  }
}

static void TestMakesOnlyMissing() {
  std::printf("-- TestMakesOnlyMissing\n");
  World w;
  FakeGuest g = PlantedGuest();
  RemoveFiles(w, {1, 5});
  const std::map<std::string, FileState> before = Snapshot(w.colours);
  Gen gen;
  std::vector<size_t> starts;
  const MakeOptions o = Opt(gen, &starts);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestMakesOnlyMissing", r);
  const std::map<std::string, FileState> after = Snapshot(w.colours);
  CHECK(r.good == 5 && r.made == 2 && r.not_made == 0);
  CHECK(r.notes.empty());
  CHECK(starts.size() == 1 && starts[0] == 2);
  for (size_t i = 0; i < kSlotCount; ++i) {
    const bool missing = i == 1 || i == 5;
    CHECK(gen.Calls(i) == (missing ? 1 : 0));
    if (!missing) CHECK(Kept(before, after, SlotName(i) + ".tpr"));  // bytes and write time
  }
  CHECK(ReadAll(w.File(1)) == ColourBytes(1) && ReadAll(w.File(5)) == ColourBytes(5));
  CHECK(TmpCount(w.colours) == 0);
  bool shape = false;
  const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
  CHECK(shape && lines == StdLines());
}

static void TestWrongFileRemade() {
  std::printf("-- TestWrongFileRemade\n");
  World w;
  FakeGuest g = PlantedGuest();
  std::vector<uint8_t> other = ColourBytes(2);  // the same size, other bytes
  other[other.size() / 2] ^= 0x5A;
  Write(w.File(2), other);
  std::vector<uint8_t> longer = ColourBytes(4);  // another size
  longer.push_back(0);
  Write(w.File(4), longer);
  Gen gen;
  std::vector<size_t> starts;
  const MakeOptions o = Opt(gen, &starts);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestWrongFileRemade", r);
  CHECK(r.good == 5 && r.made == 2 && r.not_made == 0);
  CHECK(r.notes.empty());
  CHECK(starts.size() == 1 && starts[0] == 2);
  CHECK(gen.Calls(2) == 1 && gen.Calls(4) == 1 && gen.Total() == 2);
  CHECK(ReadAll(w.File(2)) == ColourBytes(2));
  CHECK(ReadAll(w.File(4)) == ColourBytes(4));
  CHECK(TmpCount(w.colours) == 0);
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(s.usable && s.notes.empty());
  CheckSlots(s, {});
}

static void TestCrashLeftoverIgnored() {
  std::printf("-- TestCrashLeftoverIgnored\n");
  World w;
  FakeGuest g = PlantedGuest();
  RemoveFiles(w, {2});
  const std::vector<uint8_t> junk = Content(77, 50);
  const fs::path fresh = w.colours / "alpha_lilac.tpr.123-9.tmp";    // a sweep form, just written: stays
  const fs::path half = w.colours / "spartan_h2_cyan.tpr.5-5.tmp";   // a sweep form, 30 minutes old: stays
  const fs::path old_slot = w.colours / "alpha_blue.tpr.77-1.tmp";   // a sweep form, two hours old: deleted
  const fs::path old_manifest = w.colours / "manifest.txt.9-9.tmp";  // a sweep form, two hours old: deleted
  const fs::path old_notes = w.colours / "notes.tmp";                // two hours old, not a sweep form: stays
  const fs::path old_other = w.colours / "other.tpr.1.tmp";          // two hours old, not a slot's name: stays
  for (const fs::path& p : {fresh, half, old_slot, old_manifest, old_notes, old_other}) Write(p, junk);
  SetAge(half, 30);
  for (const fs::path& p : {old_slot, old_manifest, old_notes, old_other}) SetAge(p, 120);
  Gen gen;
  const MakeOptions o = Opt(gen);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestCrashLeftoverIgnored", r);
  CHECK(r.good == 6 && r.made == 1 && r.not_made == 0);
  CHECK(r.notes.empty());
  CHECK(gen.Calls(2) == 1 && gen.Total() == 1);  // the slot was made from its source,
  CHECK(ReadAll(w.File(2)) == ColourBytes(2));   // not taken from the leftover
  CHECK(ReadAll(fresh) == junk);
  CHECK(ReadAll(half) == junk);
  CHECK(!fs::exists(old_slot));
  CHECK(!fs::exists(old_manifest));
  CHECK(ReadAll(old_notes) == junk);
  CHECK(ReadAll(old_other) == junk);
}

// M1's second half: a good <slot>.tpr without a manifest line is adopted; only the manifest is owed.
static void TestFileWithoutLineAdopted() {
  std::printf("-- TestFileWithoutLineAdopted\n");
  World w;
  FakeGuest g = PlantedGuest();
  std::string manifest;
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (i != 4) manifest += SlotName(i) + " " + SlotName(i) + ".tpr\n";
  }
  WriteText(w.colours / "manifest.txt", manifest);
  const std::map<std::string, FileState> before = Snapshot(w.colours);
  Gen gen;
  std::vector<size_t> starts;
  const MakeOptions o = Opt(gen, &starts);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestFileWithoutLineAdopted", r);
  const std::map<std::string, FileState> after = Snapshot(w.colours);
  CHECK(r.good == 7 && r.made == 0 && r.not_made == 0);
  CHECK(r.notes.empty());
  CHECK(gen.Total() == 0);
  CHECK(starts.empty());
  bool shape = false;
  const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
  CHECK(shape && lines == StdLines());
  size_t kept = 0;
  for (size_t i = 0; i < kSlotCount; ++i) kept += Kept(before, after, SlotName(i) + ".tpr") ? 1 : 0;
  CHECK(kept == kSlotCount);
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(s.usable && s.notes.empty());
  CheckSlots(s, {});
}

static void TestSourceWrongHash() {
  std::printf("-- TestSourceWrongHash\n");
  World w;
  FakeGuest g = PlantedGuest();
  RemoveAllFiles(w);
  std::vector<uint8_t> other = StandInBytes(34);  // spartan_h3_pink's source: the right size, one byte changed
  other[5] ^= 0x33;
  WriteChrthree(w, 48, {{34, other}});
  CHECK(other.size() == TestDefs()[3].source_size);  // precondition: only the SHA-1 can tell
  Gen gen;
  const MakeOptions o = Opt(gen);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestSourceWrongHash", r);
  CHECK(r.good == 0 && r.made == 6 && r.not_made == 1);
  CHECK(r.notes.size() == 1 && Cause(r, 3) == "source differs");
  CHECK(gen.Calls(3) == 0);  // a source that differs is never passed to the generator
  CHECK(gen.Total() == 6);
  CHECK(!fs::exists(w.File(3)));
  CHECK(GoodFiles(w) == 6);
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(s.usable);
  CHECK(s.slots.size() == kSlotCount && s.slots[3].stand_in && Holds(s.slots[3].tpr, other));
  CHECK(s.Summary() == "colour costumes: 7 slots, 6 from files, 1 stand-ins");
}

static void TestSourceWrongSize() {
  std::printf("-- TestSourceWrongSize\n");
  FakeGuest g = PlantedGuest();
  {
    World w;
    RemoveAllFiles(w);
    std::vector<uint8_t> shorter = StandInBytes(34);
    shorter.pop_back();
    WriteChrthree(w, 48, {{34, shorter}});
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestSourceWrongSize", r);
    CHECK(r.good == 0 && r.made == 6 && r.not_made == 1);
    CHECK(r.notes.size() == 1 && Cause(r, 3) == "source differs");
    CHECK(gen.Calls(3) == 0 && gen.Total() == 6);
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(!s.usable);  // the slot has no file and its stand-in is of another size
    CHECK(NotesNaming(s, "its stand-in is") == 1);
  }
  {
    // The size is checked on its own, not only through the SHA-1: the right source against a def one byte larger.
    World w;
    RemoveAllFiles(w);
    Defs d = TestDefs();
    d[5].source_size += 1;
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o, TestSlots(), d);
    CHECK(r.good == 0 && r.made == 6 && r.not_made == 1 && Cause(r, 5) == "source differs");
    CHECK(gen.Calls(5) == 0 && gen.Total() == 6);
  }
}

static void TestSourceMissing() {
  std::printf("-- TestSourceMissing\n");
  FakeGuest g = PlantedGuest();
  {
    // The archive has 40 entries: orchid's source (entry 40) and cyan's (42) are past its end.
    World w(40);
    RemoveAllFiles(w);
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestSourceMissing (a)", r);
    CHECK(r.good == 0 && r.made == 5 && r.not_made == 2);
    CHECK(r.notes.size() == 2 && Cause(r, 4) == "source missing" && Cause(r, 6) == "source missing");
    CHECK(gen.Calls(4) == 0 && gen.Calls(6) == 0 && gen.Total() == 5);
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(!s.usable);
  }
  {
    // No chrthree.afs at all.
    World w(48, false);
    RemoveAllFiles(w);
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestSourceMissing (b)", r);
    CHECK(r.good == 0 && r.made == 0 && r.not_made == 7);
    size_t missing = 0;
    for (size_t i = 0; i < kSlotCount; ++i) missing += Cause(r, i) == "source missing" ? 1 : 0;
    CHECK(r.notes.size() == 7 && missing == 7);
    CHECK(gen.Total() == 0);
    CHECK(GoodFiles(w) == 0 && TmpCount(w.colours) == 0);
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(!s.usable);
  }
  {
    // A source file number with no place in the game's file table.
    World w;
    RemoveAllFiles(w);
    Defs d = TestDefs();
    d[2].source_file = 1700;
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o, TestSlots(), d);
    CHECK(r.good == 0 && r.made == 6 && r.not_made == 1 && Cause(r, 2) == "source missing");
    CHECK(gen.Calls(2) == 0 && gen.Total() == 6);
  }
}

static void TestMadeFileFailsHashNotWritten() {
  std::printf("-- TestMadeFileFailsHashNotWritten\n");
  FakeGuest g = PlantedGuest();
  {
    // The generator flips one byte of alpha_blue's file and returns spartan_h2_orange's one byte short.
    World w;
    RemoveAllFiles(w);
    Gen gen;
    gen.post = [](size_t i, Made& result, std::string*) {
      if (!result) return;
      if (i == 1) (*result)[result->size() / 2] ^= 1;
      if (i == 5) result->pop_back();
    };
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestMadeFileFailsHashNotWritten", r);
    CHECK(r.good == 0 && r.made == 5 && r.not_made == 2);
    CHECK(r.notes.size() == 2 && Cause(r, 1) == "made file fails its check" && Cause(r, 5) == "made file fails its check");
    CHECK(!fs::exists(w.File(1)) && !fs::exists(w.File(5)));
    CHECK(TmpCount(w.colours) == 0);
    CHECK(Snapshot(w.colours).size() == 6);  // five files and the manifest
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape && lines.size() == 5);
  }
  {
    // The size is checked on its own, not only through the SHA-1: the table's size one more than the made bytes.
    World w;
    SlotTable t = TestSlots();
    t[4].size += 1;
    const std::map<std::string, FileState> before = Snapshot(w.colours);
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o, t);
    PrintMake("TestMadeFileFailsHashNotWritten (size)", r);
    CHECK(r.good == 6 && r.made == 0 && r.not_made == 1);
    CHECK(Cause(r, 4) == "made file fails its check");
    CHECK(gen.Calls(4) == 1);  // it was made, then refused
    CHECK(Kept(before, Snapshot(w.colours), SlotName(4) + ".tpr"));
    CHECK(TmpCount(w.colours) == 0);
  }
}

static void TestGeneratorRefuses() {
  std::printf("-- TestGeneratorRefuses\n");
  World w;
  FakeGuest g = PlantedGuest();
  RemoveFiles(w, {0, 3});
  Gen gen;
  gen.post = [](size_t i, Made& result, std::string* why) {
    if (i != 3) return;
    result.reset();
    if (why) *why = "texture 11: format 20\x01" " not DXT1";  // a control byte: the note must carry it escaped
  };
  std::vector<size_t> starts;
  const MakeOptions o = Opt(gen, &starts);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestGeneratorRefuses", r);
  CHECK(r.good == 5 && r.made == 1 && r.not_made == 1);
  CHECK(r.notes.size() == 1 && Cause(r, 3) == "generator: texture 11: format 20\\x01 not DXT1");
  CHECK(starts.size() == 1 && starts[0] == 2);
  CHECK(!fs::exists(w.File(3)) && ReadAll(w.File(0)) == ColourBytes(0));
}

static void TestGeneratorThrows() {
  std::printf("-- TestGeneratorThrows\n");
  FakeGuest g = PlantedGuest();
  for (unsigned threads : {1u, 4u}) {
    World w;
    RemoveAllFiles(w);
    Gen gen;
    gen.post = [](size_t i, Made&, std::string*) {
      if (i == 2) throw std::runtime_error("boom 7");
      if (i == 5) throw 42;
    };
    MakeOptions o = Opt(gen);
    o.threads = threads;
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestGeneratorThrows", r);
    CHECK(r.good == 0 && r.made == 5 && r.not_made == 2);
    CHECK(r.notes.size() == 2 && Cause(r, 2) == "generator: boom 7");
    CHECK(StartsWith(Cause(r, 5), "generator: "));
    CHECK(!fs::exists(w.File(2)) && !fs::exists(w.File(5)));
    CHECK(ReadAll(w.File(6)) == ColourBytes(6));  // the slots after the throws are made
    CHECK(TmpCount(w.colours) == 0);
  }
}

static void TestNoSpace() {
  std::printf("-- TestNoSpace\n");
  FakeGuest g = PlantedGuest();
  const uint64_t size4 = TestSlots()[4].size;
  {
    // One byte under the bound: the file's size + 65,536 is needed.
    World w;
    RemoveFiles(w, {4});
    Gen gen;
    int asked = 0;
    fs::path asked_of;
    MakeOptions o = Opt(gen);
    o.free_bytes = [&](const fs::path& p) {
      ++asked;
      asked_of = p;
      return size4 + 65535;
    };
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestNoSpace (under)", r);
    CHECK(r.good == 6 && r.made == 0 && r.not_made == 1);
    CHECK(r.notes.size() == 1 && Cause(r, 4) == "no space");
    CHECK(gen.Total() == 0);
    CHECK(!fs::exists(w.File(4)));
    CHECK(asked == 1 && asked_of == w.colours);
  }
  {
    // Exactly the bound: made.
    World w;
    RemoveFiles(w, {4});
    Gen gen;
    MakeOptions o = Opt(gen);
    o.free_bytes = [&](const fs::path&) { return size4 + 65536; };
    const MakeReport r = RunMake(w, g, o);
    CHECK(r.good == 6 && r.made == 1 && r.not_made == 0);
    CHECK(r.notes.empty());
    CHECK(ReadAll(w.File(4)) == ColourBytes(4));
  }
  {
    // Cumulative: all seven missing and room for exactly the first two (each alone would fit).
    World w;
    RemoveAllFiles(w);
    Gen gen;
    int asked = 0;
    const uint64_t room = TestSlots()[0].size + TestSlots()[1].size + 65536;
    MakeOptions o = Opt(gen);
    o.free_bytes = [&](const fs::path&) {
      ++asked;
      return room;
    };
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestNoSpace (cumulative)", r);
    CHECK(r.good == 0 && r.made == 2 && r.not_made == 5);
    size_t refused = 0;
    for (size_t i = 2; i < kSlotCount; ++i) refused += Cause(r, i) == "no space" ? 1 : 0;
    CHECK(r.notes.size() == 5 && refused == 5);
    CHECK(gen.Calls(0) == 1 && gen.Calls(1) == 1 && gen.Total() == 2);
    CHECK(asked == 1);  // the free space is read once a call
    CHECK(ReadAll(w.File(0)) == ColourBytes(0) && ReadAll(w.File(1)) == ColourBytes(1));
    CHECK(GoodFiles(w) == 2);
  }
}

static void TestFolderIsAFile() {
  std::printf("-- TestFolderIsAFile\n");
  World w;
  FakeGuest g = PlantedGuest();
  fs::remove_all(w.colours);
  const std::vector<uint8_t> junk = Content(3, 20);
  Write(w.colours, junk);
  CHECK(fs::is_regular_file(w.colours));
  Gen gen;
  const MakeOptions o = Opt(gen);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestFolderIsAFile", r);
  CHECK(r.good == 0 && r.made == 0 && r.not_made == 7);
  size_t cannot = 0;
  for (size_t i = 0; i < kSlotCount; ++i) cannot += StartsWith(Cause(r, i), "cannot write: ") ? 1 : 0;
  CHECK(cannot == 7);
  CHECK(ReadAll(w.colours) == junk);
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(s.usable);
  CheckSlots(s, {0, 1, 2, 3, 4, 5, 6});
}

// The target is a WRONG file that another program holds open without delete sharing: the rename is refused, the slot is
// not made, the temporary is removed and the wrong file keeps its bytes. First with read sharing only (the brief's
// row), then with write sharing too: there a writer that wrote in place would get through, only the rename is refused.
static void TestTargetHeldOpen() {
  std::printf("-- TestTargetHeldOpen\n");
  FakeGuest g = PlantedGuest();
  for (DWORD share : {DWORD(FILE_SHARE_READ), DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE)}) {
    World w;
    std::vector<uint8_t> wrong = ColourBytes(3);
    wrong[7] ^= 0x40;
    Write(w.File(3), wrong);
    const HANDLE held = Hold(w.File(3), share);
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestTargetHeldOpen", r);
    CHECK(r.good == 6 && r.made == 0 && r.not_made == 1);
    CHECK(r.notes.size() == 1 && StartsWith(Cause(r, 3), "cannot write: "));
    CHECK(gen.Calls(3) == 1);  // it was made: only the write failed
    CHECK(TmpCount(w.colours) == 0);
    CHECK(ReadAll(w.File(3)) == wrong);
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape && lines.size() == 6);  // one line per slot good on disk
    Release(held);
  }
}

// While this call was making a slot, another process put the good file in place and holds it open: the rename is
// refused, the target passes its check, the slot counts as good.
static void TestRenameOntoHeldGoodFile() {
  std::printf("-- TestRenameOntoHeldGoodFile\n");
  World w;
  FakeGuest g = PlantedGuest();
  RemoveFiles(w, {3});
  HANDLE held = INVALID_HANDLE_VALUE;
  Gen gen;
  gen.post = [&](size_t i, Made& result, std::string*) {
    if (i != 3 || !result) return;
    Write(w.File(3), ColourBytes(3));
    held = CreateFileW(w.File(3).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
  };
  const MakeOptions o = Opt(gen);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestRenameOntoHeldGoodFile", r);
  CHECK(held != INVALID_HANDLE_VALUE);  // precondition: the target was held when the rename came
  CHECK(r.good == 7 && r.made == 0 && r.not_made == 0);
  CHECK(r.notes.empty());
  CHECK(gen.Calls(3) == 1);
  CHECK(TmpCount(w.colours) == 0);
  bool shape = false;
  const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
  CHECK(shape && lines == StdLines());
  Release(held);
}

static void TestManifestWriteFails() {
  std::printf("-- TestManifestWriteFails\n");
  World w;
  FakeGuest g = PlantedGuest();
  RemoveAllFiles(w);
  WriteText(w.colours / "manifest.txt", "# held\n");
  const HANDLE held = Hold(w.colours / "manifest.txt", FILE_SHARE_READ);  // no write, no delete: it cannot be replaced
  Gen gen;
  const MakeOptions o = Opt(gen);
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestManifestWriteFails", r);
  CHECK(r.good == 0 && r.made == 7 && r.not_made == 0);  // the made slots still count as made
  CHECK(r.notes.size() == 1 && StartsWith(RNote(r, 0), "manifest: cannot write: "));
  // WHICH refusal: the manifest could be read again (the hold shares reading) and the write was tried and refused, so
  // the note carries the system's text. Excludes: the re-read judged unreadable (kManifestUnreadableNote, the rule of
  // TestManifestRereadFails), which shares the prefix above and writes nothing either.
  CHECK(RNote(r, 0).size() > std::string("manifest: cannot write: ").size());
  CHECK(RNote(r, 0) != kManifestUnreadableNote);
  CHECK(Text(w.colours / "manifest.txt") == "# held\n");
  CHECK(TmpCount(w.colours) == 0);
  CHECK(GoodFiles(w) == 7);
  {
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);  // the manifest names nothing: Load is as before
    CHECK(s.usable);
    CheckSlots(s, {0, 1, 2, 3, 4, 5, 6});
  }
  Release(held);
  Gen gen2;
  std::vector<size_t> starts2;
  const MakeOptions o2 = Opt(gen2, &starts2);
  const MakeReport r2 = RunMake(w, g, o2);
  PrintMake("TestManifestWriteFails (second call)", r2);
  CHECK(r2.good == 7 && r2.made == 0 && r2.not_made == 0);  // all seven adopted
  CHECK(r2.notes.empty());
  CHECK(gen2.Total() == 0);
  CHECK(starts2.empty());
  bool shape = false;
  const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
  CHECK(shape && lines == StdLines());
  const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
  CHECK(s.usable && s.notes.empty());
  CheckSlots(s, {});
}

// Nothing escapes MakeMissing (RunMake's own check). A throwing free_bytes stops the call: seven "not attempted" notes
// and the thrown text; a throwing on_start does not stop it. Load afterwards is as it would have been.
static void TestNothingEscapes() {
  std::printf("-- TestNothingEscapes\n");
  FakeGuest g = PlantedGuest();
  {
    // free_bytes throws: the call stops there. Every slot not decided by then is "not attempted" (in the table's order),
    // and one note carries the thrown text.
    World w;
    RemoveAllFiles(w);
    Gen gen;
    MakeOptions o = Opt(gen);
    o.free_bytes = [](const fs::path&) -> uint64_t { throw std::runtime_error("space boom"); };
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestNothingEscapes (free_bytes)", r);
    std::vector<std::string> want;
    for (size_t i = 0; i < kSlotCount; ++i) want.push_back(SlotName(i) + ": not attempted: the call stopped first");
    want.push_back("stopped: space boom");
    CHECK(r.good == 0 && r.made == 0 && r.not_made == 7);
    CHECK(r.notes == want);
    CHECK(gen.Total() == 0);
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable && s.slots.size() == kSlotCount);
  }
  {
    // The same stop with five slots good on disk and two missing: the five stay good, and only the two undecided
    // slots are "not attempted", in the table's order. Excludes: a stop that forgets what was already decided (seven
    // not made, or good 0), a note for a good slot, and the two notes in another order.
    World w;
    RemoveFiles(w, {1, 5});
    Gen gen;
    MakeOptions o = Opt(gen);
    o.free_bytes = [](const fs::path&) -> uint64_t { throw std::runtime_error("space boom"); };
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestNothingEscapes (free_bytes, five good)", r);
    const std::vector<std::string> want = {SlotName(1) + ": not attempted: the call stopped first",
                                           SlotName(5) + ": not attempted: the call stopped first", "stopped: space boom"};
    CHECK(r.good == 5 && r.made == 0 && r.not_made == 2);
    CHECK(r.notes == want);
    CHECK(gen.Total() == 0);
    CHECK(GoodFiles(w) == 5 && !fs::exists(w.File(1)) && !fs::exists(w.File(5)));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable);
    CheckSlots(s, {1, 5});
  }
  {
    // on_start is a notification: what it throws becomes one note and the making goes on.
    World w;
    RemoveAllFiles(w);
    Gen gen;
    MakeOptions o = Opt(gen);
    o.on_start = [](size_t) { throw std::runtime_error("start boom"); };
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestNothingEscapes (on_start)", r);
    CHECK(r.good == 0 && r.made == 7 && r.not_made == 0);
    CHECK(r.notes == std::vector<std::string>{"on_start: start boom"});
    CHECK(GoodFiles(w) == 7);
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable && s.slots.size() == kSlotCount && s.notes.empty());
  }
  {
    World w;
    RemoveAllFiles(w);
    Gen gen;
    MakeOptions o = Opt(gen);
    o.on_start = [](size_t) { throw 7; };  // not a std::exception: no text to carry
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestNothingEscapes (on_start, an int)", r);
    CHECK(r.good == 0 && r.made == 7 && r.not_made == 0);
    CHECK(r.notes.size() == 1 && StartsWith(RNote(r, 0), "on_start: "));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable && s.slots.size() == kSlotCount && s.notes.empty());
  }
}

// R1: the manifest that is there cannot be read at the moment it is to be replaced (another program holds it): it is
// not replaced. Excluded wrong version: a manifest written from "no lines", which drops the line of a good file kept
// under another name. The hold is taken inside the generator, so the first read of the call still worked. First with
// no sharing at all, then with delete sharing only. Measured on Windows 11: either hold also refuses the replacement
// itself, so here the unchanged-bytes checks hold for the wrong version too; what separates the two is the note (the
// wrong version tries the write and reports the system's refusal). The wrong version loses lines only when the hold
// ends between its read and its rename, which no fixture here can place.
static void TestManifestRereadFails() {
  std::printf("-- TestManifestRereadFails\n");
  FakeGuest g = PlantedGuest();
  std::vector<std::string> start = StdLines();
  start[0] = "alpha_red custom_red.tpr";
  const std::string start_text = ManifestOf(start, "\n");
  const auto arrange = [&start_text](const World& w) {
    fs::rename(w.File(0), w.colours / "custom_red.tpr");
    RemoveFiles(w, {1, 5});
    WriteText(w.colours / "manifest.txt", start_text);
  };
  for (DWORD share : {DWORD(0), DWORD(FILE_SHARE_DELETE)}) {
    World w;
    arrange(w);
    HANDLE held = INVALID_HANDLE_VALUE;
    Gen gen;
    gen.post = [&](size_t, Made&, std::string*) {
      if (held != INVALID_HANDLE_VALUE) return;
      held = CreateFileW((w.colours / "manifest.txt").c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestManifestRereadFails", r);
    CHECK(held != INVALID_HANDLE_VALUE);  // precondition: the manifest was held when it was read again
    Release(held);
    CHECK(r.good == 5 && r.made == 2 && r.not_made == 0);  // the two made files still count as made
    CHECK(r.notes == std::vector<std::string>{"manifest: cannot write: the current one cannot be read"});
    CHECK(Text(w.colours / "manifest.txt") == start_text);
    CHECK(Contains(Text(w.colours / "manifest.txt"), "alpha_red custom_red.tpr\n"));
    CHECK(ReadAll(w.File(1)) == ColourBytes(1) && ReadAll(w.File(5)) == ColourBytes(5));
    CHECK(TmpCount(w.colours) == 0);
  }
  {
    // Control: the same world without the hold: a manifest of seven lines that still names custom_red.tpr.
    World w;
    arrange(w);
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    CHECK(r.good == 5 && r.made == 2 && r.not_made == 0 && r.notes.empty());
    std::vector<std::string> want = StdLines();
    want[0] = "alpha_red custom_red.tpr";
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape && lines == want);
  }
}

// R2: a slot that counts as good or made, but has no file that passes when the manifest is written, gets no line and
// one note that says so; the counts stay. Here a slot that was good at the start loses its file while another slot is
// being made. Control: without the loss, no such note.
static void TestGoodSlotLosesItsFile() {
  std::printf("-- TestGoodSlotLosesItsFile\n");
  FakeGuest g = PlantedGuest();
  for (bool lose : {true, false}) {
    World w;
    RemoveFiles(w, {4});
    Gen gen;
    gen.post = [&](size_t i, Made&, std::string*) {
      if (!lose || i != 4) return;
      std::error_code ec;
      fs::remove(w.File(2), ec);
    };
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestGoodSlotLosesItsFile", r);
    std::vector<std::string> want_notes;
    std::vector<std::string> want_lines = StdLines();
    if (lose) {
      want_notes.push_back("manifest: alpha_lilac not listed");
      want_lines.erase(want_lines.begin() + 2);
    }
    CHECK(fs::exists(w.File(2)) == !lose);                 // precondition: the file went only in the first pass
    CHECK(r.good == 6 && r.made == 1 && r.not_made == 0);  // the counts are as they were
    CHECK(r.notes == want_notes);
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape && lines == want_lines);
  }
  {
    // The same rule for a slot MADE in this call, and two such notes in the table's order. Nothing a caller supplies
    // runs between a slot's write and the manifest, so the loss is produced by the slot table: a table (one the game
    // never has) in which slots 5 and 6 carry the file names of slots 3 and 4. Each of the four is made and written
    // in the table's order, so 5's bytes replace 3's file and 6's replace 4's: when the manifest is written, 3 and 4
    // count as made and their file no longer passes. Excludes: the note only for a slot that was good at the start
    // (the "made" half of the condition dropped), and the two notes in another order (by name, orchid comes first).
    World w;
    RemoveFiles(w, {3, 4, 5, 6});
    CHECK(fs::remove(w.colours / "manifest.txt"));  // no line for any slot: each is judged by <slot>.tpr alone
    SlotTable table = TestSlots();
    table[5].name = table[3].name;
    table[6].name = table[4].name;
    std::array<int, kSlotCount> made{};
    Gen unused;
    MakeOptions o = Opt(unused);
    o.generate = [&made](std::string_view, std::span<const uint8_t> source, std::string* why) -> Made {
      for (size_t i = 3; i < kSlotCount; ++i) {  // the four sources differ in size: the source tells the slot
        const std::vector<uint8_t> want = StandInFor(i);
        if (source.size() == want.size() && std::equal(source.begin(), source.end(), want.begin())) {
          ++made[i];
          return ColourBytes(i);
        }
      }
      if (why) *why = "not a source of this test";
      return std::nullopt;
    };
    const MakeReport r = RunMake(w, g, o, table);
    PrintMake("TestGoodSlotLosesItsFile (made slots)", r);
    CHECK((made == std::array<int, kSlotCount>{0, 0, 0, 1, 1, 1, 1}));  // precondition: each of the four was made once
    // precondition: the shared files hold the later slots' bytes, which the earlier slots' checks refuse
    CHECK(ReadAll(w.File(3)) == ColourBytes(5) && ReadAll(w.File(4)) == ColourBytes(6));
    CHECK(ColourBytes(5).size() != ColourBytes(3).size() && ColourBytes(6).size() != ColourBytes(4).size());
    CHECK(r.good == 3 && r.made == 4 && r.not_made == 0);  // the counts are as they were when each was written
    const std::vector<std::string> want_notes = {"manifest: " + SlotName(3) + " not listed",
                                                 "manifest: " + SlotName(4) + " not listed"};
    CHECK(r.notes == want_notes);
    CHECK(SlotName(4) < SlotName(3));  // precondition: the table's order is not the names' order
    const std::vector<std::string> want_lines = {SlotName(0) + " " + SlotName(0) + ".tpr", SlotName(1) + " " + SlotName(1) + ".tpr",
                                                 SlotName(2) + " " + SlotName(2) + ".tpr", SlotName(3) + " " + SlotName(3) + ".tpr",
                                                 SlotName(4) + " " + SlotName(4) + ".tpr"};
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape && lines == want_lines);  // the two shared names are listed once each, for slots 5 and 6
  }
}

// R3: a slot whose usable line names a failing file is still good when <slot>.tpr passes: only the manifest is owed.
// One line names a file with other bytes, one a file that is not there. With no free space at all the result is the
// same: nothing is made, so nothing needs space.
static void TestLineNamesFailingFileButDefaultIsGood() {
  std::printf("-- TestLineNamesFailingFileButDefaultIsGood\n");
  FakeGuest g = PlantedGuest();
  for (bool no_space : {false, true}) {
    World w;
    std::vector<uint8_t> bad = ColourBytes(1);
    bad[3] ^= 0x11;
    Write(w.colours / "bad.tpr", bad);
    std::vector<std::string> start = StdLines();
    start[1] = "alpha_blue bad.tpr";
    start[5] = "spartan_h2_orange gone.tpr";
    WriteText(w.colours / "manifest.txt", ManifestOf(start, "\n"));
    const std::map<std::string, FileState> before = Snapshot(w.colours);
    Gen gen;
    std::vector<size_t> starts;
    MakeOptions o = Opt(gen, &starts);
    if (no_space) o.free_bytes = [](const fs::path&) { return uint64_t(0); };
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestLineNamesFailingFileButDefaultIsGood", r);
    const std::map<std::string, FileState> after = Snapshot(w.colours);
    CHECK(r.good == 7 && r.made == 0 && r.not_made == 0);
    CHECK(r.notes.empty());
    CHECK(gen.Total() == 0);
    CHECK(starts.empty());
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape && lines == StdLines());  // both lines now name <slot>.tpr
    size_t kept = 0;
    for (size_t i = 0; i < kSlotCount; ++i) kept += Kept(before, after, SlotName(i) + ".tpr") ? 1 : 0;
    CHECK(kept == kSlotCount);  // no file was written again
  }
}

// The temporary file's name carries the call's temp_tag: a DIRECTORY planted at exactly that name stops that slot's
// write (and stays, with its file); a call with another tag makes the slot. Excluded wrong version: a temporary name
// that ignores temp_tag.
static void TestTempTagNamesTheTemporary() {
  std::printf("-- TestTempTagNamesTheTemporary\n");
  World w;
  FakeGuest g = PlantedGuest();
  RemoveFiles(w, {1, 4});
  const fs::path planted = w.colours / "alpha_blue.tpr.31-7.tmp";
  const std::vector<uint8_t> junk = Content(5, 30);
  fs::create_directories(planted);
  Write(planted / "keep.bin", junk);
  CHECK(fs::is_directory(planted) && ReadAll(planted / "keep.bin") == junk);
  Gen gen;
  MakeOptions o = Opt(gen);
  o.temp_tag = "31-7";
  const MakeReport r = RunMake(w, g, o);
  PrintMake("TestTempTagNamesTheTemporary", r);
  CHECK(r.good == 5 && r.made == 1 && r.not_made == 1);
  CHECK(r.notes.size() == 1 && StartsWith(Cause(r, 1), "cannot write: "));
  CHECK(gen.Calls(1) == 1);  // it was made: only its temporary could not be written
  CHECK(!fs::exists(w.File(1)));
  CHECK(fs::is_directory(planted) && ReadAll(planted / "keep.bin") == junk);
  CHECK(ReadAll(w.File(4)) == ColourBytes(4));
  Gen gen2;
  MakeOptions o2 = Opt(gen2);
  o2.temp_tag = "31-8";
  const MakeReport r2 = RunMake(w, g, o2);
  PrintMake("TestTempTagNamesTheTemporary (another tag)", r2);
  CHECK(r2.good == 6 && r2.made == 1 && r2.not_made == 0 && r2.notes.empty());
  CHECK(ReadAll(w.File(1)) == ColourBytes(1));
  CHECK(fs::is_directory(planted) && ReadAll(planted / "keep.bin") == junk);
}

// Two makers (two processes at one lobby start) on one folder, each with its own temp_tag. Both are held inside their
// generators until each has entered all seven (so both saw the folder empty and both write every file); the wait gives
// up after 10 s, which fails a check.
static void TestTwoMakersOneFolder() {
  std::printf("-- TestTwoMakersOneFolder\n");
  World w;
  FakeGuest guests[2] = {PlantedGuest(), PlantedGuest()};
  RemoveAllFiles(w);
  CHECK(fs::remove(w.colours / "manifest.txt"));
  struct Barrier {
    std::mutex m;
    std::condition_variable cv;
    int entered[2] = {0, 0};
    bool expired = false;
  } b;
  Gen gens[2];
  MakeOptions opts[2];
  for (int m = 0; m < 2; ++m) {
    gens[m].post = [&b, m](size_t, Made&, std::string*) {
      std::unique_lock<std::mutex> lock(b.m);
      ++b.entered[m];
      b.cv.notify_all();
      const bool met = b.cv.wait_for(lock, std::chrono::seconds(10), [&b] {
        return (b.entered[0] >= int(kSlotCount) && b.entered[1] >= int(kSlotCount)) || b.expired;
      });
      if (!met) {
        b.expired = true;
        b.cv.notify_all();
      }
    };
    opts[m] = Opt(gens[m]);
    opts[m].threads = unsigned(kSlotCount);
    opts[m].temp_tag = m ? "22-2" : "11-1";
  }
  const SlotTable& slots = TestSlots();
  const Defs& defs = TestDefs();
  MakeReport reports[2];
  bool escaped[2] = {false, false};
  auto run = [&](int m) {
    try {
      reports[m] = MakeMissing(slots, defs, w.colours, w.game, guests[m].guest, opts[m]);
    } catch (...) {
      escaped[m] = true;
    }
  };
  std::thread other(run, 1);
  run(0);
  other.join();
  PrintMake("TestTwoMakersOneFolder (0)", reports[0]);
  PrintMake("TestTwoMakersOneFolder (1)", reports[1]);
  CHECK(!escaped[0] && !escaped[1]);
  CHECK(!b.expired);
  CHECK(b.entered[0] == int(kSlotCount) && b.entered[1] == int(kSlotCount));
  // Per maker, the designed outcome of two copies writing one folder (the design's degrade row "cannot write: ...
  // file held by another running copy: stand-in this run; made at a later start; one log line"): every slot is
  // good, made or not made; a slot may be not made ONLY because its write was refused while the other maker held
  // the file, and then exactly one note says so, "<slot>: cannot write: <the system's text>". The manifest notes
  // are allowed too (any note that starts with "manifest: "): two makers meet at the manifest as well, and by design
  // one of them may find it unreadable for a moment (kManifestUnreadableNote), have its write refused, or leave a
  // slot unlisted. Every other note fails: another cause for a slot (nothing here is short of space or of a
  // source), a second note for one slot, a note of neither form. "No slot not made" is not asserted: it holds only
  // by timing (a refused rename whose re-check is refused too is rare, and designed).
  for (int m = 0; m < 2; ++m) {
    CHECK(gens[m].Total() == int(kSlotCount));
    CHECK(reports[m].good + reports[m].made + reports[m].not_made == kSlotCount);
    size_t refused_writes = 0, manifest_notes = 0, other_notes = 0;
    for (const std::string& note : reports[m].notes) {
      bool refused_write = false;
      for (size_t i = 0; i < kSlotCount; ++i) {
        const std::string start = SlotName(i) + ": cannot write: ";
        refused_write = refused_write || (StartsWith(note, start) && note.size() > start.size());
      }
      (refused_write ? refused_writes : StartsWith(note, "manifest: ") ? manifest_notes : other_notes) += 1;
    }
    // Excludes: a slot under contention reported with another cause ("no space", ...), and a note of neither form.
    CHECK(other_notes == 0);
    CHECK(refused_writes == reports[m].not_made);  // one such note per slot not made; excludes a second for one slot
    (void)manifest_notes;  // allowed in any number: counted only to say what the third class is not
    CHECK(SlotNotes(reports[m]) == reports[m].not_made);  // RunMake's check, for these calls made outside it
  }
  // The folder's outcome is strict whatever each maker reported: seven good files, no temporary, the standard lines.
  CHECK(GoodFiles(w) == 7);
  CHECK(TmpCount(w.colours) == 0);
  bool shape = false;
  const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
  CHECK(shape && lines == StdLines());
}

// M6: four workers give the files, counts and notes of one. Slot 3's source differs; the generator refuses slots 0 and
// 2; the space is enough for slots 0 to 4 (so 1 and 4 are made, 5 and 6 are not). The generator sleeps (7 - i) x 20 ms
// for slot i, so with four workers slot 2's refusal is known before slot 0's: notes kept in the order they arise would
// differ from one worker's. (The sleeps only order the finishes; no time is compared with a bound.)
static void TestThreadsSameResult() {
  std::printf("-- TestThreadsSameResult\n");
  struct Result {
    MakeReport r;
    std::map<std::string, std::vector<uint8_t>> files;
    std::vector<size_t> order;
  };
  auto run = [](unsigned threads) {
    World w;
    FakeGuest g = PlantedGuest();
    RemoveAllFiles(w);
    std::vector<uint8_t> other = StandInBytes(34);
    other[5] ^= 0x33;
    WriteChrthree(w, 48, {{34, other}});
    Gen gen;
    std::mutex m;
    std::vector<size_t> order;
    gen.post = [&](size_t i, Made& result, std::string* why) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20 * (7 - int(i))));
      if (i == 0 || i == 2) {
        result.reset();
        if (why) *why = "refused " + std::to_string(i);
      }
      std::lock_guard<std::mutex> lock(m);
      order.push_back(i);
    };
    MakeOptions o = Opt(gen);
    o.threads = threads;
    o.temp_tag = "5-5";
    uint64_t room = 65536;
    for (size_t i = 0; i <= 4; ++i) room += TestSlots()[i].size;
    o.free_bytes = [room](const fs::path&) { return room; };
    Result out;
    out.r = RunMake(w, g, o);
    PrintMake("TestThreadsSameResult", out.r);
    for (const auto& [name, state] : Snapshot(w.colours)) out.files[name] = state.bytes;
    out.order = order;
    return out;
  };
  const Result one = run(1);
  const Result four = run(4);
  const std::vector<size_t> table_order = {0, 1, 2, 4};
  const std::vector<std::string> want_notes = {"alpha_red: generator: refused 0", "alpha_lilac: generator: refused 2",
                                               "spartan_h3_pink: source differs", "spartan_h2_orange: no space",
                                               "spartan_h2_cyan: no space"};
  CHECK(one.order == table_order);                              // one worker: the generator ran in table order
  // Precondition: on four workers the generator of slot 2 finished before the generator of slot 0 (the two refusing
  // slots), so notes kept in the order they arise would name slot 2 first.
  const auto finished_at = [&four](size_t slot) {
    return size_t(std::find(four.order.begin(), four.order.end(), slot) - four.order.begin());
  };
  CHECK(four.order.size() == 4 && finished_at(2) < finished_at(0) && finished_at(0) < four.order.size());
  CHECK(one.r.good == 0 && one.r.made == 2 && one.r.not_made == 5);
  CHECK(one.r.notes == want_notes);
  CHECK(four.r.notes == want_notes);  // table order, whatever finished first
  CHECK(four.r.good == one.r.good && four.r.made == one.r.made && four.r.not_made == one.r.not_made);
  CHECK(one.files.size() == 3 && one.files.count("alpha_blue.tpr") == 1 && one.files.count("spartan_h3_orchid.tpr") == 1);
  CHECK(four.files == one.files);
}

static void TestManifestKeepsGoodNames() {
  std::printf("-- TestManifestKeepsGoodNames\n");
  FakeGuest g = PlantedGuest();
  {
    // A good file under another name keeps its line; a line naming a missing file gets <slot>.tpr once that is made.
    World w;
    fs::rename(w.File(0), w.colours / "custom_red.tpr");
    RemoveFiles(w, {1, 5});
    std::vector<std::string> start = StdLines();
    start[0] = "alpha_red custom_red.tpr";
    start[5] = "spartan_h2_orange gone.tpr";
    WriteText(w.colours / "manifest.txt", ManifestOf(start, "\n"));
    Gen gen;
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestManifestKeepsGoodNames", r);
    CHECK(r.good == 5 && r.made == 2 && r.not_made == 0);
    CHECK(r.notes.empty());
    CHECK(gen.Calls(0) == 0 && gen.Calls(1) == 1 && gen.Calls(5) == 1 && gen.Total() == 2);
    std::vector<std::string> want = StdLines();
    want[0] = "alpha_red custom_red.tpr";
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape && lines == want);
    CHECK(!fs::exists(w.File(0)));  // red was not made again under its own name
    CHECK(ReadAll(w.File(5)) == ColourBytes(5));
    const Status s = Load(TestSlots(), w.colours, w.game, g.guest);
    CHECK(s.usable && s.notes.empty());
    CheckSlots(s, {});
  }
  {
    // A line another process wrote for a good file while this call was making its files is kept: the manifest is read
    // again just before it is replaced.
    World w;
    RemoveFiles(w, {1, 4});
    Gen gen;
    gen.post = [&](size_t i, Made&, std::string*) {
      if (i != 4) return;
      Write(w.colours / "custom_blue.tpr", ColourBytes(1));
      std::vector<std::string> theirs = StdLines();
      theirs[1] = "alpha_blue custom_blue.tpr";
      WriteText(w.colours / "manifest.txt", ManifestOf(theirs, "\n"));
    };
    const MakeOptions o = Opt(gen);
    const MakeReport r = RunMake(w, g, o);
    PrintMake("TestManifestKeepsGoodNames (another process)", r);
    CHECK(r.good == 5 && r.made == 2 && r.not_made == 0);
    std::vector<std::string> want = StdLines();
    want[1] = "alpha_blue custom_blue.tpr";
    bool shape = false;
    const std::vector<std::string> lines = ManifestBody(w.colours, &shape);
    CHECK(shape && lines == want);
  }
}

static void TestMakeTempTag() {
  std::printf("-- TestMakeTempTag\n");
  const std::string a = MakeTempTag();
  const std::string b = MakeTempTag();
  auto plain = [](const std::string& tag) {
    if (tag.empty()) return false;
    for (char c : tag) {
      if (!((c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
  };
  CHECK(plain(a) && plain(b));
  CHECK(a != b);
  CHECK(StartsWith(a, std::to_string(GetCurrentProcessId()) + "-") && a.size() > std::to_string(GetCurrentProcessId()).size() + 1);
}

static std::optional<fs::path> EnvPath(const wchar_t* name) {
  wchar_t buf[4096];
  const DWORD len = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
  if (len == 0 || len >= std::size(buf)) return std::nullopt;
  return fs::path(buf);
}

// The real game folder (read-only): every stand-in of the real slot table is the ledgered retail entry, and avt.afs holds
// the assigned entries where the overlay expects them.
static void TestRealGameFolder(const fs::path& game) {
  FakeGuest g = ImageGuest();
  const Status s = Load(game / "no_colour_folder_here", game, g.guest);
  PrintNotes("real game", s);
  CHECK(s.usable && s.notes.size() == 1 && s.slots.size() == kSlotCount);
  for (size_t i = 0; i < kSlotCount && i < s.slots.size(); ++i) {
    const LoadedSlot& l = s.slots[i];
    CHECK(l.def == &Slots()[i] && l.stand_in && l.tpr);
    if (!l.tpr) continue;
    CHECK(l.tpr->size() == kStandInLedger[i].size);
    CHECK(HashOf(*l.tpr) == kStandInLedger[i].sha1);
  }
  const auto avt = afs::FileSource::Open(game / "avt.afs");
  CHECK(avt != nullptr);
  if (!avt) return;
  std::string why;
  const auto toc = afs::ReadToc(*avt, &why);
  CHECK(toc && toc->count == 700 && toc->offsets[0] == 0x1800);
  if (!toc || toc->count != 700) return;
  for (uint32_t e = kProvenFirst; e <= kProvenLast; ++e) CHECK(toc->sizes[e] == 26624);
}

// The private colour folder (read-only): the approved files load against the compiled-in sizes and SHA-1s.
static void TestRealColourFolder(const fs::path& colours, const fs::path& game) {
  FakeGuest g = ImageGuest();
  const Status s = Load(colours, game, g.guest);
  PrintNotes("real colours", s);
  CHECK(s.usable && s.notes.empty() && s.slots.size() == kSlotCount);
  for (const LoadedSlot& l : s.slots) CHECK(!l.stand_in && l.tpr);
  CHECK(s.Summary() == "colour costumes: 7 slots, 7 from files, 0 stand-ins");
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string why;
  const std::optional<doa4_test::CostumeImage> image = doa4_test::LoadCostumeImage(&why);

  TestSlotTableValues();
  TestAlphaRows();
  TestSummaryOff();
  TestLoadAllGood();
  TestLoadNoFolder();
  TestLoadNoManifest();
  TestLoadWrongSize();
  TestLoadSameSizeOtherBytes();
  TestStandInUnreadable();
  TestSharedEntry();
  TestReplacementsByArchive();
  TestEntryLabels();
  TestManifestGrains();
  TestOldManifestBesideNewTable();
  TestTprWithoutPlace();
  TestDecideRows();
  TestNotInstalledNoExtras();
  TestAppendedModels();
  TestNotesPrintable();
  TestColourFolder();
  TestStandInWrongSizeNotUsable();
  TestKindOf();
  TestShouldMake();
  TestRowsOn();
  TestMustRefuse();
  TestFreeBytesFrom();
  TestIsPassingNote();
  TestRefusalMessage();
  TestRefusalNamesLoadsCause();
  TestMakeDefsCoverSlots();
  TestAllGoodTouchesNothing();
  TestMakesAllFromEmpty();
  TestMakesOnlyMissing();
  TestWrongFileRemade();
  TestCrashLeftoverIgnored();
  TestFileWithoutLineAdopted();
  TestSourceWrongHash();
  TestSourceWrongSize();
  TestSourceMissing();
  TestMadeFileFailsHashNotWritten();
  TestGeneratorRefuses();
  TestGeneratorThrows();
  TestNoSpace();
  TestFolderIsAFile();
  TestTargetHeldOpen();
  TestRenameOntoHeldGoodFile();
  TestManifestWriteFails();
  TestNothingEscapes();
  TestManifestRereadFails();
  TestGoodSlotLosesItsFile();
  TestLineNamesFailingFileButDefaultIsGood();
  TestTempTagNamesTheTemporary();
  TestTwoMakersOneFolder();
  TestThreadsSameResult();
  TestManifestKeepsGoodNames();
  TestMakeTempTag();

  if (image) {
    g_image = &*image;
    TestSlotTableOnImage();
    TestSourcesOnImage();
    TestNotInstalledNoRecords();
    TestAppendedModelsOnImage();
  } else if (doa4_test::ReportNoImage("slot table on the image", why)) {
    ++g_checks;
    ++g_failures;
  }
  const std::optional<fs::path> game = EnvPath(L"DOA4_SUITS_GAME_DIR");
  if (!game) {
    std::printf("real game folder: SKIPPED (DOA4_SUITS_GAME_DIR unset)\n");
  } else if (!g_image) {
    std::printf("FAIL real game folder: DOA4_SUITS_GAME_DIR is set but the image is not loaded\n");
    ++g_checks;
    ++g_failures;
  } else {
    TestRealGameFolder(*game);
    std::printf("real game folder: ran\n");
  }
  const std::optional<fs::path> colours = EnvPath(L"DOA4_COLOURS_DIR");
  if (!colours) {
    std::printf("real colour folder: SKIPPED (DOA4_COLOURS_DIR unset)\n");
  } else if (!g_image || !game) {
    std::printf("FAIL real colour folder: DOA4_COLOURS_DIR is set but the image or the game folder is not\n");
    ++g_checks;
    ++g_failures;
  } else {
    TestRealColourFolder(*colours, *game);
    std::printf("real colour folder: ran\n");
  }

  std::printf("colour_costumes: %d checks run\n", g_checks);
  if (g_failures) {
    std::printf("FAILED: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }
  std::printf("PASSED: %d checks, 0 failures\n", g_checks);
  return 0;
}
