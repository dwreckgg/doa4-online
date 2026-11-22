// Making the missing colour costume files at a lobby start. The seven files are made on the player's PC from the
// player's own game files (colourgen::Make); a made file is written only when its size and SHA-1 equal the slot table's.
// MakeMissing finds which slots of the colour folder have no good file, makes those, and writes the manifest; KindOf and
// ShouldMake are the caller's decision whether to call it at all. Nothing here logs: the caller logs the report.
// The other decisions the app's start-up reads are here too, as pure functions: RowsOn and MustRefuse (what a process
// kind does with the slots), RefusalMessage, FreeBytesFrom and IsPassingNote.
//
// MakeMissing never throws and never costs Load anything: after any failure, Load on the same folder behaves as if
// MakeMissing had not run, apart from the files it made good. It writes only inside `folder` (a temporary file per
// write, then a rename) and reads only `folder`, the archives under `game_root` and `guest`.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "doa4_colour_costumes.h"

namespace doa4::colour_costumes {

enum class ProcessKind : uint8_t { kLobby, kMatch };
// kMatch: a netplay session process, or DOA4_NP set to anything. kLobby: every other start.
ProcessKind KindOf(bool session, std::string_view np_mode);
// kLobby && !launch_refused && !folder_overridden
bool ShouldMake(ProcessKind kind, bool launch_refused, bool folder_overridden);
// Whether the colour costumes' ROWS go into this process's costume tables (their files are loaded and overlaid in every
// process): everywhere, except a lobby process in vanilla mode. False only for kLobby && vanilla.
bool RowsOn(ProcessKind kind, bool vanilla);
// A match process whose colour entries were not installed cannot build the tables the other PC builds, so it must not
// run: kMatch && !Installed(outcome). A lobby process is never refused.
bool MustRefuse(ProcessKind kind, InstallOutcome outcome);
// What the refused process says (np::RefuseRun's reason): "colour costumes: this PC cannot build the 0.3.8 costume
// tables: <cause><remedy>". The remedy is "; check the game files (chrthree.afs) and restart" for kNotUsable (the
// stand-ins are read from that file), for the test switch and for kInstalled, and "; check the game files and the
// swimsuit import, then restart" for kDropped and kFailed (a swimsuit archive can break the build too). The cause: "test switch
// DOA4_COLOURS_TEST_UNUSABLE" when test_unusable; else for kNotUsable the LAST of load_notes (Status::notes: Load pushes
// the note that says why nothing is usable right before it returns; the notes before it say why a stand-in was
// needed), or "the colour slots were not loaded" when there is none; for kDropped `error`; for kFailed with the colour
// slots usable (colours_usable: Status::usable) `error` + " / " + `retry_error` (`error` alone when retry_error is
// empty), and for kFailed with them not usable that kNotUsable cause + " / " + `error` (there was no second try, and
// the install error alone would not say that the colour slots could not be loaded); for kInstalled nothing (never
// asked for). An empty `error` reads "the overlay install failed". colours_usable is read for kFailed only.
std::string RefusalMessage(InstallOutcome outcome, bool colours_usable, const std::vector<std::string>& load_notes,
                           std::string_view error, std::string_view retry_error, bool test_unusable);

// The free bytes std::filesystem::space reported, for MakeOptions::free_bytes. UINT64_MAX when the query could not be
// answered (`ec` set, or `available` at the value space() gives on an error, uintmax_t(-1), which is not 0): a query
// without an answer does not refuse the making; a write that then finds no room fails as any other write does.
uint64_t FreeBytesFrom(const std::filesystem::space_info& info, const std::error_code& ec);

// MakeMissing's note when the manifest that is there cannot be read at the moment it is to be replaced. It can appear
// for a moment when two copies of the game start together (one reads while the other renames its manifest into
// place); the next lobby start writes the manifest.
inline constexpr std::string_view kManifestUnreadableNote = "manifest: cannot write: the current one cannot be read";
// True for exactly that note: the caller logs it as information, every other note as a warning.
bool IsPassingNote(std::string_view note);

// What a slot is made from: a global file number of the game (found through the guest's file table at run time), and
// the size and SHA-1 that file has on the supported disc. A source that differs is never passed to the generator.
struct MakeDef {
  const char* slot;
  uint16_t source_file;
  uint64_t source_size;
  const char* source_sha1;
};
const std::array<MakeDef, kSlotCount>& MakeDefs();  // Slots() order, built from colourgen::kSources

using Generator = std::function<std::optional<std::vector<uint8_t>>(std::string_view slot,
                                                                    std::span<const uint8_t> source, std::string* why)>;
struct MakeOptions {
  Generator generate;                                                // colourgen::Make in the app
  std::function<uint64_t(const std::filesystem::path&)> free_bytes;  // the folder's free space (unset: not checked)
  std::string temp_tag;                                              // unique per process: digits and '-' only
  unsigned threads = 1;                                              // generator calls at once
  std::function<void(size_t to_make)> on_start;                      // called once, only when to_make > 0; may throw
};
// good + made + not_made = kSlotCount on every return. good: the slot had a good file before the call, or another
// process made it meanwhile. notes: one per slot not made ("<slot>: <cause>", the table's order), then the others.
struct MakeReport {
  size_t good = 0, made = 0, not_made = 0;
  std::vector<std::string> notes;
  double seconds = 0;
};
// A slot is good when the manifest names a plain file for it that passes its size and SHA-1, or when <slot>.tpr in the
// folder passes, whatever its line says. When every slot is good through its own line, nothing is touched. Otherwise:
// temporary files older than an hour that an earlier run left are deleted; every slot that is not good is made from its
// source (free space, then the source's size and SHA-1, are checked first), written as <slot>.tpr; and the manifest is
// written again, one line per slot whose file is good at that moment (a line that names a good file is kept, any other
// names <slot>.tpr). A manifest that is there and cannot be read at that moment is not replaced. `guest` is read on the
// calling thread only; only `opt.generate` runs on other threads.
// Notes: "<slot>: <cause>" with the cause one of "source missing", "source differs", "generator: <text>", "made file
// fails its check", "no space", "cannot write: <text>", "not attempted: the call stopped first"; then, as they arise,
// "sweep: <text>", "folder: cannot create: <text>", "on_start: <text>" (what on_start threw; the making goes on),
// "manifest: <slot> not listed" (a good or made slot with no good file when the manifest was written), "manifest:
// cannot write: <text>", "stopped: <text>" (what ended the call early).
MakeReport MakeMissing(const SlotTable& slots, const std::array<MakeDef, kSlotCount>& defs,
                       const std::filesystem::path& folder, const std::filesystem::path& game_root,
                       const GuestBytes& guest, const MakeOptions& opt);
std::string MakeTempTag();  // "<process id>-<steady clock ticks>"

}  // namespace doa4::colour_costumes
