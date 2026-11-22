// doa4_vanilla.h
// The Extras tab's "Vanilla DoA4 content only" box (cvar doa4_vanilla_only, saved, off by default). While it is on, a
// LOBBY process (an offline game) shows retail DOA4 content only: the load-time apply of the six tweak boxes, the raise of
// the save's costume counts, the Age box's offline reader and the costume tables' colour and suit rows each test
// Running(), and the menu greys the covered rows with kOffText. It is a start-up latch: SetUpExtras latches the saved
// value once (LatchValue: a match process ignores the setting entirely), and a change in the menu applies at the next
// start. Vanilla mode never WRITES a covered setting, neither the live value nor the settings file; the hand-off reads
// the live values, so a host in vanilla mode carries its own boxes and Age. It is not one of tweaks::AllTweaks() nor a
// carried port setting: it is never sent.
#pragma once

#include <atomic>

#include "doa4_colour_make.h"

namespace doa4::vanilla {

// The note beside every row vanilla mode greys out, and the menu's status when a box is refused.
inline constexpr const char* kOffText = "off: vanilla mode";

namespace detail {
// Read by the Age hook on the morph worker threads: atomics. Latch is called once, on the UI thread, before any of them runs.
inline std::atomic<bool> g_latched{false};
inline std::atomic<bool> g_running{false};
}  // namespace detail

// What a process latches: the saved box in a lobby process, off in a match process (an online session, a record run, a
// replay) whatever is saved.
inline bool LatchValue(colour_costumes::ProcessKind kind, bool saved) {
  return kind == colour_costumes::ProcessKind::kLobby && saved;
}

// The FIRST call sets the value; every later call changes nothing (the setting applies at the next start).
inline void Latch(bool on) {
  bool expected = false;
  if (detail::g_latched.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    detail::g_running.store(on, std::memory_order_release);
  }
}

// The latched value; false before Latch (a reader that runs before it sees the non-vanilla state).
inline bool Running() { return detail::g_running.load(std::memory_order_acquire); }

// Whether the menu refuses to turn a tweak box to `on`: exactly when turning one ON in vanilla mode. Turning one off (the
// wipe) is never refused.
inline bool RefusesTweak(bool on) { return on && Running(); }

// Back to "never latched". For the host tests; the tree's test seams are not guarded (doa4_updater.h ResetForTest), and
// no product code calls it.
inline void ResetForTest() {
  detail::g_running.store(false, std::memory_order_release);
  detail::g_latched.store(false, std::memory_order_release);
}

}  // namespace doa4::vanilla
