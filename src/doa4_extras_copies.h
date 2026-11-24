// Extras copies (extras-copies plan, Task 8): the start-up builder. When the player has switched on Helena's opera
// dress or Kokoro's kimonos, the game calls Build once at start-up: it reads the three retail costumes out of the
// player's chrtwo.afs (through a callback), checks each file's SHA-1 against retail, runs the slim core
// (suitconv::slim::SlimCopies, slim mode, the costume as its own reference) to give it reflection and shadow copies,
// checks the size limits and returns the replacements for the archive overlay. A costume that cannot be built keeps its
// stand-in (the retail files) and one log line says why; the other costumes go on.
//
// Pure apart from ReadEntryFrom (Win32 file reads through afs::FileSource) and the SHA-1 (Windows CNG through
// gamefiles::Sha1), so the host test (tests/host/extras_copies_host_test.cpp) builds it.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "doa4_afs_overlay.h"
#include "doa4_colour_costumes.h"

namespace doa4::extras_copies {

struct Costume {
  const char* tweak_cvar;  // the box that selects it: "doa4_helena_opera" or "doa4_kokoro_kimonos"
  const char* label;       // "hel03 (extras copy)": the first-read line's label (Task 9)
  const char* name;        // "hel03": the core's name argument, and the file names hel03.tpr / hel03.cat
  uint32_t tpr_entry, cat_entry;  // chrtwo.afs entries
  const char* tpr_sha1;    // the retail files' SHA-1, lower-case hex
  const char* cat_sha1;
  uint32_t tpr_limit, cat_limit;  // a built file larger than its limit is refused (limit = size builds)
};

// The game's table: hel03 16/17, hak03 46/47, hak08 56/57, in that order (SHA-1s and sizes from G0's g0_sha1.txt).
std::span<const Costume> Costumes();

struct Built {
  std::vector<afs::Replacement> chrtwo;  // two per built costume (tpr entry, then cat entry), in table order
  std::string sha1;                      // SHA-1 over tpr_1 cat_1 tpr_2 cat_2 ... of the INSTALLED costumes (chrtwo,
                                         // in order), 40 lower-case hex; "" exactly when chrtwo is empty
  unsigned boxes_at_start = 0;           // bit 0 opera_on, bit 1 kimonos_on: the inputs, whatever was built
  std::vector<std::string> log;          // one line per selected costume (built or kept, with the reason), one per
                                         // table row with an unknown box, then "extras copies: <k> built of <n>
                                         // selected in <ms> ms" ("... selected (the copies could not be hashed;
                                         // none installed) in <ms> ms" when the hash failed: then chrtwo is empty)
};

// Reads chrtwo.afs entry `entry` into *out: exactly the entry's TOC-size bytes. False when the archive or the entry
// cannot be read. Build also treats a throw as false, and puts a std::exception's text in the costume's line
// ("could not read chrtwo.afs entry N: <text>").
using ReadEntry = std::function<bool(uint32_t entry, std::vector<uint8_t>* out)>;

// The SHA-1 of the installed replacements (a test seam): 40 lower-case hex chars, or anything else for "could not hash".
using Hasher = std::function<std::string(const std::vector<afs::Replacement>& chrtwo)>;

// Never throws. With neither box on, read_entry is never called. sha1 is computed at the end from chrtwo as installed
// (`hash`, empty = gamefiles::Sha1 over each replacement's bytes read back). Whatever the input and whatever fails
// (an allocation, the hash): sha1.empty() == chrtwo.empty(), sha1 is 40 hex chars when not empty, and the closing
// line's built count == chrtwo.size() / 2. When copies were built but their hash is not 40 hex chars (after one retry
// of a throw), nothing is installed: chrtwo is cleared and the closing line says so; the costumes' "built" lines stay.
Built Build(bool opera_on, bool kimonos_on, const ReadEntry& read_entry, std::span<const Costume> table = Costumes(),
            const Hasher& hash = {});

// A ReadEntry over an AFS archive: afs::ReadToc, then a read of exactly sizes[entry] bytes at offsets[entry]. The TOC
// is read on the first call (and only once). False for every call when the source is null, and for an entry past the
// count; when the TOC is refused, every call throws std::runtime_error with ReadToc's reason.
//
// Threads: the returned ReadEntry (from either function) is for ONE thread (its first-call state is not locked), and
// it keeps the archive open for as long as it lives (or any copy of it). Task 9 destroys it right after Build.
ReadEntry ReadEntryFromSource(std::shared_ptr<const afs::Source> archive);
// The same over a file. The file is opened on the first call, not here, so a Build with both boxes off never opens it.
// `open` is the opener (a test seam); empty = afs::FileSource::Open (read-only).
using Opener = std::function<std::shared_ptr<const afs::Source>(const std::filesystem::path&)>;
ReadEntry ReadEntryFrom(const std::filesystem::path& archive, Opener open = {});

// ---- The archive overlay's install order (Task 9; plan Amendment 2, "Retry order as a pure function") ----
// One overlay attempt: which optional entries were in its build (the swimsuits are in every attempt).
struct Attempt {
  bool extras = false;   // the copies' chrtwo.afs entries
  bool colours = false;  // the colour costumes' avt.afs entries
  bool operator==(const Attempt&) const = default;
};
struct InstallPlan {
  std::vector<Attempt> attempts;  // in the order made
  colour_costumes::InstallOutcome outcome = colour_costumes::InstallOutcome::kNotUsable;  // what the app acts on
};
// The order SetUpSuits' InstallSuitsAndColours follows (dead_or_alive_4_app.h; the app's wiring is checked by runs only):
// attempt X = {extras, colours} first, only when extras are present. If X builds and installs, that is the outcome
// colour_costumes::Decide(colours_usable, true, true, _) gives, and nothing else is tried. Otherwise the extras are
// dropped and the block of before Task 9 runs unchanged: only when suits are present or colours are usable, {colours}
// (or {} when colours are not usable); after a failure with colours, {} again when suits are present (with no suits
// that retry counts as done, "nothing else to install"); its outcome is Decide's. Neither block runs: kNotUsable. So an
// extras failure never yields kFailed on its own (Decide never sees it). results[i] is whether attempt i built and
// installed; an attempt past the end of `results` counts as failed.
InstallPlan InstallOrder(bool extras_present, bool colours_usable, bool suits_present, const std::vector<bool>& results);

}  // namespace doa4::extras_copies
