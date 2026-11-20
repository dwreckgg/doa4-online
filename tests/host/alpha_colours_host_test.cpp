// Host test for src/doa4_alpha_colours.h: Alpha-152's colour row written around each fighter's own draw and put back
// (BeginFighterDraw / EndFighterDraw), the player -> load slot -> model lookup (SlotForPlayer, ModelForPlayer), the
// game's own "an Alpha is shown" predicate (AlphaShown, AlphaInMatch), the per-player kick-arc trail words (TrailBefore /
// TrailAfter) and the model -> row map SetUpSuits installs (RowsByModel). All on synthetic guest bytes (a map; unset bytes
// read 0); no game byte is in this file. Built and run by run_alpha_colours_test.bat. Prints "alpha_colours: N checks run"
// before its verdict line.
//
// Where the addresses come from (generated code at e4b1cf6, generated/default/dead_or_alive_4_recomp.N.cpp):
//   player record 0x83033B40 + 156 * p, +33 = the load slot (players 0, 1): sub_8234FCB0 3.cpp:23711-23721,
//     sub_8234F9A8 3.cpp:23275-23285;
//   players 2, 3: byte +8 of 0x830BC6C0 (odd p) or 0x830BCEB0 (even p): sub_8234FCB0 3.cpp:23728-23746, sub_8234F9A8
//     3.cpp:23296-23315;
//   load rows 0x830ADBEC + 44 * slot, +5 character and +6 model once loaded: src/doa4_costumes.h:230-236;
//   sub_8245D598 (shown and character 16): players 0, 1: +32 shown, +1 character (12.cpp:50323-50340); players >= 2:
//     the record 0x830BC6C0 when the word 0x831A5FC4 is 1, else 0x830BCEB0; +4 shown, +6 character (12.cpp:50343-50377);
//     true when shown != 0 and character == 16 (12.cpp:50378-50396);
//   the trail draw sub_8234C2B8(r3, packed): slot = (packed >> 4) & 15, player = packed & 15 (3.cpp:15114-15125); the slot
//     word 0x83028B6C differing from the slot -> store the slot, zero 0x8302907C and 0x82FE055C (3.cpp:15134-15151); the
//     slot's character at 0x8302BB34 + 8 * slot + 2, 2 / 4 / 16 drawn, anything else returns (3.cpp:15158-15187);
//   RefractColor 0x8357EE30 (4 floats) and its guard word 0x8357EE40, bit 0 (clrlwi 31): 12.cpp:47530-47545; outline tint
//     0x82F6A330 (4 floats): 12.cpp:47924-47990.

#include "../../src/doa4_alpha_colours.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/doa4_colour_costumes.h"
#include "../../src/doa4_costumes.h"
#include "../../src/doa4_guest.h"

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

namespace ac = doa4::alpha_colours;
namespace cc = doa4::colour_costumes;
namespace costumes = doa4::costumes;

// The addresses, written here on their own (cited above), so a changed constant in the header is a failure.
static constexpr uint32_t kRefractAt = 0x8357EE30, kGuardAt = 0x8357EE40, kOutlineAt = 0x82F6A330;
static constexpr uint32_t kSlotWord = 0x83028B6C, kPhase = 0x8302907C, kType = 0x83029078, kFade = 0x82FE055C;
static constexpr uint32_t kPlayerRec = 0x83033B40, kPlayerStride = 156;
static constexpr uint32_t kTagOdd = 0x830BC6C0, kTagEven = 0x830BCEB0, kTagWord = 0x831A5FC4;
static constexpr uint32_t kLoadRow = 0x830ADBEC, kLoadStride = 44;
static constexpr uint32_t kSlotChar = 0x8302BB34;

// Models used by the fixtures (Alpha's): 7 red, 8 blue, 9 lilac; 0 her COS. 1 (no row); 3 a decoy with no row.
static constexpr uint8_t kRed = 7, kBlue = 8, kLilac = 9, kVanilla = 0, kDecoy = 3;

struct Fake {
  std::unordered_map<uint32_t, uint8_t> mem;
  size_t writes = 0;
  doa4::GuestBytes g;
  Fake()
      : g{[this](uint32_t a) {
            const auto it = mem.find(a);
            return it == mem.end() ? uint8_t(0) : it->second;
          },
          [this](uint32_t a, uint8_t v) {
            ++writes;
            mem[a] = v;
          }} {}
  Fake(const Fake&) = delete;
  Fake& operator=(const Fake&) = delete;
  // Fixture writes go straight into the map (not counted as writes of the code under test).
  void Put8(uint32_t a, uint8_t v) { mem[a] = v; }
  void Put32(uint32_t a, uint32_t v) {
    for (uint32_t i = 0; i < 4; ++i) mem[a + i] = uint8_t(v >> (24 - 8 * i));
  }
  uint8_t Get8(uint32_t a) const {
    const auto it = mem.find(a);
    return it == mem.end() ? uint8_t(0) : it->second;
  }
  uint32_t Get32(uint32_t a) const {
    return (uint32_t(Get8(a)) << 24) | (uint32_t(Get8(a + 1)) << 16) | (uint32_t(Get8(a + 2)) << 8) | uint32_t(Get8(a + 3));
  }
  // The nine words in the order of Saved: refract 0-3, the guard word, outline 0-3.
  std::array<uint32_t, 9> Nine() const {
    std::array<uint32_t, 9> n{};
    for (uint32_t i = 0; i < 5; ++i) n[i] = Get32(kRefractAt + 4 * i);
    for (uint32_t i = 0; i < 4; ++i) n[5 + i] = Get32(kOutlineAt + 4 * i);
    return n;
  }
  void SetNine(const std::array<uint32_t, 9>& n) {
    for (uint32_t i = 0; i < 5; ++i) Put32(kRefractAt + 4 * i, n[i]);
    for (uint32_t i = 0; i < 4; ++i) Put32(kOutlineAt + 4 * i, n[5 + i]);
  }
  std::array<uint32_t, 4> Trail() const { return {Get32(kSlotWord), Get32(kPhase), Get32(kType), Get32(kFade)}; }
  void SetTrail(const std::array<uint32_t, 4>& w) {
    Put32(kSlotWord, w[0]), Put32(kPhase, w[1]), Put32(kType, w[2]), Put32(kFade, w[3]);
  }
};

// A distinctive pattern for the nine words before any Begin: not zeros, not any row's values, the guard's bit 0 CLEAR
// (Begin sets it, so restoring and forgetting to restore read differently) and its other bits set (a whole-word write
// of 1 differs from setting the bit).
static constexpr std::array<uint32_t, 9> kPattern = {0x3F1A2B3C, 0x3E4D5E6F, 0x3F708192, 0x3EA3B4C5, 0xA5A5A5A4,
                                                     0x3DD6E7F8, 0x3E09A1B2, 0x3EC3D4E5, 0x41F6A7B8};

// Players 0 and 1: the record's load slot (+33), shown (+32), character (+1); +6 holds a decoy model with a row (blue),
// for the "player record +6" mutant.
static void SetPlayer(Fake& f, uint32_t p, uint8_t slot, uint8_t shown, uint8_t character) {
  const uint32_t rec = kPlayerRec + kPlayerStride * p;
  f.Put8(rec + 33, slot);
  f.Put8(rec + 32, shown);
  f.Put8(rec + 1, character);
  f.Put8(rec + 6, kBlue);
}
// A load row: +3/+4 what is asked (+4 a decoy model with a row: lilac), +5/+6 what is loaded.
static void SetLoadRow(Fake& f, uint32_t slot, uint8_t character, uint8_t model) {
  const uint32_t row = kLoadRow + kLoadStride * slot;
  f.Put8(row + 3, character);
  f.Put8(row + 4, kLilac);
  f.Put8(row + 5, character);
  f.Put8(row + 6, model);
}
static void SetSlotCharacter(Fake& f, uint32_t slot, uint8_t character) { f.Put8(kSlotChar + 8 * slot + 2, character); }

static cc::AlphaRow Row(cc::Slot s) { return *cc::AlphaRowFor(s); }
// The nine words a fighter with row `r` sees, the guard word being `guard_before` with bit 0 set.
static std::array<uint32_t, 9> Expect(const cc::AlphaRow& r, uint32_t guard_before) {
  std::array<uint32_t, 9> n{};
  for (int i = 0; i < 4; ++i) n[i] = std::bit_cast<uint32_t>(r.refract[i]);
  n[4] = guard_before | 1u;
  for (int i = 0; i < 4; ++i) n[5 + i] = std::bit_cast<uint32_t>(r.outline[i]);
  return n;
}
static void CheckNine(const Fake& f, const std::array<uint32_t, 9>& want, int line) {
  const std::array<uint32_t, 9> got = f.Nine();
  for (int i = 0; i < 9; ++i) {
    ++g_checks;
    if (got[i] != want[i]) {
      std::printf("FAIL %s:%d: word %d is %08X, want %08X\n", __FILE__, line, i, got[i], want[i]);
      std::fflush(stdout);
      ++g_failures;
    }
  }
}
#define CHECK_NINE(f, want) CheckNine((f), (want), __LINE__)

static ac::Rows ColourRows(bool installed) {
  ac::Rows rows;
  rows.by_model[kRed] = Row(cc::Slot::kAlphaRed);
  rows.by_model[kBlue] = Row(cc::Slot::kAlphaBlue);
  rows.by_model[kLilac] = Row(cc::Slot::kAlphaLilac);
  rows.installed = installed;
  return rows;
}

// ---- Begin / End ------------------------------------------------------------------------------------------------------

static void TestBeginEndRestores() {
  const ac::Rows rows = ColourRows(true);
  for (const uint32_t guard : {0xA5A5A5A4u, 0x5A5A5A5Bu}) {  // bit 0 clear, then already set
    Fake f;
    std::array<uint32_t, 9> before = kPattern;
    before[4] = guard;
    f.SetNine(before);
    SetPlayer(f, 0, 1, 1, 16);
    SetLoadRow(f, 1, 16, kRed);
    const ac::Saved s = ac::BeginFighterDraw(f.g, 0, rows);
    CHECK(s.valid);
    CHECK(s.slot == 1 && s.model == kRed);
    CHECK_NINE(f, Expect(Row(cc::Slot::kAlphaRed), guard));
    // Literal pins of red's row, independent of the float conversion above: 1.4f, 0.4f, 10.0f.
    CHECK(f.Get32(kRefractAt) == 0x3FB33333u);
    CHECK(f.Get32(kOutlineAt) == 0x3ECCCCCDu);
    CHECK(f.Get32(kOutlineAt + 12) == 0x41200000u);
    CHECK(f.Get32(kGuardAt) == (guard | 1u));
    for (int i = 0; i < 4; ++i) CHECK(s.refract[i] == before[i]);
    CHECK(s.refract[4] == guard);
    for (int i = 0; i < 4; ++i) CHECK(s.outline[i] == before[5 + i]);
    ac::EndFighterDraw(f.g, s);
    CHECK_NINE(f, before);  // all nine back, the guard word too
  }
}

static void TestTwoFighters() {
  const ac::Rows rows = ColourRows(true);
  {  // T7-R3: fighter 0 red, fighter 1 her COS. 1 (no row): fighter 1 sees the words from before fighter 0's Begin
    Fake f;
    f.SetNine(kPattern);
    SetPlayer(f, 0, 0, 1, 16);
    SetLoadRow(f, 0, 16, kRed);
    SetPlayer(f, 1, 1, 1, 16);
    SetLoadRow(f, 1, 16, kVanilla);
    const ac::Saved s0 = ac::BeginFighterDraw(f.g, 0, rows);
    CHECK(s0.valid);
    CHECK_NINE(f, Expect(Row(cc::Slot::kAlphaRed), kPattern[4]));
    ac::EndFighterDraw(f.g, s0);
    CHECK_NINE(f, kPattern);
    const size_t writes = f.writes;
    const ac::Saved s1 = ac::BeginFighterDraw(f.g, 1, rows);
    CHECK(!s1.valid);
    CHECK(s1.slot == 1 && s1.model == kVanilla);
    CHECK(f.writes == writes);
    CHECK_NINE(f, kPattern);  // NOT AlphaVanilla(): the game's own words
    ac::EndFighterDraw(f.g, s1);
    CHECK(f.writes == writes);
    CHECK_NINE(f, kPattern);
  }
  {  // red against blue, two rounds: each draw sees her own row
    Fake f;
    f.SetNine(kPattern);
    SetPlayer(f, 0, 0, 1, 16);
    SetLoadRow(f, 0, 16, kRed);
    SetPlayer(f, 1, 1, 1, 16);
    SetLoadRow(f, 1, 16, kBlue);
    for (int round = 0; round < 2; ++round) {
      const ac::Saved s0 = ac::BeginFighterDraw(f.g, 0, rows);
      CHECK_NINE(f, Expect(Row(cc::Slot::kAlphaRed), kPattern[4]));
      ac::EndFighterDraw(f.g, s0);
      const ac::Saved s1 = ac::BeginFighterDraw(f.g, 1, rows);
      CHECK_NINE(f, Expect(Row(cc::Slot::kAlphaBlue), kPattern[4]));
      CHECK(f.Get32(kOutlineAt) == 0x3D23D70Au);  // blue's outline A (0.04), ledger T2-R1
      ac::EndFighterDraw(f.g, s1);
      CHECK_NINE(f, kPattern);
    }
  }
}

static void TestNotInstalledNoWrite() {
  // The rows hold a row for her model: only the installed flag decides (#0759).
  const ac::Rows rows = ColourRows(false);
  Fake f;
  f.SetNine(kPattern);
  SetPlayer(f, 0, 0, 1, 16);
  SetLoadRow(f, 0, 16, kRed);
  const ac::Saved s = ac::BeginFighterDraw(f.g, 0, rows);
  CHECK(!s.valid);
  CHECK(f.writes == 0);
  CHECK_NINE(f, kPattern);
  // End with a Saved that is not valid writes nothing, even when it holds words that differ from the guest's.
  ac::Saved stale = s;
  for (int i = 0; i < 5; ++i) stale.refract[i] = 0x11111111u * uint32_t(i + 1);
  for (int i = 0; i < 4; ++i) stale.outline[i] = 0x01010101u * uint32_t(i + 1);
  ac::EndFighterDraw(f.g, stale);
  CHECK(f.writes == 0);
  CHECK_NINE(f, kPattern);
}

static void TestBeginGuards() {
  const ac::Rows rows = ColourRows(true);
  {  // the load row's loaded character is not hers: her model number means another character's model
    Fake f;
    f.SetNine(kPattern);
    SetPlayer(f, 0, 0, 1, 16);
    SetLoadRow(f, 0, 3, kRed);
    const ac::Saved s = ac::BeginFighterDraw(f.g, 0, rows);
    CHECK(!s.valid);
    CHECK(f.writes == 0);
  }
  {  // a slot past the four load rows: no model (the byte past the table holds a model with a row)
    Fake f;
    f.SetNine(kPattern);
    SetPlayer(f, 0, 4, 1, 16);
    SetLoadRow(f, 4, 16, kRed);
    CHECK(ac::ModelForPlayer(f.g, 0) == ac::kNoModel);
    const ac::Saved s = ac::BeginFighterDraw(f.g, 0, rows);
    CHECK(!s.valid);
    CHECK(f.writes == 0);
  }
  {  // control: the same fighter with her loaded character 16 and slot 0 is coloured
    Fake f;
    f.SetNine(kPattern);
    SetPlayer(f, 0, 0, 1, 16);
    SetLoadRow(f, 0, 16, kRed);
    CHECK(ac::BeginFighterDraw(f.g, 0, rows).valid);
  }
}

// ---- player -> slot -> model --------------------------------------------------------------------------------------

static void TestModelForPlayer() {
  Fake f;
  // Players 0, 1 by +33; their records' +6 hold a decoy (SetPlayer: blue), the load rows' +4 another (lilac).
  SetPlayer(f, 0, 2, 1, 16);
  SetLoadRow(f, 2, 16, kRed);
  SetPlayer(f, 1, 3, 1, 16);
  SetLoadRow(f, 3, 16, kDecoy);
  CHECK(ac::SlotForPlayer(f.g, 0) == 2);
  CHECK(ac::ModelForPlayer(f.g, 0) == kRed);
  CHECK(ac::SlotForPlayer(f.g, 1) == 3);
  CHECK(ac::ModelForPlayer(f.g, 1) == kDecoy);
  // Players 2, 3: +8 of 0x830BCEB0 (even) / 0x830BC6C0 (odd), whatever the word 0x831A5FC4 says (that word picks the
  // record for sub_8245D598, not for the slot); +33 of their 0x83033B40 records holds a decoy slot.
  f.Put8(kTagEven + 8, 1);
  f.Put8(kTagOdd + 8, 0);
  f.Put8(kPlayerRec + kPlayerStride * 2 + 33, 3);
  f.Put8(kPlayerRec + kPlayerStride * 3 + 33, 2);
  SetLoadRow(f, 1, 16, kBlue);
  SetLoadRow(f, 0, 16, kLilac);
  for (const uint32_t word : {1u, 0u}) {
    f.Put32(kTagWord, word);
    CHECK(ac::SlotForPlayer(f.g, 2) == 1);
    CHECK(ac::ModelForPlayer(f.g, 2) == kBlue);
    CHECK(ac::SlotForPlayer(f.g, 3) == 0);
    CHECK(ac::ModelForPlayer(f.g, 3) == kLilac);
  }
  // A slot of 4 or more has no load row: kNoModel, outside 0..255.
  f.Put8(kPlayerRec + 33, 4);
  SetLoadRow(f, 4, 16, kRed);
  CHECK(ac::SlotForPlayer(f.g, 0) == 4);
  CHECK(ac::ModelForPlayer(f.g, 0) == ac::kNoModel);
  CHECK(ac::kNoModel >= 256);
}

static void TestAlphaInMatch() {
  {  // none shown
    Fake f;
    CHECK(!ac::AlphaInMatch(f.g));
  }
  for (uint32_t p = 0; p < 2; ++p) {  // player 0 or 1 alone
    Fake f;
    SetPlayer(f, p, 0, 1, 16);
    CHECK(ac::AlphaShown(f.g, p));
    CHECK(!ac::AlphaShown(f.g, 1 - p));
    CHECK(ac::AlphaInMatch(f.g));
  }
  {  // player 2 alone: the word 0x831A5FC4 == 1 picks 0x830BC6C0 (+4 shown, +6 character)
    Fake f;
    f.Put32(kTagWord, 1);
    f.Put8(kTagOdd + 4, 1);
    f.Put8(kTagOdd + 6, 16);
    CHECK(ac::AlphaShown(f.g, 2));
    CHECK(ac::AlphaInMatch(f.g));
    f.Put32(kTagWord, 0);  // now 0x830BCEB0, which is empty
    CHECK(!ac::AlphaShown(f.g, 2));
    CHECK(!ac::AlphaInMatch(f.g));
    f.Put8(kTagEven + 4, 1);
    f.Put8(kTagEven + 6, 16);
    CHECK(ac::AlphaShown(f.g, 2));
    CHECK(ac::AlphaInMatch(f.g));
  }
  {  // shown but not character 16
    Fake f;
    SetPlayer(f, 0, 0, 1, 2);
    SetPlayer(f, 1, 1, 1, 4);
    CHECK(!ac::AlphaInMatch(f.g));
  }
  {  // character 16 but not shown (players 0, 1 and the Tag record): the shown flag decides (#0759)
    Fake f;
    SetPlayer(f, 0, 0, 0, 16);
    SetPlayer(f, 1, 1, 0, 16);
    f.Put32(kTagWord, 1);
    f.Put8(kTagOdd + 6, 16);
    CHECK(!ac::AlphaInMatch(f.g));
  }
}

// ---- trails ---------------------------------------------------------------------------------------------------------

// The retail draw's use of the four words (3.cpp:15134-15151): a slot word that differs from the call's slot is set to
// it and the phase and fade are zeroed. Then this stand-in advances them the way a drawn arc does: the phase by 1, the
// type to the player's mark, the fade by 2.
static void FakeDraw(Fake& f, uint32_t packed) {
  const uint32_t slot = (packed >> 4) & 15, player = packed & 15;
  if (f.Get32(kSlotWord) != slot) {
    f.Put32(kSlotWord, slot);
    f.Put32(kPhase, 0);
    f.Put32(kFade, 0);
  }
  f.Put32(kPhase, f.Get32(kPhase) + 1);
  f.Put32(kType, 0x100 + player);
  f.Put32(kFade, f.Get32(kFade) + 2);
}
static uint32_t Packed(uint32_t player, uint32_t slot) { return (slot << 4) | player; }
static std::array<uint32_t, 4> W(const ac::TrailState& t) { return {t.w[0], t.w[1], t.w[2], t.w[3]}; }
// One hooked call: TrailBefore, the words at the draw's entry, the draw, TrailAfter.
static std::array<uint32_t, 4> Call(Fake& f, uint32_t packed, std::array<ac::TrailState, 4>& per, bool installed) {
  ac::TrailBefore(f.g, packed, per, installed);
  const std::array<uint32_t, 4> entry = f.Trail();
  FakeDraw(f, packed);
  ac::TrailAfter(f.g, packed, per, installed);
  return entry;
}

static void AlternateAndCheck(Fake& f, int line) {
  std::array<ac::TrailState, 4> per{};
  for (uint32_t round = 0; round < 3; ++round) {
    for (uint32_t p = 0; p < 2; ++p) {
      const std::array<uint32_t, 4> entry = Call(f, Packed(p, p), per, true);
      if (round == 0) continue;  // the first call of each starts from whatever is there (retail)
      ++g_checks;
      if (entry[0] != p || entry[1] != round || entry[2] != 0x100 + p || entry[3] != 2 * round) {
        std::printf("FAIL %s:%d: round %u player %u entered with %08X %08X %08X %08X\n", __FILE__, line, round, p,
                    entry[0], entry[1], entry[2], entry[3]);
        std::fflush(stdout);
        ++g_failures;
      }
    }
  }
}

static void TestTrailPerPlayer() {
  Fake f;
  SetSlotCharacter(f, 0, 16);
  SetSlotCharacter(f, 1, 16);
  SetPlayer(f, 0, 0, 1, 16);
  SetPlayer(f, 1, 1, 1, 16);
  f.SetTrail({0, 0, 0, 0});
  AlternateAndCheck(f, __LINE__);
}

static void TestTrailBesideAlpha() {
  for (const uint8_t other : {uint8_t(2), uint8_t(4)}) {  // a character-2 or -4 fighter (player 1) beside an Alpha
    Fake f;
    SetSlotCharacter(f, 0, 16);
    SetSlotCharacter(f, 1, other);
    SetPlayer(f, 0, 0, 1, 16);
    SetPlayer(f, 1, 1, 1, other);
    f.SetTrail({0, 0, 0, 0});
    AlternateAndCheck(f, __LINE__);
  }
}

static void TestTrailOtherKind() {
  for (const uint8_t kind : {uint8_t(7), uint8_t(18)}) {  // a character that is not 2, 4 or 16 (18: Spartan-458)
    Fake f;
    SetSlotCharacter(f, 2, kind);
    SetPlayer(f, 0, 0, 1, 16);  // an Alpha is shown
    std::array<ac::TrailState, 4> per{};
    per[0] = {true, {2, 50, 3, 9}};  // a copy that would match the call (its slot is 2)
    f.SetTrail({5, 6, 7, 8});
    ac::TrailBefore(f.g, Packed(0, 2), per, true);
    CHECK(f.writes == 0);
    CHECK((f.Trail() == std::array<uint32_t, 4>{5, 6, 7, 8}));
    FakeDraw(f, Packed(0, 2));
    ac::TrailAfter(f.g, Packed(0, 2), per, true);
    CHECK(per[0].valid);
    CHECK((W(per[0]) ==std::array<uint32_t, 4>{2, 50, 3, 9}));
    ac::TrailAfter(f.g, Packed(1, 2), per, true);
    CHECK(!per[1].valid);
  }
}

static void TestTrailResetDropsCopies() {
  for (const bool reset : {true, false}) {
    Fake f;
    SetSlotCharacter(f, 0, 16);
    SetSlotCharacter(f, 1, 16);
    SetSlotCharacter(f, 2, 7);
    SetPlayer(f, 0, 0, 1, 16);
    std::array<ac::TrailState, 4> per{};
    per[0] = {true, {0, 5, 1, 7}};
    per[1] = {true, {1, 6, 2, 8}};
    f.SetTrail({reset ? 0xFFFFFFFFu : 1u, 0, 0x33, 0});
    ac::TrailBefore(f.g, Packed(0, 0), per, true);
    if (reset) {  // the game reset the words: nothing written back, every copy dropped
      CHECK(f.writes == 0);
      CHECK((f.Trail() == std::array<uint32_t, 4>{0xFFFFFFFFu, 0, 0x33, 0}));
      CHECK(!per[0].valid && !per[1].valid);
    } else {  // control: no reset, so player 0's copy goes in
      CHECK((f.Trail() == std::array<uint32_t, 4>{0, 5, 1, 7}));
      CHECK(per[0].valid && per[1].valid);
    }
  }
  {  // the reset is seen before deciding: a call that is not kept (character 7) drops the copies too
    Fake f;
    SetSlotCharacter(f, 2, 7);
    SetPlayer(f, 0, 0, 1, 16);
    std::array<ac::TrailState, 4> per{};
    per[1] = {true, {1, 6, 2, 8}};
    f.SetTrail({0xFFFFFFFFu, 0, 0, 0});
    ac::TrailBefore(f.g, Packed(0, 2), per, true);
    CHECK(!per[1].valid);
  }
}

static void TestTrailNoAlphaInMatch() {
  Fake f;
  SetSlotCharacter(f, 0, 2);
  SetSlotCharacter(f, 1, 4);
  SetPlayer(f, 0, 0, 1, 2);
  SetPlayer(f, 1, 1, 1, 4);
  f.Put32(kTagWord, 1);
  f.Put8(kTagOdd + 6, 16);  // a character-16 Tag record that is not shown
  std::array<ac::TrailState, 4> per{};
  per[0] = {true, {0, 5, 1, 7}};
  per[1] = {true, {1, 6, 2, 8}};
  f.SetTrail({1, 9, 9, 9});
  ac::TrailBefore(f.g, Packed(0, 0), per, true);
  CHECK(f.writes == 0);
  FakeDraw(f, Packed(0, 0));
  const size_t writes = f.writes;
  ac::TrailAfter(f.g, Packed(0, 0), per, true);
  CHECK(f.writes == writes);
  CHECK((W(per[0]) ==std::array<uint32_t, 4>{0, 5, 1, 7}));
  CHECK((W(per[1]) ==std::array<uint32_t, 4>{1, 6, 2, 8}));
}

static void TestTrailPlayerPast3() {  // a packed player of 4 or more has no copy: nothing read, written or kept
  Fake f;
  SetSlotCharacter(f, 0, 16);
  SetPlayer(f, 0, 0, 1, 16);
  std::array<ac::TrailState, 4> per{};
  for (uint32_t p = 0; p < 4; ++p) per[p] = {true, {0, 5 + p, 1, 7}};
  f.SetTrail({1, 9, 9, 9});
  ac::TrailBefore(f.g, Packed(4, 0), per, true);
  CHECK(f.writes == 0);
  CHECK(!ac::TrailKept(f.g, Packed(4, 0), true));
  CHECK(ac::TrailKept(f.g, Packed(0, 0), true));  // control: player 0 on the same slot is kept
}

static void TestTrailNotInstalled() {
  Fake f;
  SetSlotCharacter(f, 0, 16);
  SetPlayer(f, 0, 0, 1, 16);
  std::array<ac::TrailState, 4> per{};
  per[0] = {true, {0, 5, 1, 7}};
  f.SetTrail({1, 9, 9, 9});
  ac::TrailBefore(f.g, Packed(0, 0), per, false);
  CHECK(f.writes == 0);
  FakeDraw(f, Packed(0, 0));
  ac::TrailAfter(f.g, Packed(0, 0), per, false);
  CHECK((W(per[0]) ==std::array<uint32_t, 4>{0, 5, 1, 7}));
}

// ---- the installed map ------------------------------------------------------------------------------------------------

static void TestRowsByModel() {
  std::vector<costumes::Applied> applied;
  applied.push_back({16, kRed, 1, "colour alpha_red", true});
  applied.push_back({16, 10, 3, "colour alpha_blue", false});
  applied.push_back({16, 11, 4, "colour alpha_lilac", false});
  applied.push_back({18, 12, 7, "colour spartan_h3_red", false});
  applied.push_back({16, 13, 5, "a swimsuit", false});
  applied.push_back({18, kBlue, 8, "colour alpha_blue", false});  // Alpha's label on another character: no row
  const ac::RowMap m = ac::RowsByModel(applied);
  int rows = 0;
  for (const auto& r : m.by_model) rows += r.has_value() ? 1 : 0;
  CHECK(rows == 3);
  CHECK(m.by_model[kRed].has_value() && m.by_model[kRed]->refract[0] == 1.4f && m.by_model[kRed]->outline[0] == 0.4f);
  CHECK(m.by_model[10].has_value() && m.by_model[10]->refract[2] == 2.55f && m.by_model[10]->outline[0] == 0.04f);
  CHECK(m.by_model[11].has_value() && m.by_model[11]->refract[1] == 0.6f && m.by_model[11]->outline[1] == 0.065f);
  CHECK(!m.by_model[kVanilla].has_value());
  CHECK(!m.by_model[12].has_value() && !m.by_model[13].has_value() && !m.by_model[kBlue].has_value());
  CHECK(m.names[kRed] == "alpha_red" && m.names[10] == "alpha_blue" && m.names[11] == "alpha_lilac");
  CHECK(m.names[12].empty() && m.names[kVanilla].empty());
}

// ---- the process-wide state (the hooks' entry points) -------------------------------------------------------------------

static void TestGlobalNotInstalled() {  // runs first: nothing installed yet in this process
  CHECK(!ac::Installed());
  Fake f;
  f.SetNine(kPattern);
  SetPlayer(f, 0, 0, 1, 16);
  SetLoadRow(f, 0, 16, kRed);
  SetSlotCharacter(f, 0, 16);
  const ac::Saved s = ac::BeginFighterDraw(f.g, 0);
  CHECK(!s.valid);
  std::array<ac::TrailState, 4> per{};
  per[0] = {true, {0, 5, 1, 7}};
  f.SetTrail({1, 9, 9, 9});
  ac::TrailBefore(f.g, Packed(0, 0), per);
  ac::TrailAfter(f.g, Packed(0, 0), per);
  CHECK(f.writes == 0);
  CHECK((W(per[0]) ==std::array<uint32_t, 4>{0, 5, 1, 7}));
}

static void TestGlobalInstalled() {
  const ac::RowMap m = ac::RowsByModel({{16, kRed, 1, "colour alpha_red", true}});
  ac::Install(m.by_model, m.names);
  CHECK(ac::Installed());
  CHECK(ac::RowName(kRed) == "alpha_red");
  CHECK(ac::RowName(kVanilla).empty());
  CHECK(ac::RowName(ac::kNoModel).empty());
  Fake f;
  f.SetNine(kPattern);
  SetPlayer(f, 0, 0, 1, 16);
  SetLoadRow(f, 0, 16, kRed);
  SetSlotCharacter(f, 0, 16);
  const ac::Saved s = ac::BeginFighterDraw(f.g, 0);
  CHECK(s.valid);
  CHECK_NINE(f, Expect(Row(cc::Slot::kAlphaRed), kPattern[4]));
  ac::EndFighterDraw(f.g, s);
  CHECK_NINE(f, kPattern);
  std::array<ac::TrailState, 4> per{};
  f.SetTrail({0, 4, 0x100, 8});
  ac::TrailBefore(f.g, Packed(0, 0), per);
  FakeDraw(f, Packed(0, 0));
  ac::TrailAfter(f.g, Packed(0, 0), per);
  CHECK(per[0].valid);
  CHECK((W(per[0]) ==std::array<uint32_t, 4>{0, 5, 0x100, 10}));
}

int main() {
  TestGlobalNotInstalled();
  TestBeginEndRestores();
  TestTwoFighters();
  TestNotInstalledNoWrite();
  TestBeginGuards();
  TestModelForPlayer();
  TestAlphaInMatch();
  TestTrailPerPlayer();
  TestTrailBesideAlpha();
  TestTrailOtherKind();
  TestTrailResetDropsCopies();
  TestTrailNoAlphaInMatch();
  TestTrailPlayerPast3();
  TestTrailNotInstalled();
  TestRowsByModel();
  TestGlobalInstalled();
  std::printf("alpha_colours: %d checks run\n", g_checks);
  if (g_failures != 0) {
    std::printf("FAILED: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }
  std::printf("PASSED: %d checks, 0 failures\n", g_checks);
  return 0;
}
