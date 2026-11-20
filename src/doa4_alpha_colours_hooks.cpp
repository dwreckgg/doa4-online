// Alpha-152's colour per fighter and the per-player kick-arc trail: the hooks (what they do and why: doa4_alpha_colours.h).
//
// Two hooks. Without the colour slots installed (every netplay session, every replay, an offline process whose colour
// files are not installed) each is one flag test in front of the game's own function.
//   sub_8245C270  her body and outline composite, one call per shown Alpha per frame. The game reaches it only by the
//                 direct call in sub_8245D538 (from sub_8245D718, the frame renderer's effect pass; no pointer to it
//                 exists). Around it: her row in (BeginFighterDraw), the game's function, the nine words back
//                 (EndFighterDraw). The select panel calls the function through __imp__ (doa4_alpha_select_hooks.cpp),
//                 which bypasses this hook; it does its own Begin/End.
//   sub_8234C2B8  the queued kick-arc trail draw. Around it: the player's four trail words in and back out (TrailBefore,
//                 TrailAfter), only for a character 2/4/16 slot while an Alpha is shown.
// Both run on the thread that draws the frame (the frame renderer sub_824F4878). The per-player trail copies and the
// per-player "last row" below are process-global and touched only from that thread.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "doa4_alpha_colours.h"
#include "doa4_guest.h"

extern "C" {
REX_EXTERN(__imp__sub_8245C270);
REX_EXTERN(__imp__sub_8234C2B8);
}

namespace {
namespace ac = doa4::alpha_colours;

doa4::GuestBytes Guest(uint8_t* base) {
  return doa4::GuestBytes{[base](uint32_t address) { return base[address]; },
                          [base](uint32_t address, uint8_t value) { base[address] = value; }};
}

// The trail draw's four words per player (the draw thread only).
std::array<ac::TrailState, 4> g_trail{};

// Per player, the (slot, model) of the last row line written: 0xFFFFFFFF = none yet.
std::atomic<uint32_t> g_last_row[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
}  // namespace

namespace doa4::alpha_colours {
// One info line when a player's (slot, model) changes, never per frame: the row the draw uses from now on.
void NoteFighterRow(uint32_t player, const Saved& s) {
  if (player >= 4) return;
  const uint32_t key = (s.slot << 16) | (s.model & 0xFFFF);
  if (g_last_row[player].exchange(key, std::memory_order_relaxed) == key) return;
  const std::string name = RowName(s.model);
  REXLOG_INFO("[doa4][COLOURS] player {}: slot {} model {}: {}", player, s.slot, s.model,
              s.valid ? "colour row " + name
                      : (name.empty() ? std::string("no colour row (the game's own colours)")
                                      : "colour row " + name + " not written (her slot's loaded character is not 16)"));
}
}  // namespace doa4::alpha_colours

REX_HOOK_RAW(sub_8245C270) {
  if (!ac::Installed()) {
    __imp__sub_8245C270(ctx, base);
    return;
  }
  const doa4::GuestBytes guest = Guest(base);
  const uint32_t player = ctx.r3.u32;
  const ac::Saved saved = ac::BeginFighterDraw(guest, player);
  ac::NoteFighterRow(player, saved);
  __imp__sub_8245C270(ctx, base);
  ac::EndFighterDraw(guest, saved);
}

REX_HOOK_RAW(sub_8234C2B8) {
  if (!ac::Installed()) {
    __imp__sub_8234C2B8(ctx, base);
    return;
  }
  const doa4::GuestBytes guest = Guest(base);
  const uint32_t packed = ctx.r4.u32;  // b << 8 | slot << 4 | player (r4 does not survive the call)
  ac::TrailBefore(guest, packed, g_trail);
  __imp__sub_8234C2B8(ctx, base);
  ac::TrailAfter(guest, packed, g_trail);
}
