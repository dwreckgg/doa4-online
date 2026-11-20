// Host test for src/doa4_colour_costumes.cpp: the slot table, Load's degrade grains against a synthetic game folder
// (a chrthree-shaped archive holding the stand-in entries, an avt-shaped archive with entry 0 at 0x1800) and synthetic
// colour files, the replacements, table rows, labels and Alpha's rows. The synthetic checks plant the file-table words of
// the files they use in a fake guest, so they run without the image. Optional tiers:
//   DOA4_TEST_IMAGE (doa4_test_image.h): the real file table and costume records for the slot table's file numbers;
//   DOA4_SUITS_GAME_DIR=<the DOA4 folder> (read-only): with the image, the real stand-ins and avt.afs entries;
//   DOA4_COLOURS_DIR=<the private colour folder> (read-only): with the image and the game folder, the approved files load.
// Set but unusable, a tier FAILS. No game byte and no colour file is in this repository. Built and run by
// run_colour_costumes_test.bat. Prints "colour_costumes: N checks run" before its verdict line.

#include "../../src/doa4_colour_costumes.h"

#include <algorithm>
#include <bit>
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
static std::vector<uint8_t> ColourBytes(size_t slot) { return Content(int(1 + slot), 1000 + 37 * slot); }

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

static void TestEnabledTable() {
  CHECK(Enabled(false, "") == true);
  CHECK(Enabled(false, "record") == true);
  CHECK(Enabled(false, "replay") == false);
  CHECK(Enabled(false, "hash") == false);
  CHECK(Enabled(false, "other") == false);
  CHECK(Enabled(true, "") == false);
  CHECK(Enabled(true, "record") == false);
}

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

  TestEnabledTable();
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

  if (image) {
    g_image = &*image;
    TestSlotTableOnImage();
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
