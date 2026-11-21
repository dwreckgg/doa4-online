// Making the missing colour costume files at a lobby start. The seven files are made on the player's PC from the
// player's own game files (colourgen::Make); a made file is written only when its size and SHA-1 equal the slot table's.
// MakeMissing finds which slots of the colour folder have no good file, makes those, and writes the manifest; KindOf and
// ShouldMake are the caller's decision whether to call it at all. Nothing here logs: the caller logs the report.
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
#include <vector>

#include "doa4_colour_costumes.h"

namespace doa4::colour_costumes {

enum class ProcessKind : uint8_t { kLobby, kMatch };
// kMatch: a netplay session process, or DOA4_NP set to anything. kLobby: every other start.
ProcessKind KindOf(bool session, std::string_view np_mode);
// kLobby && !launch_refused && !folder_overridden
bool ShouldMake(ProcessKind kind, bool launch_refused, bool folder_overridden);

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
  std::function<void(size_t to_make)> on_start;                      // called once, only when to_make > 0
};
// good + made + not_made = kSlotCount on every return. good: the slot had a good file before the call, or another
// process made it meanwhile. notes: one per slot not made ("<slot>: <cause>", the table's order), then the others.
struct MakeReport {
  size_t good = 0, made = 0, not_made = 0;
  std::vector<std::string> notes;
  double seconds = 0;
};
// A slot is good when the manifest names a plain file for it that passes its size and SHA-1, or when no usable line
// names it and <slot>.tpr in the folder passes. When every slot is good and has its line, nothing is touched. Otherwise:
// temporary files older than an hour that an earlier run left are deleted; every slot that is not good is made from its
// source (free space, then the source's size and SHA-1, are checked first), written as <slot>.tpr; and the manifest is
// written again, one line per slot whose file is good at that moment. `guest` is read on the calling thread only; only
// `opt.generate` runs on other threads.
MakeReport MakeMissing(const SlotTable& slots, const std::array<MakeDef, kSlotCount>& defs,
                       const std::filesystem::path& folder, const std::filesystem::path& game_root,
                       const GuestBytes& guest, const MakeOptions& opt);
std::string MakeTempTag();  // "<process id>-<steady clock ticks>"

}  // namespace doa4::colour_costumes
