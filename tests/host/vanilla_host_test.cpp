// Host test for src/doa4_vanilla.h (the Extras tab's "Vanilla DoA4 content only" box): the start-up latch and the
// import's status text (the tweaks and Age are not vanilla mode's: they follow their own boxes). Each test starts from
// "never latched" (ResetForTest). Each call of the function under test is a statement of its own; each row's comment
// names the wrong version it excludes. Built and run by run_vanilla_test.bat.

#include "../../src/doa4_vanilla.h"

#include <cstdio>
#include <cstring>

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

using doa4::colour_costumes::ProcessKind;
namespace vanilla = doa4::vanilla;

// What a process latches: the saved box in a lobby process only.
static void TestLatchValue() {
  std::printf("-- TestLatchValue\n");
  vanilla::ResetForTest();
  // A match process with the box saved on: off. Excludes: the saved value alone, the kind not read (v1): a vanilla
  // player's match process would build other tables than the other PC's.
  { const bool v = vanilla::LatchValue(ProcessKind::kMatch, true); CHECK(v == false); }
  // A lobby with the box off: off. Excludes: the kind alone, the saved value not read (v2).
  { const bool v = vanilla::LatchValue(ProcessKind::kLobby, false); CHECK(v == false); }
  // The one true row. Excludes: always off.
  { const bool v = vanilla::LatchValue(ProcessKind::kLobby, true); CHECK(v == true); }
  // A match process with the box off: off. Excludes: "or" for "and" with the kind inverted (true for any match process).
  { const bool v = vanilla::LatchValue(ProcessKind::kMatch, false); CHECK(v == false); }
}

// Before the latch every reader sees the non-vanilla state; ResetForTest brings that back after a latch.
static void TestRunningBeforeLatch() {
  std::printf("-- TestRunningBeforeLatch\n");
  vanilla::ResetForTest();
  // Excludes: a latch that starts on.
  { const bool r = vanilla::Running(); CHECK(r == false); }
  vanilla::Latch(true);
  vanilla::ResetForTest();
  // Excludes: a reset that leaves the value (the test seam would carry one test's latch into the next).
  { const bool r = vanilla::Running(); CHECK(r == false); }
}

// The first call decides; a later call (the box changed in the menu, read again) changes nothing until the next start.
static void TestLatchOnce() {
  std::printf("-- TestLatchOnce\n");
  vanilla::ResetForTest();
  vanilla::Latch(true);
  vanilla::Latch(false);
  // Excludes: a latch that takes every call, i.e. reads the value later instead of the start value (v3, plan probe 1).
  { const bool r = vanilla::Running(); CHECK(r == true); }
  // The mirror: off at the start stays off when a later call says on. Excludes: the same mutant from the other side, and
  // a latch that ORs every call in.
  vanilla::ResetForTest();
  vanilla::Latch(false);
  vanilla::Latch(true);
  { const bool r = vanilla::Running(); CHECK(r == false); }
}

// The swimsuit import's status in vanilla mode (the import is still refused, doa4_menu.cpp), exactly. Excludes: another
// wording (the design's picture shows this one).
static void TestOffTextKept() {
  std::printf("-- TestOffTextKept\n");
  CHECK(std::strcmp(vanilla::kOffText, "off: vanilla mode") == 0);
}

int main() {
  TestLatchValue();
  TestRunningBeforeLatch();
  TestLatchOnce();
  TestOffTextKept();
  vanilla::ResetForTest();
  std::printf("vanilla: %d checks run\n", g_checks);
  if (g_failures != 0) {
    std::printf("FAILED: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }
  std::printf("PASSED: %d checks, 0 failures\n", g_checks);
  return 0;
}
