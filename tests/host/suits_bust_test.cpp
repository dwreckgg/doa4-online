// Host test for doa4_suits_bust.h: the live rescale of a bust morph's poses by the "Age:" slider, for converted suits
// ("DX2B" blocks) and retail bust blocks (tagged "D4AG" the first time an age other than 18 is wanted).
// Build: clang++ -std=c++23 -I src tests/host/suits_bust_test.cpp -o suits_bust_test.exe
// Every number below is synthetic. Ranges quoted from the census (notes/research/bust_age_unknowns_2026-11-20.md) are
// magnitudes only; no vertex is copied from a game file.
#include "doa4_suits_bust.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace b = doa4::suits::bust;

int g_checks = 0;
int g_fails = 0;
int g_outside = 0;  // reads and writes outside the test memory: counted, not thrown, so a stray access cannot end the run

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_fails;
    std::printf("FAIL: %s\n", what);
    std::fflush(stdout);
  }
}

// Guest memory: big-endian words in a byte array, addresses start at kBase. Reads and writes are counted, and the first
// and last written addresses kept.
constexpr uint32_t kBase = 0x40000000;

struct Mem {
  std::vector<uint8_t> bytes;
  int writes = 0;
  mutable int reads = 0;
  uint32_t first_write = 0;
  uint32_t last_write = 0;
  explicit Mem(size_t size = 0x400) : bytes(size, 0xAB) {}
  bool Inside(uint32_t a) const { return a >= kBase && size_t(a - kBase) + 4 <= bytes.size(); }
  uint32_t Raw(uint32_t a) const {
    const uint8_t* p = &bytes.at(a - kBase);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  void Put(uint32_t a, uint32_t v) {
    uint8_t* p = &bytes.at(a - kBase);
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  }
  uint32_t ReadU32(uint32_t a) const {
    ++reads;
    if (!Inside(a)) {
      ++g_outside;
      return 0xDEADBEEF;
    }
    return Raw(a);
  }
  void WriteU32(uint32_t a, uint32_t v) {
    if (writes == 0) first_write = a;
    ++writes;
    last_write = a;
    if (!Inside(a)) {
      ++g_outside;
      return;
    }
    Put(a, v);
  }
  void Fill(uint32_t a, uint32_t v) { Put(a, v); }  // fixture set-up, not counted
  void FillF(uint32_t a, float f) { Fill(a, std::bit_cast<uint32_t>(f)); }
  float F(uint32_t a) const { return std::bit_cast<float>(Raw(a)); }
  void Reset() {
    writes = 0;
    reads = 0;
    first_write = last_write = 0;
  }
};

constexpr uint32_t kMpp = kBase + 0x10;
constexpr uint32_t kRest = kBase + 0x100;
constexpr uint32_t kDown = kBase + 0x200;
constexpr uint32_t kUp = kBase + 0x300;

constexpr uint32_t kMarkedList = kBase + 0x80;

// Two records. Rest (1,2,3) (4,5,6); down (1,1,3) (4,5,5); up (1,4,3) (6,5,6). After bind: +8 = 1, pose words absolute.
// Full range: down at ratio 2, up at ratio 3, different so a mix-up of the two shows.
// "MPP\0", kind 0 and a converter-style mapping list (one word per record, bit 31 on all but the first,
// physics_port.py:141) so the stale-block walk finds the stored count (fix round 1, ruling A15).
Mem Fixture() {
  Mem m;
  m.Fill(kMpp + 0x00, doa4::suits::bust::kMagicMpp);
  m.Fill(kMpp + 0x10, 0);
  m.Fill(kMpp + 0x18, kMarkedList);
  m.Fill(kMarkedList + 0, 0x01000000);
  m.Fill(kMarkedList + 4, 0x81000020);
  m.Fill(kMarkedList + 8, 0xFFFFFFFF);
  m.Fill(kMpp + 0x08, 1);
  m.Fill(kMpp + 0x14, 3);
  m.Fill(kMpp + 0x1C, kRest);
  m.Fill(kMpp + 0x20, kDown);
  m.Fill(kMpp + 0x24, kUp);
  m.Fill(kMpp + 0x28, doa4::suits::bust::kMagic);
  m.Fill(kMpp + 0x2C, 2);
  m.FillF(kMpp + 0x30, 1.0f);
  m.FillF(kMpp + 0x34, 2.0f);
  m.FillF(kMpp + 0x38, 1.0f);
  m.FillF(kMpp + 0x3C, 3.0f);
  const float rest[] = {1, 2, 3, 4, 5, 6}, down[] = {1, 1, 3, 4, 5, 5}, up[] = {1, 4, 3, 6, 5, 6};
  for (uint32_t i = 0; i < 6; ++i) {
    m.FillF(kRest + 4 * i, rest[i]);
    m.FillF(kDown + 4 * i, down[i]);
    m.FillF(kUp + 4 * i, up[i]);
  }
  // A third record's worth of rest differs from the poses' filler, so a loop that overruns changes the poses' bytes.
  for (uint32_t i = 6; i < 9; ++i) m.FillF(kRest + 4 * i, 1.0f);
  return m;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void TestFullScalesEachPoseByItsOwnRatio() {
  Mem m = Fixture();
  Check(doa4::suits::bust::Rescale(m, kMpp, 99), "a marked block goes to the full range");
  // Hand-derived, pose = rest + (pose - rest) * ratio.
  // Down, ratio 2: y 2 + (1 - 2) * 2 = 0; second record z 6 + (5 - 6) * 2 = 4.
  // Up, ratio 3: y 2 + (4 - 2) * 3 = 8; second record x 4 + (6 - 4) * 3 = 10.
  const float down[] = {1, 0, 3, 4, 5, 4}, up[] = {1, 8, 3, 10, 5, 6}, rest[] = {1, 2, 3, 4, 5, 6};
  for (uint32_t i = 0; i < 6; ++i) {
    Check(Near(m.F(kDown + 4 * i), down[i]), "down pose at its own ratio, every component of every record");
    Check(Near(m.F(kUp + 4 * i), up[i]), "up pose at its own ratio, every component of every record");
    Check(m.F(kRest + 4 * i) == rest[i], "rest pose untouched");
  }
  Check(m.F(kMpp + 0x30) == 2.0f, "down's current ratio recorded");
  Check(m.F(kMpp + 0x38) == 3.0f, "up's current ratio recorded");
  Check(m.F(kMpp + 0x34) == 2.0f && m.F(kMpp + 0x3C) == 3.0f, "full ratios unchanged");
  Check(m.ReadU32(kDown + 24) == 0xABABABAB && m.ReadU32(kUp + 24) == 0xABABABAB, "bytes after the last record untouched");
}

void TestBackToTheFilesStrength() {
  Mem m = Fixture();
  doa4::suits::bust::Rescale(m, kMpp, 99);
  Check(doa4::suits::bust::Rescale(m, kMpp, 18), "toggle off is applied");
  Check(Near(m.F(kDown + 4), 1.0f) && Near(m.F(kDown + 20), 5.0f), "toggle off restores the down pose");
  Check(Near(m.F(kUp + 4), 4.0f) && Near(m.F(kUp + 12), 6.0f), "toggle off restores the up pose");
  Check(m.F(kMpp + 0x30) == 1.0f && m.F(kMpp + 0x38) == 1.0f, "current ratios back to 1");
}

void TestFactorIsWantedOverCurrent() {
  // Poses at ratio 0.5 now, up at 0.25: toggle off is a factor of 2 for down and 4 for up.
  Mem m = Fixture();
  m.FillF(kMpp + 0x30, 0.5f);
  m.FillF(kMpp + 0x38, 0.25f);
  Check(doa4::suits::bust::Rescale(m, kMpp, 18), "ratios other than 1 are rewritten");
  Check(Near(m.F(kDown + 4), 0.0f), "down: 2 + (1 - 2) * 2");
  Check(Near(m.F(kUp + 4), 10.0f), "up: 2 + (4 - 2) * 4");
}

void TestOnlyOnePoseOutOfDateStillRewrites() {
  Mem m = Fixture();
  m.FillF(kMpp + 0x38, 3.0f);  // up already at 3, down at 1, toggle off wants 1 and 1
  Check(doa4::suits::bust::Rescale(m, kMpp, 18), "one pose out of date is enough");
  Check(Near(m.F(kUp + 4), 2.0f + 2.0f / 3.0f), "up: 2 + (4 - 2) / 3");
  Check(Near(m.F(kDown + 4), 1.0f), "down stays where it is");
}

void TestOnlyDownOutOfDateStillRewrites() {
  Mem m = Fixture();
  m.FillF(kMpp + 0x30, 2.0f);  // down at 2, up at 1, toggle off wants 1 and 1
  Check(doa4::suits::bust::Rescale(m, kMpp, 18), "down alone out of date is enough");
  Check(Near(m.F(kDown + 4), 1.5f), "down: 2 + (1 - 2) / 2");
  Check(Near(m.F(kUp + 4), 4.0f), "up stays where it is");
}

void TestNothingWrittenWhenAlreadyThere() {
  Mem m = Fixture();
  Check(!doa4::suits::bust::Rescale(m, kMpp, 18), "already at the wanted ratios: no rewrite");
  Check(m.writes == 0, "already at the wanted ratios: no write");
}

void TestRefusesEverythingElse() {
  struct Case {
    uint32_t offset;
    uint32_t value;
    const char* what;
  };
  const uint32_t nan = 0x7FC00000;
  const Case cases[] = {
      {0x28, 0, "a vanilla block has no magic"},
      {0x14, 21, "a block with another pose count (hands)"},
      {0x08, 0, "a block the binder has not bound (pose words are still offsets)"},
      {0x2C, 0, "no records"},
      {0x30, 0, "down's current ratio zero"},
      {0x30, nan, "down's current ratio not a number"},
      {0x38, 0, "up's current ratio zero"},
      {0x38, nan, "up's current ratio not a number"},
      {0x34, 0, "down's full ratio zero"},
      {0x34, nan, "down's full ratio not a number"},
      {0x3C, 0, "up's full ratio zero"},
      {0x3C, nan, "up's full ratio not a number"},
      {0x3C, 0x7F800000, "up's full ratio infinite"},
      {0x34, 0xBF800000, "down's full ratio negative"},
  };
  for (const Case& c : cases) {
    Mem m = Fixture();
    m.Fill(kMpp + c.offset, c.value);
    Check(!doa4::suits::bust::Rescale(m, kMpp, 99), c.what);
    Check(m.writes == 0, c.what);
  }
}

void TestIsMarked() {
  Mem m = Fixture();
  Check(doa4::suits::bust::IsMarked(m, kMpp), "a suit's block is marked");
  m.Fill(kMpp + 0x28, 0);
  Check(!doa4::suits::bust::IsMarked(m, kMpp), "a vanilla block is not");
}

// ---- The age rules (Task 2 of the bust-age plan). The 68 checks above are today's, ported true -> 99, false -> 18. ----

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

bool BitEqual(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

void TestAgeRatio() {
  Check(BitEqual(b::AgeRatio(18, 3.0f), 1.0f), "age 18 is exactly the file's strength");
  Check(BitEqual(b::AgeRatio(99, 3.0f), 3.0f), "age 99 is exactly full");
  Check(BitEqual(b::AgeRatio(99, 1.43f), 1.43f), "age 99 is exactly full (1.43, bit equal)");
  Check(BitEqual(b::AgeRatio(99, 0.1f), 0.1f), "age 99 is exactly full below 0.5 too (the line alone is not)");
  Check(BitEqual(b::AgeRatio(9, 3.0f), 0.5f), "below 18: age / 18");
  Check(BitEqual(b::AgeRatio(9, 1.43f), 0.5f), "below 18 full plays no part");
  Check(Near(b::AgeRatio(17, 3.0f), 0.9444444f), "17 / 18");
  Check(Near(b::AgeRatio(1, 3.0f), 0.0555556f), "1 / 18");
  Check(Near(b::AgeRatio(19, 3.0f), 1.0246914f), "1 + 1 / 81 * 2");
  Check(Near(b::AgeRatio(98, 3.0f), 2.9753086f), "1 + 80 / 81 * 2");
  Check(!Near(b::AgeRatio(98, 3.0f), 2.6161616f), "NOT the /99 line (1 + 80 / 99 * 2)");
  Check(BitEqual(b::AgeRatio(0, 3.0f), b::AgeRatio(1, 3.0f)), "age 0 clamps to 1");
  Check(BitEqual(b::AgeRatio(150, 3.0f), 3.0f), "age 150 clamps to 99");
}

template <class M>
concept TakesBool = requires(M& m) { b::Rescale(m, 0u, true); };

void TestConstants() {
  // "MPP\0": the binder sub_82374E98 compares +0..+3 with 77, 80, 80, 0 (generated/default/dead_or_alive_4_recomp.5.cpp
  // 18143-18165). "D4AG" and the cap: the plan (Task 2 interfaces; ledger A9).
  Check(b::kMagicMpp == 0x4D505000u, "MPP magic is 'M' 'P' 'P' 0");
  Check(b::kMagicRetail == 0x44344147u && b::kMagic == 0x44583242u, "D4AG and DX2B");
  Check(b::kMaxRecords == 16384u, "record cap 16384");
  Check(!TakesBool<Mem>, "a bool cannot reach Rescale as an age (the old toggle call does not compile)");
}

void TestEffectiveAge() {
  Check(b::EffectiveAge(0, false) == 18, "never saved, no legacy toggle: 18");
  Check(b::EffectiveAge(0, true) == 99, "never saved, legacy full bust on: 99");
  Check(b::EffectiveAge(-3, false) == 18, "hand-edited -3: never saved");
  Check(b::EffectiveAge(-3, true) == 99, "hand-edited -3 with the legacy toggle: 99");
  Check(b::EffectiveAge(1, true) == 1, "a saved 1 wins over the legacy toggle");
  Check(b::EffectiveAge(18, true) == 18, "a saved 18 wins over the legacy toggle");
  Check(b::EffectiveAge(99, false) == 99, "99 stays");
  Check(b::EffectiveAge(150, false) == 99, "hand-edited 150 clamps to 99");
}

// Part A, the "Age:" box: (raw doa4_bust_age, box doa4_bust_age_on, legacy doa4_suits_full_bust) -> what the menu shows
// {on, age} and the age the hook wants. "Box never saved" reads as false (the cvar's default): the box = F rows. Raw 0
// is "never saved": the old toggle carries over as the box ON at 99 until the first save writes both keys.
void TestEffectiveSetting() {
  struct Row {
    int32_t raw;
    bool box, legacy, on;
    int age, wanted;
  };
  constexpr bool F = false, T = true;
  const Row rows[] = {
      {0, F, F, F, 18, 18},  {0, F, T, T, 99, 99},  {0, T, F, T, 18, 18},  {0, T, T, T, 99, 99},
      {1, F, F, F, 1, 18},   {1, F, T, F, 1, 18},   {1, T, F, T, 1, 1},    {1, T, T, T, 1, 1},
      {18, F, F, F, 18, 18}, {18, F, T, F, 18, 18}, {18, T, F, T, 18, 18}, {18, T, T, T, 18, 18},
      {99, F, F, F, 99, 18}, {99, F, T, F, 99, 18}, {99, T, F, T, 99, 99}, {99, T, T, T, 99, 99},
      {-3, F, T, T, 99, 99}, {150, T, F, T, 99, 99}, {150, F, F, F, 99, 18},
  };
  for (const Row& r : rows) {
    char what[128];
    const b::AgeSetting s = b::EffectiveSetting(r.raw, r.box, r.legacy);
    std::snprintf(what, sizeof what, "EffectiveSetting(%d, box %d, legacy %d) == {on %d, age %d} (got {%d, %d})",
                  int(r.raw), int(r.box), int(r.legacy), int(r.on), r.age, int(s.on), s.age);
    Check(s == b::AgeSetting{r.on, r.age}, what);
    std::snprintf(what, sizeof what, "WantedAge(EffectiveSetting(%d, box %d, legacy %d)) == %d (got %d)", int(r.raw),
                  int(r.box), int(r.legacy), r.wanted, b::WantedAge(s));
    Check(b::WantedAge(s) == r.wanted, what);
  }
}

// Part B (0.3.7's line): LineSetting is EffectiveSetting with the old full-bust setting retired (ruling B1: nobody gets
// retail costumes changed without ticking the new box themselves). T2.2: the likeliest wrong version passes
// legacy_full = true (or reads the old cvar), which turns every never-saved row into the box on at 99; each such row
// carries that answer as an explicit NOT. Expected values from the Interfaces (LineSetting = EffectiveSetting(age, box,
// false)), traced by hand and through the pre-flight model (p1_scratch/model.py), not from the code under test.
void TestLineSetting() {
  struct Row {
    int saved_age;
    bool box, on;
    int age, wanted;
  };
  constexpr bool F = false, T = true;
  const Row rows[] = {
      {0, F, F, 18, 18},    // never saved, box off: retail; NOT {true, 99} (B1)
      {0, T, T, 18, 18},    // never saved, box on: the slider's default 18; NOT {true, 99}
      {-5, F, F, 18, 18},   // hand-edited below 1: never saved
      {99, T, T, 99, 99},
      {99, F, F, 99, 18},   // the greyed slider shows 99, the hook wants 18
      {150, T, T, 99, 99},  // hand-edited above 99: clamped
      {1, T, T, 1, 1},      // the lowest age is kept, not raised to 18
  };
  for (const Row& r : rows) {
    char what[160];
    const b::AgeSetting s = b::LineSetting(r.saved_age, r.box);
    std::snprintf(what, sizeof what, "LineSetting(%d, box %d) == {on %d, age %d} (got {%d, %d})", r.saved_age,
                  int(r.box), int(r.on), r.age, int(s.on), s.age);
    Check(s == b::AgeSetting{r.on, r.age}, what);
    std::snprintf(what, sizeof what, "LineSetting(%d, box %d) is NOT {true, 99} unless saved at 99+ with the box on",
                  r.saved_age, int(r.box));
    Check(r.saved_age >= 99 && r.box ? s == b::AgeSetting{true, 99} : !(s == b::AgeSetting{true, 99}), what);
    std::snprintf(what, sizeof what, "WantedAge(LineSetting(%d, box %d)) == %d (got %d)", r.saved_age, int(r.box),
                  r.wanted, b::WantedAge(s));
    Check(b::WantedAge(s) == r.wanted, what);
  }
}

// Part B (PB4): the session's box and age from the host's hand-off text (np::SessionBust reads it once). Age: a whole
// decimal int32, box: exactly "true" or "false"; both valid -> LineSetting; a missing line (a 0.3.7 recording's replay)
// -> box off at 18 silently; anything else -> box off at 18 and the note names the ignored value. T2.2: the likeliest
// wrong versions are a parse of the numeric prefix ("99abc" -> 99), a box read as "not false" ("maybe", "True", "" ->
// on), a missing line treated as a malformed one (a note on every old replay), and a fallback to the old full-bust
// rule ("0" with the box on -> 99). Every malformed row carries NOT {true, 99}: that is what each wrong parse gives.
using OptText = std::optional<std::string>;

std::string Show(const OptText& t) { return t ? "\"" + *t + "\"" : std::string("missing"); }

void CheckFromText(const OptText& age, const OptText& box, b::AgeSetting want, const char* note_has, const char* why) {
  std::string note = "stale";  // the reader must clear it
  const b::AgeSetting s = b::SettingFromText(age, box, &note);
  char what[256];
  std::snprintf(what, sizeof what, "SettingFromText(age %s, box %s) == {on %d, age %d} (got {%d, %d}): %s",
                Show(age).c_str(), Show(box).c_str(), int(want.on), want.age, int(s.on), s.age, why);
  Check(s == want, what);
  if (note_has == nullptr) {
    std::snprintf(what, sizeof what, "SettingFromText(age %s, box %s): no note (got \"%s\")", Show(age).c_str(),
                  Show(box).c_str(), note.c_str());
    Check(note.empty(), what);
  } else {
    std::snprintf(what, sizeof what, "SettingFromText(age %s, box %s): the note names %s (got \"%s\")",
                  Show(age).c_str(), Show(box).c_str(), note_has, note.c_str());
    Check(note.find(note_has) != std::string::npos, what);
    std::snprintf(what, sizeof what, "SettingFromText(age %s, box %s) is NOT {true, 99}", Show(age).c_str(),
                  Show(box).c_str());
    Check(!(s == b::AgeSetting{true, 99}), what);
  }
}

void TestSettingFromText() {
  const b::AgeSetting off{false, 18};
  // valid pairs: LineSetting of the parsed values
  CheckFromText(std::nullopt, std::nullopt, off, nullptr, "no lines (a 0.3.7 recording): the game's own, silently");
  CheckFromText("99", "true", {true, 99}, nullptr, "valid");
  CheckFromText("99", "false", {false, 99}, nullptr, "valid, box off");
  CheckFromText("0", "false", off, nullptr, "never saved, box off");
  CheckFromText("0", "true", {true, 18}, nullptr, "never saved, box on: 18, NOT 99 (B1)");
  CheckFromText("150", "true", {true, 99}, nullptr, "clamped like the cvar");
  CheckFromText("-5", "true", {true, 18}, nullptr, "below 1 like the cvar");
  // malformed: box off at 18, the note names the ignored value
  CheckFromText("abc", "true", off, "doa4_bust_age \"abc\"", "not a number");
  CheckFromText("99", "maybe", off, "doa4_bust_age_on \"maybe\"", "not a bool");
  CheckFromText("99abc", "true", off, "doa4_bust_age \"99abc\"", "a numeric prefix is not a number");
  CheckFromText(" 99", "true", off, "doa4_bust_age \" 99\"", "a leading space is not part of a number");
  CheckFromText("99", "True", off, "doa4_bust_age_on \"True\"", "the registry prints true in lower case");
  CheckFromText("99", "", off, "doa4_bust_age_on \"\"", "an empty box is not false");
  CheckFromText("4294967395", "true", off, "doa4_bust_age \"4294967395\"", "out of int32 (2^32 + 99): not 99");
  // one missing line: the game's own, silently, whichever line is missing
  CheckFromText("99", std::nullopt, off, nullptr, "box line missing");
  CheckFromText(std::nullopt, "true", off, nullptr, "age line missing");
}

void TestIsBustState() {
  // Constants from notes/research/bust_age_unknowns_2026-11-20.md U1b(a): four 116-byte channel-4 morph states from
  // 0x8302E5A8, the states the direct call at dead_or_alive_4_recomp.3.cpp:10080 runs.
  Check(b::IsBustState(0x8302E5A8u), "slot 0");
  Check(b::IsBustState(0x8302E5A8u + 116), "slot 1");
  Check(b::IsBustState(0x8302E5A8u + 232), "slot 2");
  Check(b::IsBustState(0x8302E5A8u + 348), "slot 3");
  Check(!b::IsBustState(0x8302E5A8u + 464), "past the last slot");
  Check(!b::IsBustState(0x8302E5A8u + 115), "one byte short of slot 1");
  Check(!b::IsBustState(0x8302E5A8u + 1), "inside slot 0");
  Check(!b::IsBustState(0x8302E5A4u), "below slot 0 (wraps, not slot 0)");
}

constexpr uint32_t Align16(uint32_t a) { return (a + 15u) & ~15u; }

struct Retail {
  Mem m;
  uint32_t list = 0, rest = 0, down = 0, up = 0, records = 0;
};

// A bound retail bust block, laid out as the census found all 67 channel-4 blocks: a 0x30-byte header ("MPP\0" at +0,
// +0x08 = 1 once bound, kind +0x10 = 0, +0x14 = 3 poses, +0x28..+0x2F zero), the mapping list at +0x30 (one word per
// vertex, bit 31 on the first word of every record after the first, 0xFFFFFFFF last), then the rest, down and up
// arrays, 12 bytes a record, each padded with zeros to 16 bytes. `extra` vertex words go into the first record.
// Records 0 and 1 carry the swimsuit fixture's numbers; later records a rest near 0.25 and moves of 0.0158.
Retail MakeRetail(uint32_t records, uint32_t extra = 1) {
  Retail r;
  r.records = records;
  r.list = kMpp + 0x30;
  const uint32_t words = records + extra + 1;
  r.rest = Align16(r.list + 4 * words);
  const uint32_t span = Align16(records * 12);
  r.down = r.rest + span;
  r.up = r.down + span;
  r.m = Mem(size_t(r.up + span + 0x40 - kBase));
  Mem& m = r.m;
  for (uint32_t a = kMpp; a < r.up + span; a += 4) m.Fill(a, 0);
  m.Fill(kMpp + 0x00, b::kMagicMpp);
  m.Fill(kMpp + 0x04, 0x00000460);  // file data the tag must leave alone
  m.Fill(kMpp + 0x08, 1);
  m.Fill(kMpp + 0x0C, 15);  // object number
  m.Fill(kMpp + 0x14, 3);
  m.Fill(kMpp + 0x18, r.list);
  m.Fill(kMpp + 0x1C, r.rest);
  m.Fill(kMpp + 0x20, r.down);
  m.Fill(kMpp + 0x24, r.up);
  uint32_t a = r.list;
  uint32_t vertex = 0;
  m.Fill(a, 0x01000000 | (0x20 * vertex++));
  a += 4;
  for (uint32_t i = 0; i < extra; ++i, a += 4) m.Fill(a, 0x01000000 | (0x20 * vertex++));
  for (uint32_t i = 1; i < records; ++i, a += 4) m.Fill(a, 0x81000000 | (0x20 * vertex++));
  m.Fill(a, 0xFFFFFFFF);
  const float rest[] = {1, 2, 3, 4, 5, 6}, down[] = {1, 1, 3, 4, 5, 5}, up[] = {1, 4, 3, 6, 5, 6};
  for (uint32_t i = 0; i < records * 3; ++i) {
    const float rv = i < 6 ? rest[i] : 0.25f + 0.001f * float(i % 37);
    m.FillF(r.rest + 4 * i, rv);
    m.FillF(r.down + 4 * i, i < 6 ? down[i] : rv - 0.0158f);
    m.FillF(r.up + 4 * i, i < 6 ? up[i] : rv + 0.0158f);
  }
  return r;
}

using Range = std::pair<uint32_t, uint32_t>;  // [first, end)

// True if every byte outside the allowed ranges equals its value in `before`.
bool OnlyChanged(const std::vector<uint8_t>& before, const Mem& now, std::initializer_list<Range> allowed) {
  if (before.size() != now.bytes.size()) return false;
  for (size_t i = 0; i < before.size(); ++i) {
    if (before[i] == now.bytes[i]) continue;
    const uint32_t a = kBase + uint32_t(i);
    bool ok = false;
    for (const Range& r : allowed) ok = ok || (a >= r.first && a < r.second);
    if (!ok) return false;
  }
  return true;
}

void TestCountRecords() {
  {
    Retail r = MakeRetail(2, 1);
    Check(b::CountRecords(r.m, kMpp) == 2, "records = bit-31 words + 1");
    Check(b::CountRecords(r.m, kMpp) != 3, "NOT the word count (a record can name several vertices)");
    Check(r.rest == r.list + 16, "precondition: the terminator is the last word below the rest pose");
  }
  {
    Retail r = MakeRetail(1, 0);
    Check(b::CountRecords(r.m, kMpp) == 1, "one record, no bit-31 word");
  }
  {
    Retail r = MakeRetail(3, 0);
    Check(r.down - r.rest == 48, "precondition: 3 records = 36 bytes + 12 of padding");
    Check(b::CountRecords(r.m, kMpp) == 3, "3 records with 12 bytes of padding");
    Check(b::CountRecords(r.m, kMpp) != (r.down - r.rest) / 12, "NOT the pose distance / 12 (overcounts by one here)");
  }
  {
    Retail r = MakeRetail(2, 1);
    r.m.Fill(r.list, 0xFFFFFFFF);
    Check(b::CountRecords(r.m, kMpp) == 0, "an empty list is no records");
  }
  {
    Retail r = MakeRetail(2, 1);
    r.m.Fill(r.list + 12, 0x01000060);  // the terminator, overwritten; the next word is the rest pose
    Check(b::CountRecords(r.m, kMpp) == 0, "no end before the rest pose: refused");
  }
  {
    Retail r = MakeRetail(2, 1);
    r.m.Fill(kMpp + 0x20, r.list + 8);  // a pose pointer below the terminator (and below +0x1C)
    Check(b::CountRecords(r.m, kMpp) == 0, "the walk is bounded by the lowest pose pointer, not only +0x1C");
  }
  {
    Retail r = MakeRetail(b::kMaxRecords);
    Check(r.m.bytes.size() >= 0x10000 + 0x30 + 8, "precondition: the memory holds a cap-sized list");
    Check(r.rest - r.list >= 4 * (b::kMaxRecords + 2), "precondition: the list holds cap + 1 words and the end");
    Check(b::CountRecords(r.m, kMpp) == b::kMaxRecords, "a list of exactly the cap is accepted");
    r.m.Reset();
    Check(b::TagRetail(r.m, kMpp) && r.m.ReadU32(kMpp + 0x2C) == ((b::kMaxRecords << 8) | 18u),
          "a cap-sized block is tagged with its count");
  }
  {
    Retail r = MakeRetail(2, b::kMaxListWords - 3);  // 65536 words, the terminator the last
    Check(r.rest - r.list == 4 * b::kMaxListWords, "precondition: a list of exactly the word cap");
    Check(b::kMaxListWords == 65536u, "word cap 65536 (4 x the census maximum of 9652 words is less)");
    Check(b::CountRecords(r.m, kMpp) == 2, "a list of exactly the word cap is accepted");
  }
  {
    Retail r = MakeRetail(2, b::kMaxListWords - 2);  // 65537 words
    Check(r.rest - r.list >= 4 * (b::kMaxListWords + 1), "precondition: a list one word past the cap");
    Check(b::CountRecords(r.m, kMpp) == 0, "one word past the word cap: refused");
    r.m.Reset();
    Check(!b::TagRetail(r.m, kMpp) && r.m.writes == 0, "one word past the word cap: not tagged, no write");
  }
  {
    Retail r = MakeRetail(b::kMaxRecords + 1);
    Check(b::CountRecords(r.m, kMpp) == 0, "cap + 1 records: refused");
    r.m.Reset();
    Check(!b::TagRetail(r.m, kMpp) && r.m.writes == 0, "cap + 1 records: not tagged, no write");
  }
}

void TestIsRetailBustCandidate() {
  Retail r = MakeRetail(2, 1);
  Check(b::IsRetailBustCandidate(r.m, kMpp, true), "a bound, untagged bust block on a bust state");
  Check(!b::IsRetailBustCandidate(r.m, kMpp, false), "the same block on another state (the face look-alike)");
  Check(!b::IsTagged(r.m, kMpp) && !b::IsMarked(r.m, kMpp), "an untagged retail block is neither tagged nor marked");
}

void TestTagRetail() {
  Retail r = MakeRetail(2, 1);
  const std::vector<uint8_t> before = r.m.bytes;
  Check(b::TagRetail(r.m, kMpp), "a candidate is tagged");
  Check(r.m.writes == 2, "exactly two writes");
  Check(r.m.ReadU32(kMpp + 0x2C) == ((2u << 8) | 18u), "+0x2C = records << 8 | age 18");
  Check(r.m.ReadU32(kMpp + 0x28) == b::kMagicRetail, "+0x28 = D4AG");
  Check(r.m.first_write == kMpp + 0x2C && r.m.last_write == kMpp + 0x28, "the count first, the magic last");
  Check(OnlyChanged(before, r.m, {{kMpp + 0x28, kMpp + 0x30}}), "header, mapping list and poses byte-identical");
  Check(b::IsTagged(r.m, kMpp) && !b::IsMarked(r.m, kMpp), "tagged, not marked");
  r.m.Reset();
  Check(!b::TagRetail(r.m, kMpp) && r.m.writes == 0, "a second tag is refused with no write");
}

void TestTagRefuses() {
  struct Case {
    uint32_t offset;  // from kMpp; kList means the first mapping word
    uint32_t value;
    const char* what;
  };
  constexpr uint32_t kList = 0xFFFF;
  const Case cases[] = {
      {0x28, b::kMagic, "+0x28 already DX2B"},
      {0x28, 1, "+0x28 nonzero"},
      {0x28, b::kMagicRetail, "+0x28 already D4AG (with +0x2C zero)"},
      {0x2C, 1, "+0x2C nonzero"},
      {0x08, 0, "unbound"},
      {0x10, 2, "kind 2 with 3 poses"},
      {0x14, 21, "kind 0 with 21 poses"},
      {0x00, 0, "no MPP magic"},
      {0x00, 0x4D505031, "MPP magic with a nonzero fourth byte"},
      {0x00, 0x4E505000, "MPP magic with a wrong first byte"},
      {kList, 0xFFFFFFFF, "no records"},
  };
  for (const Case& c : cases) {
    Retail r = MakeRetail(2, 1);
    r.m.Fill(c.offset == kList ? r.list : kMpp + c.offset, c.value);
    r.m.Reset();
    Check(!b::TagRetail(r.m, kMpp) && r.m.writes == 0, c.what);
    if (c.offset == 0x28 && c.value == b::kMagic) continue;  // a DX2B block is the swimsuit path, tested above
    // The candidate test reads the header only (no list walk on a frame with nothing to write); TagRetail walks.
    if (c.offset != kList) Check(!b::IsRetailBustCandidate(r.m, kMpp, true), c.what);
    const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, 99, 2.0f);
    Check(!s.wrote && r.m.writes == 0, c.what);
  }
  {
    Retail r = MakeRetail(2, 1);
    r.m.Fill(r.list + 12, 0x01000060);
    r.m.Reset();
    Check(!b::TagRetail(r.m, kMpp) && r.m.writes == 0, "no end before the rest pose: not tagged");
  }
  const float bad[] = {0.0f, kNaN, kInf, -1.0f};
  const int ages[] = {1, 58, 99};
  for (float f : bad) {
    for (int age : ages) {
      Retail r = MakeRetail(2, 1);
      const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, age, f);
      Check(!s.wrote && r.m.writes == 0 && !b::IsTagged(r.m, kMpp), "a bad strongest ratio: nothing tagged or moved");
    }
  }
}

void TestFullRefusedAtEveryAgeButRetail() {
  const uint32_t bad[] = {0, 0x7FC00000, 0x7F800000, 0xBF800000};
  const uint32_t at[] = {0x34, 0x3C};
  for (uint32_t off : at) {
    for (uint32_t v : bad) {
      for (int age : {1, 58}) {
        Mem m = Fixture();
        m.Fill(kMpp + off, v);
        Check(!b::Rescale(m, kMpp, age), "a bad full ratio on one pose is refused below 99 too");
        Check(m.writes == 0, "a bad full ratio on one pose: no write");
      }
    }
  }
  // At 18 the full ratio plays no part: a block left strong goes home even when its full ratio is unusable.
  Mem m = Fixture();
  b::Rescale(m, kMpp, 99);
  m.Fill(kMpp + 0x34, 0x7FC00000);
  m.Fill(kMpp + 0x3C, 0);
  Check(b::Rescale(m, kMpp, 18), "back to 18 with unusable full ratios");
  Check(Near(m.F(kDown + 4), 1.0f) && Near(m.F(kUp + 4), 4.0f), "back to 18: the file's poses");
}

void TestRescaleBetweenTheLines() {
  Mem m = Fixture();
  Check(b::Rescale(m, kMpp, 58), "age 58 is applied");
  // Down ratio 1 + 40/81 * (2 - 1) = 1.4938272; up 1 + 40/81 * (3 - 1) = 1.9876543.
  Check(Near(m.F(kDown + 4), 0.5061728f), "down y 2 + (1 - 2) * 1.4938272");
  Check(Near(m.F(kUp + 4), 5.9753086f), "up y 2 + (4 - 2) * 1.9876543");
  Check(Near(m.F(kMpp + 0x30), 1.4938272f) && Near(m.F(kMpp + 0x38), 1.9876543f), "ratios at 58 recorded");
  Check(b::Rescale(m, kMpp, 9), "age 9 from 58");
  Check(Near(m.F(kDown + 4), 1.5f), "down y 2 + (1 - 2) * 0.5");
  Check(Near(m.F(kUp + 4), 3.0f), "up y 2 + (4 - 2) * 0.5");
  Check(m.F(kMpp + 0x30) == 0.5f && m.F(kMpp + 0x38) == 0.5f, "ratios at 9 are exactly 0.5");
}

void TestRoundTripTo18() {
  // 1e-5 bound: P1 measured 3.8e-6 at most on this path (p1_scratch/trace.txt), so the margin is about 2.6x.
  Mem m = Fixture();
  const Mem start = m;
  b::Rescale(m, kMpp, 99);
  b::Rescale(m, kMpp, 1);
  Check(b::Rescale(m, kMpp, 18), "back to 18 after 99 and 1");
  bool near = true;
  for (uint32_t i = 0; i < 6; ++i) {
    near = near && Near(m.F(kDown + 4 * i), start.F(kDown + 4 * i)) && Near(m.F(kUp + 4 * i), start.F(kUp + 4 * i));
  }
  Check(near, "every pose component within 1e-5 of the file's");
  Check(BitEqual(m.F(kMpp + 0x30), 1.0f) && BitEqual(m.F(kMpp + 0x38), 1.0f), "ratios exactly 1.0 again");
}

float UlpOf(float x) {
  const float a = std::fabs(x);
  return std::nextafter(a, kInf) - a;
}

// Magnitudes from the census (AM1): rest up to 0.2864 (and down to -0.1395), a largest move of 0.0158.
void FillWalkPoses(Mem& m, uint32_t rest, uint32_t down, uint32_t up, uint32_t records) {
  const float rv[] = {0.2864f, -0.1395f, 0.2864f, 0.1f, 0.2864f, -0.1395f, 0.2f, 0.0f, 0.2864f};
  for (uint32_t i = 0; i < records * 3; ++i) {
    m.FillF(rest + 4 * i, rv[i % 9]);
    m.FillF(down + 4 * i, rv[i % 9] - 0.0158f);
    m.FillF(up + 4 * i, rv[i % 9] + 0.0158f);
  }
}

// 1000 ages over 1..99 from a fixed seed (age 1 forced every 97 steps, since low ages amplify the error on the way back),
// then 18. Returns the largest error of any pose component against the start, in ulps of the largest rest coordinate.
template <class Step>
float WalkError(Mem& m, uint32_t down, uint32_t up, uint32_t records, Step step, int* seen_1, int* seen_99) {
  const Mem start = m;
  uint32_t s = 0x2545F491u;
  for (int i = 0; i < 1000; ++i) {
    s = s * 1664525u + 1013904223u;
    const int age = i % 97 == 0 ? 1 : 1 + int((s >> 8) % 99u);
    *seen_1 += age == 1;
    *seen_99 += age == 99;
    step(age);
  }
  step(18);
  float worst = 0.0f;
  std::printf("  per-component error in ulps of 0.2864 (down, up):");
  for (uint32_t i = 0; i < records * 3; ++i) {
    const float dn = std::fabs(m.F(down + 4 * i) - start.F(down + 4 * i));
    const float upe = std::fabs(m.F(up + 4 * i) - start.F(up + 4 * i));
    std::printf(" %.1f/%.1f", dn / UlpOf(0.2864f), upe / UlpOf(0.2864f));
    worst = std::fmax(worst, std::fmax(dn, upe));
  }
  std::printf("\n");
  return worst / UlpOf(0.2864f);
}

void TestRandomWalkEndsAtRetail() {
  for (float full : {2.0f, 4.0f}) {
    {
      Retail r = MakeRetail(3, 0);
      FillWalkPoses(r.m, r.rest, r.down, r.up, 3);
      const Mem start = r.m;
      int seen_1 = 0, seen_99 = 0;
      const float ulps = WalkError(r.m, r.down, r.up, 3, [&](int age) { b::ApplyAge(r.m, kMpp, true, age, full); },
                                   &seen_1, &seen_99);
      std::printf("random walk, retail, full %.1f: largest error %.1f ulps of 0.2864\n", full, ulps);
      Check(seen_1 > 0 && seen_99 > 0, "precondition: the walk visits 1 and 99");
      Check(b::IsTagged(r.m, kMpp) && (r.m.ReadU32(kMpp + 0x2C) & 0xFF) == 18, "precondition: tagged, back at 18");
      Check(ulps <= 64.0f, "retail: back at 18 within 64 ulps after 1000 moves");
      Check(OnlyChanged(start.bytes, r.m, {{kMpp + 0x28, kMpp + 0x30}, {r.down, r.down + 36}, {r.up, r.up + 36}}),
            "retail walk: rest, list and padding untouched");
    }
    {
      Mem m = Fixture();
      FillWalkPoses(m, kRest, kDown, kUp, 2);
      m.FillF(kMpp + 0x34, full);
      m.FillF(kMpp + 0x3C, full);
      int seen_1 = 0, seen_99 = 0;
      const float ulps =
          WalkError(m, kDown, kUp, 2, [&](int age) { b::ApplyAge(m, kMpp, true, age, 2.5f); }, &seen_1, &seen_99);
      std::printf("random walk, swimsuit, full %.1f: largest error %.1f ulps of 0.2864\n", full, ulps);
      Check(m.writes > 1000 && BitEqual(m.F(kMpp + 0x30), 1.0f) && BitEqual(m.F(kMpp + 0x38), 1.0f),
            "precondition: the swimsuit walk moved the poses and ended at ratio 1");
      Check(ulps <= 64.0f, "swimsuit: back at 18 within 64 ulps after 1000 moves");
    }
  }
}

void TestApplyAge() {
  {
    Retail r = MakeRetail(2, 1);
    const std::vector<uint8_t> before = r.m.bytes;
    const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, 18, 2.0f);
    Check(!s.wrote && r.m.writes == 0, "a candidate at 18: nothing written, not even the tag");
    Check(before == r.m.bytes, "a candidate at 18: memory unchanged");
  }
  {
    Retail r = MakeRetail(2, 1);
    const std::vector<uint8_t> before = r.m.bytes;
    const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, 99, 2.0f);
    Check(s.wrote && !s.follow_pins, "a candidate at 99: written, no pins");
    Check(b::IsTagged(r.m, kMpp), "a candidate at 99 is tagged");
    Check(r.m.writes == 2 + 2 * 6 + 1, "tag 2 + two poses of 6 words + the age byte's word");
    Check(r.m.ReadU32(kMpp + 0x2C) == ((2u << 8) | 99u), "+0x2C now says age 99");
    Check(Near(r.m.F(r.down + 4), 0.0f), "down y 2 + (1 - 2) * 2");
    Check(Near(r.m.F(r.up + 4), 6.0f), "up y 2 + (4 - 2) * 2");
    Check(Near(r.m.F(r.down + 20), 4.0f) && Near(r.m.F(r.up + 12), 8.0f), "second record: down z 4, up x 8");
    Check(OnlyChanged(before, r.m, {{kMpp + 0x28, kMpp + 0x30}, {r.down, r.down + 24}, {r.up, r.up + 24}}),
          "only the tag and the two poses' records change");
    const b::AgeStep back = b::ApplyAge(r.m, kMpp, true, 18, 2.0f);
    Check(back.wrote && !back.follow_pins, "back to 18: written, no pins");
    Check(Near(r.m.F(r.down + 4), 1.0f) && Near(r.m.F(r.up + 4), 4.0f) && Near(r.m.F(r.down + 20), 5.0f) &&
              Near(r.m.F(r.up + 12), 6.0f),
          "back to 18: poses within 1e-5 of the file's");
    Check(r.m.ReadU32(kMpp + 0x2C) == ((2u << 8) | 18u), "+0x2C says age 18 again");
  }
  {
    Retail r = MakeRetail(2, 1);
    b::ApplyAge(r.m, kMpp, true, 58, 2.0f);
    // Retail: one strongest ratio for both poses, so up at 58 is 1 + 40/81 * (2 - 1) = 1.4938272 too.
    Check(Near(r.m.F(r.down + 4), 0.5061728f), "retail at 58: down y 2 + (1 - 2) * 1.4938272");
    Check(Near(r.m.F(r.up + 4), 4.9876543f), "retail at 58: up y 2 + (4 - 2) * 1.4938272");
    b::ApplyAge(r.m, kMpp, true, 9, 2.0f);
    Check(Near(r.m.F(r.down + 4), 1.5f) && Near(r.m.F(r.up + 4), 3.0f), "retail at 9 from 58: down 1.5, up 3.0");
  }
  {
    Retail r = MakeRetail(2, 1);
    b::ApplyAge(r.m, kMpp, true, 150, 2.0f);
    Check(r.m.ReadU32(kMpp + 0x2C) == ((2u << 8) | 99u), "age 150 is stored as 99");
    b::ApplyAge(r.m, kMpp, true, 0, 2.0f);
    Check(r.m.ReadU32(kMpp + 0x2C) == ((2u << 8) | 1u), "age 0 is stored as 1");
  }
  {
    // A strongest ratio of 1: ages 18..99 all mean ratio 1; the age is still recorded, the poses not rewritten.
    Retail r = MakeRetail(2, 1);
    const std::vector<uint8_t> before = r.m.bytes;
    const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, 58, 1.0f);
    Check(s.wrote && r.m.writes == 3, "full 1 at 58: the tag and the age word only");
    Check(OnlyChanged(before, r.m, {{kMpp + 0x28, kMpp + 0x30}}) && (r.m.ReadU32(kMpp + 0x2C) & 0xFF) == 58,
          "full 1 at 58: poses untouched, age 58 recorded");
  }
  {
    Mem m = Fixture();
    const b::AgeStep s = b::ApplyAge(m, kMpp, true, 99, 2.5f);
    Check(s.wrote && s.follow_pins, "a marked suit at 99: written, pins follow");
    Check(Near(m.F(kDown + 4), 0.0f) && Near(m.F(kUp + 4), 8.0f), "a marked suit uses its own header fulls, not 2.5");
    m.Reset();
    const b::AgeStep again = b::ApplyAge(m, kMpp, true, 99, 2.5f);
    Check(!again.wrote && again.follow_pins && m.writes == 0, "a marked suit again at 99: no write, pins still follow");
  }
  {
    Retail r = MakeRetail(2, 1);
    b::ApplyAge(r.m, kMpp, true, 40, 2.0f);
    bool any_pins = false;
    for (int age = 1; age <= 99; ++age) any_pins = any_pins || b::ApplyAge(r.m, kMpp, true, age, 2.0f).follow_pins;
    Check(b::IsTagged(r.m, kMpp) && !any_pins, "a tagged block never asks for pins, at any age");
  }
}

void TestSecondCallSameAgeWritesNothing() {
  Retail r = MakeRetail(2, 60);  // a long first record, so a walk shows in the read count
  Check(b::ApplyAge(r.m, kMpp, true, 99, 2.0f).wrote, "first call at 99 writes");
  r.m.Reset();
  const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, 99, 2.0f);
  Check(!s.wrote && r.m.writes == 0, "second call at the same age: no write");
  Check(r.m.reads < 20, "second call at the same age: no walk of the mapping list");
}

void TestStaleBlockRefused() {
  struct Case {
    uint32_t offset;  // from kMpp; kListWord means the second mapping word
    uint32_t value;
    const char* what;
  };
  constexpr uint32_t kListWord = 0xFFFF;
  const Case cases[] = {
      {kListWord, 0x81000020, "a tagged block whose mapping list no longer matches its count"},
      {0x00, 0, "a tagged block whose MPP magic was cleared"},
      {0x08, 0, "a tagged block no longer bound"},
      {0x10, 2, "a tagged block whose kind changed"},
      {0x14, 21, "a tagged block whose pose count changed"},
      {0x2C, (2u << 8) | 0u, "a tagged block with age 0 stored"},
      {0x2C, (2u << 8) | 100u, "a tagged block with age 100 stored"},
  };
  for (const Case& c : cases) {
    Retail r = MakeRetail(2, 1);
    b::ApplyAge(r.m, kMpp, true, 40, 2.0f);
    r.m.Fill(c.offset == kListWord ? r.list + 4 : kMpp + c.offset, c.value);
    r.m.Reset();
    bool any = false;
    for (int age = 1; age <= 99; ++age) any = any || b::ApplyAge(r.m, kMpp, true, age, 2.0f).wrote;
    Check(!any && r.m.writes == 0, c.what);
  }
  {
    Retail r = MakeRetail(2, 1);
    b::ApplyAge(r.m, kMpp, true, 40, 2.0f);
    r.m.Reset();
    Check(b::ApplyAge(r.m, kMpp, true, 41, 2.0f).wrote, "control: the same tagged block, unchanged, moves");
  }
  // A tagged block (now at 9, below the line) is not moved to any age but 18 with an unusable strongest ratio.
  const float bad[] = {0.0f, kNaN, kInf, -1.0f};
  for (float f : bad) {
    for (int age : {1, 58, 99}) {
      Retail r = MakeRetail(2, 1);
      b::ApplyAge(r.m, kMpp, true, 9, 2.0f);
      r.m.Reset();
      Check(!b::ApplyAge(r.m, kMpp, true, age, f).wrote && r.m.writes == 0, "a tagged block with a bad strongest ratio");
    }
  }
  // Ruling A16: from above 18 back to 18 an unusable strongest ratio is refused, even where the current ratio it gives
  // is positive (at 58: full 0 gives 0.506, full -1 gives 0.012). At 99 the current ratio is full itself.
  for (int from : {99, 58}) {
    for (float f : {0.0f, -1.0f, kNaN}) {
      Retail r = MakeRetail(2, 1);
      b::ApplyAge(r.m, kMpp, true, from, 2.0f);
      r.m.Reset();
      Check(!b::ApplyAge(r.m, kMpp, true, 18, f).wrote && r.m.writes == 0, "above 18 -> 18 with a bad strongest ratio");
    }
  }
  // Back to 18 from above the line needs the current ratio, which comes from retail_full: NaN or infinite gives no
  // usable current ratio, so nothing is written (no NaN poses). Below the line (from 9) full plays no part: it goes home.
  for (float f : {kNaN, kInf}) {
    Retail r = MakeRetail(2, 1);
    b::ApplyAge(r.m, kMpp, true, 58, 2.0f);
    r.m.Reset();
    Check(!b::ApplyAge(r.m, kMpp, true, 18, f).wrote && r.m.writes == 0, "58 -> 18 with no usable current ratio: refused");
    Retail low = MakeRetail(2, 1);
    b::ApplyAge(low.m, kMpp, true, 9, 2.0f);
    Check(b::ApplyAge(low.m, kMpp, true, 18, f).wrote && Near(low.m.F(low.down + 4), 1.0f),
          "9 -> 18 with a bad strongest ratio: home, full plays no part");
  }
}

// Ruling A15: a marked block is validated like a tagged one on every frame that wants a write.
void TestMarkedStaleRefused() {
  struct Case {
    uint32_t at;
    uint32_t value;
    const char* what;
  };
  const Case cases[] = {
      {kMpp + 0x00, 0, "a marked block whose MPP magic was cleared"},
      {kMpp + 0x2C, 3, "a marked block whose list walks to 2 records, 3 stored"},
      {kMarkedList + 4, 0x01000020, "a marked block whose list walks to 1 record, 2 stored"},
      {kMpp + 0x10, 2, "a marked block of kind 2"},
      {kMarkedList + 8, 0x81000040, "a marked block whose list has no end before the rest pose"},
  };
  for (const Case& c : cases) {
    for (int age : {1, 58, 99}) {
      Mem m = Fixture();
      m.Fill(c.at, c.value);
      const std::vector<uint8_t> before = m.bytes;
      Check(!b::ApplyAge(m, kMpp, true, age, 2.0f).wrote && m.writes == 0 && before == m.bytes, c.what);
    }
  }
  for (int age : {1, 58, 99}) {
    Mem m = Fixture();
    m.Fill(kMpp + 0x2C, 0);
    m.Fill(kMarkedList, 0xFFFFFFFF);  // no records stored and none walked: still no block to move
    Check(!b::Rescale(m, kMpp, age) && m.writes == 0, "a marked block with 0 records stored and an empty list");
  }
  {
    Mem m = Fixture();
    Check(b::Rescale(m, kMpp, 58), "control: the fixture itself moves");
    m.Reset();
    Check(!b::ApplyAge(m, kMpp, true, 58, 2.0f).wrote && m.writes == 0 && m.reads < 15,
          "a marked block at its age: no write and no walk");
  }
}

void TestNeverTouchesHandsOrFace() {
  for (int age : {1, 50, 99}) {
    for (bool state : {true, false}) {
      Retail r = MakeRetail(2, 1);
      r.m.Fill(kMpp + 0x10, 2);
      r.m.Fill(kMpp + 0x14, 21);
      r.m.Fill(kMpp + 0x28, r.up);  // with 21 poses, +0x28 and on are more pose pointers
      r.m.Fill(kMpp + 0x2C, r.up + 24);
      const std::vector<uint8_t> before = r.m.bytes;
      b::ApplyAge(r.m, kMpp, state, age, 2.0f);
      Check(r.m.writes == 0 && before == r.m.bytes, "a hand block (kind 2, 21 poses) is never touched");
    }
    Retail face = MakeRetail(2, 1);  // kind 0 and 3 poses on another channel: only the state tells it apart
    const std::vector<uint8_t> before = face.m.bytes;
    const b::AgeStep s = b::ApplyAge(face.m, kMpp, false, age, 2.0f);
    Check(!s.wrote && face.m.writes == 0 && before == face.m.bytes, "the face look-alike is never touched");
  }
  Retail control = MakeRetail(2, 1);
  Check(b::ApplyAge(control.m, kMpp, true, 50, 2.0f).wrote, "control: the face fixture on a bust state is moved");
}

void TestFollowPinsOnlyMarked() {
  // A tagged retail block whose mapping words at +0x40/+0x44 happen to read like a pin table (offset 0x60, 2 rows).
  Retail r = MakeRetail(2, 24);
  r.m.Fill(kMpp + 0x40, 0x00000060);
  r.m.Fill(kMpp + 0x44, 0x00000002);
  r.m.Fill(kMpp + 0x60, 0x00000100);
  r.m.Fill(kMpp + 0x88, 0x00000110);
  Check(b::ApplyAge(r.m, kMpp, true, 99, 2.0f).wrote && b::IsTagged(r.m, kMpp), "precondition: the block is tagged");
  r.m.Reset();
  b::FollowPins(r.m, kMpp, b::Blend{0.5f, 0.5f});
  Check(r.m.writes == 0, "FollowPins writes nothing for a tagged block");
  // (Review fix round 1: this test's old control, the same bytes marked DX2B, no longer drives the loop, because the pin
  // table is not where the converter puts it. TestFollowPinsValidated has the controls now.)
}

// The swimsuit fixture with a converter-style pin table: right after the up pose, 16-aligned (kUp + 2 records * 12 =
// kBase+0x318 -> kBase+0x320, i.e. +0x310 from the block), one 40-byte row whose target is kBase+0x70.
Mem FixtureWithPins() {
  Mem m = Fixture();
  m.Fill(kMpp + 0x40, 0x310);
  m.Fill(kMpp + 0x44, 1);
  const uint32_t row = kMpp + 0x310;
  m.Fill(row, 0x60);
  const float rest[] = {0.1f, 0.2f, 0.3f}, dn[] = {0.01f, 0.0f, 0.0f}, up[] = {0.0f, 0.02f, 0.0f};
  for (uint32_t c = 0; c < 3; ++c) {
    m.FillF(row + 4 + 4 * c, rest[c]);
    m.FillF(row + 16 + 4 * c, dn[c]);
    m.FillF(row + 28 + 4 * c, up[c]);
  }
  return m;
}

// Review I1: FollowPins runs every frame, so it validates the block from the header alone.
void TestFollowPinsValidated() {
  const b::Blend blend{0.5f, 0.5f};
  {
    Mem m = FixtureWithPins();
    b::FollowPins(m, kMpp, blend);
    Check(m.writes == 3 && Near(m.F(kBase + 0x70), 0.105f) && Near(m.F(kBase + 0x74), 0.21f) &&
              Near(m.F(kBase + 0x78), 0.3f),
          "control: a valid swimsuit block's pin row is written (0.1 + 0.5 * 0.01, 0.2 + 0.5 * 0.02, 0.3)");
  }
  struct Case {
    uint32_t at;
    uint32_t value;
    const char* what;
  };
  const Case refused[] = {
      {kMpp + 0x28, 0, "not marked: no pins (IsMarked)"},
      {kMpp + 0x28, b::kMagicRetail, "tagged, not marked: no pins (IsMarked)"},
      {kMpp + 0x00, 0x12345678, "MPP magic overwritten: no pins (BustShape)"},
      {kMpp + 0x10, 7, "kind 7: no pins (BustShape)"},
      {kMpp + 0x08, 0, "unbound: no pins (BustShape)"},
      {kMpp + 0x40, 0x300, "pin table not after the up pose: no pins (pin-table place)"},
      {kMpp + 0x40, 0x318, "pin table after the up pose but not 16-aligned: no pins (pin-table place)"},
      {kMpp + 0x2C, 3, "stored count disagrees with the pin table's place: no pins (pin-table place)"},
  };
  for (const Case& c : refused) {
    Mem m = FixtureWithPins();
    m.Fill(c.at, c.value);
    const std::vector<uint8_t> before = m.bytes;
    b::FollowPins(m, kMpp, blend);
    Check(m.writes == 0 && before == m.bytes, c.what);
  }
  {
    // Accepted gap: the list walks to 1 record against 2 stored, but the header is intact. FollowPins does not walk the
    // list (it runs every frame), so the pins are still written; Rescale, which walks, refuses the poses.
    Mem m = FixtureWithPins();
    m.Fill(kMarkedList + 4, 0x01000020);
    Check(!b::Rescale(m, kMpp, 58) && m.writes == 0, "list count mismatch: Rescale refuses");
    b::FollowPins(m, kMpp, blend);
    Check(m.writes == 3, "list count mismatch: FollowPins still writes (the accepted gap, documented)");
  }
}

// M1: the two refusal branches Task 2 left without a fixture.
void TestRefusalBranchesM1() {
  {
    // Every pose pointer at or below the list (one equal to it): no bound for the walk, so CountRecords stops at once.
    // The list itself still ends after two records, so a walk without the bound would count 2.
    Retail r = MakeRetail(2, 1);
    r.m.Fill(kMpp + 0x1C, r.list);
    r.m.Fill(kMpp + 0x20, r.list - 4);
    r.m.Fill(kMpp + 0x24, kMpp);
    r.m.Reset();
    Check(b::CountRecords(r.m, kMpp) == 0, "M1: no pose pointer above the list: 0 records (limit 0)");
    Check(r.m.reads == 4, "M1: limit 0 returns before reading any mapping word");
    Check(b::detail::WalkRecords(r.m, kMpp).why == b::Refusal::kNoPoseAbove,
          "M1: a pose pointer equal to the list does not bound the walk");
  }
  {
    // A tagged block whose stored count is 0 and whose list is empty: the walk (0) equals the stored count, so only
    // the records == 0 arm refuses it.
    Retail r = MakeRetail(2, 1);
    r.m.Fill(r.list, 0xFFFFFFFF);
    r.m.Fill(kMpp + 0x28, b::kMagicRetail);
    r.m.Fill(kMpp + 0x2C, 18);
    r.m.Reset();
    Check(!b::RescaleTagged(r.m, kMpp, 99, 2.0f) && r.m.writes == 0, "M1: tagged with 0 records: refused, no write");
    const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, 99, 2.0f);
    Check(!s.wrote && r.m.writes == 0 && s.refused == b::Refusal::kNone, "M1: through ApplyAge too, no reason");
  }
}

// M3: ApplyAge(m, mpp, bool bust_state, int age, float) with the bool and the int swapped does not compile.
template <class M>
concept ApplyAgeRightOrder = requires(M& m) { b::ApplyAge(m, 0u, true, 18, 2.0f); };
template <class M>
concept ApplyAgeSwapped = requires(M& m) { b::ApplyAge(m, 0u, 18, true, 2.0f); };

void TestApplyAgeArgumentOrderM3() {
  Check(ApplyAgeRightOrder<Mem>, "M3: positive control, (bool, int) compiles");
  Check(!ApplyAgeSwapped<Mem>, "M3: (int, bool) does not compile");
}

// M4: a candidate refused at an age other than 18 says why; each reason from its own fixture, with no write.
void TestRefusalReasonsM4() {
  const auto expect = [](Retail& r, float full, b::Refusal want, const char* what) {
    r.m.Reset();
    const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, 99, full);
    Check(!s.wrote && !s.follow_pins && r.m.writes == 0 && s.refused == want, what);
  };
  {
    Retail r = MakeRetail(2, 1);
    expect(r, 0.0f, b::Refusal::kBadStrongest, "M4: strongest 0 -> unusable strongest value");
    expect(r, kNaN, b::Refusal::kBadStrongest, "M4: strongest NaN -> unusable strongest value");
  }
  {
    Retail r = MakeRetail(2, 1);
    r.m.Fill(kMpp + 0x1C, r.list - 4);
    r.m.Fill(kMpp + 0x20, r.list - 8);
    r.m.Fill(kMpp + 0x24, r.list - 12);
    expect(r, 2.0f, b::Refusal::kNoPoseAbove, "M4: no pose above the list");
  }
  {
    Retail r = MakeRetail(2, 1);
    r.m.Fill(r.list, 0xFFFFFFFF);
    expect(r, 2.0f, b::Refusal::kEmptyList, "M4: an empty list -> 0 records");
  }
  {
    Retail r = MakeRetail(2, 1);
    r.m.Fill(r.list + 12, 0x01000060);
    expect(r, 2.0f, b::Refusal::kNoEnd, "M4: no end below the poses");
  }
  {
    Retail r = MakeRetail(2, b::kMaxListWords - 2);
    expect(r, 2.0f, b::Refusal::kTooManyWords, "M4: one word past the word cap");
  }
  {
    Retail r = MakeRetail(b::kMaxRecords + 1);
    expect(r, 2.0f, b::Refusal::kTooManyRecords, "M4: cap + 1 records");
  }
  {
    // No reason where nothing was wanted or nothing is a candidate.
    Retail r = MakeRetail(2, 1);
    r.m.Fill(r.list, 0xFFFFFFFF);
    Check(b::ApplyAge(r.m, kMpp, true, 18, 2.0f).refused == b::Refusal::kNone, "M4: at 18 no reason (nothing wanted)");
    Check(b::ApplyAge(r.m, kMpp, true, 18, 0.0f).refused == b::Refusal::kNone, "M4: at 18 a bad strongest: no reason");
    Check(b::ApplyAge(r.m, kMpp, false, 99, 2.0f).refused == b::Refusal::kNone, "M4: not a bust state: no reason");
    Check(b::ApplyAge(r.m, kMpp, false, 99, 0.0f).refused == b::Refusal::kNone,
          "M4: not a bust state with a bad strongest: no reason");
    Check(r.m.writes == 0, "M4: none of these writes");
  }
  {
    Retail r = MakeRetail(2, 1);
    const b::AgeStep s = b::ApplyAge(r.m, kMpp, true, 99, 2.0f);
    Check(s.wrote && s.refused == b::Refusal::kNone, "M4: a tag that succeeds has no reason");
    Mem m = Fixture();
    Check(b::ApplyAge(m, kMpp, true, 99, 2.0f).refused == b::Refusal::kNone, "M4: a marked suit has no reason");
  }
  const b::Refusal all[] = {b::Refusal::kNone,        b::Refusal::kBadStrongest, b::Refusal::kNoPoseAbove,
                            b::Refusal::kEmptyList,   b::Refusal::kNoEnd,        b::Refusal::kTooManyWords,
                            b::Refusal::kTooManyRecords};
  bool distinct = true;
  for (b::Refusal x : all) {
    for (b::Refusal y : all) distinct = distinct && (x == y || std::strcmp(b::RefusalName(x), b::RefusalName(y)) != 0);
  }
  Check(distinct, "M4: every reason has its own name for the WARN");
  Check(std::strcmp(b::RefusalName(b::Refusal::kEmptyList), "0 records") == 0 &&
            std::strcmp(b::RefusalName(b::Refusal::kBadStrongest), "unusable strongest value") == 0,
        "M4: the AM25 words for 0 records and an unusable strongest value");
}

// DOA4_BUST_AGE_MAX (route G3: one <float>), and M2: below 0.05 is dropped like <= 0 and not finite.
void TestParseStrongest() {
  struct Case {
    const char* text;
    float want;
    int dropped;
    const char* what;
  };
  const Case cases[] = {
      {"2.5", 2.5f, 0, "well-formed 2.5"},
      {"3", 3.0f, 0, "an integer"},
      {" \t3.25\r\n", 3.25f, 0, "blanks around are ignored"},
      {"", 1.75f, 0, "empty: fallback, no drop"},
      {"  ", 1.75f, 0, "blank: fallback, no drop"},
      {"abc", 1.75f, 1, "junk: dropped"},
      {"2.5x", 1.75f, 1, "trailing junk: dropped"},
      {"2.5 3", 1.75f, 1, "two numbers: dropped"},
      {"x2.5", 1.75f, 1, "leading junk: dropped"},
      {"4=2.0", 1.75f, 1, "the G1 <charId>= form is not accepted (route G3)"},
      {"0", 1.75f, 1, "0: dropped"},
      {"-2", 1.75f, 1, "negative: dropped"},
      {"nan", 1.75f, 1, "nan: dropped"},
      {"inf", 1.75f, 1, "inf: dropped"},
      {"1e40", 1.75f, 1, "out of float range: dropped"},
      {"0.0499", 1.75f, 1, "M2: 0.0499 (below 0.05): dropped"},
      {"0.01", 1.75f, 1, "M2: 0.01: dropped"},
      {"0.05", 0.05f, 0, "M2: exactly 0.05 is kept"},
      {"0.5", 0.5f, 0, "0.5 is kept (a weaker-than-retail strongest is allowed)"},
  };
  for (const Case& c : cases) {
    const b::StrongestTable t = b::ParseStrongest(c.text, 1.75f);
    Check(BitEqual(t.strongest, c.want) && t.dropped == c.dropped, c.what);
  }
  Check(BitEqual(b::ParseStrongest("", 2.0f).strongest, 2.0f), "the fallback is the caller's (2.0)");
  Check(b::kMinStrongest == 0.05f, "M2: the floor is 0.05");
}

// The strongest ratio at 99 without the test switch: 2.0 for every girl (the user's verdict A29, route G3).
void TestRetailStrongestPinned() { Check(b::kRetailStrongest == 2.0f, "the retail strongest ratio is 2.0 (A29)"); }

}  // namespace

int main() {
  TestFullScalesEachPoseByItsOwnRatio();
  TestBackToTheFilesStrength();
  TestFactorIsWantedOverCurrent();
  TestOnlyOnePoseOutOfDateStillRewrites();
  TestOnlyDownOutOfDateStillRewrites();
  TestNothingWrittenWhenAlreadyThere();
  TestRefusesEverythingElse();
  TestIsMarked();
  TestAgeRatio();
  TestConstants();
  TestEffectiveAge();
  TestEffectiveSetting();
  TestLineSetting();
  TestSettingFromText();
  TestIsBustState();
  TestCountRecords();
  TestIsRetailBustCandidate();
  TestTagRetail();
  TestTagRefuses();
  TestFullRefusedAtEveryAgeButRetail();
  TestRescaleBetweenTheLines();
  TestRoundTripTo18();
  TestRandomWalkEndsAtRetail();
  TestApplyAge();
  TestSecondCallSameAgeWritesNothing();
  TestStaleBlockRefused();
  TestMarkedStaleRefused();
  TestNeverTouchesHandsOrFace();
  TestFollowPinsOnlyMarked();
  TestFollowPinsValidated();
  TestRefusalBranchesM1();
  TestApplyAgeArgumentOrderM3();
  TestRefusalReasonsM4();
  TestParseStrongest();
  TestRetailStrongestPinned();
  Check(g_outside == 0, "no read or write outside the test memory");
  std::printf("%d checks\n", g_checks);
  if (g_fails) {
    std::printf("%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("suits_bust: all checks passed\n");
  return 0;
}
