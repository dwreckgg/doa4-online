// doa4_vanilla.h
// The Extras tab's "Vanilla DoA4 content only" box (cvar doa4_vanilla_only, saved, off by default). While it is on, a
// LOBBY process (an offline game) leaves out the costume tables' colour and swimsuit rows (ChooseTableRows tests
// Running()) and the Extras tab refuses the swimsuit import with kOffText. The six tweak boxes and the Age box follow
// their own boxes, exactly as with vanilla mode off. It is a start-up latch: SetUpExtras latches the saved value once
// (LatchValue: a match process ignores the setting entirely), and a change in the menu applies at the next start.
// Vanilla mode never WRITES another setting, neither a live value nor the settings file. It is not one of
// tweaks::AllTweaks() nor a carried port setting: it is never sent.
#pragma once

#include <atomic>

#include "doa4_colour_make.h"

namespace doa4::vanilla {

// The swimsuit import's status in vanilla mode (the import is refused; the files on disk stay as they are).
inline constexpr const char* kOffText = "off: vanilla mode";

namespace detail {
// Read by the start-up code (SetUpSuits, SetUpExtras) and the menu; kept atomic so a reader on another thread stays safe.
// Latch is called once, on the UI thread, before any of them runs.
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

// Back to "never latched". For the host tests; the tree's test seams are not guarded (doa4_updater.h ResetForTest), and
// no product code calls it.
inline void ResetForTest() {
  detail::g_running.store(false, std::memory_order_release);
  detail::g_latched.store(false, std::memory_order_release);
}

}  // namespace doa4::vanilla
