// Suit slim copies (extras-copies plan, Tasks 3 to 7): a C++ port of the DOAX2 suit add-on that builds the group 0x02
// (reflection) and 0x04 (shadow) copy objects of a suit, byte for byte as the Python tool
// (doax2_work/physics_spike/scripts/slim_addon.py, slim.py, tpr.py, convert.py, lod_chain.py) builds them. The port
// contract is notes/specs/2026-11-20-doax2-suit-slim-port-contract.md; the Python tool is the truth.
//
// Task 3 (this file's container layer, bodies in doa4_suitconv_slimread.cpp): T2Bytes / MapBytes (convert.t2_bytes,
// slim_addon.map_bytes), CatWithMap (slim_addon.cat_with_map with _apa_fields and the in-place branch _map_in_place),
// BufSet (tpr.buf_set; BufSpan, TexAddrs and TexSet are tpr's), Suit / ReadSuit (slim_addon.read_suit with _users,
// _check_user0, _set_user0, _res_bases, _remove_resources: the parse and the strip of old copies BY ID) and Rebuild
// (slim_addon.rebuild with _container). Every SlimError of the Python tool is a ConvertError whose what() is the
// SlimError's text, character for character. The grid and the writers (AddCopies, SlimCopies, Merge ...) are declared
// here and defined by later tasks.
//
// Pure C++ (no SDK).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doa4_suitconv_num.h"
#include "doa4_suitconv_tpr.h"

namespace doa4::suitconv::slim {

using Bytes = tpr::Bytes;

// ---------------------------------------------------------------- the public entry (Tasks 5 to 7)
enum class CopyMode { kSlim, kFull };
struct Result {
  Bytes tpr;
  Bytes cat;
};
// Throws ConvertError (through Fail) with the Python tool's SlimError text. Pure function of its four inputs.
Result AddCopies(std::span<const uint8_t> tpr, std::span<const uint8_t> cat, std::span<const uint8_t> ref_tpr,
                 std::span<const uint8_t> ref_cat, CopyMode mode);
// The public entry: runs AddCopies under num::RunUnderConvertEnv. False = refused, *error holds the reason.
bool SlimCopies(std::span<const uint8_t> tpr, std::span<const uint8_t> cat, std::span<const uint8_t> ref_tpr,
                std::span<const uint8_t> ref_cat, CopyMode mode, Result* out, std::string* error);

// ---------------------------------------------------------------- grid (Task 4)
// A corner carries a position and an opaque payload index; a key is an opaque integer id given in first-traversal
// order by the caller.
struct Tri {
  uint32_t key;
  std::array<std::array<double, 3>, 3> pos;
  std::array<uint32_t, 3> payload;
};
struct MergedKey {
  uint32_t key;
  std::vector<std::array<double, 3>> pos;
  std::vector<uint32_t> payload;
  std::vector<std::array<uint32_t, 3>> faces;
};
using Merged = std::vector<MergedKey>;  // keys in first-traversal order
std::array<double, 3> Origin(std::span<const Tri>);
Merged Merge(std::span<const Tri>, std::optional<double> h, const std::array<double, 3>& org);
size_t Count(const Merged&);
struct Chosen {
  double h;
  Merged merged;
  double lo;
};
Chosen ChooseH(std::span<const Tri>, size_t target);  // Fail when the count at H_HI exceeds the target

// ---------------------------------------------------------------- container layer (Task 3)
// convert.t2_bytes(kind, recs): one group of the type-2 map back to bytes (the main group has no marker byte). The
// reader's accept set is asserted: kind 1..0x7F other than 0x60, every code 0x9000..0xAFFF.
Bytes T2Bytes(const tpr::T2Group& group);
// slim_addon.map_bytes(groups): the groups' bytes and the end byte.
Bytes MapBytes(std::span<const tpr::T2Group> groups);
// slim_addon.cat_with_map(cat, map_bytes(groups)): the cat with its type-2 map at 0x18 replaced. A cat of the
// converter's directory shape is laid out again (every later part moved, absolute offsets moved with it); any other
// cat whose map is at 0x18 gets the new map written in place when it is no longer than the old one (zero-filled to the
// old map's end, nothing else changed) and is refused when it is longer (port contract (i)).
Bytes CatWithMap(std::span<const uint8_t> cat, std::span<const tpr::T2Group> map);

// tpr.buf_set(desc, base, size): the descriptor with its last two words' addresses replaced (low 2 bits kept). base and
// size are Python ints: each must be a multiple of 4 (assert) and the result must fit a u32 (struct.error).
Bytes BufSet(std::span<const uint8_t> desc, int64_t base, int64_t size);
using tpr::BufSpan;
using tpr::TexAddrs;
using tpr::TexSet;

// slim_addon.Suit: the parsed suit with its old copies stripped.
struct Suit {
  std::string name;                // the file name without its extension (the SlimError prefix)
  Bytes tpr, cat;                  // the input bytes
  tpr::Xpr x;                      // the container, copies stripped (USER0 rewritten when something was stripped)
  std::vector<tpr::ObjGeo> geo;    // MdlGeo objects, stripped
  std::vector<tpr::ObjInfo> info;  // MdlInfo objects, stripped
  std::vector<tpr::Node> nodes;    // Hie nodes, stripped (kept nodes in file order, positions remapped)
  std::vector<tpr::T2Group> groups;                  // the input's type-2 map
  std::vector<std::pair<uint16_t, uint8_t>> main;    // its main group (groups[0])
  std::string stripped;            // "nothing" or "old copies ids A..B, N buffers"
};
// slim_addon.read_suit: parse, then strip the old group 0x02/0x04 copies by id. `name` is what the Python tool takes
// from the .tpr path (os.path.splitext(os.path.basename(path))[0]); it prefixes the refusal texts. (The plan's
// Interfaces wrote ReadSuit(tpr, cat); the name is the one widening, since the refusals carry it.)
Suit ReadSuit(std::string_view name, std::span<const uint8_t> tpr, std::span<const uint8_t> cat);
// slim_addon.rebuild: the container with nothing added (USER1-3 re-encoded from the parsed lists), Build(1).
Bytes Rebuild(const Suit& suit);

}  // namespace doa4::suitconv::slim
