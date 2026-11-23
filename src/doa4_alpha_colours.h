// Alpha-152's colour per fighter (the colour costumes, doa4_colour_costumes.h), her outline,
// and her kick-arc trail when two trail fighters share one set of trail words.
//
// Two of her colours are not in her files but in two single globals that only her body-and-composite draw sub_8245C270
// reads (study notes/research/2026-11-20-alpha-two-fighter-outline.md, Q1 and Q6): her body "RefractColor" (4 floats at
// kRefract, used only while bit 0 of the guard word kRefract + 16 is set; clear, the game builds its own and sets the bit)
// and the outline tint (4 floats at kOutline). That draw runs once per shown Alpha per frame, and the outline composite is
// drawn inside it from her own glow buffers, so writing her row just before it and putting the nine words back right after
// it gives each fighter her own body colour and outline (BeginFighterDraw / EndFighterDraw). The row is chosen by the MODEL
// her load slot has loaded (plan D3): each colour costume has its own model record. A model with no row (her COS. 1, a
// swimsuit, anything not installed) gets nothing written: the game's own words, as in retail (ruling T7-R3).
//
// The trail: the queued draw sub_8234C2B8(r3, packed = b << 8 | slot << 4 | player) keeps four single words (kTrailWords:
// the slot it last drew, a phase, a type, a fade); a call whose slot differs from the slot word zeroes the phase and the
// fade. With two trail fighters in one match each call meets the other's slot and the arc is lost. TrailBefore /
// TrailAfter keep the four words per player, only for a slot whose character uses that draw (2, 4 or 16) and only while
// an Alpha is shown (AlphaInMatch, the game's own predicate sub_8245D598); every match without Alpha stays retail. Every
// game reset of the words writes 0xFFFFFFFF into the slot word (sub_8234A9E0, sub_8234AAA8, sub_8234C678; the draw itself
// stores only a slot 0-15 there), so a call that finds it drops every player's copy first and starts from the reset words.
//
// The writes are gated on Installed(): her rows are installed when the colour entries are overlaid and the colour rows
// are in the tables (SetUpSuits), in every kind of process: an online session, a record run and a replay too, where
// both PCs choose the row by the model the fighter loaded. A process without them is untouched. Because a session
// hashes guest memory every frame, the sites that write also check which thread they run on (the hook thread check,
// at the end of this file).
//
// Header only (the host test, tests/host/alpha_colours_host_test.cpp, needs no SDK); the hooks are in
// doa4_alpha_colours_hooks.cpp and the select panel's in doa4_alpha_select_hooks.cpp. Addresses: see the host test's
// header for the generated-code lines each comes from.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "doa4_colour_costumes.h"
#include "doa4_costumes.h"
#include "doa4_guest.h"

namespace doa4::alpha_colours {

inline constexpr uint32_t kRefract = 0x8357EE30;  // 4 be floats, guard word at +16 (bit 0)
inline constexpr uint32_t kRefractGuard = kRefract + 16;
inline constexpr uint32_t kOutline = 0x82F6A330;  // 4 be floats
inline constexpr uint32_t kTrailWords[4] = {0x83028B6C, 0x8302907C, 0x83029078, 0x82FE055C};
inline constexpr uint32_t kTrailReset = 0xFFFFFFFF;  // the slot word after any of the game's resets
// The slot's character (+ 8 * slot + 2): the trail draw tests it (sub_8234C2B8) and the aura pass builds her aura on it.
inline constexpr uint32_t kSlotCharacter = 0x8302BB34;
inline constexpr uint32_t kSlotCharacterStride = 8, kSlotCharacterOffset = 2;
// Players 0 and 1: one record each, 156 bytes; +1 character, +32 shown, +33 the load slot her draws use.
inline constexpr uint32_t kPlayers = 0x83033B40, kPlayerSize = 156;
inline constexpr uint32_t kPlayerCharacter = 1, kPlayerShown = 32, kPlayerSlot = 33;
// Players 2 and 3 (Tag): the slot is +8 of kTagOdd (odd player) or kTagEven (even player) (sub_8234FCB0, sub_8234F9A8);
// sub_8245D598 instead picks kTagOdd when the word kTagPick is 1, else kTagEven, and reads +4 shown, +6 character.
inline constexpr uint32_t kTagOdd = 0x830BC6C0, kTagEven = 0x830BCEB0, kTagPick = 0x831A5FC4;
inline constexpr uint32_t kTagSlot = 8, kTagShown = 4, kTagCharacter = 6;
inline constexpr uint8_t kAlphaCharacter = 16;
// The load rows (doa4_costumes.h): +5 the character and +6 the model once loaded.
inline constexpr uint32_t kRowCharacter = 5, kRowModel = 6;
inline constexpr uint32_t kNoModel = 0x100;  // ModelForPlayer for a slot with no load row: outside every model index

// Install's input: per model index, her colour row (and the slot's name for the log), or none.
using ByModel = std::array<std::optional<colour_costumes::AlphaRow>, 256>;
struct RowMap {
  ByModel by_model{};
  std::array<std::string, 256> names{};
};
struct Rows : RowMap {
  bool installed = false;
};

// The models of Alpha's applied colour extras (character 16, label "colour <slot name>" of one of her slots) -> their rows
// (colour_costumes::AlphaRowFor); every other model has none (ruling T7-R3).
inline RowMap RowsByModel(const std::vector<costumes::Applied>& applied) {
  RowMap m;
  for (const costumes::Applied& a : applied) {
    if (a.character != kAlphaCharacter || a.model >= 256) continue;
    for (const colour_costumes::SlotDef& d : colour_costumes::Slots()) {
      if (d.character != kAlphaCharacter || a.label != std::string("colour ") + d.name) continue;
      const std::optional<colour_costumes::AlphaRow> row = colour_costumes::AlphaRowFor(d.slot);
      if (!row) continue;
      m.by_model[a.model] = row;
      m.names[a.model] = d.name;
    }
  }
  return m;
}

namespace detail {
// The process's rows: written once by Install at start-up (before any guest code runs), read by the draw hooks.
inline Rows& State() {
  static Rows rows;
  return rows;
}
inline std::atomic<bool>& InstalledFlag() {
  static std::atomic<bool> flag{false};
  return flag;
}
inline void Put32(const GuestBytes& g, uint32_t a, uint32_t v) {
  for (uint32_t i = 0; i < 4; ++i) g.Write(a + i, uint8_t(v >> (24 - 8 * i)));
}
}  // namespace detail

// SetUpSuits, after Apply, only when the colour entries were installed and their rows are in the tables.
inline void Install(const ByModel& by_model, const std::array<std::string, 256>& names = {}) {
  Rows& s = detail::State();
  s.by_model = by_model;
  s.names = names;
  s.installed = true;
  detail::InstalledFlag().store(true, std::memory_order_release);
}
inline bool Installed() { return detail::InstalledFlag().load(std::memory_order_acquire); }
// The slot name of a model's row ("alpha_red"), empty for a model with none.
inline std::string RowName(uint32_t model) {
  return Installed() && model < 256 ? detail::State().names[model] : std::string();
}

// The load slot player `p`'s draws use, read as sub_8234FCB0 / sub_8234F9A8 read it.
inline uint32_t SlotForPlayer(const GuestBytes& g, uint32_t p) {
  if (p < 2) return g.Read(kPlayers + kPlayerSize * p + kPlayerSlot);
  return g.Read(((p & 1) == 1 ? kTagOdd : kTagEven) + kTagSlot);
}
// The model her slot has loaded (the load row's +6), or kNoModel for a slot with no load row.
inline uint32_t ModelForPlayer(const GuestBytes& g, uint32_t p) {
  const uint32_t slot = SlotForPlayer(g, p);
  if (slot >= costumes::kLoadSlotCount) return kNoModel;
  return g.Read(costumes::kLoadSlots + costumes::kLoadSlotBytes * slot + kRowModel);
}

// sub_8245D598(p): player p is shown and is character 16.
inline bool AlphaShown(const GuestBytes& g, uint32_t p) {
  uint8_t shown, character;
  if (p < 2) {
    const uint32_t rec = kPlayers + kPlayerSize * p;
    shown = g.Read(rec + kPlayerShown);
    character = g.Read(rec + kPlayerCharacter);
  } else {
    const uint32_t rec = g.ReadBe32(kTagPick) == 1 ? kTagOdd : kTagEven;
    shown = g.Read(rec + kTagShown);
    character = g.Read(rec + kTagCharacter);
  }
  return shown != 0 && character == kAlphaCharacter;
}
// An Alpha is shown: sub_8245D598 holds for one of the three players sub_8245D718 draws her pass for (0, 1, 2).
inline bool AlphaInMatch(const GuestBytes& g) { return AlphaShown(g, 0) || AlphaShown(g, 1) || AlphaShown(g, 2); }

// ---- her row around one fighter's draw ----

struct Saved {
  uint32_t refract[5];  // RefractColor's 4 words and the guard word, as they were
  uint32_t outline[4];
  bool valid = false;               // something was written: End puts the nine words back
  uint32_t slot = 0, model = kNoModel;  // what Begin read (for the hooks' log line)
};

// Writes the row of player p's model (refract 4 floats, the guard's bit 0 set, outline 4 floats) and returns the nine
// words as they were. Writes nothing (valid false) when not installed, when her slot has no load row, when that row's
// loaded character is not 16 (the model number would be another character's), or when the model has no row.
inline Saved BeginFighterDraw(const GuestBytes& g, uint32_t player, const Rows& rows) {
  Saved s{};
  s.slot = SlotForPlayer(g, player);
  s.model = ModelForPlayer(g, player);
  if (!rows.installed || s.model >= 256 || !rows.by_model[s.model]) return s;
  if (g.Read(costumes::kLoadSlots + costumes::kLoadSlotBytes * s.slot + kRowCharacter) != kAlphaCharacter) return s;
  const colour_costumes::AlphaRow& row = *rows.by_model[s.model];
  for (uint32_t i = 0; i < 5; ++i) s.refract[i] = g.ReadBe32(kRefract + 4 * i);
  for (uint32_t i = 0; i < 4; ++i) s.outline[i] = g.ReadBe32(kOutline + 4 * i);
  for (uint32_t i = 0; i < 4; ++i) detail::Put32(g, kRefract + 4 * i, std::bit_cast<uint32_t>(row.refract[i]));
  detail::Put32(g, kRefractGuard, s.refract[4] | 1u);
  for (uint32_t i = 0; i < 4; ++i) detail::Put32(g, kOutline + 4 * i, std::bit_cast<uint32_t>(row.outline[i]));
  s.valid = true;
  return s;
}
inline Saved BeginFighterDraw(const GuestBytes& g, uint32_t player) {
  if (!Installed()) return BeginFighterDraw(g, player, Rows{});
  return BeginFighterDraw(g, player, detail::State());
}
// Puts the nine words back (nothing when Begin wrote nothing).
inline void EndFighterDraw(const GuestBytes& g, const Saved& s) {
  if (!s.valid) return;
  for (uint32_t i = 0; i < 5; ++i) detail::Put32(g, kRefract + 4 * i, s.refract[i]);
  for (uint32_t i = 0; i < 4; ++i) detail::Put32(g, kOutline + 4 * i, s.outline[i]);
}

// ---- the trail words per player ----

struct TrailState {
  bool valid = false;
  uint32_t w[4]{};
};

// Whether the call's four words are kept per player: installed, a player 0-3, a slot whose character uses the trail draw
// (2, 4 or 16), and an Alpha shown.
inline bool TrailKept(const GuestBytes& g, uint32_t packed, bool installed) {
  const uint32_t player = packed & 15, slot = (packed >> 4) & 15;
  if (!installed || player >= 4) return false;
  const uint8_t character = g.Read(kSlotCharacter + kSlotCharacterStride * slot + kSlotCharacterOffset);
  if (character != 2 && character != 4 && character != kAlphaCharacter) return false;
  return AlphaInMatch(g);
}
// Before the draw: a reset slot word drops every copy; then, for a kept call, the player's copy (if any) goes in.
inline void TrailBefore(const GuestBytes& g, uint32_t packed, std::array<TrailState, 4>& per_player, bool installed) {
  if (g.ReadBe32(kTrailWords[0]) == kTrailReset) {
    for (TrailState& t : per_player) t.valid = false;
  }
  if (!TrailKept(g, packed, installed)) return;
  const TrailState& t = per_player[packed & 15];
  if (!t.valid) return;
  for (uint32_t i = 0; i < 4; ++i) detail::Put32(g, kTrailWords[i], t.w[i]);
}
// After it: for a kept call, the four words become the player's copy.
inline void TrailAfter(const GuestBytes& g, uint32_t packed, std::array<TrailState, 4>& per_player, bool installed) {
  if (!TrailKept(g, packed, installed)) return;
  TrailState& t = per_player[packed & 15];
  for (uint32_t i = 0; i < 4; ++i) t.w[i] = g.ReadBe32(kTrailWords[i]);
  t.valid = true;
}
inline void TrailBefore(const GuestBytes& g, uint32_t packed, std::array<TrailState, 4>& per_player) {
  TrailBefore(g, packed, per_player, Installed());
}
inline void TrailAfter(const GuestBytes& g, uint32_t packed, std::array<TrailState, 4>& per_player) {
  TrailAfter(g, packed, per_player, Installed());
}

// The hooks' log line when a player's row changes (defined in doa4_alpha_colours_hooks.cpp; the select hooks call it too).
void NoteFighterRow(uint32_t player, const Saved& s);

// ---- the hook thread check ----
// The session's sync check hashes guest memory in the frame hook (np::OnFrame). The words the three sites below write
// (the body hook, the trail hook, the select panel's pass) are safe from it only while each site runs on the frame
// hook's thread: then a write and its putting back happen between two hashes. So each site, at every call, compares its
// thread with the frame hook's and says so in the log: once that it is the same thread, and once, at whatever later
// call it first happens, that it is not. Log only: the hook goes on either way.

enum class HookThread : uint8_t { kUnknown, kFrameThread, kOtherThread };
// frame_thread 0: no frame hook has run in this process yet (np::FrameHookThread), so nothing can be judged.
inline HookThread JudgeHookThread(uint64_t frame_thread, uint64_t this_thread) {
  if (frame_thread == 0) return HookThread::kUnknown;
  return frame_thread == this_thread ? HookThread::kFrameThread : HookThread::kOtherThread;
}
// The calling thread's number: 1, 2, 3, ... in the order threads first ask, the same at every call on one thread, never
// 0, never given to two threads of the process. The frame hook and the three sites all take their thread's from here.
inline uint64_t ThreadNumber() {
  static std::atomic<uint64_t> next{1};
  thread_local const uint64_t mine = next.fetch_add(1, std::memory_order_relaxed);
  return mine;
}
// One site's two "already said" marks (one object per site, for the whole process).
struct HookSite {
  std::atomic<bool> frame_thread_noted{false};
  std::atomic<bool> sync_risk_noted{false};
};
// What the site has to log at this call: nothing while the frame hook's thread is unknown; the first time it is this
// thread, kFrameThread; the first time it is another, kSyncRisk; nothing at every other call. The site's own thread
// number is read here, so no caller can pass the frame hook's in its place.
enum class HookNote : uint8_t { kNone, kFrameThread, kSyncRisk };
inline HookNote NoteHookSite(HookSite& site, uint64_t frame_thread) {
  switch (JudgeHookThread(frame_thread, ThreadNumber())) {
    case HookThread::kUnknown:
      break;
    case HookThread::kFrameThread:
      if (!site.frame_thread_noted.load(std::memory_order_relaxed) &&
          !site.frame_thread_noted.exchange(true, std::memory_order_relaxed)) {
        return HookNote::kFrameThread;
      }
      break;
    case HookThread::kOtherThread:
      if (!site.sync_risk_noted.load(std::memory_order_relaxed) &&
          !site.sync_risk_noted.exchange(true, std::memory_order_relaxed)) {
        return HookNote::kSyncRisk;
      }
      break;
  }
  return HookNote::kNone;
}
// The site's check and its log line (defined in doa4_alpha_colours_hooks.cpp; the select hooks call it too): the frame
// hook's thread number from np::FrameHookThread, then "[doa4][COLOURS] hook thread check: <name> on the frame hook's
// thread (frame <n>, hook <n>)" or the WARN "[doa4][COLOURS] SYNC-RISK: <name> ran on another thread than the frame
// hook (frame <n>, hook <n>)".
void CheckHookThread(const char* name, HookSite& site);

// How often the body hook (sub_8245C270's) was called in this process: one count per shown Alpha per drawn frame. The
// session log prints it beside the frame hook's count ("[doa4][COLOURS] draws <n> frames <m>", np_session.cpp), which
// shows whether a PC drew fewer fighter frames than it simulated.
inline std::atomic<uint64_t>& BodyDraws() {
  static std::atomic<uint64_t> draws{0};
  return draws;
}

}  // namespace doa4::alpha_colours
