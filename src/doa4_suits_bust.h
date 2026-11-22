// doa4_suits_bust.h
// Live strength of a bust morph, set in the ESC menu by the "Age:" box and the slider below it (1-99, 18 = the file's
// own strength). Box off: age 18 for every block (retail DOA4); box on: the slider's age.
//
// The game writes a morph's poses straight into the vertices as absolute positions, so the strength is in the pose
// data: a pose moves about the rest pose, pose = rest + (pose - rest) * wanted / current. AgeRatio maps an age to a
// ratio: below 18, age / 18; from 18 to 99, the line from 1 to the block's full ratio.
//
// Every MPP block starts with "MPP\0" (kMagicMpp, checked byte by byte by the binder sub_82374E98,
// generated/default/dead_or_alive_4_recomp.5.cpp:18143-18165). The binder makes +0x18 and the pose pointers absolute and
// sets +0x08 = 1; it reads nothing at +0x28..+0x2F, so marks there survive loading. Two kinds of block are moved:
//
// Converted DOAX2 suits ("DX2B", kMagic, written by the converter). Header:
//   +0x08 1 once bound; +0x14 pose count (rest, down, up); +0x1C/+0x20/+0x24 absolute pose addresses, 12 bytes a record
//   +0x28 kMagic; +0x2C record count
//   +0x30 / +0x34 float: the down pose's ratio now, and its ratio at the full DOAX2 range (its age-99 ratio)
//   +0x38 / +0x3C float: the same for the up pose
//   +0x40 pin block, from the block; +0x44 pin count. A chest cloth's pinned edge follows the morph (DOAX2 shirts do this; DOA4 has no such mechanism).
//   A row is 40 bytes: s32 from the block to the cloth's FIX_GRID rest position, then rest, down and up as 3 floats each.
// DOAX2's up target is about twice its down target and DOA4's is not, so the two poses have their own ratios. A ratio
// is relative to the strength baked into the file, which is 1.
//
// Retail bust blocks ("D4AG", kMagicRetail), tagged here the first time a frame wants an age other than 18. The retail
// header is 0x30 bytes: +0x30 on is the game's vertex mapping list (+0x18 points at it), so only +0x28..+0x2F are free
// (zero in every retail block of the census). The tag:
//   +0x28 kMagicRetail, written last; +0x2C (record count << 8) | current age (1..99).
// The current ratio is not stored: it is AgeRatio(current age, retail_full), with retail_full the per-girl strongest
// ratio the caller passes (the same for both poses, constant for a run). The record count is the mapping list's
// (words with bit 31) + 1, walked from +0x18 to 0xFFFFFFFF below the lowest pose pointer, at most kMaxRecords. A morph
// state can keep a stale MPP pointer after a costume reload, so every write, on either kind of block and to the pins,
// re-checks "MPP\0", +0x08, kind and pose count. A pose write also needs the walked count to equal the stored one
// (+0x2C for a converted suit, +0x2C >> 8 for a tagged block); the walk runs only on a frame that wants a pose write.
// FollowPins runs every frame and does not walk: it checks the pin table's place instead (see FollowPins).
// FollowPins stays "DX2B" only: in a retail block +0x40/+0x44 are mapping words, not a pin table.
//
// Pure code over a big-endian memory accessor (ReadU32 / WriteU32), so host tests run it without the game. The test
// switch DOA4_BUST_AGE_MAX=<float> (the retail strongest ratio) is parsed here too (ParseStrongest), and the "Age:"
// box's rule (LineSetting on this line, over Part A's EffectiveSetting; SettingFromText for a session's hand-off text;
// WantedAge: box off = age 18 for every block).
#pragma once

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace doa4::suits::bust {

inline constexpr uint32_t kMagic = 0x44583242;        // "DX2B"
inline constexpr uint32_t kMagicRetail = 0x44344147;  // "D4AG"
inline constexpr uint32_t kMagicMpp = 0x4D505000;     // "MPP\0"
static_assert(kMagicRetail != kMagic);

inline constexpr int kAgeMin = 1, kAgeRetail = 18, kAgeMax = 99;
inline constexpr uint32_t kMaxRecords = 16384;  // census: retail 193-2605, DOA2U 501-912, converted up to 5250
// Mapping words walked at most, the terminator included: census maximum 9652 words in a block
// (physics_survey/bust_age_census.txt, field cnt); 4 x 9652 = 38608 is below 65536, so 65536.
inline constexpr uint32_t kMaxListWords = 65536;

// The four channel-4 morph states, 116 bytes apart (one per fighter slot); only these run the bust's morph.
inline constexpr uint32_t kBustStateFirst = 0x8302E5A8, kBustStateSize = 116, kBustStates = 4;

namespace detail {
inline bool Usable(float ratio) { return std::isfinite(ratio) && ratio > 0.0f; }
inline int ClampAge(int age) { return age < kAgeMin ? kAgeMin : age > kAgeMax ? kAgeMax : age; }
}  // namespace detail

// The slider's raw value to the slider's age (EffectiveSetting; the hook applies it only while the box is on). Below 1
// means never saved (the default is 0): 18, or 99 when the old doa4_suits_full_bust toggle is on.
inline int EffectiveAge(int32_t raw, bool legacy_full) {
  if (raw < kAgeMin) return legacy_full ? kAgeMax : kAgeRetail;
  if (raw > kAgeMax) return kAgeMax;
  return raw;
}

// The "Age:" box and the slider as the menu shows them; `age` is always 1..99.
struct AgeSetting {
  bool on;
  int age;
  bool operator==(const AgeSetting&) const = default;
};

// The saved settings to what the menu shows: raw doa4_bust_age, box doa4_bust_age_on (never saved reads as false),
// legacy_full the old doa4_suits_full_bust (Part A's rule; on 0.3.7's line it is retired: LineSetting passes false).
// Until the first save (raw below 1) the old toggle on shows the box ON at 99; a save writes both keys, so after it the
// box alone decides.
inline AgeSetting EffectiveSetting(int32_t raw_age, bool box, bool legacy_full) {
  const bool on = raw_age < kAgeMin ? box || legacy_full : box;
  return {on, EffectiveAge(raw_age, legacy_full)};
}

// This line's rule (0.3.7, ruling B1): the old full-bust setting is retired, so the box alone decides, from the first
// run: never saved reads as the box off at 18. Every reader on this line (the hook, the menu) calls this, never
// EffectiveSetting.
inline AgeSetting LineSetting(int saved_age, bool box) {
  return EffectiveSetting(saved_age, box, /*legacy_full=*/false);
}

// The offline reader's rule (AgeWanted outside a session): in vanilla mode (doa4::vanilla::Running(), passed in so this
// header stays free of it) retail movement, the box off at 18, whatever is saved; else LineSetting. The saved values are
// only read, never changed: turning vanilla mode off shows them again.
inline AgeSetting OfflineSetting(int saved_age, bool box, bool vanilla) {
  if (vanilla) return {false, kAgeRetail};
  return LineSetting(saved_age, box);
}

// A session's box and age from the host's hand-off text (np::SessionBust; a replay's from its recording). Age: a whole
// decimal int32 (from_chars over the whole text: no sign '+', no spaces, no suffix); box: exactly "true" or "false" (the
// cvar registry's text). Both valid: LineSetting. A missing line (either nullopt: a recording made before the box, on
// 0.3.7) is the game's own, box off at 18, silently. Anything else: box off at 18, and *note names each ignored value.
// *note is cleared first. Pure, like doa4::language::ResolveText.
inline AgeSetting SettingFromText(const std::optional<std::string>& age, const std::optional<std::string>& box,
                                  std::string* note) {
  note->clear();
  constexpr AgeSetting kGameOwn{false, kAgeRetail};
  if (!age || !box) return kGameOwn;
  int32_t raw = 0;
  const char* const end = age->data() + age->size();
  const auto [stop, ec] = std::from_chars(age->data(), end, raw);
  const bool age_ok = ec == std::errc() && stop == end;
  const bool box_ok = *box == "true" || *box == "false";
  if (age_ok && box_ok) return LineSetting(raw, *box == "true");
  if (!age_ok) *note = "ignored doa4_bust_age \"" + *age + "\" (not a whole number)";
  if (!box_ok) {
    if (!note->empty()) *note += "; ";
    *note += "ignored doa4_bust_age_on \"" + *box + "\" (not true or false)";
  }
  return kGameOwn;
}

// The age the hook wants: the slider's with the box on, retail DOA4 (18) for every block with it off.
inline int WantedAge(AgeSetting s) { return s.on ? s.age : kAgeRetail; }

// The pose ratio for an age, with `full` the ratio at 99. 18 gives exactly 1 and 99 exactly `full`.
inline float AgeRatio(int age, float full) {
  age = detail::ClampAge(age);
  if (age <= kAgeRetail) {
    if (age == kAgeRetail) return 1.0f;
    return age / 18.0f;
  }
  if (age == kAgeMax) return full;
  return 1.0f + (age - 18) / 81.0f * (full - 1.0f);
}

inline bool IsBustState(uint32_t state) {
  const uint32_t d = state - kBustStateFirst;
  return d < kBustStates * kBustStateSize && d % kBustStateSize == 0;
}

template <class Mem>
bool IsMarked(const Mem& m, uint32_t mpp) {
  return m.ReadU32(mpp + 0x28) == kMagic;
}

template <class Mem>
bool IsTagged(const Mem& m, uint32_t mpp) {
  return m.ReadU32(mpp + 0x28) == kMagicRetail;
}

namespace detail {
// A bound bust-shaped MPP block: "MPP\0", bound, kind 0, 3 poses. Retail and converted blocks alike.
template <class Mem>
bool BustShape(const Mem& m, uint32_t mpp) {
  return m.ReadU32(mpp + 0x00) == kMagicMpp && m.ReadU32(mpp + 0x08) == 1 && m.ReadU32(mpp + 0x10) == 0 &&
         m.ReadU32(mpp + 0x14) == 3;
}

template <class Mem>
bool Untagged(const Mem& m, uint32_t mpp) {
  return BustShape(m, mpp) && m.ReadU32(mpp + 0x28) == 0 && m.ReadU32(mpp + 0x2C) == 0;
}

// Moves the down and up poses about the rest pose by wanted / current, each pose by its own pair.
template <class Mem>
void MovePoses(Mem& m, uint32_t mpp, uint32_t records, const float current[2], const float wanted[2]) {
  const uint32_t rest = m.ReadU32(mpp + 0x1C);
  const uint32_t slots[2] = {0x20, 0x24};
  for (int k = 0; k < 2; ++k) {
    const float factor = wanted[k] / current[k];
    const uint32_t pose = m.ReadU32(mpp + slots[k]);
    for (uint32_t i = 0; i < records * 3; ++i) {
      const float r = std::bit_cast<float>(m.ReadU32(rest + 4 * i));
      const float p = std::bit_cast<float>(m.ReadU32(pose + 4 * i));
      m.WriteU32(pose + 4 * i, std::bit_cast<uint32_t>(r + (p - r) * factor));
    }
  }
}
}  // namespace detail

// An untagged retail block the bust's morph state points at. `bust_state` is IsBustState(the morph state's address),
// from the hook: kind 0 with 3 poses alone also matches a face block on another channel.
template <class Mem>
bool IsRetailBustCandidate(const Mem& m, uint32_t mpp, bool bust_state) {
  return bust_state && detail::Untagged(m, mpp);
}

// Why ApplyAge left a retail candidate untagged at an age other than 18 (the hook's capped WARN names it).
enum class Refusal : uint8_t {
  kNone,
  kBadStrongest,    // retail_full not finite or not above 0
  kNoPoseAbove,     // no pose pointer above the mapping list, so no bound for the walk
  kEmptyList,       // the list's first word is the end: 0 records
  kNoEnd,           // no 0xFFFFFFFF below the lowest pose pointer above the list
  kTooManyWords,    // the walk passed kMaxListWords words
  kTooManyRecords,  // more than kMaxRecords records
};

inline const char* RefusalName(Refusal r) {
  switch (r) {
    case Refusal::kNone: return "none";
    case Refusal::kBadStrongest: return "unusable strongest value";
    case Refusal::kNoPoseAbove: return "no pose above the mapping list";
    case Refusal::kEmptyList: return "0 records";
    case Refusal::kNoEnd: return "no end below the poses";
    case Refusal::kTooManyWords: return "no end within the word cap";
    case Refusal::kTooManyRecords: return "more records than the cap";
  }
  return "unknown";
}

namespace detail {
struct Walk {
  uint32_t records;  // 0 when refused
  Refusal why;
};

// The walk behind CountRecords, with the reason for a 0.
template <class Mem>
Walk WalkRecords(const Mem& m, uint32_t mpp) {
  const uint32_t list = m.ReadU32(mpp + 0x18);
  uint32_t limit = 0;
  for (uint32_t k = 0; k < 3; ++k) {
    const uint32_t p = m.ReadU32(mpp + 0x1C + 4 * k);
    if (p > list && (limit == 0 || p < limit)) limit = p;
  }
  if (limit == 0) return {0, Refusal::kNoPoseAbove};
  uint32_t records = 1;
  uint32_t words = 0;
  for (uint32_t a = list; limit - a >= 4; a += 4) {
    if (++words > kMaxListWords) return {0, Refusal::kTooManyWords};
    const uint32_t w = m.ReadU32(a);
    if (w == 0xFFFFFFFF) return a == list ? Walk{0, Refusal::kEmptyList} : Walk{records, Refusal::kNone};
    if (w & 0x80000000u) ++records;
    if (records > kMaxRecords) return {0, Refusal::kTooManyRecords};
  }
  return {0, Refusal::kNoEnd};
}

// +0x2C = records << 8 | 18, then +0x28 = kMagicRetail. The caller has checked the block and walked its list.
template <class Mem>
void WriteTag(Mem& m, uint32_t mpp, uint32_t records) {
  m.WriteU32(mpp + 0x2C, records << 8 | uint32_t(kAgeRetail));
  m.WriteU32(mpp + 0x28, kMagicRetail);  // last: a block reads as tagged only once its count is in place
}
}  // namespace detail

// Records of a bound block: (mapping words with bit 31) + 1, from +0x18 to 0xFFFFFFFF. 0 when the list is empty, has no
// end below the lowest pose pointer above it, holds more than kMaxRecords, or runs past kMaxListWords words.
template <class Mem>
uint32_t CountRecords(const Mem& m, uint32_t mpp) {
  return detail::WalkRecords(m, mpp).records;
}

// Tags an untagged retail block at age 18: +0x2C = records << 8 | 18, then +0x28 = kMagicRetail. Two writes, the poses
// untouched. True when written.
template <class Mem>
bool TagRetail(Mem& m, uint32_t mpp) {
  if (!detail::Untagged(m, mpp)) return false;
  const uint32_t records = CountRecords(m, mpp);
  if (records == 0) return false;
  detail::WriteTag(m, mpp, records);
  return true;
}

// Moves a marked ("DX2B"), bound block's down and up poses to this age, each pose to AgeRatio(age, its full ratio).
// When a write is wanted, the block must still walk to its stored count (a stale pointer after a reload is refused;
// the converter writes one mapping word per record, bit 31 on all but the first, so its blocks pass).
// True if it wrote anything.
template <class Mem>
bool Rescale(Mem& m, uint32_t mpp, int age) {
  if (!IsMarked(m, mpp) || !detail::BustShape(m, mpp)) return false;
  const uint32_t records = m.ReadU32(mpp + 0x2C);
  if (records == 0) return false;
  struct Ratio {
    uint32_t now_at;
    uint32_t full_at;
  };
  const Ratio ratios[2] = {{0x30, 0x34}, {0x38, 0x3C}};
  const bool at_retail = detail::ClampAge(age) == kAgeRetail;  // at 18 the full ratio plays no part
  float current[2], wanted[2];
  for (int k = 0; k < 2; ++k) {
    current[k] = std::bit_cast<float>(m.ReadU32(mpp + ratios[k].now_at));
    const float full = std::bit_cast<float>(m.ReadU32(mpp + ratios[k].full_at));
    if (!at_retail && !detail::Usable(full)) return false;
    wanted[k] = AgeRatio(age, full);  // usable whenever full is: 1, age / 18, or between 1 and full
    if (!detail::Usable(current[k])) return false;
  }
  if (current[0] == wanted[0] && current[1] == wanted[1]) return false;
  if (CountRecords(m, mpp) != records) return false;  // walked only when a write is wanted
  detail::MovePoses(m, mpp, records, current, wanted);
  for (int k = 0; k < 2; ++k) m.WriteU32(mpp + ratios[k].now_at, std::bit_cast<uint32_t>(wanted[k]));
  return true;
}

// A bool would convert to age 0 or 1; the old toggle call must not compile.
template <class Mem>
bool Rescale(Mem&, uint32_t, bool) = delete;

// Moves a tagged retail block from its stored age to this one, both poses at AgeRatio(age, retail_full), then stores
// the new age. Refuses (no write) a block that no longer has the bust's shape or whose list walk disagrees with the
// stored count. The walk runs only when the age changes. True if it wrote anything.
template <class Mem>
bool RescaleTagged(Mem& m, uint32_t mpp, int age, float retail_full) {
  if (!IsTagged(m, mpp) || !detail::BustShape(m, mpp)) return false;
  const uint32_t tag = m.ReadU32(mpp + 0x2C);
  const int now = int(tag & 0xFF);
  const int want = detail::ClampAge(age);
  if (now < kAgeMin || now > kAgeMax || now == want) return false;
  // retail_full plays a part unless the move is from at or below 18 to 18 (ruling A16: also refuse from above 18).
  if (want != kAgeRetail && !detail::Usable(retail_full)) return false;
  if (now > kAgeRetail && !detail::Usable(retail_full)) return false;
  // Both ratios are usable here: either retail_full is (1, age / 18, or between 1 and full), or the move is from at or
  // below 18 to 18 (now / 18 or 1).
  const float current = AgeRatio(now, retail_full), wanted = AgeRatio(want, retail_full);
  const uint32_t records = CountRecords(m, mpp);
  if (records == 0 || records != tag >> 8) return false;
  if (current != wanted) {
    const float c[2] = {current, current}, w[2] = {wanted, wanted};
    detail::MovePoses(m, mpp, records, c, w);
  }
  m.WriteU32(mpp + 0x2C, records << 8 | uint32_t(want));
  return true;
}

struct AgeStep {
  bool wrote;
  bool follow_pins;                     // the caller runs FollowPins (converted suits only)
  Refusal refused = Refusal::kNone;     // set only for a candidate the age wanted tagged and that could not be
};

// The hook's one call per bust morph run. Marked: Rescale, pins follow. Tagged: RescaleTagged. An untagged candidate
// is tagged only when the age is not 18 and retail_full is usable, then moved; otherwise `refused` says why. Anything
// else: no write, no reason.
template <class Mem>
AgeStep ApplyAge(Mem& m, uint32_t mpp, bool bust_state, int age, float retail_full) {
  if (IsMarked(m, mpp)) return {Rescale(m, mpp, age), true};
  if (IsTagged(m, mpp)) return {RescaleTagged(m, mpp, age, retail_full), false};
  if (detail::ClampAge(age) == kAgeRetail || !IsRetailBustCandidate(m, mpp, bust_state)) return {false, false};
  if (!detail::Usable(retail_full)) return {false, false, Refusal::kBadStrongest};
  const detail::Walk walk = detail::WalkRecords(m, mpp);
  if (walk.records == 0) return {false, false, walk.why};
  detail::WriteTag(m, mpp, walk.records);
  RescaleTagged(m, mpp, age, retail_full);
  return {true, false};
}

// The age and the bust-state flag swapped (an int where the bool goes and back) must not compile.
template <class Mem>
AgeStep ApplyAge(Mem&, uint32_t, int, bool, float) = delete;

// DOA4_BUST_AGE_MAX=<float>: the retail strongest ratio (one value for every girl, route G3). Anything else in the text
// (junk, a second number, a value not finite or below kMinStrongest) keeps the fallback and counts one drop. Empty or
// blank text keeps the fallback with no drop.
inline constexpr float kMinStrongest = 0.05f;

// The retail strongest ratio (age 99) without the test switch: 2.0 for every girl (the user's verdict A29).
inline constexpr float kRetailStrongest = 2.0f;

struct StrongestTable {
  float strongest;
  int dropped = 0;
};

inline StrongestTable ParseStrongest(std::string_view text, float fallback) {
  StrongestTable out{fallback};
  const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && blank(text.back())) text.remove_suffix(1);
  if (text.empty()) return out;
  float v = 0.0f;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || end != text.data() + text.size() || !std::isfinite(v) || !(v >= kMinStrongest)) {
    out.dropped = 1;
    return out;
  }
  out.strongest = v;
  return out;
}

// The morph state's blend, in pose weights (pose 0 rest, 1 down, 2 up), as sub_82373E50 hands it to its six appliers.
struct Blend {
  float down = 0.0f;
  float up = 0.0f;
};

// state: +12/+16/+20 pose numbers (-1 none), +44/+48/+52 weights, +76 count of a longer list at the same places.
template <class Mem>
Blend ReadBlend(const Mem& m, uint32_t state) {
  Blend out;
  const auto pose = [&](int i) { return static_cast<int32_t>(m.ReadU32(state + 12 + 4 * i)); };
  const auto weight = [&](int i) { return std::bit_cast<float>(m.ReadU32(state + 44 + 4 * i)); };
  const auto add = [&](int p, float w) {
    if (p == 1) out.down += w;
    if (p == 2) out.up += w;
  };
  const int32_t list = static_cast<int32_t>(m.ReadU32(state + 76));
  if (list > 0) {
    for (int i = 0; i < list && i < 3; ++i) add(pose(i), weight(i));
    return out;
  }
  if (pose(0) >= 0 && pose(0) == pose(1) && weight(2) == 0.0f) {  // one pose, drawn whole
    add(pose(0), 1.0f);
    return out;
  }
  for (int i = 0; i < 3; ++i) {
    if (pose(i) >= 0) add(pose(i), weight(i));
  }
  return out;
}

// Writes the chest cloth's pin positions for this blend. The age's strength applies to them too. "DX2B" blocks only.
// It runs every frame, so it checks the header without walking the list: the block's shape ("MPP\0", bound, kind 0,
// 3 poses), and that the pin table sits where the converter puts it, right after the up pose, 16-aligned
// (physics_port.py build_mpv), with +0x2C records. A stale block whose header is intact but whose mapping list differs
// still gets its pins written (accepted gap: only the walk Rescale runs on a write frame would see it).
template <class Mem>
void FollowPins(Mem& m, uint32_t mpp, const Blend& blend) {
  if (!IsMarked(m, mpp) || !detail::BustShape(m, mpp)) return;
  const uint32_t block = m.ReadU32(mpp + 0x40);
  const uint32_t count = m.ReadU32(mpp + 0x44);
  if (block == 0 || count == 0 || count > 64) return;
  const uint32_t up_end = m.ReadU32(mpp + 0x24) - mpp + m.ReadU32(mpp + 0x2C) * 12;
  if (block != ((up_end + 15u) & ~15u)) return;
  const float dn_now = std::bit_cast<float>(m.ReadU32(mpp + 0x30));
  const float up_now = std::bit_cast<float>(m.ReadU32(mpp + 0x38));
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t row = mpp + block + 40 * i;
    const uint32_t target = mpp + static_cast<uint32_t>(static_cast<int32_t>(m.ReadU32(row)));
    for (uint32_t c = 0; c < 3; ++c) {
      const float rest = std::bit_cast<float>(m.ReadU32(row + 4 + 4 * c));
      const float dn = std::bit_cast<float>(m.ReadU32(row + 16 + 4 * c));
      const float up = std::bit_cast<float>(m.ReadU32(row + 28 + 4 * c));
      m.WriteU32(target + 4 * c, std::bit_cast<uint32_t>(rest + blend.down * dn_now * dn + blend.up * up_now * up));
    }
  }
}

}  // namespace doa4::suits::bust
