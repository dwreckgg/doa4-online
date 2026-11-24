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
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doa4_suitconv_core.h"
#include "doa4_suitconv_num.h"
#include "doa4_suitconv_tpr.h"

namespace doa4::suitconv::slim {

using Bytes = tpr::Bytes;

// ---------------------------------------------------------------- the public entry (Tasks 6 and 7)
// Bodies in doa4_suitconv_slimwrite.cpp. `name` is the suit's name as the Python tool takes it from the .tpr path (the
// file name without its extension): every SlimError text starts with it (it is passed to ReadSuit). The reference is
// core::LoadModel over ref_tpr / ref_cat (slim_addon.load_ref). (The plan's Interfaces had no name: Task 6 ruling R2.)
// The Python tool's report lines are not ported.
enum class CopyMode { kSlim, kFull };
struct Result {
  Bytes tpr;
  Bytes cat;
};
// slim_addon.add_copies(read_suit(...), load_ref(...), mode): throws ConvertError with the Python tool's SlimError text
// (any other Python raise: a ConvertError naming the exception type). Pure function of its inputs.
Result AddCopies(std::string_view name, std::span<const uint8_t> tpr, std::span<const uint8_t> cat,
                 std::span<const uint8_t> ref_tpr, std::span<const uint8_t> ref_cat, CopyMode mode);
// The public entry: runs AddCopies under num::RunUnderConvertEnv and never throws. True = built, *out holds the files.
// False = refused: *error holds the reason (a ConvertError's text; "out of memory (std::bad_alloc)"; "unexpected
// error: <what()>" for any other std::exception; "unexpected non-standard exception"), and *out is left unchanged.
bool SlimCopies(std::string_view name, std::span<const uint8_t> tpr, std::span<const uint8_t> cat,
                std::span<const uint8_t> ref_tpr, std::span<const uint8_t> ref_cat, CopyMode mode, Result* out,
                std::string* error);

// ---------------------------------------------------------------- grid (Task 4, bodies in doa4_suitconv_slimgrid.cpp)
// slim.py: origin, merge, count, choose_h. A corner carries a source identity (vid), a position and an opaque payload
// index; a key is an opaque integer id given by the caller (it only names the bucket: output order is the keys'
// first-traversal order, never their numeric order). A cell's members are the DISTINCT vids that land in it (slim.py's
// b.seen), so two corners at one position with different vids are two members, and a vid met again in the same cell
// is one. (The plan's Interfaces had no vid; Python's membership needs one, so Tri carries it: Task 4 ruling R2.)
struct Tri {
  uint32_t key;
  std::array<uint32_t, 3> vid;
  std::array<std::array<double, 3>, 3> pos;
  std::array<uint32_t, 3> payload;
};
// slim.py's constants (H_LO, H_HI, HALVINGS).
inline constexpr double kHLo = 0.001;
inline constexpr double kHHi = 1.0;
inline constexpr int kHalvings = 40;
struct MergedKey {
  uint32_t key;
  std::vector<std::array<double, 3>> pos;
  std::vector<uint32_t> payload;
  std::vector<std::array<uint32_t, 3>> faces;
};
using Merged = std::vector<MergedKey>;  // keys in first-traversal order
// Per-axis minimum over every corner (strict <, starting from the first corner). Fails "origin: no triangles".
std::array<double, 3> Origin(std::span<const Tri>);
// h empty: identity mode (cell = vid). Otherwise cell = floor((p - org) / h) per axis, held as the floored double
// itself (an exact integer: two are equal as doubles iff Python's ints are, with -0.0 made 0.0, so no range limit);
// the non-finite and h == 0 cases fail with Python's exception texts ("ValueError: ...", "OverflowError: ...",
// "ZeroDivisionError: ..."), at the corner where Python raises.
// The cell position is a plain sequential sum from 0.0 over the members, divided by their number; the payload is the
// first member's; identity mode keeps the first member's own position.
Merged Merge(std::span<const Tri>, std::optional<double> h, const std::array<double, 3>& org);
size_t Count(const Merged&);
struct Chosen {
  double h;
  Merged merged;
  double lo;
};
Chosen ChooseH(std::span<const Tri>, size_t target);  // Fail when the count at H_HI exceeds the target

// ---------------------------------------------------------------- container layer (Task 3)
// One group of the type-2 map in convert.py's CURRENT grammar (parse_t2 / t2_bytes): the main group, a group opened
// by a marker byte (kind = that byte; 0x02 low-detail copy, 0x04 shadow copy) or a state group opened by '60 NN'
// (kind ('state', NN)). A code is 16 bits for a 0x9X / 0xAX entry (3 bytes) and 24 bits for a 0xDX entry (4 bytes).
// Since Task 6b this is tpr::T2Group, and ParseMap / T2Bytes call tpr::ParseT2 / tpr::T2Bytes: the converter and the
// slim core share one implementation of the grammar.
using MapGroup = tpr::T2Group;
// convert.parse_t2(c, off): the groups and the offset just past the end byte. Refusals (ValueError texts): an entry
// byte with the top bit set that is not 0x9X / 0xAX / 0xDX, and '60 NN' with NN 0 or NN's top bit set.
std::pair<std::vector<MapGroup>, uint64_t> ParseMap(std::span<const uint8_t> c, uint64_t off);
// convert.t2_bytes(kind, recs): one group back to bytes (the main group has no marker byte). The reader's accept set is
// asserted with the AssertionError's text: a marker 1..0x7F other than 0x60, a state NN 1..0x7F, every code
// 0x9000..0xAFFF or 0xD00000..0xDFFFFF.
Bytes T2Bytes(const MapGroup& group);
// slim_addon.map_bytes(groups): the groups' bytes and the end byte.
Bytes MapBytes(std::span<const MapGroup> groups);
// slim_addon.cat_with_map(cat, map_bytes(groups)): the cat with its type-2 map at 0x18 replaced. A cat of the
// converter's directory shape is laid out again (every later part moved, absolute offsets moved with it); any other
// cat whose map is at 0x18 gets the new map written in place when it is no longer than the old one (zero-filled to the
// old map's end, nothing else changed) and is refused when it is longer (port contract (i)).
Bytes CatWithMap(std::span<const uint8_t> cat, std::span<const MapGroup> map);

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
  std::vector<MapGroup> groups;                      // the input's type-2 map
  std::vector<std::pair<uint32_t, uint8_t>> main;    // its main group (groups[0])
  std::string stripped;            // "nothing" or "old copies ids A..B, N buffers"
};
// slim_addon.read_suit: parse, then strip the old group 0x02/0x04 copies by id. `name` is what the Python tool takes
// from the .tpr path (os.path.splitext(os.path.basename(path))[0]); it prefixes the refusal texts. (The plan's
// Interfaces wrote ReadSuit(tpr, cat); the name is the one widening, since the refusals carry it.)
Suit ReadSuit(std::string_view name, std::span<const uint8_t> tpr, std::span<const uint8_t> cat);
// slim_addon.rebuild: the container with nothing added (USER1-3 re-encoded from the parsed lists), Build(1).
Bytes Rebuild(const Suit& suit);

// ---------------------------------------------------------------- bind guard, layout, scan (Task 5)
// Bodies in doa4_suitconv_slimscan.cpp. The reference is a core::Model from core::LoadModel (convert.Model; in the game
// the reference is the suit's own file). The type-2 map is read with ParseMap (convert.parse_t2's CURRENT grammar; since
// Task 6b ParseMap is tpr::ParseT2, which core::ChannelMapOf also reads).

// slim_addon.ref_groups(ref): the reference's type-2 map at dict(cat_dir(ref.cat))[2].
std::vector<MapGroup> RefGroups(const core::Model& ref);
// convert.channel_map(ref)[0]: channel code -> reference node id over the main group, the shallowest node (Hie depth)
// then the lowest id per code, then CHANNEL_ALIAS in sorted code order for a code the main group lacks.
std::map<uint32_t, int64_t> ChannelMap(const core::Model& ref);

// slim_addon.bind_guard: every node of the reference's main group is in the suit, at the same world translation
// (1e-5 m). Throws the two SlimErrors.
void BindGuard(const Suit& suit, const core::Model& ref);

// lod_chain._frame(w): R rows, t, dev = max |R - I| (Python's max: the first of equal values).
struct Frame {
  std::array<std::array<double, 3>, 3> R;
  std::array<double, 3> t;
  double dev;
};
Frame FrameOf(const std::vector<double>& w);
// lod_chain._to_local(v, R, t, dev): q = v - t (direction: q = v, Python's t=None); dev < 1e-6 gives q, otherwise
// q_j = sum(q[i] * R[j][i] for i in 0..2) with Python's built-in sum (num::PySum).
std::array<double, 3> ToLocal(const std::array<double, 3>& v, const Frame& f, bool direction);

// One copy group of slim_addon.Layout (kind 2 or 4).
struct CopyGroup {
  std::vector<std::pair<uint32_t, uint8_t>> rows;  // the reference group's (code, id) rows in file order
  std::vector<int64_t> ids;                        // sorted distinct reference ids
  std::map<int64_t, int64_t> new_id;               // reference id -> new id
  std::vector<tpr::Node> nodes;                    // the group's Hie nodes (new ids, in reference-id order)
  std::map<uint32_t, int64_t> node_of_code;        // code -> new id: the shallowest node, then the lowest new id
};
// slim_addon.Layout: the two copy groups of the reference; new ids from len(suit.nodes), 0x02 first, then 0x04.
struct Layout {
  CopyGroup g2, g4;
  std::vector<std::pair<MapGroup::Kind, uint8_t>> kinds;  // groups[1:]'s kinds in map order (state groups included)
  std::vector<tpr::Node> allnodes;                         // the suit's nodes, then g2's, then g4's
  std::map<int64_t, Frame> frame;                          // node id (suit or new) -> Frame of WorldMats(allnodes)
  const CopyGroup& Of(int kind) const;                     // 2 or 4; any other kind is the KeyError
};
// Throws the four Layout SlimErrors (and KeyError / IndexError texts where Python raises them).
Layout MakeLayout(const Suit& suit, const core::Model& ref);

// slim_addon._scan(suit, ref, kind, lay): the copy group's triangles in traversal order (object, decl, mesh, strip).
// Python's tuples become indices into tables: Tri.key -> keys (dense, first-traversal order), Tri.vid[c] -> vid_src
// (dense, first-appearance order over the whole scan, one-to-one with Python's (object, decl, vertex)), Tri.payload[c]
// = 3 * t + c -> payload. Tri.pos[c] is the decoded position plus the object's world translation.
struct ScanKey {
  int64_t node = 0;  // k[0]: the copy node's new id
  // kind 2 only (zero / empty for kind 4): the matkey (stage-0 texture, LOD_PASS[pass], flag, mesh u32 at 4,
  // mesh bytes 0x70:0x74 and 0x80:0xC0 as clamped slices) and matsrc, the whole record of the FIRST mesh that made the key
  uint32_t tex = 0, pass = 0, flag = 0, word4 = 0;
  Bytes m70, m80, matsrc;
};
struct ScanPayload {
  uint32_t vid = 0;
  uint32_t tri = 0;    // t, the triangle's index in the scan
  uint8_t corner = 0;  // ci
  // kind 2: decode_elem(...)[:3] and [:2] (a shorter element gives fewer values: nrm_n / uv_n count them); kind 4: none
  uint8_t nrm_n = 0, uv_n = 0;
  std::array<double, 3> nrm{};
  std::array<double, 2> uv{};
};
struct Scan {
  std::vector<Tri> tris;
  std::vector<ScanKey> keys;
  std::vector<std::array<uint32_t, 3>> vid_src;  // vid -> (object, decl, vertex)
  std::vector<ScanPayload> payload;              // 3 per triangle
};
// Throws the four _scan SlimErrors; every other raise of the Python (a read past the data, an index past a list, a
// missing key) is a ConvertError naming the Python exception type. Never reads out of bounds.
Scan ScanKind(const Suit& suit, const core::Model& ref, const Layout& lay, int kind);

// ---------------------------------------------------------------- writers (Task 6, doa4_suitconv_slimwrite.cpp)
// slim_addon._set_user0(x): USER0's ntex, nvb, first VBUF, nib, first IBUF recomputed from the resource list (body in
// doa4_suitconv_slimread.cpp).
void SetUser0(tpr::Xpr& x);

// slim_addon._node_verts(keys, merged): one vertex list for a node over its keys (`keys` = indices into `merged`, in the
// writer's order). Identity = (payload vid, position) compared BY VALUE (0.0 == -0.0: one vertex); the vertex kept is
// the occurrence with the lowest (triangle, corner) over all keys; vertices are ordered by that lowest (triangle,
// corner). A NaN coordinate is refused with a ConvertError (Python's result there depends on object identity).
struct NodeVerts {
  std::vector<std::pair<std::array<double, 3>, uint32_t>> verts;  // (position, payload index), output order
  std::vector<std::vector<uint32_t>> remap;  // per entry of `keys`: merged vertex index -> output vertex index
};
NodeVerts NodeVertsOf(std::span<const uint32_t> keys, const Merged& merged, std::span<const ScanPayload> payload);

// TEST HOOKS (the test exe only; the game calls AddCopies / SlimCopies above): `layout` runs on the Layout right after
// MakeLayout (it lets a test put the copy ids out of step with the objects, a state no input reaches).
struct TestHooks {
  void (*layout)(Layout&) = nullptr;
};
Result AddCopies(std::string_view name, std::span<const uint8_t> tpr, std::span<const uint8_t> cat,
                 std::span<const uint8_t> ref_tpr, std::span<const uint8_t> ref_cat, CopyMode mode,
                 const TestHooks& hooks);

}  // namespace doa4::suitconv::slim
