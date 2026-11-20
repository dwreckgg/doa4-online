// The colour costumes of the local offline build: seven compiled-in slots (Alpha-152: red takes over COS. 2's list entry,
// blue and lilac appended; Spartan-458: four appended), each a body .tpr served in an avt.afs creature-placeholder entry.
// This module is the one place that knows the slots: the slot table, loading the seven texture files from a private
// folder (a stand-in per slot that fails), the overlay replacements, the costume-table rows for costumes::Apply, the
// first-read labels and Alpha's colour rows. "Bytes for slot X, or a stand-in": the upstream step later swaps only the
// source of the bytes.
//
// The tables are FIXED: every slot is listed and gets a record whatever the folder holds. A slot whose file is missing or
// wrong is served its stand-in (Alpha: her retail entry 22, file 427; Spartan: the retail tpr of the slot it was built
// on), so it still loads. The manifest names files only; each slot's size and SHA-1 are compiled in, so a hand-edited
// manifest cannot approve a wrong file. When a needed stand-in cannot be read from the game folder, nothing is installed
// (`usable` false): a record naming an entry that is not overlaid would load another character's file.
//
// Manifest (manifest.txt in the folder): one line per slot, "<slot name> <file name>", the two separated by spaces or
// tabs. Lines end in LF or CR LF; a UTF-8 byte-order mark at the start is skipped; blank lines and lines starting with '#'
// are skipped. A slot with no line, or whose file name holds '/', '\', ':' or "..", gets its stand-in (one note each). A
// line that is not two words, names an unknown slot, or repeats a slot is ignored (one note each; a repeat keeps the
// first line).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doa4_afs_overlay.h"
#include "doa4_costumes.h"
#include "doa4_guest.h"

namespace doa4::colour_costumes {

enum class Slot : uint8_t { kAlphaRed, kAlphaBlue, kAlphaLilac, kSpartanH3Red, kSpartanH3Blue, kSpartanH2Orange, kSpartanH2Cyan };
inline constexpr size_t kSlotCount = 7;
inline constexpr uint64_t kManifestLimit = 64 * 1024;

struct SlotDef {
  Slot slot;
  const char* name;        // manifest key and file stem: "alpha_red", ..., "spartan_h2_cyan"
  uint8_t character;       // 16 (Alpha-152) or 18 (Spartan-458)
  int replaces;            // costume number (0-based) whose list entry it takes over, or -1 = appended
  uint16_t tpr_file;       // the global file number of its avt.afs entry (914 + entry)
  uint16_t cat_file;       // Alpha: 428; Spartan: the built-on slot's retail cat
  uint16_t stand_in_file;  // Alpha: 427 (her entry 22); Spartan: the built-on slot's retail tpr
  uint64_t size;           // the approved file's size
  const char* sha1;        // the approved file's SHA-1, lower-case hex
};
using SlotTable = std::array<SlotDef, kSlotCount>;

// Alpha's rows first (red replaces costume 1; blue, lilac append), then Spartan's four (append).
const SlotTable& Slots();

// The colour slots are an offline feature: true only with no netplay session and DOA4_NP "" or "record".
bool Enabled(bool session, std::string_view np_mode);

struct LoadedSlot {
  const SlotDef* def = nullptr;
  std::shared_ptr<const afs::Source> tpr;  // the slot's file, or its stand-in entry; null only when !usable
  bool stand_in = false;
};
struct Status {
  bool usable = false;             // every slot has bytes (its file or its stand-in): the slots may be installed
  std::vector<LoadedSlot> slots;   // the table's order (empty for a Status that was never loaded)
  std::vector<std::string> notes;  // one line per grain that degraded
  std::string Summary() const;
};

// Reads `folder` (manifest.txt and the files it names) against Slots(); stand-ins are read from the game folder's
// archives, found through `guest`'s file table (costumes::FileLocation). Files stay open without write sharing.
Status Load(const std::filesystem::path& folder, const std::filesystem::path& game_root, const GuestBytes& guest);
// The same against another slot table (host tests: synthetic sizes and SHA-1s). LoadedSlot::def points into `slots`,
// which must outlive the Status.
Status Load(const SlotTable& slots, const std::filesystem::path& folder, const std::filesystem::path& game_root,
            const GuestBytes& guest);

// The overlay replacements, by archive name ("avt.afs"): one per distinct entry, holding the bytes of the first slot that
// names it. Empty unless s.usable.
std::map<std::string, std::vector<afs::Replacement>> Replacements(const Status& s, const GuestBytes& guest);
// The costume-table rows for costumes::Apply, in the table's order: {character, tpr_file, cat_file, replaces,
// "colour <name>"}. Empty unless s.usable.
std::vector<costumes::Extra> TableExtras(const Status& s);
// Per archive, per entry: "colour <name>" (a shared entry: its first slot's name). Empty unless s.usable.
std::map<std::string, std::map<uint32_t, std::string>> EntryLabels(const Status& s, const GuestBytes& guest);

// Alpha's body RefractColor and outline tint per slot.
struct AlphaRow {
  float refract[4];
  float outline[4];
};
AlphaRow AlphaVanilla();                         // the game's own: (0.9, 1, 1, 0.4) / (0.1, 0.4, 0.3, 10)
std::optional<AlphaRow> AlphaRowFor(Slot slot);  // Alpha's three slots; nullopt for Spartan's

}  // namespace doa4::colour_costumes
