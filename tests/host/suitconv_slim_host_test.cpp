// Host test for src/doa4_suitconv_slimread.cpp (the slim-copies container layer, Task 3 of the extras-copies plan): the
// type-2 map writer, cat_with_map (converter shape and the in-place branch), buf_set, read_suit's parse and its strip
// of old copies BY ID, and rebuild. Built and run by run_suitconv_slim_test.bat with the converter's compile flags.
//
// Every model is synthetic (slim_synth.h: built by the C++ writers from the literal numbers typed below; no game byte).
// The expected values are the Python tool's: `--dump DIR` writes every case's input files to DIR, and
//   py -3 -I tests/host/slim_expect.py <doax2_work>/physics_spike/scripts DIR
// runs slim_addon.read_suit / rebuild / cat_with_map on them and prints the table kExpected below (pasted verbatim).
// A summary is "OK fnv=<FNV-1a 64 of the output> len=<bytes> ..." or "REFUSED <the SlimError text>".
//
// Groups: synth_round_trip, strip_removes_copies, strip_not_trailing_in_list_order, strip_refusals, cat_map_converter,
// cat_map_in_place, cat_refusals, map_grammar, buf_set; the grid (Task 4, doa4_suitconv_slimgrid.cpp): merge_identity,
// merge_cell_mean, merge_degenerate_and_dup, merge_all_collapse, merge_keys_separate, merge_unreferenced, choose_h_fits,
// choose_h_too_small, deterministic, empty_key, bisect_le_edge, cell_mean_is_sequential, vid_membership,
// grid_refusals; the layout and scan (Task 5, doa4_suitconv_slimscan.cpp): channel_lowest_code, shallowest_then_lowest_id,
// ids_from_own_count, heaviest_tie_lowest_bone, first_vertex_after_swap, strip_tris_restart,
// corner_adds_world_translation, world_mats_compensated, t1_first_row_per_code, neck_n2, kind2_skips_untextured,
// matsrc_first_mesh, to_local_compensated, frame_dev_threshold, vids_one_to_one, key_fields_separate,
// index_slice_clamps, out_of_range_refuses, kind4_payload_has_no_attributes, scan_refusals; the writers and the assembly
// (Task 6, doa4_suitconv_slimwrite.cpp): same_pass_keys_keep_order, fusion_by_vid_and_position,
// vertex_order_by_first_appearance, reflection_frame_rotates_only_past_1e-6, shadow_minus_joint, bounds_signed_zero_tie,
// bounds_radius_compensated, placeholder_bytes, placeholder_texture_from_merged_order, collapse_refused,
// full_mode_never_refuses, map_rule_i1, kind_order_follows_marker_order, buffers_at_4k, user0_recomputed,
// ids_out_of_step_refused, twice_equal, self_reference, nrm_uv_short_refused, vertex_limit_edges,
// slim_copies_refusal_contract, add_refusals, add_rows. `--bench` prints the scan's time and AddCopies' (slim mode) on a
// synthetic suit of about 25000 triangles. Last line: "PASSED: N checks, 0 failures" or "FAILED: ...".

#include "../../src/doa4_suitconv_slim.h"
#include "slim_synth.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace tp = doa4::suitconv::tpr;
namespace sl = doa4::suitconv::slim;
using tp::Bytes;

static int g_checks = 0;
static int g_failures = 0;

static void Check(bool ok, const char* file, int line, const char* expr, const char* fmt = nullptr, ...) {
  ++g_checks;
  if (ok) return;
  ++g_failures;
  std::printf("FAIL %s:%d: %s", file, line, expr);
  if (fmt) {
    std::printf("  [");
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("]");
  }
  std::printf("\n");
}
#define CHECK(c) Check((c), __FILE__, __LINE__, #c)
#define CHECKF(c, ...) Check((c), __FILE__, __LINE__, #c, __VA_ARGS__)

static void RunGroup(void (*fn)(), const char* name) {
  const int before = g_checks, fb = g_failures;
  try {
    fn();
  } catch (const std::exception& e) {
    Check(false, __FILE__, __LINE__, "group threw", "%s: %s", name, e.what());
  }
  std::printf("%s: %d checks, %d failures\n", name, g_checks - before, g_failures - fb);
}

static uint64_t Fnv(const Bytes& b) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t v : b) {
    h ^= v;
    h *= 0x100000001b3ull;
  }
  return h;
}

static std::string OkLine(const Bytes& b) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "OK fnv=%016llx len=%zu", static_cast<unsigned long long>(Fnv(b)), b.size());
  return buf;
}

// ---------------------------------------------------------------- the models (literal numbers)
using synth::Group;
using synth::Main;

// Seven node ids: kept 0..3 (0 root; 1, 2 under 0; 3 under 1), old copies 4..6 (group 0x02: 4 with child 5; group
// 0x04: 6). Hie FRONT order [4, 0, 1, 2, 3, 5, 6]: a copy root before the kept nodes, and kept node 1 has its child 3
// at position 4 >= the kept count 4 (a positional children check would refuse it, a by-id one not).
static synth::Model Front() {
  synth::Model m;
  m.SetNodes({{4, -1, {5}, 0.5, 1.0, 0.0},
              {0, -1, {2, 3}, 0.0, 1.0, 0.0},
              {1, 1, {4}, 0.25, 0.5, 0.0},
              {2, 1, {}, -0.25, 0.5, 0.0},
              {3, 2, {}, 0.25, 0.25, 0.125},
              {5, 0, {}, 0.5, 0.75, 0.0},
              {6, -1, {}, 0.0, 0.0, 0.0}});
  std::vector<synth::Object> objs;
  for (uint32_t k = 0; k < 7; ++k) objs.push_back({k, k, k, k, 3 + k, 6 + k, k < 4 ? 0u : 1u});
  m.SetObjects(objs);
  // data: copies' buffers (VB4, VB5, VB6, IB4, IB5, IB6) between and after the kept ones; the textures after the
  // copies (TX0 with a mip inside its span); the data ends 0x800 past a 4 KiB boundary inside IB6's span.
  m.SetVbufs({{0x0000, 0x100, 0x11}, {0x2000, 0x104, 0x12}, {0x4000, 0x108, 0x13}, {0x5000, 0x10C, 0x14},
              {0x1000, 0x80, 0x15}, {0x3000, 0x84, 0x16}, {0x6000, 0x88, 0x17}});
  m.SetIbufs({{0x7000, 0x40, 0x21}, {0x8000, 0x44, 0x22}, {0xA000, 0x48, 0x23}, {0xB000, 0x4C, 0x24},
              {0x9000, 0x20, 0x25}, {0xC000, 0x24, 0x26}, {0xF000, 0x28, 0x27}});
  m.SetTextures({{0xD000, 0xD800, 0x400, 0x31}, {0xE000, 0, 0x200, 0x32}});
  m.SetDataEnd(0xF800);
  m.SetGroups({Main({{0x9001, 0}, {0x9002, 1}, {0x9003, 2}, {0x9004, 3}}), Group(2, {{0x9002, 4}, {0x9003, 5}}),
               Group(4, {{0x9002, 6}})});
  return m;
}

// The same suit without the copies, laid out where the strip leaves it (every removed span taken out of the data).
static synth::Model NoCopies() {
  synth::Model m;
  m.SetNodes({{0, -1, {1, 2}, 0.0, 1.0, 0.0}, {1, 0, {3}, 0.25, 0.5, 0.0}, {2, 0, {}, -0.25, 0.5, 0.0},
              {3, 1, {}, 0.25, 0.25, 0.125}});
  std::vector<synth::Object> objs;
  for (uint32_t k = 0; k < 4; ++k) objs.push_back({k, k, k, k, 3 + k, 6 + k, 0u});
  m.SetObjects(objs);
  m.SetVbufs({{0x0000, 0x100, 0x11}, {0x1000, 0x104, 0x12}, {0x2000, 0x108, 0x13}, {0x3000, 0x10C, 0x14}});
  m.SetIbufs({{0x4000, 0x40, 0x21}, {0x5000, 0x44, 0x22}, {0x6000, 0x48, 0x23}, {0x7000, 0x4C, 0x24}});
  m.SetTextures({{0x8000, 0x8800, 0x400, 0x31}, {0x9000, 0, 0x200, 0x32}});
  m.SetDataEnd(0xA000);
  m.SetGroups({Main({{0x9001, 0}, {0x9002, 1}, {0x9003, 2}, {0x9004, 3}})});
  return m;
}

// The copies at the last three positions but not in id order: [0, 1, 2, 3, 6, 4, 5].
static synth::Model TrailingUnordered() {
  synth::Model m = Front();
  m.SetNodes({{0, -1, {1, 2}, 0.0, 1.0, 0.0},
              {1, 0, {3}, 0.25, 0.5, 0.0},
              {2, 0, {}, -0.25, 0.5, 0.0},
              {3, 1, {}, 0.25, 0.25, 0.125},
              {6, -1, {}, 0.0, 0.0, 0.0},
              {4, -1, {6}, 0.5, 1.0, 0.0},
              {5, 5, {}, 0.5, 0.75, 0.0}});
  return m;
}

// The old strip's shape: the copies at the last positions in id order [0, 1, 2, 3, 4, 5, 6].
static synth::Model TrailingOrdered() {
  synth::Model m = Front();
  m.SetNodes({{0, -1, {1, 2}, 0.0, 1.0, 0.0},
              {1, 0, {3}, 0.25, 0.5, 0.0},
              {2, 0, {}, -0.25, 0.5, 0.0},
              {3, 1, {}, 0.25, 0.25, 0.125},
              {4, -1, {5}, 0.5, 1.0, 0.0},
              {5, 4, {}, 0.5, 0.75, 0.0},
              {6, -1, {}, 0.0, 0.0, 0.0}});
  return m;
}

// Front() with one edit, by Hie position (FRONT positions: 0 id4, 1 id0, 2 id1, 3 id2, 4 id3, 5 id5, 6 id6).
static synth::Model FrontWith(const std::function<void(synth::Model&)>& edit) {
  synth::Model m = Front();
  edit(m);
  return m;
}

// The container with USER3 and the first TX2D swapped (a USER resource out of place).
static Bytes UsersSwapped() {
  tp::Xpr x = tp::Xpr::Parse(Front().MakeTpr());
  std::swap(x.res[3], x.res[4]);
  return x.Build(1);
}

// ---------------------------------------------------------------- the cases
struct Case {
  std::string name;
  Bytes tpr, cat;   // a read case (cat holds the model's cat)
  Bytes map;        // a cat case: non-empty = cat_with_map(cat, map)
  bool is_cat = false;
};

static synth::Model StatesModel() {
  synth::Model m = Front();
  m.SetGroups({Main({{0x9001, 0}, {0x9002, 1}, {0xD00102, 2}, {0x9003, 2}, {0x9004, 3}}),
               synth::State(0x31, {{0x9003, 2}}), synth::State(0x32, {{0x9004, 3}, {0xD00304, 1}}),
               Group(2, {{0x9002, 4}, {0x9003, 5}}), Group(4, {{0x9002, 6}})});
  return m;
}

// The groups as slim_expect.py's desc() prints them: "main[9001:0,...] s10[...] 02[...]".
static std::string Desc(const std::vector<sl::MapGroup>& groups) {
  std::string s;
  char buf[32];
  for (size_t k = 0; k < groups.size(); ++k) {
    const auto& g = groups[k];
    if (k) s += " ";
    if (g.kind == sl::MapGroup::Kind::kMain) {
      s += "main";
    } else {
      std::snprintf(buf, sizeof buf, g.kind == sl::MapGroup::Kind::kState ? "s%02x" : "%02x", g.value);
      s += buf;
    }
    s += "[";
    for (size_t i = 0; i < g.items.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%s%x:%u", i ? "," : "", g.items[i].first, unsigned(g.items[i].second));
      s += buf;
    }
    s += "]";
  }
  return s;
}

static Case ReadCase(const std::string& name, const synth::Model& m) { return {name, m.MakeTpr(), m.MakeCat(), {}, false}; }
static Case CatCase(const std::string& name, Bytes cat, const std::vector<sl::MapGroup>& groups) {
  return {name, {}, std::move(cat), sl::MapBytes(groups), true};
}

// the retail-shaped cat's maps (port contract (i), test_slim_addon check_cat_map_in_place)
static std::vector<sl::MapGroup> RetailOld() {
  return {Main({{0x9001, 0}, {0x9002, 1}, {0x9003, 2}}), Group(4, {{0x9002, 3}}), Group(2, {{0x9002, 4}})};
}
static std::vector<sl::MapGroup> RetailSame() {
  return {Main({{0x9001, 1}, {0x9002, 0}, {0x9003, 2}}), Group(4, {{0x9002, 3}}), Group(2, {{0x9002, 4}})};
}
static std::vector<sl::MapGroup> RetailShort() {
  return {Main({{0x9001, 0}, {0x9002, 1}}), Group(4, {{0x9002, 3}}), Group(2, {{0x9002, 4}})};
}
static std::vector<sl::MapGroup> RetailLong() {
  return {Main({{0x9001, 0}, {0x9002, 1}, {0x9003, 2}, {0x9004, 5}}), Group(4, {{0x9002, 3}}), Group(2, {{0x9002, 4}})};
}

static std::vector<Case> Cases() {
  std::vector<Case> cs;
  cs.push_back(ReadCase("nocopy", NoCopies()));
  cs.push_back(ReadCase("strip_front", Front()));
  cs.push_back(ReadCase("strip_trail_unordered", TrailingUnordered()));
  cs.push_back(ReadCase("strip_trail_ordered", TrailingOrdered()));
  // the current map grammar: a 0xDX entry (24-bit code) in the main group and two '60 NN' state groups before the
  // copy groups (the shape of a reference with a 0xDX entry and state groups; every code, id and state number made up)
  cs.push_back(ReadCase("strip_states", StatesModel()));
  // fix round 1, I1: a copy's buffer base past the data end (VB4 at 0x10000, the kept VB3 at 0x11000, data ending at
  // 0xF800): both spans pass the 4 KiB check and the strip slices x.data[p:a] past its end, which Python clamps
  cs.push_back(ReadCase("strip_base_past_end", FrontWith([](synth::Model& m) {
    m.vbufs[4].base = 0x10000;
    m.vbufs[3].base = 0x11000;
  })));
  // fix round 1, I2: kept objects on buffer indices above a copy's, so the kept indices are renumbered (not the
  // identity): copy 4 on VB1 and IB0, kept object 1 on VB4, kept object 0 on IB4
  cs.push_back(ReadCase("strip_renumber", FrontWith([](synth::Model& m) {
    m.objects[4].vb = 1;
    m.objects[1].vb = 4;
    m.objects[4].ib = 0;
    m.objects[0].ib = 4;
  })));
  {
    Case c = ReadCase("ref_map_unknown", Front());  // an entry byte 0xB0 (top bit set, not 0x9X / 0xAX / 0xDX)
    c.cat = synth::Model::MakeCatWith({0x90, 0x01, 0x00, 0xB0, 0x00, 0x00, 0x00});
    cs.push_back(std::move(c));
  }
  {
    Case c = ReadCase("ref_map_state00", Front());  // '60 00'
    c.cat = synth::Model::MakeCatWith({0x90, 0x01, 0x00, 0x60, 0x00, 0x00});
    cs.push_back(std::move(c));
  }
  // refusals, one per SlimError of _users, _check_user0, _remove_resources, read_suit
  {
    Case c = ReadCase("ref_users", Front());
    c.tpr = UsersSwapped();
    cs.push_back(std::move(c));
  }
  cs.push_back(ReadCase("ref_user0", FrontWith([](synth::Model& m) {
    m.user0 = std::vector<uint32_t>{7, 4, 2, 5, 7, 6, 7, 13, 1, 2, 3, 16};
  })));
  cs.push_back(ReadCase("ref_span_start", FrontWith([](synth::Model& m) { m.vbufs[4].base = 0x1400; })));
  cs.push_back(ReadCase("ref_span_end", FrontWith([](synth::Model& m) { m.vbufs[1].base = 0x2800; })));
  cs.push_back(ReadCase("ref_span_shared", FrontWith([](synth::Model& m) { m.vbufs[1].base = 0x1000; })));
  cs.push_back(ReadCase("ref_top_ids", FrontWith([](synth::Model& m) {
    m.nodes.push_back({7, -1, {}, 0.0, 0.0, 0.0});
    m.objects.push_back({7, 7, 0, 0, 3, 6, 0});
  })));
  cs.push_back(ReadCase("ref_geo_word", FrontWith([](synth::Model& m) { m.objects[2].node_word = 3; })));
  cs.push_back(ReadCase("ref_info_word", FrontWith([](synth::Model& m) { m.objects[2].info_word1 = 3; })));
  cs.push_back(ReadCase("ref_ids", FrontWith([](synth::Model& m) { m.nodes[4].id = 5; })));
  cs.push_back(ReadCase("ref_parent_out", FrontWith([](synth::Model& m) { m.nodes[4].parent = 99; })));
  cs.push_back(ReadCase("ref_child_out", FrontWith([](synth::Model& m) { m.nodes[3].children = {99}; })));
  cs.push_back(ReadCase("ref_copy_kept_parent", FrontWith([](synth::Model& m) { m.nodes[5].parent = 1; })));
  cs.push_back(ReadCase("ref_copy_kept_child", FrontWith([](synth::Model& m) { m.nodes[0].children = {5, 3}; })));
  cs.push_back(ReadCase("ref_kept_copy_parent", FrontWith([](synth::Model& m) { m.nodes[3].parent = 0; })));
  cs.push_back(ReadCase("ref_kept_copy_child", FrontWith([](synth::Model& m) { m.nodes[4].children = {6}; })));
  cs.push_back(ReadCase("ref_main_names_copy", FrontWith([](synth::Model& m) { m.groups[0].items.push_back({0x9005, 4}); })));
  cs.push_back(ReadCase("ref_buffer_shared", FrontWith([](synth::Model& m) { m.objects[5].vb = 1; })));
  // cat_with_map: the converter shape
  const Bytes conv = Front().MakeCat();
  cs.push_back(CatCase("cat_conv_same", conv, Front().groups));
  cs.push_back(CatCase("cat_conv_longer", conv,
                       {Main({{0x9001, 0}, {0x9002, 1}, {0x9003, 2}, {0x9004, 3}}),
                        Group(2, {{0x9002, 4}, {0x9003, 5}, {0x9004, 7}, {0x9005, 8}}), Group(4, {{0x9002, 6}})}));
  cs.push_back(CatCase("cat_conv_shorter", conv, {Main({{0x9001, 0}, {0x9002, 1}})}));
  // cat_with_map: the in-place branch
  const Bytes retail = synth::Model::MakeRetailCat(sl::MapBytes(RetailOld()));
  cs.push_back(CatCase("cat_place_same", retail, RetailSame()));
  cs.push_back(CatCase("cat_place_short", retail, RetailShort()));
  cs.push_back(CatCase("cat_place_long", retail, RetailLong()));
  // cat_with_map with the current grammar: the same map back, and a changed one (a third state group, a second Dx)
  const Bytes states = StatesModel().MakeCat();
  cs.push_back(CatCase("cat_states_same", states, StatesModel().groups));
  {
    auto g = StatesModel().groups;
    g.insert(g.begin() + 3, synth::State(0x12, {{0x9001, 0}, {0xD01020, 1}}));
    cs.push_back(CatCase("cat_states_changed", states, g));
  }
  // cat_with_map: refusals
  {
    Bytes moved = retail;  // the directory's type-2 entry at 0x20
    const uint32_t D = doa4::suitconv::be::Reader(moved).U32(4);  // the directory; its first entry is (2, 0x18)
    doa4::suitconv::be::PackIntoU32(moved, D + 4, 0x20);
    cs.push_back(CatCase("cat_ref_shape", moved, RetailSame()));
  }
  {
    Bytes hdr = retail;
    hdr[3] = 1;
    cs.push_back(CatCase("cat_ref_header", hdr, RetailSame()));
  }
  {
    // a non-zero byte between the map's end byte and the next 8-byte boundary: a 4-byte map ends at 0x1c
    const std::vector<sl::MapGroup> one = {Main({{0x9001, 0}})};
    Bytes junk = synth::Model::MakeCatWith(sl::MapBytes(one));
    junk.at(0x1c) = 0x5A;
    cs.push_back(CatCase("cat_ref_after_map", junk, one));
  }
  {
    Bytes apa = conv;  // the type-5 block's magic broken
    const uint32_t D = doa4::suitconv::be::Reader(apa).U32(4);
    const uint32_t B = doa4::suitconv::be::Reader(apa).U32(D + 16 + 4);  // the third entry is (5, B)
    apa.at(B + 3) = 0x62;
    cs.push_back(CatCase("cat_ref_apa", apa, Front().groups));
  }
  return cs;
}

// What the C++ gives for a case, in slim_expect.py's format.
static std::string Summary(const Case& c) {
  try {
    if (c.is_cat) return OkLine(sl::CatWithMap(c.cat, sl::ParseMap(c.map, 0).first));
    const sl::Suit s = sl::ReadSuit(c.name, c.tpr, c.cat);
    std::string ids = "[";
    for (size_t k = 0; k < s.nodes.size(); ++k) ids += (k ? ", " : "") + std::to_string(s.nodes[k].id);
    return OkLine(sl::Rebuild(s)) + " stripped=" + s.stripped + " ids=" + ids + "] groups=" + Desc(s.groups);
  } catch (const doa4::suitconv::ConvertError& e) {
    return std::string("REFUSED ") + e.what();
  } catch (const std::exception& e) {  // never a refusal: a row fails by its text, the run goes on
    return std::string("EXCEPTION ") + e.what();
  }
}

// ---------------------------------------------------------------- expected (pasted from slim_expect.py)
struct Expected {
  const char* name;
  const char* summary;
};
// Pasted verbatim from (2026-11-23, slim_addon.py as of that day):
//   tests/host/run_suitconv_slim_test.bat --dump DIR
//   py -3 -I tests/host/slim_expect.py <doax2_work>/physics_spike/scripts DIR
// (its last line: "# buf_set(d, 0x3000, 0x80) = aabbccdd0000300300000082", pasted in TestBufSet)
static const Expected kExpected[] = {
    {"cat_conv_longer", "OK fnv=91c9f528613776a7 len=176"},
    {"cat_conv_same", "OK fnv=10679c3ba774c741 len=176"},
    {"cat_conv_shorter", "OK fnv=51f91cdffa3b6b2b len=160"},
    {"cat_place_long", "REFUSED cat directory [(2, 24), (5, 112), (4, 128)] not of the converter shape: new type-2 map 21 bytes is longer than the old map 18 bytes"},
    {"cat_place_same", "OK fnv=a06ad4aa06b77485 len=176"},
    {"cat_place_short", "OK fnv=cccda8c6b252427a len=176"},
    {"cat_ref_after_map", "REFUSED bytes after the type-2 map"},
    {"cat_ref_apa", "REFUSED type-5 block at 0x60 is not an apa block"},
    {"cat_ref_header", "REFUSED cat header not of the converter shape"},
    {"cat_ref_shape", "REFUSED cat directory [(2, 32), (5, 112), (4, 128)] not of the converter shape"},
    {"cat_states_changed", "OK fnv=27db24acd0f40be7 len=208"},
    {"cat_states_same", "OK fnv=53037409cf82aae7 len=192"},
    {"nocopy", "OK fnv=2b5cfef084a35e7a len=45056 stripped=nothing ids=[0, 1, 2, 3] groups=main[9001:0,9002:1,9003:2,9004:3]"},
    {"ref_buffer_shared", "REFUSED ref_buffer_shared: a buffer is shared by a kept object and an old copy"},
    {"ref_child_out", "REFUSED ref_child_out: Hie node 2 has a child at position 99, outside 0..6"},
    {"ref_copy_kept_child", "REFUSED ref_copy_kept_child: old copy node 4 has a kept child 2"},
    {"ref_copy_kept_parent", "REFUSED ref_copy_kept_parent: old copy node 5 has a kept parent 0"},
    {"ref_geo_word", "REFUSED ref_geo_word: object 2: MdlGeo node word 3, not its index"},
    {"ref_ids", "REFUSED ref_ids: Hie node ids are not 0..6, each once"},
    {"ref_info_word", "REFUSED ref_info_word: object 2: MdlInfo word 1 is 3, not its index"},
    {"ref_kept_copy_child", "REFUSED ref_kept_copy_child: kept node 3 has a child among the old copies"},
    {"ref_kept_copy_parent", "REFUSED ref_kept_copy_parent: kept node 2 has an old-copy parent 4"},
    {"ref_main_names_copy", "REFUSED ref_main_names_copy: the main group names an old copy node"},
    {"ref_map_state00", "REFUSED type-2 map: state switch 60 00 at 0x1b"},
    {"ref_map_unknown", "REFUSED type-2 map: unknown entry code 0xb0 at 0x1b"},
    {"ref_parent_out", "REFUSED ref_parent_out: Hie node 3 has its parent at position 99, outside 0..6"},
    {"ref_span_end", "REFUSED ref_span_end: copy buffer span 0x1000..0x2800 not on 4 KiB boundaries"},
    {"ref_span_shared", "REFUSED ref_span_shared: resource 7 shares a span with a copy buffer"},
    {"ref_span_start", "REFUSED ref_span_start: copy buffer span 0x1400..0x2000 not on 4 KiB boundaries"},
    {"ref_top_ids", "REFUSED ref_top_ids: old copies 4..6 are not the top 3 ids of the 8 nodes/objects"},
    {"ref_user0", "REFUSED ref_user0: USER0 [7, 4, 2, 5, 7, 6, 7, 13, 1, 2, 3, 16] does not describe its resource list [7, 4, 2, 4, 7, 6, 7, 13, 1, 2, 3, 16]"},
    {"ref_users", "REFUSED USER resources at [0, 1, 2, 4], expected 0..3"},
    {"strip_base_past_end", "OK fnv=5a8cedd8414d5bdb len=49152 stripped=old copies ids 4..6, 6 buffers ids=[0, 1, 2, 3] groups=main[9001:0,9002:1,9003:2,9004:3] 02[9002:4,9003:5] 04[9002:6]"},
    {"strip_front", "OK fnv=2b5cfef084a35e7a len=45056 stripped=old copies ids 4..6, 6 buffers ids=[0, 1, 2, 3] groups=main[9001:0,9002:1,9003:2,9004:3] 02[9002:4,9003:5] 04[9002:6]"},
    {"strip_renumber", "OK fnv=5d3a8f26b074fee2 len=45056 stripped=old copies ids 4..6, 6 buffers ids=[0, 1, 2, 3] groups=main[9001:0,9002:1,9003:2,9004:3] 02[9002:4,9003:5] 04[9002:6]"},
    {"strip_states", "OK fnv=2b5cfef084a35e7a len=45056 stripped=old copies ids 4..6, 6 buffers ids=[0, 1, 2, 3] groups=main[9001:0,9002:1,d00102:2,9003:2,9004:3] s31[9003:2] s32[9004:3,d00304:1] 02[9002:4,9003:5] 04[9002:6]"},
    {"strip_trail_ordered", "OK fnv=2b5cfef084a35e7a len=45056 stripped=old copies ids 4..6, 6 buffers ids=[0, 1, 2, 3] groups=main[9001:0,9002:1,9003:2,9004:3] 02[9002:4,9003:5] 04[9002:6]"},
    {"strip_trail_unordered", "OK fnv=2b5cfef084a35e7a len=45056 stripped=old copies ids 4..6, 6 buffers ids=[0, 1, 2, 3] groups=main[9001:0,9002:1,9003:2,9004:3] 02[9002:4,9003:5] 04[9002:6]"},
};

static const char* ExpectedFor(const std::string& name) {
  for (const auto& e : kExpected)
    if (name == e.name) return e.summary;
  return nullptr;
}

static void CheckCase(const std::string& name) {
  for (const Case& c : Cases()) {
    if (c.name != name) continue;
    const char* want = ExpectedFor(name);
    const std::string got = Summary(c);
    CHECKF(want != nullptr && got == want, "%s: got \"%s\" want \"%s\"", name.c_str(), got.c_str(), want ? want : "(none)");
    return;
  }
  CHECKF(false, "no case %s", name.c_str());
}

// ---------------------------------------------------------------- non-throwing drivers (a refusal is one failed check,
// never the end of a group)
static Bytes CatOr(std::span<const uint8_t> cat, const std::vector<sl::MapGroup>& groups) {
  try {
    return sl::CatWithMap(cat, groups);
  } catch (const std::exception&) {
    return {};
  }
}
// ReadSuit then Rebuild; empty on any exception. *stripped and *groups (when given) take the suit's fields.
static Bytes StripOr(const char* name, const Bytes& tpr, const Bytes& cat, std::string* stripped = nullptr,
                     std::vector<sl::MapGroup>* groups = nullptr) {
  try {
    const sl::Suit s = sl::ReadSuit(name, tpr, cat);
    if (stripped) *stripped = s.stripped;
    if (groups) *groups = s.groups;
    return sl::Rebuild(s);
  } catch (const std::exception&) {
    return {};
  }
}

// ---------------------------------------------------------------- groups
static void TestSynthRoundTrip() {
  const synth::Model m = NoCopies();
  const Bytes t = m.MakeTpr(), c = m.MakeCat();
  std::string stripped;
  std::vector<sl::MapGroup> groups;
  CHECK(StripOr("nocopy", t, c, &stripped, &groups) == t);
  CHECK(stripped == "nothing");
  CHECK(!groups.empty() && CatOr(c, groups) == c);
  CheckCase("nocopy");
  CheckCase("cat_conv_same");
  const Bytes fc = Front().MakeCat();
  CHECK(CatOr(fc, Front().groups) == fc);
  // the pasted table and the case list name the same cases
  const auto cases = Cases();
  size_t found = 0;
  for (const Case& cs : cases) found += ExpectedFor(cs.name) != nullptr;
  CHECKF(found == cases.size() && cases.size() == std::size(kExpected), "cases %zu, with a row %zu, rows %zu",
         cases.size(), found, std::size(kExpected));
}

static void TestStripRemovesCopies() {
  const Bytes want = NoCopies().MakeTpr();
  for (const char* name : {"strip_trail_ordered", "strip_trail_unordered"}) {
    const synth::Model m = std::string(name) == "strip_trail_ordered" ? TrailingOrdered() : TrailingUnordered();
    CHECKF(StripOr(name, m.MakeTpr(), m.MakeCat()) == want, "%s: not the model built without copies", name);
    CheckCase(name);
  }
  CheckCase("strip_base_past_end");
  CheckCase("strip_renumber");
  // fixture guard (I2): in strip_renumber a kept object uses a buffer index above a copy's, for VBs and for IBs
  synth::Model r = Front();
  r.objects[4].vb = 1, r.objects[1].vb = 4, r.objects[4].ib = 0, r.objects[0].ib = 4;
  CHECK(r.objects[1].vb > r.objects[4].vb && r.objects[0].ib > r.objects[4].ib);
}

static void TestStripNotTrailing() {
  // fixture guard: kept node 1 (position 2) has a child at a position >= the kept count 4
  const synth::Model f = Front();
  CHECK(f.nodes[2].id == 1 && f.nodes[2].children == std::vector<int64_t>{4});
  std::string stripped;
  CHECK(StripOr("strip_front", f.MakeTpr(), f.MakeCat(), &stripped) == NoCopies().MakeTpr());
  CHECK(stripped == "old copies ids 4..6, 6 buffers");
  CheckCase("strip_front");
}

static void TestStripRefusals() {
  for (const char* name : {"ref_users", "ref_user0", "ref_span_start", "ref_span_end", "ref_span_shared", "ref_top_ids",
                           "ref_geo_word", "ref_info_word", "ref_ids", "ref_parent_out", "ref_child_out",
                           "ref_copy_kept_parent", "ref_copy_kept_child", "ref_kept_copy_parent", "ref_kept_copy_child",
                           "ref_main_names_copy", "ref_buffer_shared"}) {
    const char* want = ExpectedFor(name);
    CHECKF(want != nullptr && std::strncmp(want, "REFUSED ", 8) == 0, "%s: the Python tool does not refuse it", name);
    CheckCase(name);
  }
}

static void TestCatMapConverter() {
  CheckCase("cat_conv_longer");
  CheckCase("cat_conv_shorter");
}

static void TestCatMapInPlace() {
  const Bytes old_map = sl::MapBytes(RetailOld());
  const Bytes c = synth::Model::MakeRetailCat(old_map);
  const size_t end = 0x18 + old_map.size();
  const Bytes same = sl::MapBytes(RetailSame()), shrt = sl::MapBytes(RetailShort()), lng = sl::MapBytes(RetailLong());
  // fixture: lengths, and the old map's bytes past the short map's end are not zero (a missing zero fill shows)
  CHECK(same.size() == old_map.size() && same != old_map && shrt.size() + 3 == old_map.size() && lng.size() > old_map.size());
  bool tail_nonzero = false;
  for (size_t k = 0x18 + shrt.size(); k + 1 < end; ++k) tail_nonzero = tail_nonzero || c[k] != 0;
  CHECK(tail_nonzero);
  {
    Bytes want(c.begin(), c.begin() + 0x18);
    want.insert(want.end(), same.begin(), same.end());
    want.insert(want.end(), c.begin() + end, c.end());
    CHECK(CatOr(c, RetailSame()) == want);
  }
  {
    Bytes want(c.begin(), c.begin() + 0x18);
    want.insert(want.end(), shrt.begin(), shrt.end());
    want.insert(want.end(), 3, uint8_t(0));
    want.insert(want.end(), c.begin() + end, c.end());
    CHECK(CatOr(c, RetailShort()) == want);
  }
  CheckCase("cat_place_same");
  CheckCase("cat_place_short");
  CheckCase("cat_place_long");
}

static void TestCatRefusals() {
  CheckCase("cat_ref_shape");
  CheckCase("cat_ref_header");
  CheckCase("cat_ref_after_map");
  CheckCase("cat_ref_apa");
}

// t2_bytes' refusals: each label's group, written by T2Bytes, against the text slim_expect.py prints for the same call
struct T2Assert {
  const char* label;
  sl::MapGroup group;
};
static std::vector<T2Assert> T2Asserts() {
  return {{"marker 0x60", Group(0x60, {})},
          {"marker 0", Group(0, {})},
          {"state 0x80", synth::State(0x80, {})},
          {"code 0x8fff", Main({{0x9001, 0}, {0x8FFF, 5}})},
          {"code 0xe00000", Main({{0xE00000, 1}})}};
}
static const Expected kT2Expected[] = {
    {"marker 0x60", "('type-2 group kind out of range', 96)"},
    {"marker 0", "('type-2 group kind out of range', 0)"},
    {"state 0x80", "('type-2 group kind out of range', ('state', 128))"},
    {"code 0x8fff", "('type-2 entry out of range', '0x8fff', 5)"},
    {"code 0xe00000", "('type-2 entry out of range', '0xe00000', 1)"},
};

static void TestMapGrammar() {
  CheckCase("strip_states");
  CheckCase("ref_map_unknown");
  CheckCase("ref_map_state00");
  CheckCase("cat_states_same");
  CheckCase("cat_states_changed");
  // the map read back from the states cat and written again is the same bytes
  const Bytes map = sl::MapBytes(StatesModel().groups);
  Bytes again;
  try {
    const auto parsed = sl::ParseMap(map, 0);
    again = sl::MapBytes(parsed.first);
    CHECK(parsed.second == map.size());
  } catch (const std::exception&) {
    CHECK(false);
  }
  CHECK(again == map);
  for (const auto& a : T2Asserts()) {
    const char* want = nullptr;
    for (const auto& e : kT2Expected)
      if (std::strcmp(e.name, a.label) == 0) want = e.summary;
    std::string got = "no refusal";
    try {
      sl::T2Bytes(a.group);
    } catch (const std::exception& e) {
      got = e.what();
    }
    CHECKF(want != nullptr && got == want, "%s: got \"%s\" want \"%s\"", a.label, got.c_str(), want ? want : "(none)");
  }
}

static void TestBufSet() {
  const Bytes d = {0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x01, 0x20, 0x03, 0x00, 0x00, 0x04, 0x02};
  // tpr.buf_set(d, 0x3000, 0x80) = aabbccdd0000300300000082 (slim_expect.py's last line): the low two bits of each
  // word kept, the head bytes unchanged
  const Bytes want = {0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x30, 0x03, 0x00, 0x00, 0x00, 0x82};
  Bytes got;
  try {
    got = sl::BufSet(d, 0x3000, 0x80);
  } catch (const std::exception&) {
  }
  CHECK(got == want);
  bool threw = false;
  try {
    sl::BufSet(d, 0x3002, 0x80);
  } catch (const doa4::suitconv::ConvertError&) {
    threw = true;
  }
  CHECK(threw);
}

// ---------------------------------------------------------------- grid merge and bisection (Task 4, slim.py)
// The ten checks of doax2_work/physics_spike/scripts/test_slim.py and test_slim_addon.py's check_bisect_le_edge, on the
// same fixtures (grid_tris :54, quad_tris :68). Python's string vids, payloads and tuple keys become integers (a
// one-to-one renaming: slim.py only compares and hashes them). Expected values are copied from the Python file (its
// line is named beside each); every fixture is also dumped (--dump DIR writes DIR/grid/<case>.grid) and run through
// slim.py itself:
//   py -3 -I tests/host/slim_expect.py --grid <doax2_work>/physics_spike/scripts DIR/grid
// whose rows are kGridExpected below (pasted verbatim); a row's fnv is over the canonical text Canon() builds.
namespace grid {

struct Corner {
  uint32_t vid;
  double x, y, z;
  uint32_t payload;
};
static Corner C(uint32_t vid, double x, double y, double z, uint32_t payload = 0) { return {vid, x, y, z, payload}; }
static sl::Tri T(uint32_t key, const Corner& a, const Corner& b, const Corner& c) {
  sl::Tri t{};
  t.key = key;
  const Corner* cs[3] = {&a, &b, &c};
  for (int i = 0; i < 3; ++i) {
    t.vid[i] = cs[i]->vid;
    t.pos[i] = {cs[i]->x, cs[i]->y, cs[i]->z};
    t.payload[i] = cs[i]->payload;
  }
  return t;
}
using Tris = std::vector<sl::Tri>;

static uint64_t Bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, sizeof u);
  return u;
}
static std::string Hex16(uint64_t u) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(u));
  return buf;
}
// slim_expect.py canon(): per key 'K<key>:', 'v<x>,<y>,<z>,<payload>;' per vertex (IEEE bits), 'f<i>,<j>,<k>;' per
// face, '|'.
static std::string Canon(const sl::Merged& m) {
  std::string s;
  for (const auto& k : m) {
    s += "K" + std::to_string(k.key) + ":";
    for (size_t i = 0; i < k.pos.size(); ++i) {
      s += "v" + Hex16(Bits(k.pos[i][0])) + "," + Hex16(Bits(k.pos[i][1])) + "," + Hex16(Bits(k.pos[i][2])) + ",";
      s += (i < k.payload.size() ? std::to_string(k.payload[i]) : std::string("?")) + ";";
    }
    for (const auto& f : k.faces)
      s += "f" + std::to_string(f[0]) + "," + std::to_string(f[1]) + "," + std::to_string(f[2]) + ";";
    s += "|";
  }
  return s;
}
static uint64_t FnvText(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char v : s) {
    h ^= v;
    h *= 0x100000001b3ull;
  }
  return h;
}
static std::string Summ(const sl::Merged& m) {
  return "n=" + std::to_string(sl::Count(m)) + " fnv=" + Hex16(FnvText(Canon(m)));
}

// An expected key written as test_slim.py writes it: (key, ([(pos, payload)...], [faces])).
struct WantKey {
  uint32_t key;
  std::vector<std::pair<std::array<double, 3>, uint32_t>> verts;
  std::vector<std::array<uint32_t, 3>> faces;
};
static std::string Canon(const std::vector<WantKey>& w) {
  sl::Merged m;
  for (const auto& k : w) {
    sl::MergedKey mk;
    mk.key = k.key;
    for (const auto& v : k.verts) {
      mk.pos.push_back(v.first);
      mk.payload.push_back(v.second);
    }
    mk.faces = k.faces;
    m.push_back(mk);
  }
  return Canon(m);
}

// Non-throwing drivers: a refusal is a value, never the end of a group.
struct Run {
  bool ok = false;
  std::string error;
  sl::Merged merged;
  double h = 0, lo = 0;
};
static Run MergeRun(const Tris& t, std::optional<double> h) {
  Run r;
  try {
    r.merged = sl::Merge(t, h, sl::Origin(t));
    r.ok = true;
  } catch (const std::exception& e) {
    r.error = e.what();
  }
  return r;
}
static Run MergeAt(const Tris& t, std::optional<double> h, const std::array<double, 3>& org) {
  Run r;
  try {
    r.merged = sl::Merge(t, h, org);
    r.ok = true;
  } catch (const std::exception& e) {
    r.error = e.what();
  }
  return r;
}
static Run ChooseRun(const Tris& t, size_t target) {
  Run r;
  try {
    sl::Chosen c = sl::ChooseH(t, target);
    r.merged = std::move(c.merged);
    r.h = c.h;
    r.lo = c.lo;
    r.ok = true;
  } catch (const std::exception& e) {
    r.error = e.what();
  }
  return r;
}
static std::array<double, 3> OriginOr(const Tris& t) {
  try {
    return sl::Origin(t);
  } catch (const std::exception&) {
    return {-12345.0, -12345.0, -12345.0};
  }
}
#define CHECK_MERGED(run, want)                                                                                    \
  do {                                                                                                             \
    const std::string got_ = (run).ok ? Canon((run).merged) : "REFUSED " + (run).error;                            \
    const std::string want_ = Canon(want);                                                                         \
    CHECKF(got_ == want_, "got %s want %s", got_.c_str(), want_.c_str());                                          \
  } while (0)

// ---- fixtures (test_slim.py)
// grid_tris (:54-65): 10x10 vertex plane, spacing 0.01, two triangles per square; vid = payload = 10 i + j.
static Tris GridTris(uint32_t key = 9) {
  auto v = [](int i, int j) {
    const uint32_t n = static_cast<uint32_t>(i * 10 + j);
    return C(n, i * 0.01, j * 0.01, 0.0, n);
  };
  Tris t;
  for (int j = 0; j < 9; ++j)
    for (int i = 0; i < 9; ++i) {
      t.push_back(T(key, v(i, j), v(i + 1, j), v(i + 1, j + 1)));
      t.push_back(T(key, v(i, j), v(i + 1, j + 1), v(i, j + 1)));
    }
  return t;
}
// grid_tris with triangle n under key ids[n % 3] (the deterministic check's key-id permutation).
static Tris GridKeyed(const std::array<uint32_t, 3>& ids) {
  Tris t = GridTris();
  for (size_t n = 0; n < t.size(); ++n) t[n].key = ids[n % 3];
  return t;
}
// quad_tris (:68-86), K = 5: Q0..Q3 = vids 0..3, C = vid 4 (payload = vid).
static Tris QuadTris() {
  const Corner q0 = C(0, 0.0, 0.0, 0.0, 0), q1 = C(1, 3.75, 0.0, 0.0, 1), q2 = C(2, 3.75, 3.75, 0.0, 2),
               q3 = C(3, 0.0, 3.75, 0.0, 3), c = C(4, 3.25, 0.5, 0.0, 4);
  return {T(5, q0, q1, c), T(5, q1, q2, c), T(5, q2, q3, c), T(5, q3, q0, c),
          T(5, c, q3, q2), T(5, q0, q1, q3), T(5, q0, q2, q3)};
}
// check_merge_identity (:92-98), K = 7, payload b'pN' = N.
static Tris IdentityTris() {
  const Corner v30 = C(30, 0, 0, 0, 30), v10 = C(10, 1, 0, 0, 10), v20 = C(20, 0, 1, 0, 20), v40 = C(40, 1, 1, 0, 40);
  return {T(7, v30, v10, v20), T(7, v10, v40, v20)};
}
// (:107-110), K2 = 8, payload 'sN' = N.
static Tris IdentityTris2() {
  const Corner s2 = C(2, 1, 0, 0, 2), s3 = C(3, 0, 1, 0, 3);
  return {T(8, C(1, 0.0, 0, 0, 1), s2, s3), T(8, C(4, -0.0, 0, 0, 4), s2, s3)};
}
// check_merge_cell_mean (:118-124), K = 3.
static Tris MeanTris() {
  const Corner v1 = C(1, 0.0, 0.0, 0.0, 1), v2 = C(2, 0.1, 0.0, 0.0, 2), v3 = C(3, 5.0, 0.0, 0.0, 3),
               v4 = C(4, 0.0, 5.0, 0.0, 4), v5 = C(5, 0.0, 0.0, 5.0, 5);
  return {T(3, v1, v3, v4), T(3, v2, v3, v5), T(3, v1, v4, v5)};
}
// (:146-155), K3 = 6: m1..m3 = 1..3, a..e = 4..8.
static Tris MeanNegTris() {
  const Corner m1 = C(1, 0.1, 0, 0, 1), m2 = C(2, 0.2, 0, 0, 2), m3 = C(3, 0.3, 0, 0, 3);
  const Corner a = C(4, -1.0, 0.5, -2.2, 4), b = C(5, 0.5, -1.5, 1.0, 5), c = C(6, -1.0, 0.5, -1.6, 6),
               d = C(7, -1.0, 0.5, -2.1, 7), e = C(8, -1.0, 0.5, -1.75, 8);
  return {T(6, m1, m2, a), T(6, m3, a, b), T(6, c, a, b), T(6, d, e, b)};
}
// check_merge_all_collapse (:191-192), K = 1, payload None = 0.
static Tris CollapseTris() { return {T(1, C(0, 0.0, 0, 0), C(1, 0.1, 0, 0), C(3, 0.3, 0, 0))}; }
// check_merge_keys_separate (:199-205): Kb = (0,'b') = 9 traversed first, Ka = (0,'a') = 4 (a LOWER id, so an
// id-ordered output differs from first traversal); vid 'v0' = 0 under both keys.
static Tris KeysTris() {
  return {T(9, C(0, 0, 0, 0, 0), C(1, 0, 0, 5, 1), C(2, 5, 5, 5, 2)),
          T(4, C(0, 0, 0, 0, 0), C(3, 5, 0, 0, 3), C(4, 0, 5, 0, 4))};
}
// check_merge_unreferenced (:214-221), K = 2.
static Tris UnrefTris() {
  const Corner u1 = C(1, 0, 0, 0, 1), u2 = C(2, 5, 0, 0, 2), u3 = C(3, 0, 5, 0, 3), u6 = C(6, 9.0, 9.0, 9.0, 6),
               u7 = C(7, 9.5, 9.0, 9.0, 7);
  return {T(2, u6, u7, u2), T(2, u1, u2, u3)};
}
// check_choose_h_too_small (:263-264), K = 4.
static Tris SmallTris() { return {T(4, C(0, 0, 0, 0), C(1, 2, 0, 0), C(2, 0, 2, 0))}; }
// check_empty_key (:322-331): Kd = (8,'dead') = 11, Kl = (8,'live') = 10.
static Tris EmptyKeyTris(bool identity) {
  const Corner d1 = C(1, 0, 0, 0, 1), d2 = C(2, 0.25, 0, 0, 2), d3 = C(3, 0, 0.5, 0, 3), d4 = C(4, 5, 5, 5, 4);
  const sl::Tri live = T(10, C(11, 0, 0, 0, 11), C(12, 5, 0, 0, 12), C(13, 0, 5, 0, 13));
  if (identity) return {T(11, d1, d1, d4), live};
  return {T(11, d1, d2, d3), T(11, d1, d1, d4), live};
}
static const WantKey kLiveWant = {10, {{{0.0, 0.0, 0.0}, 11}, {{5.0, 0.0, 0.0}, 12}, {{0.0, 5.0, 0.0}, 13}}, {{0, 1, 2}}};
// New (brief R5): one cell of three members whose x are 1e16, 1.0, -1e16 (h = 1e17, so all three share a cell),
// then a kept face; slim.py:68-71 sums them left to right from 0.0.
static Tris SeqTris() {
  const Corner m1 = C(1, 1e16, 0, 0, 1), m2 = C(2, 1.0, 0, 0, 2), m3 = C(3, -1e16, 0, 0, 3);
  return {T(1, m1, m2, m3), T(1, m1, C(4, 0, 2e17, 0, 4), C(5, 0, 0, 2e17, 5))};
}
// New (ruling R2): vids 1 and 2 at one position, vid 3 in the same 1.0-cell: three members, not two.
static Tris VidSharedPosTris() {
  const Corner a = C(1, 0, 0, 0, 1), b = C(2, 0, 0, 0, 2), c = C(3, 0.5, 0, 0, 3);
  return {T(1, a, b, c), T(1, a, C(4, 5, 0, 0, 4), C(5, 0, 5, 0, 5))};
}
// New (ruling R2): vid 1 met again in its cell at another position (one member, the first position) and once more in
// another cell (a member there too).
static Tris VidRepeatTris() {
  const Corner a = C(1, 0, 0, 0, 1), a2 = C(1, 0.5, 0, 0, 99), a3 = C(1, 5, 0, 0, 98), b = C(2, 0.25, 0, 0, 2);
  const Corner y = C(5, 0, 5, 0, 5), z = C(6, 0, 0, 5, 6);
  return {T(1, a, y, z), T(1, b, a2, a3), T(1, a3, y, z)};
}

// ---- the cases run through slim.py (the dump) and their expected rows
struct GridCase {
  std::string name;
  Tris tris;
  bool choose = false;
  std::optional<double> h;
  size_t target = 0;
};
static GridCase M(std::string n, Tris t, std::optional<double> h) { return {std::move(n), std::move(t), false, h, 0}; }
static GridCase Ch(std::string n, Tris t, size_t target) {
  return {std::move(n), std::move(t), true, std::nullopt, target};
}
static std::vector<GridCase> GridCases() {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return {
      M("identity", IdentityTris(), std::nullopt),
      M("identity2", IdentityTris2(), std::nullopt),
      M("cell_mean", MeanTris(), 1.0),
      M("cell_mean_neg", MeanNegTris(), 0.5),
      M("quad", QuadTris(), 1.0),
      M("all_collapse", CollapseTris(), 1.0),
      M("keys_separate", KeysTris(), 1.0),
      M("unreferenced", UnrefTris(), 1.0),
      M("grid_at_lo", GridTris(), 0.001),
      M("grid_at_hi", GridTris(), 1.0),
      Ch("choose_grid_30", GridTris(), 30),
      Ch("choose_grid_25", GridTris(), 25),
      Ch("too_small_0", SmallTris(), 0),
      Ch("too_small_2", SmallTris(), 2),
      Ch("too_small_3", SmallTris(), 3),
      M("empty_key_grid", EmptyKeyTris(false), 1.0),
      M("empty_key_identity", EmptyKeyTris(true), std::nullopt),
      M("cell_mean_seq", SeqTris(), 1e17),
      M("vid_shared_pos", VidSharedPosTris(), 1.0),
      M("vid_repeat", VidRepeatTris(), 1.0),
      M("keyed_grid", GridKeyed({5, 9, 2}), 0.025),
      Ch("choose_empty", {}, 5),
      M("pos_inf", {T(1, C(1, inf, 0, 0), C(2, 1, 0, 0), C(3, 0, 1, 0))}, 1.0),
      M("pos_nan_first", {T(1, C(1, nan, 0, 0), C(2, 1, 0, 0), C(3, 0, 1, 0))}, 1.0),
      M("h_zero", QuadTris(), 0.0),
      // fix round 1 (ChooseH's count-only probes): several keys sharing positions, a -0.0 beside 0.0 (one cell: the
      // first face is degenerate, count 3 at every h), and an infinity (the probe gives way to Merge, which raises)
      Ch("choose_keyed_grid", GridKeyed({5, 9, 2}), 60),
      Ch("choose_negzero",
         {T(1, C(1, 0.0, 0, 0, 1), C(2, -0.0, 0, 0, 2), C(3, 0, 2, 0, 3)),
          T(1, C(1, 0.0, 0, 0, 1), C(4, 2, 0, 0, 4), C(3, 0, 2, 0, 3))},
         3),
      Ch("choose_inf", {T(1, C(1, inf, 0, 0), C(2, 1, 0, 0), C(3, 0, 1, 0))}, 5),
      // a degenerate face whose cell no kept face references (u6, u7 at H_HI) must not count
      Ch("choose_unreferenced", UnrefTris(), 3),
      // cells far past 2^63 (Python's floor is a big int; the C++ keys the floored double, exact): 1e300 at h 0.001,
      // and x = 1e17 inside ChooseH (past 2^63 at H_LO only)
      M("big_cell_merge", {T(1, C(1, 1e300, 0, 0), C(2, 0, 0, 0), C(3, 0, 1, 0))}, 0.001),
      Ch("choose_bigcell",
         {T(1, C(1, 0, 0, 0), C(2, 1e17, 0, 0), C(3, 0, 2, 0)), T(1, C(1, 0, 0, 0), C(4, 0.5, 0, 0), C(3, 0, 2, 0))}, 3),
      // x = 1e306: finite at H_HI, an infinity at H_LO (the first probe after it), so merge(lo) raises OverflowError
      Ch("choose_overflow_at_lo",
         {T(1, C(1, 0, 0, 0), C(2, 1e306, 0, 0), C(3, 0, 2, 0)), T(1, C(1, 0, 0, 0), C(4, 0.5, 0, 0), C(3, 0, 2, 0))},
         3),
      // -0.0 beside 0.0 in Merge itself: one cell (the first face degenerate)
      M("negzero_merge",
        {T(1, C(1, 0.0, 0, 0, 1), C(2, -0.0, 0, 0, 2), C(3, 0, 2, 0, 3)),
         T(1, C(1, 0.0, 0, 0, 1), C(4, 2, 0, 0, 4), C(3, 0, 2, 0, 3))},
        1.0),
  };
}
// Pasted verbatim from (2026-11-23; the four choose_inf / choose_keyed_grid / choose_negzero / choose_unreferenced rows
// added in fix round 1 from the same command, then big_cell_merge / choose_bigcell / choose_overflow_at_lo /
// negzero_merge when the int64 cell deviation was withdrawn; the other 25 unchanged):
//   tests/host/run_suitconv_slim_test.bat --dump DIR
//   py -3 -I tests/host/slim_expect.py --grid <doax2_work>/physics_spike/scripts DIR/grid
// (py -3 = CPython 3.14.7, whose ZeroDivisionError text is "division by zero"; older ones say "float division by zero")
static const Expected kGridExpected[] = {
    {"all_collapse", "n=0 fnv=cbf29ce484222325"},
    {"big_cell_merge", "n=3 fnv=df5bcc337d3cc41e"},
    {"choose_bigcell", "h=3fe0000000000040 lo=3fdfffffffffc090 n=3 fnv=3f2841252947422e"},
    {"choose_overflow_at_lo", "REFUSED OverflowError: cannot convert float infinity to integer"},
    {"negzero_merge", "n=3 fnv=6da4117c7f0e7acc"},
    {"cell_mean", "n=4 fnv=cd7f7b107b169817"},
    {"cell_mean_neg", "n=4 fnv=3857fe37c3648a05"},
    {"cell_mean_seq", "n=3 fnv=8b5bf9a90140e8a2"},
    {"choose_empty", "REFUSED origin: no triangles"},
    {"choose_grid_25", "h=3f926e978d52c7b0 lo=3f926e978d4ec8b6 n=25 fnv=6aa665befd4df7e5"},
    {"choose_grid_30", "h=3f926e978d52c7b0 lo=3f926e978d4ec8b6 n=25 fnv=6aa665befd4df7e5"},
    {"choose_inf", "REFUSED OverflowError: cannot convert float infinity to integer"},
    {"choose_keyed_grid", "h=3f970a3d70a675c0 lo=3f970a3d70a276c7 n=38 fnv=97a10f8ea9e34b98"},
    {"choose_negzero", "h=3f50624dd2f1a9fc lo=3f50624dd2f1a9fc n=3 fnv=6da4117c7f0e7acc"},
    {"choose_unreferenced", "h=3fe9555555555570 lo=3fe9555555553578 n=3 fnv=0dde03b502129eeb"},
    {"empty_key_grid", "n=3 fnv=d2eb8400e834b41d"},
    {"empty_key_identity", "n=3 fnv=d2eb8400e834b41d"},
    {"grid_at_hi", "n=0 fnv=cbf29ce484222325"},
    {"grid_at_lo", "n=100 fnv=3a49efd2b8c50db1"},
    {"h_zero", "REFUSED ZeroDivisionError: division by zero"},
    {"identity", "n=4 fnv=e5fe8d38b315047c"},
    {"identity2", "n=4 fnv=3c4a789678e858e7"},
    {"keyed_grid", "n=34 fnv=45392ed65bee6637"},
    {"keys_separate", "n=6 fnv=925af96c1f788f04"},
    {"pos_inf", "REFUSED OverflowError: cannot convert float infinity to integer"},
    {"pos_nan_first", "REFUSED ValueError: cannot convert float NaN to integer"},
    {"quad", "n=4 fnv=dcce830d6584dd3a"},
    {"too_small_0", "REFUSED count 3 at H_HI = 1.0 exceeds target 0"},
    {"too_small_2", "REFUSED count 3 at H_HI = 1.0 exceeds target 2"},
    {"too_small_3", "h=3f50624dd2f1a9fc lo=3f50624dd2f1a9fc n=3 fnv=5c8d55cf4ab71703"},
    {"unreferenced", "n=3 fnv=0dde03b502129eeb"},
    {"vid_repeat", "n=4 fnv=81c2d4269d17983a"},
    {"vid_shared_pos", "n=3 fnv=2eeaf7b0e616e7b3"},
};
static std::string CaseSummary(const GridCase& c) {
  const Run r = c.choose ? ChooseRun(c.tris, c.target) : MergeRun(c.tris, c.h);
  if (!r.ok) return "REFUSED " + r.error;
  if (!c.choose) return Summ(r.merged);
  return "h=" + Hex16(Bits(r.h)) + " lo=" + Hex16(Bits(r.lo)) + " " + Summ(r.merged);
}
static void CheckGridCase(const std::string& name) {
  for (const GridCase& c : GridCases()) {
    if (c.name != name) continue;
    const char* want = nullptr;
    for (const auto& e : kGridExpected)
      if (name == e.name) want = e.summary;
    const std::string got = CaseSummary(c);
    CHECKF(want != nullptr && got == want, "%s: got \"%s\" want \"%s\"", name.c_str(), got.c_str(), want ? want : "(none)");
    return;
  }
  CHECKF(false, "no grid case %s", name.c_str());
}
static void WriteGridCase(const fs::path& dir, const GridCase& c) {
  std::string s = "case " + c.name + "\n";
  if (c.choose) {
    s += "choose " + std::to_string(c.target) + "\n";
  } else {
    s += std::string("merge ") + (c.h ? Hex16(Bits(*c.h)) : std::string("none")) + "\n";
  }
  for (const auto& t : c.tris) {
    s += "t " + std::to_string(t.key);
    for (int i = 0; i < 3; ++i) {
      s += " " + std::to_string(t.vid[i]);
      for (int a = 0; a < 3; ++a) s += " " + Hex16(Bits(t.pos[i][a]));
      s += " " + std::to_string(t.payload[i]);
    }
    s += "\n";
  }
  std::ofstream f(dir / (c.name + ".grid"), std::ios::binary);
  f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

}  // namespace grid

static void TestMergeIdentity() {  // test_slim.py check_merge_identity :91-114
  using namespace grid;
  const Run r = MergeRun(IdentityTris(), std::nullopt);
  // want :100-102
  CHECK_MERGED(r, (std::vector<WantKey>{{7,
                                         {{{0.0, 0.0, 0.0}, 30}, {{1.0, 0.0, 0.0}, 10}, {{0.0, 1.0, 0.0}, 20},
                                          {{1.0, 1.0, 0.0}, 40}},
                                         {{0, 1, 2}, {1, 3, 2}}}}));
  CHECK(r.ok && sl::Count(r.merged) == 4);  // :104
  // :112-113: two vids at one position stay two vertices; vid 4's -0.0 kept (bits compared)
  const Run r2 = MergeRun(IdentityTris2(), std::nullopt);
  CHECK_MERGED(r2, (std::vector<WantKey>{{8,
                                          {{{0.0, 0.0, 0.0}, 1}, {{1.0, 0.0, 0.0}, 2}, {{0.0, 1.0, 0.0}, 3},
                                           {{-0.0, 0.0, 0.0}, 4}},
                                          {{0, 1, 2}, {3, 1, 2}}}}));
  CheckGridCase("identity");
  CheckGridCase("identity2");
}

static void TestMergeCellMean() {  // check_merge_cell_mean :117-173
  using namespace grid;
  const Tris t = MeanTris();
  const auto org = OriginOr(t);
  CHECK((org == std::array<double, 3>{0.0, 0.0, 0.0}));  // :126
  const Run r = MergeAt(t, 1.0, org);
  const double ax = (0.0 + 0.1) / 2;  // :130
  // :131-133
  CHECK_MERGED(r, (std::vector<WantKey>{{3,
                                         {{{ax, 0.0, 0.0}, 1}, {{5.0, 0.0, 0.0}, 3}, {{0.0, 5.0, 0.0}, 4},
                                          {{0.0, 0.0, 5.0}, 5}},
                                         {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}}}}));
  const double every_corner = (0.0 + 0.1 + 0.0) / 3;  // :137-140
  CHECK(!(r.ok && !r.merged.empty() && !r.merged[0].pos.empty() && r.merged[0].pos[0][0] == every_corner));
  const Tris t3 = MeanNegTris();
  const auto org3 = OriginOr(t3);
  CHECK((org3 == std::array<double, 3>{-1.0, -1.5, -2.2}));  // :157
  const double seq_x = (((0.0 + 0.1) + 0.2) + 0.3) / 3;   // :164
  // :166 fixture precondition
  CHECK(seq_x != (((0.0 + 0.3) + 0.2) + 0.1) / 3);
  // :168-172
  CHECK_MERGED(MergeAt(t3, 0.5, org3),
               (std::vector<WantKey>{{6,
                                      {{{seq_x, 0.0, 0.0}, 1},
                                       {{-1.0, 0.5, (((0.0 + -2.2) + -2.1) + -1.75) / 3}, 4},
                                       {{0.5, -1.5, 1.0}, 5},
                                       {{-1.0, 0.5, -1.6}, 6}},
                                      {{0, 1, 2}, {3, 1, 2}}}}));
  CheckGridCase("cell_mean");
  CheckGridCase("cell_mean_neg");
}

static void TestMergeDegenerateAndDup() {  // check_merge_degenerate_and_dup :176-187
  using namespace grid;
  const Tris t = QuadTris();
  const auto org = OriginOr(t);
  CHECK((org == std::array<double, 3>{0.0, 0.0, 0.0}));  // :179
  // :184-186
  CHECK_MERGED(MergeAt(t, 1.0, org), (std::vector<WantKey>{{5,
                                                            {{{3.75, 3.75, 0.0}, 2}, {{0.0, 3.75, 0.0}, 3},
                                                             {{3.5, 0.25, 0.0}, 1}, {{0.0, 0.0, 0.0}, 0}},
                                                            {{0, 1, 2}, {1, 3, 2}, {3, 0, 1}}}}));
  CheckGridCase("quad");
}

static void TestMergeAllCollapse() {  // check_merge_all_collapse :190-195
  using namespace grid;
  const Run r = MergeRun(CollapseTris(), 1.0);
  CHECK(r.ok && r.merged.empty());           // :194
  CHECK(r.ok && sl::Count(r.merged) == 0);   // :195
  CheckGridCase("all_collapse");
}

static void TestMergeKeysSeparate() {  // check_merge_keys_separate :198-210
  using namespace grid;
  const Run r = MergeRun(KeysTris(), 1.0);
  // :207-208 (Kb = 9 first, Ka = 4 second)
  CHECK_MERGED(r, (std::vector<WantKey>{
                      {9, {{{0.0, 0.0, 0.0}, 0}, {{0.0, 0.0, 5.0}, 1}, {{5.0, 5.0, 5.0}, 2}}, {{0, 1, 2}}},
                      {4, {{{0.0, 0.0, 0.0}, 0}, {{5.0, 0.0, 0.0}, 3}, {{0.0, 5.0, 0.0}, 4}}, {{0, 1, 2}}}}));
  CHECK(r.ok && sl::Count(r.merged) == 6);  // :210
  CheckGridCase("keys_separate");
}

static void TestMergeUnreferenced() {  // check_merge_unreferenced :213-224
  using namespace grid;
  // :223
  CHECK_MERGED(MergeRun(UnrefTris(), 1.0),
               (std::vector<WantKey>{{2, {{{0.0, 0.0, 0.0}, 1}, {{5.0, 0.0, 0.0}, 2}, {{0.0, 5.0, 0.0}, 3}}, {{0, 1, 2}}}}));
  CheckGridCase("unreferenced");
}

static void TestChooseHFits() {  // check_choose_h_fits :227-258
  using namespace grid;
  CHECK(sl::kHLo == 0.001 && sl::kHHi == 1.0 && sl::kHalvings == 40);  // :229
  const Tris t = GridTris();
  CHECK(t.size() == 162);  // :231
  const auto org = OriginOr(t);
  CHECK((org == std::array<double, 3>{0.0, 0.0, 0.0}));  // :233
  const Run at_lo = MergeAt(t, 0.001, org);
  CHECK(at_lo.ok && sl::Count(at_lo.merged) == 100);  // :236
  const Run at_hi = MergeAt(t, 1.0, org);
  CHECK(at_hi.ok && sl::Count(at_hi.merged) == 0);    // :238
  const Run c = ChooseRun(t, 30);
  CHECKF(c.ok, "%s", c.error.c_str());
  CHECK(c.ok && sl::Count(c.merged) <= 30);  // :240
  CHECK(c.ok && c.h > c.lo);                 // :242
  const double bound = (1.0 - 0.001) / 1099511627776.0;  // :248, 2 ** 40
  const double width = c.h - c.lo;                         // :249
  CHECKF(c.ok && bound * (1 - 1e-6) < width && width <= bound * (1 + 1e-6), "width %.17g bound %.17g", width, bound);  // :250
  CHECK(c.ok && 0.001 < c.h && c.h <= 1.0);  // :252
  const Run n_lo = MergeAt(t, c.lo, org);
  CHECK(c.ok && n_lo.ok && sl::Count(n_lo.merged) > 30);  // :254-255
  const Run at_h = MergeAt(t, c.h, org);
  CHECK(c.ok && at_h.ok && !c.merged.empty() && Canon(c.merged) == Canon(at_h.merged));  // :258
  CheckGridCase("grid_at_lo");
  CheckGridCase("grid_at_hi");
  CheckGridCase("choose_grid_30");
}

static void TestChooseHTooSmall() {  // check_choose_h_too_small :261-277
  using namespace grid;
  const Tris t = SmallTris();
  const Run at_hi = MergeAt(t, 1.0, OriginOr(t));
  CHECK(at_hi.ok && sl::Count(at_hi.merged) == 3);  // :266
  for (size_t target : {size_t{0}, size_t{2}}) {    // :267-273 (the text: slim.py:147, through the dump rows)
    const Run r = ChooseRun(t, target);
    const std::string want = "count 3 at H_HI = 1.0 exceeds target " + std::to_string(target);
    CHECKF(!r.ok && r.error == want, "target %zu: got \"%s\"", target, r.error.c_str());
  }
  const Run r3 = ChooseRun(t, 3);
  CHECK(r3.ok && r3.h == 0.001 && r3.lo == 0.001);  // :276
  CHECK(r3.ok && sl::Count(r3.merged) == 3);        // :277
  CheckGridCase("too_small_0");
  CheckGridCase("too_small_2");
  CheckGridCase("too_small_3");
}

static void TestDeterministic() {  // check_deterministic :303-318, as ruled (R5)
  using namespace grid;
  const Run a = ChooseRun(GridTris(), 30), b = ChooseRun(GridTris(), 30);
  CHECK(a.ok && b.ok && !a.merged.empty() && Bits(a.h) == Bits(b.h) && Bits(a.lo) == Bits(b.lo) &&
        Canon(a.merged) == Canon(b.merged));
  const Tris q = QuadTris();
  const Run m1 = MergeAt(q, 1.0, OriginOr(q)), m2 = MergeAt(QuadTris(), 1.0, OriginOr(q));
  CHECK(m1.ok && m2.ok && !m1.merged.empty() && Canon(m1.merged) == Canon(m2.merged));
  // The same triangles with their key ids permuted (5 -> 2, 9 -> 5, 2 -> 9), traversal order unchanged: the same output
  // once the ids are mapped back. Each side has three keys (non-vacuous).
  const Run p1 = MergeRun(GridKeyed({5, 9, 2}), 0.025);
  Run p2 = MergeRun(GridKeyed({2, 5, 9}), 0.025);
  CHECK(p1.ok && p2.ok && p1.merged.size() == 3 && p2.merged.size() == 3);
  CHECK(p1.ok && p1.merged.size() == 3 && p1.merged[0].key == 5 && p1.merged[1].key == 9 && p1.merged[2].key == 2);
  for (auto& k : p2.merged) k.key = k.key == 2 ? 5 : k.key == 5 ? 9 : k.key == 9 ? 2 : 0;
  CHECK(p1.ok && p2.ok && !p1.merged.empty() && Canon(p1.merged) == Canon(p2.merged));
  CheckGridCase("keyed_grid");
}

static void TestEmptyKey() {  // check_empty_key :321-337
  using namespace grid;
  CHECK_MERGED(MergeRun(EmptyKeyTris(false), 1.0), (std::vector<WantKey>{kLiveWant}));         // :333
  CHECK_MERGED(MergeRun(EmptyKeyTris(true), std::nullopt), (std::vector<WantKey>{kLiveWant}));  // :337
  CheckGridCase("empty_key_grid");
  CheckGridCase("empty_key_identity");
}

static void TestBisectLeEdge() {  // test_slim_addon.py check_bisect_le_edge :945-957
  using namespace grid;
  const Tris t = GridTris();
  const auto org = OriginOr(t);
  const Run c = ChooseRun(t, 25);
  const Run at_lo = MergeAt(t, c.lo, org);
  const size_t n_h = c.ok ? sl::Count(c.merged) : 999, n_lo = at_lo.ok ? sl::Count(at_lo.merged) : 0;
  CHECKF(c.ok && n_h <= 25, "n_h %zu", n_h);    // :955
  CHECKF(c.ok && n_lo > 25, "n_lo %zu", n_lo);  // :956
  CHECK(c.ok && c.lo < c.h);                    // :957
  CheckGridCase("choose_grid_25");
}

static void TestCellMeanIsSequential() {  // brief R5; slim.py:68-71
  using namespace grid;
  const double plain = (((0.0 + 1e16) + 1.0) + -1e16) / 3;
  doa4::suitconv::num::PySum ps;
  for (double v : {1e16, 1.0, -1e16}) ps.Add(v);
  CHECKF(plain != ps.Result() / 3, "fixture precondition: plain %.17g compensated %.17g", plain, ps.Result() / 3);
  CHECK_MERGED(MergeRun(SeqTris(), 1e17),
               (std::vector<WantKey>{{1, {{{plain, 0.0, 0.0}, 1}, {{0.0, 2e17, 0.0}, 4}, {{0.0, 0.0, 2e17}, 5}}, {{0, 1, 2}}}}));
  CheckGridCase("cell_mean_seq");
}

static void TestVidMembership() {  // ruling R2
  using namespace grid;
  // three distinct vids in the cell: x = ((0 + 0) + 0.5) / 3, payload vid 1's
  CHECK_MERGED(MergeRun(VidSharedPosTris(), 1.0),
               (std::vector<WantKey>{{1,
                                      {{{((0.0 + 0.0) + 0.0 + 0.5) / 3, 0.0, 0.0}, 1},
                                       {{5.0, 0.0, 0.0}, 4},
                                       {{0.0, 5.0, 0.0}, 5}},
                                      {{0, 1, 2}}}}));
  CheckGridCase("vid_shared_pos");
  // cell 0 = [vid 1 at 0, vid 2 at 0.25] (vid 1's second position not a member); vid 1 at 5 is cell 3's member
  CHECK_MERGED(MergeRun(VidRepeatTris(), 1.0),
               (std::vector<WantKey>{{1,
                                      {{{(0.0 + 0.0 + 0.25) / 2, 0.0, 0.0}, 1},
                                       {{0.0, 5.0, 0.0}, 5},
                                       {{0.0, 0.0, 5.0}, 6},
                                       {{5.0, 0.0, 0.0}, 98}},
                                      {{0, 1, 2}, {3, 1, 2}}}}));
  CheckGridCase("vid_repeat");
}

static void TestGridRefusals() {
  using namespace grid;
  // Origin of nothing (slim.py:45-46), through ChooseH
  const Run e = ChooseRun({}, 5);
  CHECKF(!e.ok && e.error == "origin: no triangles", "got \"%s\"", e.error.c_str());
  CheckGridCase("choose_empty");
  CheckGridCase("pos_inf");
  CheckGridCase("pos_nan_first");
  CheckGridCase("h_zero");
  CheckGridCase("choose_keyed_grid");
  CheckGridCase("choose_negzero");
  CheckGridCase("choose_inf");
  CheckGridCase("choose_unreferenced");
  CheckGridCase("big_cell_merge");
  CheckGridCase("choose_bigcell");
  CheckGridCase("choose_overflow_at_lo");
  CheckGridCase("negzero_merge");
  // Every dumped case has its row, and every row its case.
  size_t rows = 0;
  for (const auto& r : kGridExpected) {
    (void)r;
    ++rows;
  }
  CHECKF(rows == GridCases().size(), "rows %zu cases %zu", rows, GridCases().size());
}

// ---------------------------------------------------------------- layout and scan (Task 5, doa4_suitconv_slimscan.cpp)
// Every scene is synthetic: a suit (main nodes, objects with real vertex records, a map with its main group only) and a
// reference (the suit's main nodes, plus copy nodes, one default object per node, a map with groups 0x02, a state group
// and 0x04). Each case is dumped (--dump DIR writes DIR/scan/<case>.tpr/.cat and <case>.ref.tpr/.ref.cat, plus
// DIR/scan/tolocal.txt) and run through the Python tool:
//   py -3 -I tests/host/slim_expect.py --scan <doax2_work>/physics_spike/scripts DIR/scan
// whose rows are kScanExpected and kToLocalExpected below (pasted verbatim). A row is the Layout's canonical text fnv,
// node_of_code, and per kind the triangle, key and vid counts, the fnv of the scan's canonical text (keys with every
// field and matsrc's fnv, vid sources, per corner vid, position bits and payload) and each triangle's copy node.
namespace scan {

struct NodeDef {
  int64_t id, parent_id;  // parent by id (-1: a root); positions follow the list order
  double tx, ty, tz;
  std::optional<std::array<double, 16>> mat = std::nullopt;
};
struct DeclDef {
  std::vector<Bytes> verts;
  std::vector<uint16_t> idx;
  std::vector<tp::DeclElem> elems = synth::StdElems();
  uint32_t stride = synth::kStride;
};
struct ObjDef {
  std::vector<uint16_t> palette;
  std::vector<DeclDef> decls;
  std::vector<synth::MeshSpec> meshes;
  uint32_t infos_short = 0;
};
using Rows = std::vector<std::pair<uint32_t, uint8_t>>;
struct Scene {
  std::vector<NodeDef> suit_nodes, ref_nodes;
  Rows main, ref_main, g2, g4;
  std::vector<ObjDef> objs;
  std::vector<sl::MapGroup> extra_groups;  // added to the reference's map after group 0x04
};

static std::vector<synth::Node> Tree(const std::vector<NodeDef>& defs) {
  std::vector<synth::Node> out;
  std::map<int64_t, int64_t> pos;
  for (size_t k = 0; k < defs.size(); ++k) pos[defs[k].id] = static_cast<int64_t>(k);
  for (const NodeDef& d : defs)
    out.push_back(synth::Node{d.id, d.parent_id < 0 ? -1 : pos.at(d.parent_id), {}, d.tx, d.ty, d.tz, d.mat});
  for (size_t k = 0; k < out.size(); ++k)
    if (out[k].parent >= 0) out[static_cast<size_t>(out[k].parent)].children.push_back(static_cast<int64_t>(k));
  return out;
}

static synth::Model SuitModel(const Scene& sc) {
  synth::Model m;
  m.SetNodes(Tree(sc.suit_nodes));
  uint32_t at = 0x100;
  const auto place = [&at](const Bytes& content) {
    const uint32_t base = static_cast<uint32_t>(tp::Al(at, 0x100));
    at = base + static_cast<uint32_t>(content.size());
    return synth::Buf{base, static_cast<uint32_t>(tp::Al(std::max<size_t>(content.size(), 4), 4)), 0, content};
  };
  for (size_t k = 0; k < sc.objs.size(); ++k) {
    const ObjDef& od = sc.objs[k];
    synth::Object o{static_cast<uint32_t>(k), static_cast<uint32_t>(k), 0, 0, 0, 0};
    o.palette = od.palette;
    o.meshes = od.meshes;
    o.infos_short = od.infos_short;
    for (const DeclDef& dd : od.decls) {
      Bytes vb;
      for (const Bytes& v : dd.verts) vb.insert(vb.end(), v.begin(), v.end());
      const uint32_t vbi = static_cast<uint32_t>(m.vbufs.size()), ibi = static_cast<uint32_t>(m.ibufs.size());
      m.vbufs.push_back(place(vb));
      m.ibufs.push_back(place(synth::Idx16(dd.idx)));
      o.decls.push_back(synth::DeclSpec{vbi, ibi, static_cast<uint32_t>(dd.verts.size()),
                                        static_cast<uint32_t>(dd.idx.size()), dd.stride, dd.elems});
    }
    m.objects.push_back(std::move(o));
  }
  m.SetDataEnd(static_cast<uint32_t>(tp::Al(at, 16)));
  m.SetGroups({synth::Main(sc.main)});
  return m;
}

static synth::Model RefModel(const Scene& sc) {
  synth::Model m;
  m.SetNodes(Tree(sc.ref_nodes));
  for (size_t k = 0; k < sc.ref_nodes.size(); ++k)
    m.objects.push_back(synth::Object{static_cast<uint32_t>(k), static_cast<uint32_t>(k), 0, 0, 1, 3});
  m.SetVbufs({{0x0, 0x10, 1}});
  m.SetIbufs({{0x10, 0x10, 2}});
  m.SetDataEnd(0x20);
  std::vector<sl::MapGroup> g = {synth::Main(sc.ref_main), synth::Group(2, sc.g2), synth::State(0x10, {{0x9001, 0}}),
                                 synth::Group(4, sc.g4)};
  g.insert(g.end(), sc.extra_groups.begin(), sc.extra_groups.end());
  m.SetGroups(g);
  return m;
}

static Bytes V(std::array<double, 3> p, std::array<double, 4> w = {1, 0, 0, 0}) {
  // a normal and a uv that differ per position, so payloads differ per vertex
  return synth::Vert(p, {p[1] * 0.5, 0.75, p[0] - 0.125}, {p[0] + 0.0625, 1.0 - p[2]}, w);
}

// The base scene. Suit (= reference main) nodes, list position = id:
//   0 root (0x9002); 1 under 0 (0x9003); 2 under 1 (0x9004); 3 under 0 (0x9006 AND 0x9005, rows in that order);
//   4 under 3 (0x9101, the neck); 5 under 2 (0x9007, a channel no copy group has).
// Reference copies: group 0x02 old ids 20 root (0x9002); 21 under 20 (0x9003); 22 under 21 (0x9004); 26 under 22
//   (0x9003 again, deeper, its row FIRST); 23 under 20 (0x9005, rotated 90 degrees about z); 24 under 20 (0x9006, a
//   small rotation); 25 under 20 (0x9008); 27, 28 under 20 (0x9101 on both, depth tie, 28's row first); 29 under 20
//   is in no group (left out of 0x02's children). Group 0x04: 30 root (0x9002); 31, 32 under 30 (0x9003, 0x9005).
// New ids (6 suit nodes): 20..28 -> 6..14, 30..32 -> 15..17.
static Scene Base() {
  Scene sc;
  sc.suit_nodes = {{0, -1, 0.5, 1.0, 0.25}, {1, 0, 0.125, 0.2, 0.3}, {2, 1, 0.0625, -0.125, 0.75},
                   {3, 0, -0.3, 0.1, 0.05},  {4, 3, 0.01, 0.02, 0.03},  {5, 2, 0.3, 0.3, -0.2}};
  sc.main = {{0x9002, 0}, {0x9003, 1}, {0x9004, 2}, {0x9006, 3}, {0x9005, 3}, {0x9101, 4}, {0x9007, 5}};
  sc.ref_main = sc.main;
  const double c = 0.0, s = 1.0;  // cos, sin of 90 degrees
  const std::array<double, 16> rot_z = {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0.05, 0.4, -0.1, 1};
  const std::array<double, 16> small = {1, 2e-6, 0, 0, -2e-6, 1, 0, 0, 0, 0, 1, 0, -0.05, 0.1, 0.2, 1};
  sc.ref_nodes = sc.suit_nodes;
  for (const NodeDef& d : std::vector<NodeDef>{{20, -1, 0.5, 1.0, 0.25},
                                               {21, 20, 0.125, 0.2, 0.3},
                                               {22, 21, 0.0625, -0.125, 0.75},
                                               {23, 20, 0, 0, 0, rot_z},
                                               {24, 20, 0, 0, 0, small},
                                               {25, 20, 0.2, -0.1, 0.0},
                                               {26, 22, 0.01, 0.0, 0.02},
                                               {27, 20, 0.0, 0.3, 0.1},
                                               {28, 20, 0.0, 0.31, 0.1},
                                               {29, 20, 0.7, 0.7, 0.7},
                                               {30, -1, 0.5, 1.0, 0.25},
                                               {31, 30, 0.125, 0.2, 0.3},
                                               {32, 30, -0.3, 0.1, 0.05}})
    sc.ref_nodes.push_back(d);
  sc.g2 = {{0x9002, 20}, {0x9003, 26}, {0x9003, 21}, {0x9004, 22}, {0x9005, 23},
           {0x9006, 24}, {0x9008, 25}, {0x9101, 28}, {0x9101, 27}};
  sc.g4 = {{0x9002, 30}, {0x9003, 31}, {0x9005, 32}};
  using MS = synth::MeshSpec;
  // O0: no palette (the object itself), one strip of 4
  sc.objs.push_back({{},
                     {{{V({0, 0, 0}), V({0.25, 0, 0}), V({0, 0.25, 0}), V({0.25, 0.25, 0.125})}, {0, 1, 2, 3}}},
                     {MS{.decl = 0, .istart = 0, .icount = 4, .tex = 1}}});
  // O1: palette [2, 1, 2]; v0 a tie (bone 2: 0.5, bone 1: 0.5 -> 1), v1 a sum (bone 2: 0.3 + 0.3 > bone 1: 0.4 -> 2),
  // v2 bone 1, v3 bone 2 (slot 3 is past the palette). Strip [0 1 2 3]: t0 = (0, 1, 2), t1 = (2, 1, 3) after the swap.
  sc.objs.push_back({{2, 1, 2},
                     {{{V({0, 0, 0.5}, {0.5, 0.5, 0, 0}), V({0.5, 0, 0.5}, {0.3, 0.4, 0.3, 0}),
                        V({0, 0.5, 0.5}, {0.125, 0.75, 0.125, 0}), V({0.5, 0.5, 0.5}, {0, 0.25, 0.5, 0.25})},
                       {0, 1, 2, 3}}},
                     {MS{.decl = 0, .istart = 0, .icount = 4, .tex = 2}}});
  // O2: three meshes on one decl: A (tex 3, pass 4, flag 1, word4 0x11), B untextured, C = A's key fields with
  // another index word and other bytes (same key: matsrc stays A).
  sc.objs.push_back({{},
                     {{{V({0, 0, 1}), V({0.5, 0, 1}), V({0, 0.5, 1}), V({0.5, 0.5, 1.25})}, {0, 1, 2, 1, 2, 3, 0, 2, 3}}},
                     {MS{.decl = 0, .istart = 0, .icount = 3, .tex = 3, .pass = 4, .flag = 1, .word4 = 0x11},
                      MS{.decl = 0, .istart = 3, .icount = 3, .tex = 0xFFFFFFFF},
                      MS{.decl = 0, .istart = 6, .icount = 3, .tex = 3, .pass = 4, .flag = 1, .word4 = 0x11,
                         .index_word = 2, .other = 0x55}}});
  // O3 (node with codes 0x9006 and 0x9005): decl 0 drawn by two keys (M1 tex 4, M2 tex 5) over the same vertices;
  // decl 1 has the same vertex numbers 0..2 (M3, M1's key).
  sc.objs.push_back({{},
                     {{{V({1, 0, 0}), V({1.25, 0, 0}), V({1, 0.25, 0})}, {0, 1, 2}},
                      {{V({2, 0, 0}), V({2.25, 0, 0}), V({2, 0.25, 0})}, {0, 1, 2}}},
                     {MS{.decl = 0, .istart = 0, .icount = 3, .tex = 4}, MS{.decl = 0, .istart = 0, .icount = 3, .tex = 5},
                      MS{.decl = 1, .istart = 0, .icount = 3, .tex = 4}}});
  // O4: the neck, palette [3, 3] (rule N2: slots [4, 3]); weights 0.625 / 0.375
  sc.objs.push_back({{3, 3},
                     {{{V({0, 1, 0}, {0.625, 0.375, 0, 0}), V({0.25, 1, 0}, {0.625, 0.375, 0, 0}),
                        V({0, 1.25, 0}, {0.625, 0.375, 0, 0})},
                       {0, 1, 2}}},
                     {MS{.decl = 0, .istart = 0, .icount = 3, .tex = 6}}});
  // O5: a restart and a degenerate window; mesh B's count runs past the index list (Python's slice clamps)
  sc.objs.push_back({{},
                     {{{V({3, 0, 0}), V({3.25, 0, 0}), V({3, 0.25, 0}), V({3.5, 0, 0}), V({3.5, 0.25, 0}),
                        V({3.75, 0, 0}), V({3.75, 0.25, 0}), V({4, 0.5, 0})},
                       {0, 1, 2, 0xFFFF, 3, 4, 5, 6, 7, 7, 0, 1, 3}}},
                     {MS{.decl = 0, .istart = 0, .icount = 8, .tex = 7}, MS{.decl = 0, .istart = 8, .icount = 100, .tex = 7}}});
  return sc;
}

// The key-field scene: Base with O0 replaced by two meshes A [0 1 2] and B [3 4 5] on one decl; `which` changes ONE of
// B's seven key fields (1 node, 2 tex, 3 pass, 4 flag, 5 word4, 6 m70, 7 m80), 0 changes none.
static Scene KeyScene(int which) {
  Scene sc = Base();
  using MS = synth::MeshSpec;
  const std::array<double, 4> w0 = {1, 0, 0, 0}, w1 = which == 1 ? std::array<double, 4>{0, 1, 0, 0} : w0;
  ObjDef o{{0, 1},
           {{{V({0, 0, 0}, w0), V({0.25, 0, 0}, w0), V({0, 0.25, 0}, w0), V({0.5, 0, 0}, w1), V({0.75, 0, 0}, w1),
              V({0.5, 0.25, 0}, w1)},
             {0, 1, 2, 3, 4, 5}}},
           {MS{.decl = 0, .istart = 0, .icount = 3, .tex = 1}, MS{.decl = 0, .istart = 3, .icount = 3, .tex = 1, .index_word = 1}}};
  MS& b = o.meshes[1];
  if (which == 2) b.tex = 9;
  if (which == 3) b.pass = 4;
  if (which == 4) b.flag = 1;
  if (which == 5) b.word4 = 1;
  if (which == 6) b.m70 = {0x70, 0x71, 0x72, 0x74};
  if (which == 7) b.m80 = 0x81;
  sc.objs[0] = o;
  return sc;
}

struct ScanCase {
  std::string name;
  Scene sc;
};

static std::vector<ScanCase> ScanCases() {
  std::vector<ScanCase> v;
  v.push_back({"base", Base()});
  {  // channel_map: 0x9005 also on node 4 (depth 2, its row first) -> the shallowest node 3 keeps 0x9005
    Scene sc = Base();
    for (Rows* rows : {&sc.main, &sc.ref_main}) rows->insert(rows->begin() + 3, {0x9005, 4});
    v.push_back({"chan_shallowest", sc});
  }
  {  // CHANNEL_ALIAS (a wrist-T1 reference): node 2 carries 0x9120 and no node 0x9004, group 0x02 has a 0x9004 row.
     // channel_map adds 0x9004 -> node 2 by the alias, so node 2's lowest code is 0x9004 -> node 8. Without the alias
     // node 2's channel is 0x9120 (no row) and its triangles walk up to node 1 (0x9003) -> node 7.
    Scene sc = Base();
    for (Rows* rows : {&sc.main, &sc.ref_main})
      for (auto& r : *rows)
        if (r.first == 0x9004) r.first = 0x9120;
    v.push_back({"alias_9004", sc});
  }
  {  // _heaviest's per-bone sum on a FIRST vertex: O1 drawn as [1 2 3] only, so t0 starts at v1 (palette [2, 1, 2],
     // weights 0.3, 0.4, 0.3): bone 2 sums 0.6 > bone 1's 0.4 -> node 8. Last slot wins per bone (0.3 < 0.4) or no
     // sum -> bone 1 -> node 7.
    Scene sc = Base();
    sc.objs[1].decls[0].idx = {1, 2, 3};
    sc.objs[1].meshes[0].icount = 3;
    v.push_back({"heaviest_sum", sc});
  }
  {  // a suit with one node more than the reference's main group: new ids from 7, not 6
    Scene sc = Base();
    sc.suit_nodes.push_back({6, 0, 0.4, 0.4, 0.4});
    sc.objs.push_back({{}, {{{V({5, 0, 0}), V({5.25, 0, 0}), V({5, 0.25, 0})}, {0, 1, 2}}},
                       {synth::MeshSpec{.decl = 0, .istart = 0, .icount = 3, .tex = 8}}});
    v.push_back({"ids_own_count", sc});
  }
  {  // node 1 a shear whose column 0 and 2 sum three products; node 2's translation 1e16, 1, -1e16 cancels in them:
     // builtin sum() keeps the 1 that a plain left-to-right sum loses
    Scene sc = Base();
    const std::array<double, 16> shear = {1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0.125, 0.2, 0.3, 1};
    const std::array<double, 16> big = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1e16, 1, -1e16, 1};
    for (auto* nodes : {&sc.suit_nodes, &sc.ref_nodes}) {
      (*nodes)[1].mat = shear;
      (*nodes)[2].mat = big;
    }
    v.push_back({"world_comp", sc});
  }
  {  // wrist T1: forearm 0x9120 rows (7) then (5); object 7's palette [hand 3, upper arm 6]
    Scene sc = Base();
    for (auto* nodes : {&sc.suit_nodes, &sc.ref_nodes}) {
      nodes->insert(nodes->begin() + 6, NodeDef{6, 0, -0.5, 0.2, 0.0});
      nodes->insert(nodes->begin() + 7, NodeDef{7, 6, -0.25, 0.0, 0.0});
    }
    for (Rows* rows : {&sc.main, &sc.ref_main}) {
      rows->push_back({0x9008, 6});
      rows->push_back({0x9120, 7});
      rows->push_back({0x9120, 5});
    }
    sc.objs.push_back({{}, {{{V({6, 0, 0}), V({6.25, 0, 0}), V({6, 0.25, 0})}, {0, 1, 2}}},
                       {synth::MeshSpec{.decl = 0, .istart = 0, .icount = 3, .tex = 8}}});
    const std::array<double, 4> w = {0.75, 0.25, 0, 0};
    sc.objs.push_back({{3, 6}, {{{V({7, 0, 0}, w), V({7.25, 0, 0}, w), V({7, 0.25, 0}, w)}, {0, 1, 2}}},
                       {synth::MeshSpec{.decl = 0, .istart = 0, .icount = 3, .tex = 8}}});
    v.push_back({"t1_first_row", sc});
  }
  static const char* kKeyNames[] = {"key_equal", "key_node", "key_tex", "key_pass", "key_flag", "key_word4", "key_m70",
                                    "key_m80"};
  for (int k = 0; k < 8; ++k) v.push_back({kKeyNames[k], KeyScene(k)});
  {  // packed elements (TDX T3.7: the formats real files carry): O1 with position F3 at 0, normal HEND3N at 12, uv0 H2
     // at 16, weights UB4N at 20 (integers 0..255, summed by _heaviest as ints), stride 24; the same bone choices as Base
    Scene sc = Base();
    const auto pv = [](std::array<double, 3> p, uint32_t nrm, uint16_t u, uint16_t v, std::array<uint8_t, 4> w) {
      Bytes b;
      for (double x : p) synth::PutF32(b, x);
      for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(nrm >> s));
      b.push_back(static_cast<uint8_t>(u >> 8)), b.push_back(static_cast<uint8_t>(u & 0xFF));
      b.push_back(static_cast<uint8_t>(v >> 8)), b.push_back(static_cast<uint8_t>(v & 0xFF));
      b.insert(b.end(), w.begin(), w.end());
      return b;
    };
    DeclDef& d = sc.objs[1].decls[0];
    d.verts = {pv({0, 0, 0.5}, 0x3FF001FFu, 0x3400, 0x3A00, {128, 128, 0, 0}),
               pv({0.5, 0, 0.5}, 0x80100401u, 0x3800, 0x3C00, {77, 102, 77, 0}),
               pv({0, 0.5, 0.5}, 0x001FFC00u, 0xB400, 0x3555, {32, 191, 32, 0}),
               pv({0.5, 0.5, 0.5}, 0x7FFFFFFFu, 0x0001, 0x7BFF, {0, 64, 128, 64})};
    d.elems = {tp::DeclElem{0, 0, 0x2A23B9, 0, 0, 0, 0}, tp::DeclElem{0, 12, 0x2A2190, 0, 3, 0, 0},
               tp::DeclElem{0, 16, 0x2C235F, 0, 5, 0, 0}, tp::DeclElem{0, 20, 0x1A2086, 0, 1, 0, 0}};
    d.stride = 24;
    v.push_back({"packed_elems", sc});
  }
  {  // a 0x80-byte mesh: m[0x70:0x74] lies in its stage, m[0x80:0xC0] is empty
    Scene sc = Base();
    sc.objs[0].meshes[0].size = 0x80;
    v.push_back({"short_mesh", sc});
  }
  {  // O0's last index names vertex 4 of a 4-vertex buffer: read from the data section after it, as Python reads it
    Scene sc = Base();
    sc.objs[0].decls[0].idx = {0, 1, 2, 4};
    v.push_back({"vertex_past_vbuf", sc});
  }
  // refusals
  {
    Scene sc = Base();
    sc.ref_nodes.push_back({9, 0, 0.1, 0.1, 0.1});
    sc.ref_main.push_back({0x9009, 9});
    v.push_back({"r_bind_missing", sc});
  }
  {
    Scene sc = Base();
    sc.ref_nodes[1].tx = 0.1262345;
    v.push_back({"r_bind_mm", sc});
  }
  {
    Scene sc = Base();
    sc.extra_groups.push_back(synth::Group(2, {{0x9002, 20}}));
    v.push_back({"r_two_groups", sc});
  }
  {
    Scene sc = Base();
    sc.g2.push_back({0x9007, 20});
    v.push_back({"r_named_twice", sc});
  }
  {
    Scene sc = Base();
    sc.g2.erase(sc.g2.begin() + 2);  // (0x9003, 21): 22's parent leaves the group
    v.push_back({"r_parent_outside", sc});
  }
  {
    Scene sc = Base();
    sc.ref_nodes.push_back({33, 30, 0.0, 0.0, 0.5});
    v.push_back({"r_shadow_child", sc});
  }
  {
    Scene sc = Base();
    sc.g2[0].first = 0x9001;
    v.push_back({"r_no_9002", sc});
  }
  {
    Scene sc = Base();
    sc.objs.push_back(sc.objs[0]);  // object 6, and no node 6
    v.push_back({"r_no_node", sc});
  }
  {
    Scene sc = Base();
    sc.objs[0].decls[0].elems = synth::StdElems(true, false, true);
    v.push_back({"r_no_uv", sc});
  }
  {
    Scene sc = Base();
    sc.objs[0].meshes[0].pass = 3;
    v.push_back({"r_pass", sc});
  }
  // Python raises (no SlimError): a vertex past the data end; an infos list shorter than the meshes
  {
    Scene sc = Base();
    sc.objs[5].decls[0].idx[2] = 3000;
    v.push_back({"x_vertex_past_data", sc});
  }
  {
    Scene sc = Base();
    sc.objs[2].infos_short = 1;
    v.push_back({"x_infos_short", sc});
  }
  return v;
}

static const ScanCase* FindCase(const std::string& name) {
  static const std::vector<ScanCase> cases = ScanCases();
  for (const auto& c : cases)
    if (c.name == name) return &c;
  return nullptr;
}

using grid::Bits;
using grid::FnvText;
using grid::Hex16;

static std::string HexBytes(const Bytes& b) {
  std::string s;
  char buf[4];
  for (uint8_t v : b) {
    std::snprintf(buf, sizeof buf, "%02x", v);
    s += buf;
  }
  return s;
}
static std::string Hx(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
  return buf;
}

// slim_expect.py scan_layout()
static std::string LayoutCanon(const sl::Layout& lay) {
  std::string s;
  for (int kind : {2, 4}) {
    const sl::CopyGroup& g = lay.Of(kind);
    s += "G" + std::to_string(kind) + ":r=";
    for (size_t i = 0; i < g.rows.size(); ++i) s += (i ? "," : "") + Hx(g.rows[i].first) + ":" + std::to_string(g.rows[i].second);
    s += ";i=";
    for (size_t i = 0; i < g.ids.size(); ++i) s += (i ? "," : "") + std::to_string(g.ids[i]);
    s += ";n=";
    bool first = true;
    for (const auto& [o, n] : g.new_id) s += (first ? "" : ",") + std::to_string(o) + ">" + std::to_string(n), first = false;
    s += ";h=";
    for (size_t i = 0; i < g.nodes.size(); ++i) {
      const auto& nd = g.nodes[i];
      s += (i ? "," : "") + std::to_string(nd.id) + "/" + std::to_string(nd.parent) + "/";
      for (size_t c = 0; c < nd.children.size(); ++c) s += (c ? "." : "") + std::to_string(nd.children[c]);
    }
    s += ";c=";
    first = true;
    for (const auto& [c, n] : g.node_of_code) s += (first ? "" : ",") + Hx(c) + ">" + std::to_string(n), first = false;
    s += "|";
  }
  s += "k=";
  for (size_t i = 0; i < lay.kinds.size(); ++i)
    s += (i ? "," : "") + std::string(lay.kinds[i].first == sl::MapGroup::Kind::kState ? "s" : "") + Hx(lay.kinds[i].second);
  s += "|a=";
  for (size_t i = 0; i < lay.allnodes.size(); ++i) s += (i ? "," : "") + std::to_string(lay.allnodes[i].id);
  s += "|";
  for (const auto& [id, f] : lay.frame) {
    s += "f" + std::to_string(id) + ":";
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) s += Hex16(Bits(f.R[r][c])) + ",";
    for (int a = 0; a < 3; ++a) s += Hex16(Bits(f.t[a])) + ",";
    s += Hex16(Bits(f.dev)) + ";";
  }
  return s;
}

// slim_expect.py scan_text()
static std::string ScanLine(int kind, const sl::Scan& sc) {
  std::string kt, vt, tt;
  for (const auto& k : sc.keys) {
    if (kind == 2) {
      kt += std::to_string(k.node) + "," + Hx(k.tex) + "," + std::to_string(k.pass) + "," + std::to_string(k.flag) + "," +
            Hx(k.word4) + "," + HexBytes(k.m70) + "," + HexBytes(k.m80) + "," + Hex16(Fnv(k.matsrc)) + ";";
    } else {
      kt += std::to_string(k.node) + ";";
    }
  }
  for (const auto& v : sc.vid_src) vt += std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]) + ";";
  std::string nodes;
  for (size_t t = 0; t < sc.tris.size(); ++t) {
    const sl::Tri& tr = sc.tris[t];
    if (t) tt += "|";
    tt += std::to_string(tr.key) + ":";
    for (int c = 0; c < 3; ++c) {
      const sl::ScanPayload& pl = sc.payload[tr.payload[c]];
      tt += std::to_string(tr.vid[c]) + "," + Hex16(Bits(tr.pos[c][0])) + "," + Hex16(Bits(tr.pos[c][1])) + "," +
            Hex16(Bits(tr.pos[c][2])) + "," + std::to_string(pl.vid) + "." + std::to_string(pl.tri) + "." +
            std::to_string(pl.corner);
      if (kind == 2) {
        tt += ".";
        for (int a = 0; a < pl.nrm_n; ++a) tt += (a ? "." : "") + Hex16(Bits(pl.nrm[a]));
        tt += "/";
        for (int a = 0; a < pl.uv_n; ++a) tt += (a ? "." : "") + Hex16(Bits(pl.uv[a]));
      }
      tt += ";";
    }
    nodes += (t ? "," : "") + std::to_string(sc.keys[tr.key].node);
  }
  const std::string text = "K" + kt + "V" + vt + "T" + tt;
  return "S" + std::to_string(kind) + "=n" + std::to_string(sc.tris.size()) + " k" + std::to_string(sc.keys.size()) + " v" +
         std::to_string(sc.vid_src.size()) + " fnv=" + Hex16(FnvText(text)) + " nodes" + std::to_string(kind) + "=[" + nodes +
         "]";
}

// A refusal as the Python row prints it: a SlimError (its text starts with the suit's name) or any other raise
static std::string Refusal(const std::string& name, const std::exception& e) {
  const std::string w = e.what();
  if (w.rfind(name + ": ", 0) == 0) return "REFUSED " + w;
  return "RAISED";
}

struct Built {
  Bytes tpr, cat, ref_tpr, ref_cat;
};
static Built BuildCase(const ScanCase& c) {
  const synth::Model s = SuitModel(c.sc), r = RefModel(c.sc);
  return {s.MakeTpr(), s.MakeCat(), r.MakeTpr(), r.MakeCat()};
}
struct Loaded {
  sl::Suit suit;
  doa4::suitconv::core::Model ref;
};
static Loaded Load(const std::string& name, const Built& b) {
  return {sl::ReadSuit(name, b.tpr, b.cat),
          doa4::suitconv::core::LoadModel(name + ".ref.tpr", b.ref_tpr, std::make_shared<const Bytes>(b.ref_cat))};
}

static std::string ScanSummary(const ScanCase& c) {
  Loaded ld;
  try {
    ld = Load(c.name, BuildCase(c));
  } catch (const std::exception& e) {
    return std::string("ERROR ") + e.what();
  }
  sl::Layout lay;
  try {
    sl::BindGuard(ld.suit, ld.ref);
    lay = sl::MakeLayout(ld.suit, ld.ref);
  } catch (const std::exception& e) {
    return Refusal(c.name, e);
  }
  std::string line = "L=" + Hex16(FnvText(LayoutCanon(lay)));
  for (int kind : {2, 4}) {
    line += " noc" + std::to_string(kind) + "=[";
    bool first = true;
    for (const auto& [code, n] : lay.Of(kind).node_of_code) line += (first ? "" : ",") + Hx(code) + ">" + std::to_string(n), first = false;
    line += "]";
  }
  for (int kind : {2, 4}) {
    try {
      line += " " + ScanLine(kind, sl::ScanKind(ld.suit, ld.ref, lay, kind));
    } catch (const std::exception& e) {
      line += " S" + std::to_string(kind) + "=" + Refusal(c.name, e);
    }
  }
  return line;
}

// The scan of one kind, or nothing (a refusal is a failed check by the caller, never the end of a group)
static std::optional<sl::Scan> ScanOf(const std::string& name, int kind) {
  const ScanCase* c = FindCase(name);
  if (!c) return std::nullopt;
  try {
    const Loaded ld = Load(c->name, BuildCase(*c));
    return sl::ScanKind(ld.suit, ld.ref, sl::MakeLayout(ld.suit, ld.ref), kind);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}
static std::optional<sl::Layout> LayoutOf(const std::string& name) {
  const ScanCase* c = FindCase(name);
  if (!c) return std::nullopt;
  try {
    const Loaded ld = Load(c->name, BuildCase(*c));
    return sl::MakeLayout(ld.suit, ld.ref);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}
// the copy node of each triangle whose corners come from object `obj` (vid_src), in scan order
static std::vector<int64_t> NodesOfObject(const sl::Scan& sc, uint32_t obj) {
  std::vector<int64_t> out;
  for (const auto& t : sc.tris)
    if (sc.vid_src[t.vid[0]][0] == obj) out.push_back(sc.keys[t.key].node);
  return out;
}

// every triangle's copy node, in scan order
static std::vector<int64_t> RowNodesOf(const sl::Scan& sc) {
  std::vector<int64_t> out;
  for (const auto& t : sc.tris) out.push_back(sc.keys[t.key].node);
  return out;
}

// ---------------------------------------------------------------- the frame cases (lod_chain._frame / _to_local)
struct ToLocalCase {
  const char* name;
  std::array<double, 16> w;
  std::array<double, 3> v;
  bool direction;
};
static std::vector<ToLocalCase> ToLocalCases() {
  const auto I = [](double r01, double tx, double ty, double tz) {
    return std::array<double, 16>{1, r01, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, tx, ty, tz, 1};
  };
  return {
      // row 0 of R is (1, 1, 1): q0 * 1 + q1 * 1 + q2 * 1 with q = (1e16, 1, -1e16) is 1 by builtin sum(), 0 plainly
      {"compensated", {1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5, 0.25, 0.125, 1}, {1e16 + 0.5, 1.25, -1e16 + 0.125}, false},
      {"dev_at_1e-6", I(1e-6, 0.5, 0.25, 0.125), {0.1, 0.2, 0.3}, false},        // dev == 1e-6: rotated (not <)
      {"dev_under", I(0.9e-6, 0.5, 0.25, 0.125), {0.1, 0.2, 0.3}, false},        // q unrotated
      {"dev_over", I(1.1e-6, 0.5, 0.25, 0.125), {0.1, 0.2, 0.3}, false},         // rotated
      {"direction", {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 9, 9, 9, 1}, {0.1, 0.2, 0.3}, true},  // no subtraction
      {"subtract", {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 9, 9, 9, 1}, {0.1, 0.2, 0.3}, false},
  };
}
static std::string ToLocalLine(const ToLocalCase& c) {
  const sl::Frame f = sl::FrameOf(std::vector<double>(c.w.begin(), c.w.end()));
  const auto q = sl::ToLocal(c.v, f, c.direction);
  return "dev=" + Hex16(Bits(f.dev)) + " q=" + Hex16(Bits(q[0])) + "," + Hex16(Bits(q[1])) + "," + Hex16(Bits(q[2]));
}

}  // namespace scan

// Pasted verbatim from (2026-11-23):
//   tests/host/run_suitconv_slim_test.bat --dump DIR
//   py -3 -I tests/host/slim_expect.py --scan <doax2_work>/physics_spike/scripts DIR/scan
static const Expected kScanExpected[] = {
    {"base", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k7 v29 fnv=4d90e93afc0e29b7 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v29 fnv=f23ee729c8c107c4 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"alias_9004", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k7 v29 fnv=4d90e93afc0e29b7 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v29 fnv=f23ee729c8c107c4 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"heaviest_sum", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n14 k7 v28 fnv=e9be1f64dddcfedc nodes2=[6,6,8,8,8,9,9,9,13,8,8,8,8,8] S4=n15 k3 v28 fnv=22854a0abbf78b81 nodes4=[15,15,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"chan_shallowest", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k7 v29 fnv=4d90e93afc0e29b7 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v29 fnv=f23ee729c8c107c4 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"ids_own_count", "L=92866ca5f3475738 noc2=[9002>7,9003>8,9004>9,9005>10,9006>11,9008>12,9101>14] noc4=[9002>16,9003>17,9005>18] S2=n16 k8 v32 fnv=09018aea29f3f060 nodes2=[7,7,8,8,9,9,10,10,10,14,9,9,9,9,9,7] S4=n17 k3 v32 fnv=f6ffccd18cea5b55 nodes4=[16,16,17,17,17,17,17,18,18,18,18,17,17,17,17,17,16]"},
    {"key_equal", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k7 v31 fnv=c6ca70044c9dced9 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v31 fnv=c9f5bee3d36dc796 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"key_flag", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k8 v31 fnv=07aa977d29639f1e nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v31 fnv=c9f5bee3d36dc796 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"key_m70", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k8 v31 fnv=d67f015d3d119d87 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v31 fnv=c9f5bee3d36dc796 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"key_m80", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k8 v31 fnv=31dd91ca56f13ebc nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v31 fnv=c9f5bee3d36dc796 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"key_node", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k8 v31 fnv=2d546fbccdeec4fe nodes2=[6,7,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v31 fnv=fe732e11c4160219 nodes4=[15,16,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"key_pass", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k8 v31 fnv=20c35aaee244eb85 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v31 fnv=c9f5bee3d36dc796 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"key_tex", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k8 v31 fnv=27a55e3dd6316f02 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v31 fnv=c9f5bee3d36dc796 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"key_word4", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k8 v31 fnv=80439472627698ee nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v31 fnv=c9f5bee3d36dc796 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"packed_elems", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k7 v29 fnv=618423fdbfdee2da nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v29 fnv=f23ee729c8c107c4 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"r_bind_missing", "REFUSED r_bind_missing: reference node 9 (0x9009) missing from the suit"},
    {"r_bind_mm", "REFUSED r_bind_mm: node 1 (0x9003) is 1.235 mm from the reference bind"},
    {"r_named_twice", "REFUSED r_named_twice: a group-0x2 node named twice in the reference"},
    {"r_no_9002", "L=5cd0f2adaa535532 noc2=[9001>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=REFUSED r_no_9002: reference group 0x2 has no 0x9002 row S4=n16 k3 v29 fnv=f23ee729c8c107c4 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"r_no_node", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=REFUSED r_no_node: object 6 has no node of that id S4=REFUSED r_no_node: object 6 has no node of that id"},
    {"r_no_uv", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=REFUSED r_no_uv: object 0 decl 0 has no normal or uv0 S4=n16 k3 v29 fnv=f23ee729c8c107c4 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"r_parent_outside", "REFUSED r_parent_outside: group-0x2 node 22 has its parent 21 outside the group"},
    {"r_pass", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=REFUSED r_pass: object 0 mesh 0 draw pass 3 S4=n16 k3 v29 fnv=f23ee729c8c107c4 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"r_shadow_child", "REFUSED r_shadow_child: shadow node 30 has a child 33 outside the group"},
    {"r_two_groups", "REFUSED r_two_groups: reference has no single group 0x2 (['main', 2, ('state', 16), 4, 2])"},
    {"short_mesh", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k7 v29 fnv=121f099ff89a1ad2 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v29 fnv=f23ee729c8c107c4 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"t1_first_row", "L=d0fa349280c474d3 noc2=[9002>8,9003>9,9004>10,9005>11,9006>12,9008>13,9101>15] noc4=[9002>17,9003>18,9005>19] S2=n17 k8 v35 fnv=52422f13b10a2b41 nodes2=[8,8,9,9,10,10,11,11,11,15,10,10,10,10,10,13,13] S4=n18 k3 v35 fnv=5a0196121fc33456 nodes4=[17,17,18,18,18,18,18,19,19,19,19,18,18,18,18,18,17,17]"},
    {"vertex_past_vbuf", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k7 v29 fnv=782f3167a3822b42 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v29 fnv=0a8561153ddedb47 nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"world_comp", "L=d175ca871c4d8aa6 noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=n15 k7 v29 fnv=bbec5af60ef42134 nodes2=[6,6,7,7,8,8,9,9,9,13,8,8,8,8,8] S4=n16 k3 v29 fnv=87354e0992bb2c4f nodes4=[15,15,16,16,16,16,16,17,17,17,17,16,16,16,16,16]"},
    {"x_infos_short", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=RAISED S4=RAISED"},
    {"x_vertex_past_data", "L=44ef8648abd4133a noc2=[9002>6,9003>7,9004>8,9005>9,9006>10,9008>11,9101>13] noc4=[9002>15,9003>16,9005>17] S2=RAISED S4=RAISED"},
};
static const Expected kToLocalExpected[] = {
    {"compensated", "dev=3ff0000000000000 q=3ff0000000000000,3ff0000000000000,c341c37937e08000"},
    {"dev_at_1e-6", "dev=3eb0c6f7a0b5ed8d q=bfd99999cf497ecf,bfa9999999999998,3fc6666666666666"},
    {"dev_under", "dev=3eae32f0ee144531 q=bfd999999999999a,bfa9999999999998,3fc6666666666666"},
    {"dev_over", "dev=3eb27476ca61b882 q=bfd99999d4a7e288,bfa9999999999998,3fc6666666666666"},
    {"direction", "dev=3ff0000000000000 q=3fc999999999999a,bfb999999999999a,3fd3333333333333"},
    {"subtract", "dev=3ff0000000000000 q=c02199999999999a,4021cccccccccccd,c021666666666666"},
};

static const char* ScanExpectedFor(const std::string& name) {
  for (const auto& e : kScanExpected)
    if (name == e.name) return e.summary;
  return nullptr;
}
static void CheckScanCase(const std::string& name) {
  const scan::ScanCase* c = scan::FindCase(name);
  const char* want = ScanExpectedFor(name);
  const std::string got = c ? scan::ScanSummary(*c) : std::string("(no case)");
  CHECKF(want != nullptr && got == want, "%s: got \"%s\" want \"%s\"", name.c_str(), got.c_str(), want ? want : "(none)");
}
// one node list of a kind, as the Python row prints it (the text between "nodes<kind>=[" and "]")
static std::string RowNodes(const std::string& name, int kind) {
  const char* want = ScanExpectedFor(name);
  if (!want) return "(none)";
  const std::string row = want, tag = "nodes" + std::to_string(kind) + "=[";
  const size_t at = row.find(tag);
  if (at == std::string::npos) return "(none)";
  const size_t end = row.find(']', at);
  return row.substr(at + tag.size(), end == std::string::npos ? std::string::npos : end - at - tag.size());
}
static std::string Join(const std::vector<int64_t>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
  return s;
}

static void TestChannelLowestCode() {
  // Node 3 carries 0x9006 (first row) and 0x9005: its channel is the LOWEST code, 0x9005 -> copy node 9 (old 23).
  // Wrong versions: highest code / first row -> 0x9006 -> node 10.
  CheckScanCase("base");
  const auto s2 = scan::ScanOf("base", 2);
  const auto n3 = s2 ? scan::NodesOfObject(*s2, 3) : std::vector<int64_t>{};
  CHECKF(!n3.empty() && n3 == std::vector<int64_t>(n3.size(), 9), "object 3 nodes %s (not 10)", Join(n3).c_str());
  const auto s4 = scan::ScanOf("base", 4);
  const auto m3 = s4 ? scan::NodesOfObject(*s4, 3) : std::vector<int64_t>{};
  CHECKF(!m3.empty() && m3 == std::vector<int64_t>(m3.size(), 17), "object 3 shadow nodes %s", Join(m3).c_str());
  // channel_map's own rule: 0x9005 on node 4 (depth 2, row first) and node 3 (depth 1) -> node 3. Wrong (first row):
  // node 3's channel becomes 0x9006 (node 10) and the neck's 0x9005 (node 9).
  CheckScanCase("chan_shallowest");
  const auto c2 = scan::ScanOf("chan_shallowest", 2);
  const auto c3 = c2 ? scan::NodesOfObject(*c2, 3) : std::vector<int64_t>{};
  const auto c4 = c2 ? scan::NodesOfObject(*c2, 4) : std::vector<int64_t>{};
  CHECKF(!c3.empty() && c3 == std::vector<int64_t>(c3.size(), 9) && c4 == std::vector<int64_t>{13}, "objects 3: %s, 4: %s",
         Join(c3).c_str(), Join(c4).c_str());
  // CHANNEL_ALIAS: 0x9120 on node 2, no 0x9004 in the main group -> the alias gives node 2 the code 0x9004 -> node 8.
  // Wrong (no alias): 0x9120 has no row, the walk reaches node 1 -> node 7.
  CheckScanCase("alias_9004");
  const auto a2 = scan::ScanOf("alias_9004", 2);
  const auto an = a2 ? scan::NodesOfObject(*a2, 2) : std::vector<int64_t>{};
  CHECKF(!an.empty() && an == std::vector<int64_t>(an.size(), 8), "alias_9004 object 2 nodes %s (not 7)", Join(an).c_str());
}

static void TestShallowestThenLowestId() {
  // group 0x02: 0x9003 on 26 (depth 3, row first) and 21 (depth 1) -> 21's new id 7; 0x9101 on 28 (row first) and 27,
  // both depth 1 -> the lower new id, 13. Wrong (first row wins): 12 and 14.
  const auto lay = scan::LayoutOf("base");
  CHECK(lay.has_value());
  if (!lay) return;
  const auto& noc = lay->Of(2).node_of_code;
  const auto at = [&noc](uint32_t c) { const auto it = noc.find(c); return it == noc.end() ? int64_t(-1) : it->second; };
  CHECKF(at(0x9003) == 7, "0x9003 -> %lld (not 12)", static_cast<long long>(at(0x9003)));
  CHECKF(at(0x9101) == 13, "0x9101 -> %lld (not 14)", static_cast<long long>(at(0x9101)));
  // the neck's triangles land on 13
  const auto s2 = scan::ScanOf("base", 2);
  const auto n4 = s2 ? scan::NodesOfObject(*s2, 4) : std::vector<int64_t>{};
  CHECKF(!n4.empty() && n4 == std::vector<int64_t>(n4.size(), 13), "object 4 nodes %s", Join(n4).c_str());
}

static void TestIdsFromOwnCount() {
  // 7 suit nodes, 6 in the reference's main group: group 0x02 ids start at 7 (not 6), group 0x04 at 16.
  CheckScanCase("ids_own_count");
  const auto lay = scan::LayoutOf("ids_own_count");
  CHECK(lay.has_value());
  if (!lay) return;
  CHECKF(lay->g2.new_id.size() == 9 && lay->g2.new_id.at(20) == 7 && lay->g2.new_id.at(28) == 15, "0x02 first id");
  CHECKF(lay->g4.new_id.size() == 3 && lay->g4.new_id.at(30) == 16, "0x04 first id");
  CHECK(lay->allnodes.size() == 19 && lay->allnodes[7].id == 7 && lay->allnodes[16].id == 16);
}

static void TestHeaviestTieLowestBone() {
  // O1, palette [2, 1, 2]: t0's first vertex is a tie (bone 2 in slot 0, bone 1 in slot 1, 0.5 each) -> bone 1 ->
  // 0x9003 -> node 7. Wrong: ties to the highest bone or to the first slot -> bone 2 -> node 8.
  const auto s2 = scan::ScanOf("base", 2);
  const auto n1 = s2 ? scan::NodesOfObject(*s2, 1) : std::vector<int64_t>{};
  CHECKF(n1.size() == 2 && n1[0] == 7, "object 1 nodes %s", Join(n1).c_str());
  // the per-bone sum on a first vertex (heaviest_sum: v1 = 0.3 + 0.3 on bone 2 against 0.4 on bone 1) -> node 8, not 7
  CheckScanCase("heaviest_sum");
  const auto h2 = scan::ScanOf("heaviest_sum", 2);
  const auto hs = h2 ? scan::NodesOfObject(*h2, 1) : std::vector<int64_t>{};
  CHECKF(hs == (std::vector<int64_t>{8}), "heaviest_sum object 1 nodes %s (not 7)", Join(hs).c_str());
  // the same rule over UB4N integer weights (128 / 128 tie), HEND3N normals and H2 uvs (Python's row)
  CheckScanCase("packed_elems");
  const auto p2 = scan::ScanOf("packed_elems", 2);
  const auto q1 = p2 ? scan::NodesOfObject(*p2, 1) : std::vector<int64_t>{};
  CHECKF(q1 == (std::vector<int64_t>{7, 7}), "packed object 1 nodes %s", Join(q1).c_str());
}

static void TestFirstVertexAfterSwap() {
  // O1's strip [0 1 2 3]: t1 = (2, 1, 3) after the swap; its first vertex v2 is bone 1 -> node 7. Unswapped (1, 2, 3)
  // starts at v1 (bone 2 by the summed slots 0.3 + 0.3 > 0.4) -> node 8; the last corner v3 -> node 8.
  const auto s2 = scan::ScanOf("base", 2);
  const auto n1 = s2 ? scan::NodesOfObject(*s2, 1) : std::vector<int64_t>{};
  CHECKF(n1.size() == 2 && n1[1] == 7, "object 1 nodes %s", Join(n1).c_str());
  bool order = false;
  if (s2)
    for (const auto& t : s2->tris)
      if (s2->vid_src[t.vid[0]][0] == 1 && s2->vid_src[t.vid[0]][2] == 2)
        order = s2->vid_src[t.vid[1]][2] == 1 && s2->vid_src[t.vid[2]][2] == 3;
  CHECKF(order, "t1's corners are (2, 1, 3)");
}

static void TestStripTrisRestart() {
  // O5 mesh A [0 1 2 FFFF 3 4 5 6] -> (0 1 2), (3 4 5), (5 4 6); mesh B idx[8:108] = [7 7 0 1 3] -> (0 7 1), (0 1 3)
  const auto s4 = scan::ScanOf("base", 4);
  std::vector<std::array<uint32_t, 3>> got;
  if (s4)
    for (const auto& t : s4->tris)
      if (s4->vid_src[t.vid[0]][0] == 5)
        got.push_back({s4->vid_src[t.vid[0]][2], s4->vid_src[t.vid[1]][2], s4->vid_src[t.vid[2]][2]});
  const std::vector<std::array<uint32_t, 3>> want = {{0, 1, 2}, {3, 4, 5}, {5, 4, 6}, {0, 7, 1}, {0, 1, 3}};
  CHECKF(got == want, "object 5 gives %zu triangles", got.size());
}

static void TestCornerAddsWorldTranslation() {
  // O2's vertex (0, 0, 1) on node 2: world translation 0 + 1 + 2 = (0.6875, 1.075, 1.3) per axis in double.
  // Wrong: the node's own translation (0.0625, -0.125, 0.75) or none.
  const auto s4 = scan::ScanOf("base", 4);
  bool found = false;
  if (s4)
    for (const auto& t : s4->tris)
      for (int c = 0; c < 3; ++c)
        if (s4->vid_src[t.vid[c]][0] == 2 && s4->vid_src[t.vid[c]][2] == 0) {
          const double jx = static_cast<double>(0.5f) + static_cast<double>(0.125f) + static_cast<double>(0.0625f);
          found = t.pos[c][0] == 0.0 + jx && t.pos[c][2] == 1.0 + ((static_cast<double>(0.25f) + static_cast<double>(0.3f)) +
                                                                   static_cast<double>(0.75f));
        }
  CHECKF(found, "object 2 vertex 0 position = local + world translation");
  CheckScanCase("base");
}

static void TestWorldMatsCompensated() {
  // Precondition: on this chain a plain left-to-right sum of world(2)'s x translation differs from builtin sum().
  const float big = 1e16f;
  const double prods[4] = {static_cast<double>(big) * 1.0, 1.0 * 1.0, -static_cast<double>(big) * 1.0,
                           1.0 * (static_cast<double>(0.125f) + static_cast<double>(0.5f))};
  double plain = 0.0;
  doa4::suitconv::num::PySum ps;
  for (double p : prods) plain += p, ps.Add(p);
  CHECKF(plain != ps.Result(), "plain %.17g python %.17g", plain, ps.Result());
  CheckScanCase("world_comp");  // the scan's positions (Python's bits) on object 2 and 5
}

static void TestT1FirstRowPerCode() {
  // 0x9120's rows: (7) then (5). The first row's object 7 has palette [hand 3, upper arm 6]: T1 -> slots [7, 7] ->
  // 0x9120 has no row -> up to 6 (0x9008) -> node 13. Wrong (last row wins: object 5, no T1): literal slots, bone 3
  // (0.75) -> 0x9005 -> node 11.
  CheckScanCase("t1_first_row");
  const auto s2 = scan::ScanOf("t1_first_row", 2);
  const auto n7 = s2 ? scan::NodesOfObject(*s2, 7) : std::vector<int64_t>{};
  CHECKF(n7 == std::vector<int64_t>{13}, "object 7 nodes %s", Join(n7).c_str());
}

static void TestNeckN2() {
  // O4, the neck, palette [3, 3] -> slots [4, 3]: 0.625 on the neck itself -> 0x9101 -> node 13. Literal [3, 3] ->
  // bone 3 -> node 9.
  const auto s2 = scan::ScanOf("base", 2);
  const auto n4 = s2 ? scan::NodesOfObject(*s2, 4) : std::vector<int64_t>{};
  CHECKF(n4 == std::vector<int64_t>{13}, "object 4 nodes %s", Join(n4).c_str());
}

static void TestKind2SkipsUntextured() {
  // O2's mesh B has no stage-0 texture: kind 2 has 2 of O2's triangles, kind 4 all 3.
  const auto s2 = scan::ScanOf("base", 2), s4 = scan::ScanOf("base", 4);
  CHECK(s2 && s4 && scan::NodesOfObject(*s2, 2).size() == 2 && scan::NodesOfObject(*s4, 2).size() == 3);
}

static void TestMatsrcFirstMesh() {
  // O2's meshes A and C give one key; matsrc is A's record (C differs in its index word and other bytes).
  const scan::ScanCase* c = scan::FindCase("base");
  const auto s2 = scan::ScanOf("base", 2);
  bool ok = false;
  if (c && s2) {
    const Bytes a = synth::MeshBytes(c->sc.objs[2].meshes[0]), cc = synth::MeshBytes(c->sc.objs[2].meshes[2]);
    for (const auto& t : s2->tris)
      if (s2->vid_src[t.vid[0]][0] == 2) ok = a != cc && s2->keys[t.key].matsrc == a;
  }
  CHECKF(ok, "O2's key keeps mesh A");
}

static void TestToLocalCompensated() {
  for (const auto& c : scan::ToLocalCases()) {
    if (std::string(c.name) != "compensated" && std::string(c.name) != "direction" && std::string(c.name) != "subtract")
      continue;
    const char* want = nullptr;
    for (const auto& e : kToLocalExpected)
      if (std::string(c.name) == e.name) want = e.summary;
    const std::string got = scan::ToLocalLine(c);
    CHECKF(want && got == want, "%s: got \"%s\" want \"%s\"", c.name, got.c_str(), want ? want : "(none)");
  }
}

static void TestFrameDevThreshold() {
  for (const auto& c : scan::ToLocalCases()) {
    if (std::string(c.name).rfind("dev_", 0) != 0) continue;
    const char* want = nullptr;
    for (const auto& e : kToLocalExpected)
      if (std::string(c.name) == e.name) want = e.summary;
    const std::string got = scan::ToLocalLine(c);
    CHECKF(want && got == want, "%s: got \"%s\" want \"%s\"", c.name, got.c_str(), want ? want : "(none)");
  }
  // dev exactly 1e-6 rotates: q0 = 0.1 - 0.5 + (0.2 - 0.25) * 1e-6, not 0.1 - 0.5
  const auto cs = scan::ToLocalCases();
  const sl::Frame f = sl::FrameOf(std::vector<double>(cs[1].w.begin(), cs[1].w.end()));
  CHECK(f.dev == 1e-6 && sl::ToLocal(cs[1].v, f, false)[0] != 0.1 - 0.5);
}

static void TestVidsOneToOne() {
  // O3: decl 0 and decl 1 both use vertex numbers 0..2 -> six vids; decl 0's vertices drawn by two keys (tex 4, tex 5)
  // keep one vid each; O0 and O3 share vertex numbers 0..2 and get different vids.
  const auto s2 = scan::ScanOf("base", 2);
  std::set<std::array<uint32_t, 3>> srcs;
  std::set<uint32_t> keys3, vids3;
  if (s2) {
    for (const auto& v : s2->vid_src) srcs.insert(v);
    for (const auto& t : s2->tris)
      if (s2->vid_src[t.vid[0]][0] == 3) {
        keys3.insert(t.key);
        for (uint32_t v : t.vid) vids3.insert(v);
      }
  }
  CHECKF(s2 && srcs.size() == s2->vid_src.size(), "vid sources are distinct");
  CHECKF(keys3.size() == 2 && vids3.size() == 6, "object 3: %zu keys, %zu vids", keys3.size(), vids3.size());
}

static void TestKeyFieldsSeparate() {
  // KeyScene(k): mesh B differs from mesh A in ONE key field; each gives one key more than key_equal.
  const auto keys = [](const std::string& n, int kind) {
    const auto s = scan::ScanOf(n, kind);
    return s ? static_cast<long long>(s->keys.size()) : -1LL;
  };
  const long long base2 = keys("key_equal", 2), base4 = keys("key_equal", 4);
  CHECK(base2 > 0 && base4 > 0);
  for (const char* n : {"key_node", "key_tex", "key_pass", "key_flag", "key_word4", "key_m70", "key_m80"}) {
    CheckScanCase(n);
    CHECKF(keys(n, 2) == base2 + 1, "%s: %lld keys, key_equal %lld", n, keys(n, 2), base2);
  }
  CheckScanCase("key_equal");
  // kind 4 keys by node only: no field but the node moves a kind-4 triangle. key_node sends O0's second triangle from
  // node 15 to node 16, a key O1 already made (Python's rows: k3 in both, nodes4 [15,15,...] -> [15,16,...]).
  const auto e4 = scan::ScanOf("key_equal", 4), n4 = scan::ScanOf("key_node", 4), t4 = scan::ScanOf("key_tex", 4);
  CHECKF(e4 && n4 && t4 && e4->tris.size() > 1 && n4->tris.size() == e4->tris.size() &&
             e4->keys[e4->tris[1].key].node == 15 && n4->keys[n4->tris[1].key].node == 16 &&
             keys("key_node", 4) == base4 && keys("key_tex", 4) == base4 &&
             scan::RowNodesOf(*t4) == scan::RowNodesOf(*e4),
         "kind 4 keys by node only");
}

static void TestIndexSliceClamps() {
  // O5 mesh B: istart 8 + count 100 past the 13 indices -> Python's shorter slice, no refusal (2 triangles)
  const auto s4 = scan::ScanOf("base", 4);
  CHECK(s4 && scan::NodesOfObject(*s4, 5).size() == 5);
  CheckScanCase("short_mesh");  // m[0x80:0xC0] of a 0x80-byte record is empty
  const auto sm = scan::ScanOf("short_mesh", 2);
  CHECK(sm && !sm->keys.empty() && sm->keys[0].m80.empty() && sm->keys[0].m70.size() == 4);
  CheckScanCase("vertex_past_vbuf");
}

static void TestOutOfRangeRefuses() {
  CheckScanCase("x_vertex_past_data");
  CheckScanCase("x_infos_short");
  CHECK(!scan::ScanOf("x_vertex_past_data", 4) && !scan::ScanOf("x_infos_short", 2));
}

static void TestKind4PayloadHasNoAttributes() {
  const auto s2 = scan::ScanOf("base", 2), s4 = scan::ScanOf("base", 4);
  bool k4 = s4 && !s4->payload.empty(), k2 = s2 && !s2->payload.empty();
  if (s4)
    for (const auto& p : s4->payload) k4 = k4 && p.nrm_n == 0 && p.uv_n == 0;
  if (s2)
    for (const auto& p : s2->payload) k2 = k2 && p.nrm_n == 3 && p.uv_n == 2;
  CHECK(k4 && k2);
  // payload index = 3 t + c, its triangle and corner
  bool idx = s4.has_value();
  if (s4)
    for (size_t t = 0; t < s4->tris.size(); ++t)
      for (uint32_t c = 0; c < 3; ++c) {
        const auto& p = s4->payload[s4->tris[t].payload[c]];
        idx = idx && s4->tris[t].payload[c] == 3 * t + c && p.tri == t && p.corner == c && p.vid == s4->tris[t].vid[c];
      }
  CHECK(idx);
}

static void TestScanRefusals() {
  for (const char* n : {"r_bind_missing", "r_bind_mm", "r_two_groups", "r_named_twice", "r_parent_outside",
                        "r_shadow_child", "r_no_9002", "r_no_node", "r_no_uv", "r_pass"})
    CheckScanCase(n);
  // every case has its row and every row its case
  size_t rows = 0, found = 0;
  for (const auto& e : kScanExpected) {
    ++rows;
    found += scan::FindCase(e.name) != nullptr;
  }
  CHECKF(rows == found && rows == scan::ScanCases().size(), "rows %zu found %zu cases %zu", rows, found,
         scan::ScanCases().size());
  size_t trows = 0;
  for (const auto& e : kToLocalExpected) (void)e, ++trows;
  CHECKF(trows == scan::ToLocalCases().size(), "to_local rows %zu", trows);
}

// ---------------------------------------------------------------- writers and assembly (Task 6, doa4_suitconv_slimwrite.cpp)
// Every case is a scan scene (above) whose reference gives each copy object one decl of `count` vertices (the slim
// target is the group's count sum minus its node count) and the first group-0x02 object a mesh of the native 2-stage
// shape. Each case is dumped (--dump DIR writes DIR/add/<case>.tpr/.cat/.ref.tpr/.ref.cat, <case>.hook for a test-hook
// case, plus DIR/add/nodeverts.txt and DIR/add/bounds.txt) and run through the Python tool:
//   py -3 -I tests/host/slim_expect.py --add <doax2_work>/physics_spike/scripts DIR/add
// whose rows are kAddExpected, kNodeVertsExpected and kBoundsExpected below (pasted verbatim). A row is
// "tpr=<SHA-1>:<bytes> cat=<SHA-1>:<bytes>" of add_copies' output, "REFUSED <SlimError text>" or "RAISED" (any other
// raise; Python names its type in a comment line), per case and mode (<case>/full, <case>/slim).
namespace add {

using scan::DeclDef;
using scan::NodeDef;
using scan::ObjDef;
using scan::Rows;
using scan::Scene;
using scan::V;
using MS = synth::MeshSpec;

// SHA-1 (FIPS 180-4), for the rows' output hashes
static std::string Sha1Hex(const Bytes& msg) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  Bytes m = msg;
  const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  const auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t(m[off + 4 * i]) << 24) | (uint32_t(m[off + 4 * i + 1]) << 16) | (uint32_t(m[off + 4 * i + 2]) << 8) |
             uint32_t(m[off + 4 * i + 3]);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d), k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d, k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d, k = 0xCA62C1D6u;
      }
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  char buf[48];
  std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3], h[4]);
  return buf;
}

struct AddCase {
  std::string name;
  Scene sc;
  int count = 40;          // decl head word 5 of every reference copy object
  bool g4_first = false;   // the reference's map holds group 0x04 before group 0x02
  bool self = false;       // the suit carries stand-in copies (sc.g2 / sc.g4 over its own ids) and is its own reference
  bool hook_swap = false;  // test hook: the last two copy nodes of Layout::allnodes swapped
  bool tmpl_bad = false;   // the first group-0x02 object's mesh is NOT of the 2-stage shape (0x100 bytes, one stage)
};

// The reference's node ids made equal to their list positions (add_copies reads the reference's object k as node id k,
// ref.geo[i] for a group id i, as real files are laid out): parents, group rows and extra groups follow.
static void Renumber(Scene& sc) {
  std::map<int64_t, int64_t> to;
  for (size_t k = 0; k < sc.ref_nodes.size(); ++k) to[sc.ref_nodes[k].id] = static_cast<int64_t>(k);
  for (NodeDef& n : sc.ref_nodes) {
    n.id = to.at(n.id);
    if (n.parent_id >= 0) n.parent_id = to.at(n.parent_id);
  }
  for (Rows* rows : {&sc.ref_main, &sc.g2, &sc.g4})
    for (auto& r : *rows) r.second = static_cast<uint8_t>(to.at(r.second));
  for (auto& g : sc.extra_groups)
    for (auto& r : g.items) r.second = static_cast<uint8_t>(to.at(r.second));
}

// The scan base with one more shadow row (group 0x04 gains 0x9008 on a node under its root, a channel no suit node
// has, so shadow node 18 is never fed: its placeholder), the reference renumbered by position: main 0..5, copies (scan
// ids 20..33) 6..19 (6 root of 0x02 with 7 (0x9003), 8 (0x9004, under 7), 9 (0x9005, rotated 90 degrees), 10 (0x9006,
// small rotation), 11 (0x9008), 12 (0x9003 deeper, under 8), 13 and 14 (0x9101); 15 in no group; 16 root of 0x04 with
// 17 (0x9003), 18 (0x9005), 19 (0x9008)). New ids: 0x02 6..14 (equal), 0x04 16..19 -> 15..18.
static Scene AddBase(const std::function<void(Scene&)>& before_renumber = nullptr) {
  Scene sc = scan::Base();
  sc.ref_nodes.push_back({33, 30, 0.1, 0.0, 0.2});
  sc.g4.push_back({0x9008, 33});
  if (before_renumber) before_renumber(sc);
  Renumber(sc);
  return sc;
}

// the reference of a case: scan::RefModel's, every copy object given one decl of `count` vertices and one mesh; the
// first (lowest id) group-0x02 object's mesh is the native 2-stage shape (0x140 bytes, stages (0, 0, 0x99), (1, 1, 0x78))
static synth::Model RefOf(const AddCase& c) {
  synth::Model m = scan::RefModel(c.sc);
  std::set<int64_t> g2, g4;
  for (const auto& r : c.sc.g2) g2.insert(r.second);
  for (const auto& r : c.sc.g4) g4.insert(r.second);
  const int64_t first2 = g2.empty() ? -1 : *g2.begin();
  for (size_t k = 0; k < c.sc.ref_nodes.size(); ++k) {
    const int64_t id = c.sc.ref_nodes[k].id;
    if (!g2.count(id) && !g4.count(id)) continue;
    synth::Object o{static_cast<uint32_t>(k), static_cast<uint32_t>(k), 0, 0, 0, 0};
    o.decls = {synth::DeclSpec{0, 0, static_cast<uint32_t>(c.count), 3, 12, {tp::DeclElem{0, 0, 0x2A23B9, 0, 0, 0, 0}}}};
    MS ms{.decl = 0, .istart = 0, .icount = 3, .tex = 0x99};
    if (id == first2 && !c.tmpl_bad) ms.size = 0x140, ms.nstages = 2;
    o.meshes = {ms};
    m.objects[k] = o;
  }
  if (c.g4_first) {
    auto g = m.groups;  // main, 0x02, state, 0x04 -> main, 0x04, state, 0x02
    std::swap(g[1], g[3]);
    m.SetGroups(g);
  }
  return m;
}

struct Files {
  Bytes tpr, cat, ref_tpr, ref_cat;
};
static Files FilesOf(const AddCase& c) {
  if (c.self) {
    synth::Model m = scan::SuitModel(c.sc);
    // read_suit strips the stand-ins by their buffer spans, which must start and end on 4 KiB boundaries: every
    // buffer from the copies' (decl 7 on) is moved to its own 4 KiB boundary
    uint32_t at = static_cast<uint32_t>(tp::Al(m.data_end, 0x1000)), end = m.data_end;
    for (size_t k = 7; k < m.vbufs.size(); ++k)
      for (synth::Buf* b : {&m.vbufs[k], &m.ibufs[k]}) {
        b->base = at;
        end = at + b->size;
        at = static_cast<uint32_t>(tp::Al(end, 0x1000));
      }
    m.SetDataEnd(static_cast<uint32_t>(tp::Al(end, 16)));
    m.SetGroups({synth::Main(c.sc.main), synth::Group(2, c.sc.g2), synth::State(0x10, {{0x9001, 0}}),
                 synth::Group(4, c.sc.g4)});
    const Bytes t = m.MakeTpr(), k = m.MakeCat();
    return {t, k, t, k};
  }
  const synth::Model s = scan::SuitModel(c.sc), r = RefOf(c);
  return {s.MakeTpr(), s.MakeCat(), r.MakeTpr(), r.MakeCat()};
}

static void SwapLastTwo(sl::Layout& lay) {
  const size_t n = lay.allnodes.size();
  if (n >= 2) std::swap(lay.allnodes[n - 1], lay.allnodes[n - 2]);
}

static sl::Result RunOrThrow(const AddCase& c, sl::CopyMode mode) {
  const Files f = FilesOf(c);
  sl::TestHooks h;
  if (c.hook_swap) h.layout = &SwapLastTwo;
  return sl::AddCopies(c.name, f.tpr, f.cat, f.ref_tpr, f.ref_cat, mode, h);
}

static std::string RowOf(const sl::Result& r) {
  return "tpr=" + Sha1Hex(r.tpr) + ":" + std::to_string(r.tpr.size()) + " cat=" + Sha1Hex(r.cat) + ":" +
         std::to_string(r.cat.size());
}

// the row of one case and mode; *out gets the output when built
static std::string Run(const AddCase& c, sl::CopyMode mode, std::optional<sl::Result>* out = nullptr) {
  try {
    sl::Result r = RunOrThrow(c, mode);
    const std::string row = RowOf(r);
    if (out) *out = std::move(r);
    return row;
  } catch (const std::exception& e) {
    return scan::Refusal(c.name, e);
  }
}

// O0 replaced: 60 meshes on one decl, one triangle each on its own three vertices; mesh i has texture 0x100 + i and
// pass 8 when i % 6 == 5, pass 4 when i % 6 == 2, else pass 2 (40 pass-2 keys in a row of 60 on node 6)
static Scene Keys60() {
  Scene sc = AddBase();
  ObjDef o;
  DeclDef d;
  for (uint16_t i = 0; i < 60; ++i) {
    const double x = 0.05 * i;
    d.verts.push_back(V({x, 0, 0}));
    d.verts.push_back(V({x + 0.02, 0, 0}));
    d.verts.push_back(V({x, 0.02, 0}));
    for (uint16_t k = 0; k < 3; ++k) d.idx.push_back(static_cast<uint16_t>(3 * i + k));
    const uint32_t pass = i % 6 == 5 ? 8 : i % 6 == 2 ? 4 : 2;
    o.meshes.push_back(MS{.decl = 0, .istart = 3u * i, .icount = 3, .tex = 0x100u + i, .pass = pass});
  }
  o.decls = {d};
  sc.objs[0] = o;
  return sc;
}
// the texture order Keys60's copy object of node 6 must have: pass 2 in mesh order, then pass 4, then pass 8
static std::vector<uint32_t> Keys60Order() {
  std::vector<uint32_t> out;
  for (uint32_t want : {2u, 4u, 8u})
    for (uint32_t i = 0; i < 60; ++i)
      if ((i % 6 == 5 ? 8u : i % 6 == 2 ? 4u : 2u) == want) out.push_back(0x100 + i);
  return out;
}

// O0 replaced (node 6): M1 = tex 0x21, pass 8, flag 0, triangle (v0 v1 v2), traversed FIRST; M2 = tex 0x22, pass 2,
// flag 1, triangle (v3 v1 v4). Writer order [M2, M1] (pass); v1's lowest appearance (t0, corner 1) is in M1, the LATER
// key: vertex order v0 v1 v2 v3 v4 (first met: v3 v1 v4 v0 v2). Suit and reference node 0 at x = -0.0 and copy root 6
// at x = 0.0, so M2's three vertices have x -0.0 (v1), 0.0 (v3), 0.0 (v4) in the copy frame: M2's bounds over its
// vertices in ascending index (v1 first) give centre x -0.0, in first-use order (v3 first) 0.0.
static Scene VOrder() {
  Scene sc = AddBase();
  sc.suit_nodes[0].tx = -0.0;
  sc.ref_nodes[0].tx = -0.0;
  sc.ref_nodes[6].tx = 0.0;
  ObjDef o;
  o.decls = {DeclDef{{V({0.25, 0, 0}), V({-0.0, 0.25, 0}), V({0.25, 0.25, 0.125}), V({0.0, 0.5, 0}), V({0.0, 0.5, 0.25})},
                     {0, 1, 2, 3, 1, 4}}};
  o.meshes = {MS{.decl = 0, .istart = 0, .icount = 3, .tex = 0x21, .pass = 8, .flag = 0},
              MS{.decl = 0, .istart = 3, .icount = 3, .tex = 0x22, .pass = 2, .flag = 1}};
  sc.objs[0] = o;
  return sc;
}

// the frame cases: reference node 7 (0x9003 -> copy node 7, fed by O1) rotated by dev d7, node 9 (0x9005 -> node 9,
// fed by O3) by d9 (both as rows (1, d, 0), (-d, 1, 0), (0, 0, 1), translation kept), shadow node 17 (0x9003 -> 16)
// rotated 90 degrees about z
static Scene Frames(double d7, double d9) {
  Scene sc = AddBase();
  sc.ref_nodes[7].mat = std::array<double, 16>{1, d7, 0, 0, -d7, 1, 0, 0, 0, 0, 1, 0, 0.125, 0.2, 0.3, 1};
  sc.ref_nodes[9].mat = std::array<double, 16>{1, d9, 0, 0, -d9, 1, 0, 0, 0, 0, 1, 0, 0.0, 0.0, 0.0, 1};
  sc.ref_nodes[17].mat = std::array<double, 16>{0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0.125, 0.2, 0.3, 1};
  return sc;
}

// rule I1 (map_rule_i1): suit nodes 6, 7 under 0 (objects with no triangle), so the first copy id is 8; the reference
// has at position 6 a node under the copy root (an orphan of main_chain) and at 7 a node under 0 (main_max 7), its
// copies at 8..21 (new ids 0x02 8..16, 0x04 17..20). kept = {0..7} + {8..20} - {6}. State groups: 0x10 [0] kept;
// 0x11 [6] dropped (orphan); 0x12 [8] dropped (8 = first, kept); 0x13 [7] kept (first - 1); a marker group 0x08 [1]
// dropped (not a state group).
static Scene MapI1() {
  Scene sc = AddBase([](Scene& s) {
    s.ref_nodes.insert(s.ref_nodes.begin() + 6, {{106, 20, 0.0, 0.0, 0.0}, {107, 0, 0.07, 0.0, 0.0}});
    s.extra_groups = {synth::State(0x11, {{0x9001, 106}}), synth::State(0x12, {{0x9001, 20}}),
                      synth::State(0x13, {{0x9001, 107}}), synth::Group(0x08, {{0x9001, 1}})};
  });
  for (int64_t id : {6, 7}) {
    sc.suit_nodes.push_back({id, 0, 0.07 * (id - 6), 0.0, 0.0});
    ObjDef o;
    o.decls = {DeclDef{{V({0, 0, 0})}, {}}};
    o.meshes = {MS{.decl = 0, .istart = 0, .icount = 0, .tex = 1}};
    sc.objs.push_back(o);
  }
  return sc;
}

// O0 replaced by one strip over n vertices (n - 2 triangles), textured (tex 1) or not (0xFFFFFFFF: group 0x04 only)
static Scene Strip(uint32_t n, bool textured) {
  Scene sc = AddBase();
  ObjDef o;
  DeclDef d;
  for (uint32_t k = 0; k < n; ++k) {
    d.verts.push_back(V({0.01 * (k % 256), 0.01 * (k / 256), 0.001 * (k % 3)}));
    d.idx.push_back(static_cast<uint16_t>(k));
  }
  o.decls = {d};
  o.meshes = {MS{.decl = 0, .istart = 0, .icount = n, .tex = textured ? 1u : 0xFFFFFFFFu}};
  sc.objs[0] = o;
  return sc;
}

// the suit as its own reference: the scan base's suit plus stand-in copies at ids 6..17 (reference ids 20..28 -> 6..14,
// 30..32 -> 15..17; 29 left out), copy object k with 6 + k vertices (one triangle), the first (id 6) of the 2-stage
// shape; map main, 0x02, state, 0x04 over those ids
static Scene SelfRef() {
  const Scene b = scan::Base();
  Scene sc = b;
  const std::map<int64_t, int64_t> to = {{20, 6},  {21, 7},  {22, 8},  {23, 9},  {24, 10}, {25, 11},
                                         {26, 12}, {27, 13}, {28, 14}, {30, 15}, {31, 16}, {32, 17}};
  for (const NodeDef& n : b.ref_nodes) {
    const auto it = to.find(n.id);
    if (it == to.end()) continue;
    NodeDef m = n;
    m.id = it->second;
    m.parent_id = n.parent_id < 0 ? -1 : to.at(n.parent_id);
    sc.suit_nodes.push_back(m);
  }
  sc.g2.clear();
  sc.g4.clear();
  for (const auto& [c, i] : b.g2) sc.g2.push_back({c, static_cast<uint8_t>(to.at(i))});
  for (const auto& [c, i] : b.g4) sc.g4.push_back({c, static_cast<uint8_t>(to.at(i))});
  for (int k = 0; k < 12; ++k) {
    ObjDef o;
    DeclDef d;
    for (int v = 0; v < 6 + k; ++v) d.verts.push_back(V({0.1 * v, 0.05 * k, 0.0}));
    d.idx = {0, 1, 2};
    o.decls = {d};
    MS ms{.decl = 0, .istart = 0, .icount = 3, .tex = 0x50u + static_cast<uint32_t>(k)};
    if (k == 0) ms.size = 0x140, ms.nstages = 2;
    o.meshes = {ms};
    sc.objs.push_back(o);
  }
  return sc;
}

static std::vector<AddCase> AddCases() {
  std::vector<AddCase> v;
  v.push_back({"base", AddBase()});
  v.push_back({"keys60", Keys60(), 80});
  {  // O5 replaced by a dense lattice (21 x 21 vertices 0.01 apart, 400 triangles): slim mode bisects for both groups
     // (targets 351 and 156 against about 470 vertices) and merges cells without losing a node
    Scene sc = AddBase();
    ObjDef o;
    DeclDef d;
    for (int j = 0; j <= 20; ++j)
      for (int i = 0; i <= 20; ++i) d.verts.push_back(V({3.0 + 0.01 * i, 0.01 * j, 0.002 * ((i + j) % 3)}));
    for (uint16_t j = 0; j < 20; ++j)
      for (uint16_t i = 0; i < 20; ++i) {
        if (!d.idx.empty()) d.idx.push_back(0xFFFF);
        const uint16_t a = static_cast<uint16_t>(21 * j + i);
        d.idx.insert(d.idx.end(), {a, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + 21)});
      }
    o.decls = {d};
    o.meshes = {MS{.decl = 0, .istart = 0, .icount = static_cast<uint32_t>(d.idx.size()), .tex = 7}};
    sc.objs[5] = o;
    v.push_back({"dense", sc});
  }
  v.push_back({"vorder", VOrder()});
  v.push_back({"frames", Frames(0.9e-6, 1.1e-6)});
  v.push_back({"frames0", Frames(0.0, 0.0)});
  {  // O0's normal element is F2 (two values): pack('>3f3f2f') gets 7 values
    Scene sc = AddBase();
    sc.objs[0].decls[0].elems = {tp::DeclElem{0, 0, 0x2A23B9, 0, 0, 0, 0}, tp::DeclElem{0, 12, 0x2C23A5, 0, 3, 0, 0},
                                 tp::DeclElem{0, 24, 0x2C23A5, 0, 5, 0, 0}, tp::DeclElem{0, 32, 0x1A23A6, 0, 1, 0, 0}};
    v.push_back({"nrm_short", sc});
  }
  {  // O4 (the neck, copy node 13) shrunk to a triangle of 1e-5: every grid cell holds it whole, so slim collapses it
    Scene sc = AddBase();
    sc.objs[4].decls[0].verts = {V({0.0004, 1.0004, 0.0004}, {0.625, 0.375, 0, 0}),
                                 V({0.00041, 1.0004, 0.0004}, {0.625, 0.375, 0, 0}),
                                 V({0.0004, 1.00041, 0.0004}, {0.625, 0.375, 0, 0})};
    v.push_back({"collapse", sc});
  }
  v.push_back({"map_i1", MapI1()});
  v.push_back({"g4_first", AddBase(), 40, true});
  v.push_back({"lim2_65534", Strip(65534, true)});
  v.push_back({"lim2_65535", Strip(65535, true)});
  v.push_back({"lim4_65535", Strip(65535, false)});
  v.push_back({"self", SelfRef(), 6, false, true});
  v.push_back({"ids_step", AddBase(), 40, false, false, true});
  // fix round 1. lost2: two nodes collapse, the higher id fed first: O0 (palette [4]: the neck's bone, copy node 13)
  // and O4 (node 13) shrunk to 1e-7, O3 (node 9, fed after O0) too; lost = [13, 9] in first-fed order (by id: [9, 13])
  {
    Scene sc = AddBase();
    const auto tiny = [](double x, double y, double z, std::array<double, 4> w = {1, 0, 0, 0}) {
      return std::vector<Bytes>{V({x, y, z}, w), V({x + 1e-7, y, z}, w), V({x, y + 1e-7, z}, w),
                                V({x + 1e-7, y + 1e-7, z}, w)};
    };
    sc.objs[0].palette = {4};
    sc.objs[0].decls[0].verts = tiny(0.0004, 0.0004, 0.0004);
    for (auto& d : sc.objs[3].decls) {
      d.verts = tiny(0.0004, 0.0004, 0.0004);
      d.verts.resize(3);
    }
    sc.objs[4].decls[0].verts = tiny(0.00043, 1.00043, 0.00043, {0.625, 0.375, 0, 0});
    sc.objs[4].decls[0].verts.resize(3);
    v.push_back({"lost2", sc});
  }
  {  // an infinite vertex x in O0: slim mode's merge raises math.floor's OverflowError, passed on unwrapped
    Scene sc = AddBase();
    sc.objs[0].decls[0].verts[3] = V({std::numeric_limits<double>::infinity(), 0.25, 0.125});
    v.push_back({"inf_pos", sc});
  }
  {  // a short (F2) normal on a ROTATED copy node (reference node 6, 90 degrees about z): _to_local's IndexError
    Scene sc = AddBase();
    sc.objs[0].decls[0].elems = {tp::DeclElem{0, 0, 0x2A23B9, 0, 0, 0, 0}, tp::DeclElem{0, 12, 0x2C23A5, 0, 3, 0, 0},
                                 tp::DeclElem{0, 24, 0x2C23A5, 0, 5, 0, 0}, tp::DeclElem{0, 32, 0x1A23A6, 0, 1, 0, 0}};
    sc.ref_nodes[6].mat = std::array<double, 16>{0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0.5, 1.0, 0.25, 1};
    v.push_back({"nrm_short_rot", sc});
  }
  {  // 250 more suit nodes (empty objects): the first copy id is 256, past the map's one-byte id (t2_bytes' assert)
    Scene sc = AddBase();
    for (int64_t id = 6; id < 256; ++id) {
      sc.suit_nodes.push_back({id, 0, 0.0, 0.0, 0.0});
      ObjDef o;
      o.decls = {DeclDef{{V({0, 0, 0})}, {}}};
      o.meshes = {MS{.decl = 0, .istart = 0, .icount = 0, .tex = 1}};
      sc.objs.push_back(o);
    }
    v.push_back({"id_overflow", sc});
  }
  // the refusal branches: a negative slim target (count 0: 0 - 9), no textured mesh (group 0x02 gets no triangle:
  // origin's SlimError in slim mode, every node a placeholder of texture 0 in full mode), an empty group 0x02 (the
  // report's min() raises), the template mesh of the wrong shape, a suit mesh record shorter than 0xC0 (lod_chain
  // _record's assert), a node without an object
  v.push_back({"neg_target", AddBase(), 0});
  {  // target exactly 0 (count 1: 9 - 9) with only O0 textured: its two triangles lie in one cell at H_HI, so the count
     // there is 0 <= 0 and choose_h runs on; the node is then lost (the lost-node refusal, not choose_h's)
    Scene sc = AddBase();
    for (size_t k = 1; k < sc.objs.size(); ++k)
      for (auto& ms : sc.objs[k].meshes) ms.tex = 0xFFFFFFFF;
    v.push_back({"zero_target", sc, 1});
  }
  {
    Scene sc = AddBase();
    for (auto& o : sc.objs)
      for (auto& ms : o.meshes) ms.tex = 0xFFFFFFFF;
    v.push_back({"no_tex", sc});
  }
  v.push_back({"empty_g2", AddBase([](Scene& s) { s.g2.clear(); })});
  v.push_back({"tmpl_bad", AddBase(), 40, false, false, false, true});
  {
    Scene sc = AddBase();
    sc.objs[0].meshes[0].size = 0x80;
    v.push_back({"short_mesh", sc});
  }
  {
    Scene sc = AddBase();
    sc.suit_nodes.push_back({6, 0, 0.3, 0.0, 0.0});
    v.push_back({"obj_count", sc});
  }
  return v;
}
static const AddCase* FindAdd(const std::string& name) {
  static const std::vector<AddCase> cases = AddCases();
  for (const auto& c : cases)
    if (c.name == name) return &c;
  return nullptr;
}

// ---- the output, parsed back (tpr::Xpr::Parse and the chunk parsers)
struct Out {
  tp::Xpr x;
  std::vector<tp::ObjGeo> geo;
  std::vector<tp::ObjInfo> info;
  std::vector<tp::Node> hie;
  std::vector<sl::MapGroup> map;
};
static std::optional<Out> Parse(const sl::Result& r) {
  try {
    Out o;
    o.x = tp::Xpr::Parse(r.tpr);
    o.geo = tp::ParseMdlGeo(o.x.res.at(1).blob);
    o.info = tp::ParseMdlInfo(o.x.res.at(2).blob);
    o.hie = tp::ParseHie(o.x.res.at(3).blob);
    std::optional<uint32_t> t2;
    for (const auto& [t, val] : tp::CatDir(r.cat))
      if (t == 2) t2 = val;
    if (t2) o.map = sl::ParseMap(r.cat, *t2).first;
    return o;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}
// object k's vertex buffer bytes (decl 0: head word 5 vertices of its stride) and index list; empty when absent
static Bytes VbOf(const Out& o, size_t k) {
  if (k >= o.geo.size() || o.geo[k].decls.empty()) return {};
  const tp::Decl& d = o.geo[k].decls[0];
  const auto vb = o.x.OfType(tp::MakeFourcc("VBUF"));
  if (d.Vb() >= vb.size()) return {};
  const auto [base, size] = tp::BufSpan(o.x.res[vb[d.Vb()]].blob);
  (void)size;
  const uint64_t n = uint64_t(d.head[5]) * d.Stride();
  if (base + n > o.x.data.size()) return {};
  return Bytes(o.x.data.begin() + base, o.x.data.begin() + base + n);
}
static std::vector<uint16_t> IbOf(const Out& o, size_t k) {
  if (k >= o.geo.size() || o.geo[k].decls.empty()) return {};
  const tp::Decl& d = o.geo[k].decls[0];
  const auto ib = o.x.OfType(tp::MakeFourcc("IBUF"));
  if (d.head[2] >= ib.size()) return {};
  const auto [base, size] = tp::BufSpan(o.x.res[ib[d.head[2]]].blob);
  (void)size;
  std::vector<uint16_t> out;
  for (uint32_t i = 0; i < d.head[3]; ++i) {
    const uint64_t at = base + 2ull * i;
    if (at + 2 > o.x.data.size()) return {};
    out.push_back(static_cast<uint16_t>((o.x.data[at] << 8) | o.x.data[at + 1]));
  }
  return out;
}
static std::vector<float> F32s(const Bytes& b) {
  std::vector<float> out;
  for (size_t i = 0; i + 4 <= b.size(); i += 4)
    out.push_back(std::bit_cast<float>((uint32_t(b[i]) << 24) | (uint32_t(b[i + 1]) << 16) | (uint32_t(b[i + 2]) << 8) |
                                       uint32_t(b[i + 3])));
  return out;
}
static uint32_t U32At(const Bytes& b, size_t o) {
  if (o + 4 > b.size()) return 0xDEADBEEF;
  return (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | uint32_t(b[o + 3]);
}
// the stage-0 texture of each mesh of object k (word at 0xC0 + 8 of the 0x140-byte record)
static std::vector<uint32_t> MeshTexOf(const Out& o, size_t k) {
  std::vector<uint32_t> out;
  if (k >= o.geo.size()) return out;
  for (const Bytes& m : o.geo[k].meshes) out.push_back(U32At(m, 0xC8));
  return out;
}
// a built case's parsed output (nullopt when refused or unparsable)
static std::optional<Out> OutOf(const std::string& name, sl::CopyMode mode) {
  const AddCase* c = FindAdd(name);
  if (!c) return std::nullopt;
  std::optional<sl::Result> r;
  Run(*c, mode, &r);
  if (!r) return std::nullopt;
  return Parse(*r);
}

// ---- _node_verts on hand-built merged lists (the 0.0 / -0.0 tie no add_copies input reaches: a cell mean is a sum
// from +0.0, never -0.0, and identity mode gives one vid one position)
struct NvVert {
  uint32_t vid;
  double x, y, z;
  uint32_t tri;
  uint8_t corner;
};
struct NvCase {
  const char* name;
  std::vector<std::vector<NvVert>> keys;
};
static std::vector<NvCase> NvCases() {
  return {
      // v5 in both keys at x 0.0 (key 0, t3 c0) and -0.0 (key 1, t1 c2): one vertex, kept from key 1 (x -0.0, lower
      // appearance); by bits: two vertices
      {"signed_zero",
       {{{5, 0.0, 1, 2, 3, 0}, {6, 1, 1, 1, 3, 1}, {7, 2, 2, 2, 3, 2}},
        {{8, 3, 3, 3, 1, 0}, {9, 4, 4, 4, 1, 1}, {5, -0.0, 1, 2, 1, 2}}}},
      // v2 in key 0 at t5 c1 and key 1 at t2 c1: order by the lowest appearance v4 v2 v5 v1 v3 (insertion: v1 v2 v3 v4
      // v5; first-met appearance: v4 v5 v1 v2 v3)
      {"lowest_later_key",
       {{{1, 0, 0, 0, 5, 0}, {2, 1, 0, 0, 5, 1}, {3, 0, 1, 0, 5, 2}},
        {{4, 0, 0, 1, 2, 0}, {2, 1, 0, 0, 2, 1}, {5, 1, 1, 1, 2, 2}}}},
      // one vid at two positions: two vertices
      {"vid_two_positions", {{{1, 0, 0, 0, 0, 0}, {2, 1, 0, 0, 0, 1}}, {{1, 0, 0, 0.5, 1, 0}, {3, 1, 1, 0, 1, 1}}}},
  };
}
struct NvBuilt {
  sl::Merged merged;
  std::vector<sl::ScanPayload> payload;
  std::vector<uint32_t> keys;
};
static NvBuilt NvBuild(const NvCase& c) {
  NvBuilt b;
  for (size_t k = 0; k < c.keys.size(); ++k) {
    sl::MergedKey mk;
    mk.key = static_cast<uint32_t>(k);
    for (const NvVert& v : c.keys[k]) {
      mk.pos.push_back({v.x, v.y, v.z});
      mk.payload.push_back(static_cast<uint32_t>(b.payload.size()));
      sl::ScanPayload p;
      p.vid = v.vid;
      p.tri = v.tri;
      p.corner = v.corner;
      b.payload.push_back(p);
    }
    b.merged.push_back(mk);
    b.keys.push_back(static_cast<uint32_t>(k));
  }
  return b;
}
// slim_expect.py nodeverts_line()
static std::string NvLine(const NvCase& c) {
  const NvBuilt b = NvBuild(c);
  sl::NodeVerts nv;
  try {
    nv = sl::NodeVertsOf(b.keys, b.merged, b.payload);
  } catch (const std::exception&) {
    return "RAISED";
  }
  std::string s = "V";
  for (size_t i = 0; i < nv.verts.size(); ++i) {
    const auto& [p, pl] = nv.verts[i];
    const sl::ScanPayload& py = b.payload[pl];
    s += (i ? ";" : "") + std::to_string(py.vid) + "," + scan::Hex16(scan::Bits(p[0])) + "," + scan::Hex16(scan::Bits(p[1])) +
         "," + scan::Hex16(scan::Bits(p[2])) + "," + std::to_string(py.tri) + "." + std::to_string(py.corner);
  }
  s += " R";
  for (size_t k = 0; k < nv.remap.size(); ++k) {
    if (k) s += "|";
    for (size_t i = 0; i < nv.remap[k].size(); ++i) s += (i ? "," : "") + std::to_string(nv.remap[k][i]);
  }
  return s;
}

// ---- bounds (core::Bounds, the writer's): test_slim_addon.py check_bounds_local's five points, both orders; and two
// points whose radius is sqrt(m^2 + a^2 + a^2) with m = 1.5 + 2^-24 (an f32 midpoint, lower neighbour 1.5 even; m^2
// exact, ulp 2^-51) and a = 1.2e-8 (a^2 = 1.44e-16, under half that ulp, the two together over it): a plain sum loses
// both, sqrt gives m and packs to 1.5 (tie to even); the builtin sum keeps them (m^2 + 1 ulp), sqrt rounds above m and
// packs 1.5 + 2^-23
struct BoundsCase {
  const char* name;
  std::vector<std::array<double, 3>> pts;
};
static std::vector<BoundsCase> BoundsCases() {
  const std::vector<std::array<double, 3>> five = {{-0.0, 0.225, 0.04999999999999999},
                                                   {0.0, 1.755713, 0.9032285497482799},
                                                   {-0.0, -0.07500000000000001, -0.65},
                                                   {0.0, -1.505713, -1.40322854974828},
                                                   {0.0, 0.125, -0.2}};
  const double m = 1.5 + std::ldexp(1.0, -24), a = 1.2e-8;
  return {{"signed_zero_fwd", five},
          {"signed_zero_rev", {five.rbegin(), five.rend()}},
          {"radius_compensated", {{m, a, a}, {-m, -a, -a}}}};
}
static std::string BoundsLine(const BoundsCase& c) {
  try {
    return scan::HexBytes(doa4::suitconv::core::Bounds(c.pts));
  } catch (const std::exception&) {
    return "RAISED";
  }
}
// the bounds rule with a PLAIN left-to-right radius sum (the wrong version of bounds_radius_compensated)
static Bytes BoundsPlainSum(const std::vector<std::array<double, 3>>& pts) {
  Bytes b = doa4::suitconv::core::Bounds(pts);
  std::array<double, 3> lo = pts[0], hi = pts[0], ctr{};
  for (const auto& p : pts)
    for (int c = 0; c < 3; ++c) lo[c] = p[c] < lo[c] ? p[c] : lo[c], hi[c] = p[c] > hi[c] ? p[c] : hi[c];
  for (int c = 0; c < 3; ++c) ctr[c] = (lo[c] + hi[c]) / 2;
  double r = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    double s = 0;
    for (int c = 0; c < 3; ++c) s = s + (pts[i][c] - ctr[c]) * (pts[i][c] - ctr[c]);
    const double v = std::sqrt(s);
    if (i == 0 || v > r) r = v;
  }
  const uint32_t rb = std::bit_cast<uint32_t>(static_cast<float>(r));
  for (int k = 0; k < 4; ++k) b[12 + k] = static_cast<uint8_t>(rb >> (24 - 8 * k));
  return b;
}

}  // namespace add

static uint32_t U32Bits(const Bytes& b, size_t o) { return add::U32At(b, o); }

// Pasted verbatim from (2026-11-23):
//   tests/host/run_suitconv_slim_test.bat --dump DIR
//   py -3 -I tests/host/slim_expect.py --add <doax2_work>/physics_spike/scripts DIR/add
static const Expected kAddExpected[] = {
    {"base/full", "tpr=27d7cc610f3827665431ad60246cd506ab694c71:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"base/slim", "tpr=27d7cc610f3827665431ad60246cd506ab694c71:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"collapse/full", "tpr=4661aef1bc4b50be756dc5a149cc15bfe4f90e27:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"collapse/slim", "REFUSED collapse: copy 0x02: node 13 had 1 triangles before the merge and none after (nodes [13])"},
    {"dense/full", "tpr=3e9eccf869a1e2f7f48515ad74c145110457a07c:176128 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"dense/slim", "tpr=a31f233617f30aff90303e4942f0710d007a886b:163840 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"empty_g2/full", "RAISED"},
    {"empty_g2/slim", "RAISED"},
    {"frames/full", "tpr=3bcc7f2d50f1bfe6c3f76967ea3ec726c898dfb9:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"frames/slim", "tpr=3bcc7f2d50f1bfe6c3f76967ea3ec726c898dfb9:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"frames0/full", "tpr=6698c6887607a3fccd52307d4218ab96ff39fa97:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"frames0/slim", "tpr=6698c6887607a3fccd52307d4218ab96ff39fa97:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"g4_first/full", "tpr=27d7cc610f3827665431ad60246cd506ab694c71:135168 cat=68fbba35913ea2dcf6fc2a84bc150190075def70:224"},
    {"g4_first/slim", "tpr=27d7cc610f3827665431ad60246cd506ab694c71:135168 cat=68fbba35913ea2dcf6fc2a84bc150190075def70:224"},
    {"id_overflow/full", "RAISED"},
    {"id_overflow/slim", "RAISED"},
    {"ids_step/full", "REFUSED ids_step: copy ids out of step with the objects"},
    {"ids_step/slim", "REFUSED ids_step: copy ids out of step with the objects"},
    {"inf_pos/full", "tpr=28b44a7fd3eda2931979f577bdd6656349531a68:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"inf_pos/slim", "RAISED"},
    {"keys60/full", "tpr=c044984972c4fe88e3f2de2203280ecb10311f6a:192512 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"keys60/slim", "tpr=c044984972c4fe88e3f2de2203280ecb10311f6a:192512 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"lim2_65534/full", "tpr=7439497e619147e5c713e3096849d2924f56e98e:7327744 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"lim2_65534/slim", "REFUSED lim2_65534: copy 0x04: node 15 had 65532 triangles before the merge and none after (nodes [15])"},
    {"lim2_65535/full", "REFUSED lim2_65535: copy node 6 over 65534 vertices"},
    {"lim2_65535/slim", "REFUSED lim2_65535: copy 0x04: node 15 had 65533 triangles before the merge and none after (nodes [15])"},
    {"lim4_65535/full", "REFUSED lim4_65535: shadow node 15 over 65534 vertices"},
    {"lim4_65535/slim", "REFUSED lim4_65535: copy 0x04: node 15 had 65533 triangles before the merge and none after (nodes [15])"},
    {"lost2/full", "tpr=4a1e53c3dbddf81987a46de906835e5325425f1d:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"lost2/slim", "REFUSED lost2: copy 0x02: node 13 had 3 triangles before the merge and none after (nodes [13, 9])"},
    {"map_i1/full", "tpr=e67ad05eb498bcea71b06b049f62449e52316c31:139264 cat=e752900cadca5dc3ceffaeadb35476b8c41890c1:224"},
    {"map_i1/slim", "tpr=e67ad05eb498bcea71b06b049f62449e52316c31:139264 cat=e752900cadca5dc3ceffaeadb35476b8c41890c1:224"},
    {"neg_target/full", "tpr=27d7cc610f3827665431ad60246cd506ab694c71:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"neg_target/slim", "REFUSED neg_target: copy 0x02: count 0 at H_HI = 1.0 exceeds target -9"},
    {"no_tex/full", "tpr=e584c194325ef11efd8791b01877b7b5eb3f226c:131072 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"no_tex/slim", "REFUSED no_tex: copy 0x02: origin: no triangles"},
    {"nrm_short/full", "RAISED"},
    {"nrm_short/slim", "RAISED"},
    {"nrm_short_rot/full", "RAISED"},
    {"nrm_short_rot/slim", "RAISED"},
    {"obj_count/full", "REFUSED obj_count: 6 objects for 7 nodes"},
    {"obj_count/slim", "REFUSED obj_count: 6 objects for 7 nodes"},
    {"self/full", "tpr=827d0705f45a2d189b8f882384a91a4e78739236:135168 cat=9ebc22d3e9be63981f5bbbfea39ef001a51cd270:224"},
    {"self/slim", "tpr=827d0705f45a2d189b8f882384a91a4e78739236:135168 cat=9ebc22d3e9be63981f5bbbfea39ef001a51cd270:224"},
    {"short_mesh/full", "RAISED"},
    {"short_mesh/slim", "RAISED"},
    {"tmpl_bad/full", "REFUSED tmpl_bad: reference group-0x02 mesh not of the native 2-stage shape"},
    {"tmpl_bad/slim", "REFUSED tmpl_bad: reference group-0x02 mesh not of the native 2-stage shape"},
    {"vorder/full", "tpr=a4ea7f06c99d7ae71316140a48bb5b83fcacb292:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"vorder/slim", "tpr=c76e6a81339b2d04f1ba4b9f5d9d304943927d86:135168 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"zero_target/full", "tpr=103dd487cd6e4bb9f3476be6d881bd0525ec5033:131072 cat=1fb52a9c36a8408cdca422967b4d4c87492d416a:224"},
    {"zero_target/slim", "REFUSED zero_target: copy 0x02: node 6 had 2 triangles before the merge and none after (nodes [6])"},
};
static const Expected kNodeVertsExpected[] = {
    {"signed_zero", "V8,4008000000000000,4008000000000000,4008000000000000,1.0;9,4010000000000000,4010000000000000,4010000000000000,1.1;5,8000000000000000,3ff0000000000000,4000000000000000,1.2;6,3ff0000000000000,3ff0000000000000,3ff0000000000000,3.1;7,4000000000000000,4000000000000000,4000000000000000,3.2 R2,3,4|0,1,2"},
    {"lowest_later_key", "V4,0000000000000000,0000000000000000,3ff0000000000000,2.0;2,3ff0000000000000,0000000000000000,0000000000000000,2.1;5,3ff0000000000000,3ff0000000000000,3ff0000000000000,2.2;1,0000000000000000,0000000000000000,0000000000000000,5.0;3,0000000000000000,3ff0000000000000,0000000000000000,5.2 R3,1,4|0,1,2"},
    {"vid_two_positions", "V1,0000000000000000,0000000000000000,0000000000000000,0.0;2,3ff0000000000000,0000000000000000,0000000000000000,0.1;1,0000000000000000,0000000000000000,3fe0000000000000,1.0;3,3ff0000000000000,3ff0000000000000,0000000000000000,1.1 R0,1|2,3"},
};
static const Expected kBoundsExpected[] = {
    {"signed_zero_fwd", "800000003e000000be8000003fffa725800000003e000000be8000003f800000000000003fd0bb343f939cfe00000000"},
    {"signed_zero_rev", "000000003e000000be8000003fffa725000000003e000000be8000003f800000000000003fd0bb343f939cfe00000000"},
    {"radius_compensated", "0000000000000000000000003fc000010000000000000000000000003f8000003fc00000324e288f324e288f00000000"},
};

static const char* AddExpectedFor(const std::string& name, const Expected* rows, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (name == rows[i].name) return rows[i].summary;
  return nullptr;
}
template <size_t N>
static const char* RowFor(const Expected (&rows)[N], const std::string& name) {
  return AddExpectedFor(name, rows, N);
}
// one case and mode against its Python row
static void CheckAdd(const std::string& name, sl::CopyMode mode) {
  const std::string key = name + (mode == sl::CopyMode::kFull ? "/full" : "/slim");
  const add::AddCase* c = add::FindAdd(name);
  const char* want = RowFor(kAddExpected, key);
  const std::string got = c ? add::Run(*c, mode) : std::string("(no case)");
  CHECKF(want != nullptr && got == want, "%s: got \"%s\" want \"%s\"", key.c_str(), got.c_str(), want ? want : "(none)");
}
static void CheckAddBoth(const std::string& name) {
  CheckAdd(name, sl::CopyMode::kFull);
  CheckAdd(name, sl::CopyMode::kSlim);
}
// a Python row's text (for a refusal check that names its text)
static std::string AddRow(const std::string& key) {
  const char* r = RowFor(kAddExpected, key);
  return r ? r : "(none)";
}

using sl::CopyMode;

static void TestSamePassKeysKeepOrder() {
  // contract (a): node 6 holds 60 keys (40 of pass 2, 10 of 4, 10 of 8, interleaved); mesh order = pass, then merged
  // order. Wrong version: std::sort (not stable past its insertion-sort cut), which may reorder equal passes.
  CheckAddBoth("keys60");
  for (CopyMode m : {CopyMode::kFull, CopyMode::kSlim}) {
    const auto o = add::OutOf("keys60", m);
    const auto tex = o ? add::MeshTexOf(*o, 6) : std::vector<uint32_t>{};
    CHECKF(tex == add::Keys60Order(), "mode %d: %zu meshes", int(m), tex.size());
  }
}

static void TestFusionByVidAndPosition() {
  // contract (c) identity: base O3 draws decl 0 with two keys (tex 4, tex 5) over the same three vertices; in full mode
  // node 9's object has 3 (decl 0, fused) + 3 (decl 1) = 6 vertices, not 9. Wrong version (no fusion across keys): 9.
  CheckAddBoth("base");
  const auto o = add::OutOf("base", CopyMode::kFull);
  CHECKF(o && o->geo.size() > 9 && o->geo[9].decls.size() == 1 && o->geo[9].decls[0].head[5] == 6, "node 9 vertices %u",
         o && o->geo.size() > 9 && !o->geo[9].decls.empty() ? o->geo[9].decls[0].head[5] : 0u);
  // 0.0 and -0.0 at one vid: one vertex, kept from the lower appearance (x -0.0). By bits: two.
  for (const auto& c : add::NvCases()) {
    const char* want = RowFor(kNodeVertsExpected, c.name);
    const std::string got = add::NvLine(c);
    CHECKF(want && got == want, "%s: got \"%s\" want \"%s\"", c.name, got.c_str(), want ? want : "(none)");
  }
  sl::NodeVerts nv;  // stays empty when NodeVertsOf raises (one failed check, not the end of the group)
  try {
    const add::NvBuilt b0 = add::NvBuild(add::NvCases()[0]);
    nv = sl::NodeVertsOf(b0.keys, b0.merged, b0.payload);
  } catch (const std::exception&) {
  }
  CHECKF(nv.verts.size() == 5 && std::signbit(nv.verts[2].first[0]) && nv.remap.size() == 2 && nv.remap[0][0] == 2 &&
             nv.remap[1][2] == 2,
         "%zu vertices", nv.verts.size());
  // a NaN position is refused (Python's answer there depends on object identity)
  add::NvCase nan_case{"nan", {{{1, std::numeric_limits<double>::quiet_NaN(), 0, 0, 0, 0}, {2, 1, 0, 0, 0, 1}}}};
  const add::NvBuilt nb = add::NvBuild(nan_case);
  std::string what;
  try {
    sl::NodeVertsOf(nb.keys, nb.merged, nb.payload);
  } catch (const std::exception& e) {
    what = e.what();
  }
  CHECKF(what.rfind("ValueError: a NaN position", 0) == 0, "%s", what.c_str());
}

static void TestVertexOrderByFirstAppearance() {
  // contract (c) order: vorder's node 6 writes [M2 (v3 v1 v4), M1 (v0 v1 v2)]; v1's lowest appearance is in M1, the
  // later key. Output vertex order v0 v1 v2 v3 v4 (local (x, y): (0.25, 0), (-0.0, 0.25), (0.25, 0.25), (0, 0.5),
  // (0, 0.5): node 0's world y 1.0 minus the frame's 1.0); first met gives v3 v1 v4 v0 v2, the first-met appearance
  // v0 v2 v3 v1 v4. Indices: M2 3 1 4, then M1 0 1 2 (no restart between meshes).
  CheckAddBoth("vorder");
  const auto o = add::OutOf("vorder", CopyMode::kFull);
  const auto vb = o ? add::F32s(add::VbOf(*o, 6)) : std::vector<float>{};
  std::vector<float> xy;
  for (size_t i = 0; i + 1 < vb.size(); i += 8) xy.push_back(vb[i]), xy.push_back(vb[i + 1]);
  CHECKF(xy == (std::vector<float>{0.25f, 0, -0.0f, 0.25f, 0.25f, 0.25f, 0, 0.5f, 0, 0.5f}), "%zu floats", xy.size());
  const auto ib = o ? add::IbOf(*o, 6) : std::vector<uint16_t>{};
  CHECKF(ib == (std::vector<uint16_t>{3, 1, 4, 0, 1, 2}), "%zu indices", ib.size());
}

static void TestReflectionFrameRotatesOnlyPast1e6() {
  // contract (d): copy node 7 at dev 0.9e-6 writes the bytes of the dev-0 twin (unrotated), node 9 at 1.1e-6 does not
  // (rotated). Wrong versions: always rotate (node 7 differs), never rotate (node 9 equal). Rows from Python as well.
  CheckAddBoth("frames");
  CheckAddBoth("frames0");
  const auto a = add::OutOf("frames", CopyMode::kFull), b = add::OutOf("frames0", CopyMode::kFull);
  CHECK(a && b && !add::VbOf(*a, 7).empty() && add::VbOf(*a, 7) == add::VbOf(*b, 7));
  CHECK(a && b && !add::VbOf(*a, 9).empty() && add::VbOf(*a, 9) != add::VbOf(*b, 9));
}

static void TestShadowMinusJoint() {
  // shadow positions = merged position minus the node's frame translation, no rotation. Base: shadow node 15 (reference
  // 30 at (0.5, 1, 0.25)), its first vertex O0's v0 at world (0.5, 1, 0.25): (0, 0, 0). Not subtracted: (0.5, 1, 0.25).
  CheckAddBoth("base");
  const auto o = add::OutOf("base", CopyMode::kFull);
  const auto vb = o ? add::F32s(add::VbOf(*o, 15)) : std::vector<float>{};
  CHECKF(vb.size() >= 3 && vb[0] == 0.0f && vb[1] == 0.0f && vb[2] == 0.0f, "%zu floats", vb.size());
  // a rotated shadow node is translated only: frames' node 16 (reference 31, 90 degrees about z, world t (0.625, 1.2,
  // 0.55) = suit node 1's) holds O1's v0 (0, 0, 0.5) then v1 (0.5, 0, 0.5) as they are; rotated, v1 would be (0, -0.5)
  const auto a = add::OutOf("frames", CopyMode::kFull);
  CheckAddBoth("frames");
  const auto f16 = a ? add::F32s(add::VbOf(*a, 16)) : std::vector<float>{};
  CHECKF(f16.size() >= 6 && f16[0] == 0.0f && f16[2] == 0.5f && f16[3] == 0.5f && f16[4] == 0.0f, "%zu floats",
         f16.size());
}

static void TestBoundsSignedZeroTie() {
  // contract (f): first of equal values (test_slim_addon.py check_bounds_local's points, both orders): centre x -0.0
  // forward, 0.0 reversed; Python's bytes
  for (const auto& c : add::BoundsCases()) {
    const char* want = RowFor(kBoundsExpected, c.name);
    const std::string got = add::BoundsLine(c);
    CHECKF(want && got == want, "%s: got %s want %s", c.name, got.c_str(), want ? want : "(none)");
  }
  // per mesh in ascending vertex index: vorder's M2 (mesh 0 of node 6) has vertices 1 (x -0.0), 3, 4 (x 0.0): centre x
  // -0.0 (first-use order 3 1 4 gives 0.0); its infos record's bounds start at 0x20
  const auto o = add::OutOf("vorder", CopyMode::kFull);
  const bool ok = o && o->info.size() > 6 && !o->info[6].infos.empty() && o->info[6].infos[0].size() >= 0x24;
  CHECK(ok && U32Bits(o->info[6].infos[0], 0x20) == 0x80000000u);
}

static void TestBoundsRadiusCompensated() {
  const auto cases = add::BoundsCases();
  const auto& rc = cases[2];
  // precondition: a plain radius sum packs other bytes
  CHECK(add::BoundsPlainSum(rc.pts) != doa4::suitconv::core::Bounds(rc.pts));
  const char* want = RowFor(kBoundsExpected, rc.name);
  CHECK(want && add::BoundsLine(rc) == want);
}

static void TestPlaceholderBytes() {
  // contract (h): base's copy node 10 (0x9006: no suit node's channel) and shadow node 18 (0x9008) are never fed.
  // 0x02: one vertex (0, 0, 0), normal (0, 1, 0), uv (0, 0); indices 0 0 0; one mesh of pass 2; 0x04: one vertex
  // (0, 0, 0), indices 0 0 0
  const auto o = add::OutOf("base", CopyMode::kFull);
  CHECK(o && add::F32s(add::VbOf(*o, 10)) == (std::vector<float>{0, 0, 0, 0, 1, 0, 0, 0}));
  CHECK(o && add::IbOf(*o, 10) == (std::vector<uint16_t>{0, 0, 0}));
  CHECK(o && o->info.size() > 10 && o->info[10].infos.size() == 1 && U32Bits(o->info[10].infos[0], 8) == 2);
  CHECK(o && add::F32s(add::VbOf(*o, 18)) == (std::vector<float>{0, 0, 0}));
  CHECK(o && add::IbOf(*o, 18) == (std::vector<uint16_t>{0, 0, 0}));
}

static void TestPlaceholderTextureFromMergedOrder() {
  // contract (g)/(h): vorder's first fed node (6) has keys M1 (tex 0x21, merged first) and M2 (tex 0x22, pass first):
  // node 10's placeholder mesh carries 0x21. From pass order: 0x22.
  for (CopyMode m : {CopyMode::kFull, CopyMode::kSlim}) {
    const auto o = add::OutOf("vorder", m);
    CHECKF(o && add::MeshTexOf(*o, 10) == std::vector<uint32_t>{0x21}, "mode %d", int(m));
  }
}

static void TestCollapseRefused() {
  // contract (h): collapse's node 13 (the neck, one triangle of 1e-5) is fed 1 triangle and merges to none at every
  // grid cell size. Wrong version (no check): a placeholder ships.
  CheckAdd("collapse", CopyMode::kSlim);
  CHECK(AddRow("collapse/slim") ==
        "REFUSED collapse: copy 0x02: node 13 had 1 triangles before the merge and none after (nodes [13])");
}

static void TestFullModeNeverRefuses() {
  CheckAdd("collapse", CopyMode::kFull);
  CHECK(AddRow("collapse/full").rfind("tpr=", 0) == 0);
  const auto o = add::OutOf("collapse", CopyMode::kFull);
  CHECK(o && o->geo.size() > 13 && !o->geo[13].decls.empty() && o->geo[13].decls[0].head[5] == 3);
}

static void TestMapRuleI1() {
  // rule I1: map_i1's output map = main, state 0x10 [0], state 0x13 [8], 0x02, 0x04 (0x11 orphan, 0x12 at first, the
  // marker 0x08 dropped). Wrong versions: `<=` keeps 0x12; orphans not removed keeps 0x11; any group kept keeps 0x08.
  CheckAddBoth("map_i1");
  const auto o = add::OutOf("map_i1", CopyMode::kFull);
  std::string kinds;
  if (o)
    for (const auto& g : o->map)
      kinds += (kinds.empty() ? "" : ",") + std::string(g.kind == sl::MapGroup::Kind::kMain    ? "main"
                                                        : g.kind == sl::MapGroup::Kind::kState ? "s"
                                                                                               : "") +
               std::to_string(g.value);
  CHECKF(kinds == "main0,s16,s19,2,4", "%s", kinds.c_str());
}

static void TestKindOrderFollowsMarkerOrder() {
  CheckAddBoth("g4_first");
  const auto o = add::OutOf("g4_first", CopyMode::kFull);
  std::string kinds;
  if (o)
    for (const auto& g : o->map) kinds += (kinds.empty() ? "" : ",") + std::to_string(g.value);
  CHECKF(kinds == "0,16,4,2", "%s", kinds.c_str());
}

static void TestBuffersAt4k() {
  // every new VBUF / IBUF at a 0x1000 boundary, its size a multiple of 16; low bits (3, 2) on a VBUF, (0, 0) on an
  // IBUF; the data a multiple of 0x1000. New VBUFs right after the old ones, new IBUFs right after the old IBUFs.
  const auto o = add::OutOf("base", CopyMode::kSlim);
  bool ok = o.has_value();
  size_t nv = 0, ni = 0;
  if (o) {
    const auto vb = o->x.OfType(tp::MakeFourcc("VBUF")), ib = o->x.OfType(tp::MakeFourcc("IBUF"));
    nv = vb.size(), ni = ib.size();
    // the suit's own buffers: 7 objects' decls (base: one VBUF and one IBUF per decl, 7 decls) come first
    for (size_t k = 7; k < vb.size(); ++k) {
      const Bytes& d = o->x.res[vb[k]].blob;
      ok = ok && (U32Bits(d, 12) & 0xFFF) == 3 && (U32Bits(d, 16) & 3) == 2 && (U32Bits(d, 16) & ~3u) % 16 == 0;
    }
    for (size_t k = 7; k < ib.size(); ++k) {
      const Bytes& d = o->x.res[ib[k]].blob;
      ok = ok && (U32Bits(d, 12) & 0xFFF) == 0 && (U32Bits(d, 16) & 0xF) == 0;
    }
    ok = ok && o->x.data.size() % 0x1000 == 0;
    // resource order: USER x4, VBUF x (7 + 13), IBUF x (7 + 13)
    for (size_t k = 0; k < vb.size(); ++k) ok = ok && vb[k] == 4 + k;
    for (size_t k = 0; k < ib.size(); ++k) ok = ok && ib[k] == 4 + vb.size() + k;
  }
  CHECKF(ok && nv == 20 && ni == 20, "vbufs %zu ibufs %zu", nv, ni);
}

static void TestUser0Recomputed() {
  // USER0 describes the grown resource list: [7, 4, 0, 4, 20, 4, 20, 24, 1, 2, 3, 16]
  const auto o = add::OutOf("base", CopyMode::kSlim);
  std::vector<uint32_t> w;
  if (o && o->x.res.size() > 0)
    for (int i = 0; i < 12; ++i) w.push_back(U32Bits(o->x.res[0].blob, 4 * i));
  CHECK(w == (std::vector<uint32_t>{7, 4, 0, 4, 20, 4, 20, 24, 1, 2, 3, 16}));
}

static void TestIdsOutOfStepRefused() {
  // test hook: the last two copy nodes swapped after MakeLayout (no input reaches this state)
  CheckAddBoth("ids_step");
  CHECK(AddRow("ids_step/full") == "REFUSED ids_step: copy ids out of step with the objects");
}

static void TestTwiceEqual() {
  for (const char* n : {"base", "dense", "keys60", "vorder"})
    for (CopyMode m : {CopyMode::kFull, CopyMode::kSlim}) {
      const add::AddCase* c = add::FindAdd(n);
      std::optional<sl::Result> a, b;
      if (c) add::Run(*c, m, &a), add::Run(*c, m, &b);
      CHECKF(a && b && a->tpr == b->tpr && a->cat == b->cat, "%s mode %d", n, int(m));
    }
}

static void TestSelfReference() {
  // the suit with stand-in copies at ids 6..17 is its own reference: the copies are stripped, new ids start at 6 (where
  // the old copies started), and objects 0..5 are written as they were read. The slim targets (the stand-ins' own counts
  // minus the node count: 0x02 6+..+14 - 9 = 81, 0x04 15+16+17 - 3 = 45) are NOT exercised here: both exceed the
  // vertex count at H_LO, so h stays at H_LO and slim equals full (target_no_minus is caught by neg_target and dense)
  CheckAddBoth("self");
  const add::AddCase* c = add::FindAdd("self");
  const auto f = c ? add::FilesOf(*c) : add::Files{};
  std::optional<sl::Result> r;
  if (c) add::Run(*c, CopyMode::kSlim, &r);
  const auto o = r ? add::Parse(*r) : std::nullopt;
  bool same = o && o->geo.size() == 18 && o->info.size() == 18;
  if (same) {
    const tp::Xpr in = tp::Xpr::Parse(f.tpr);
    const auto g = tp::ParseMdlGeo(in.res[1].blob);
    const auto i = tp::ParseMdlInfo(in.res[2].blob);
    for (size_t k = 0; k < 6; ++k) same = same && o->geo[k].Build() == g[k].Build() && o->info[k].Build() == i[k].Build();
    for (size_t k = 6; k < 18; ++k) same = same && o->geo[k].NodeIndex() == k && o->hie[k].id == static_cast<int64_t>(k);
  }
  CHECK(same);
}

static void TestNrmUvShortRefused() {
  // a packed normal of two values: refused (struct.error), never a crash; Python raises too
  CheckAddBoth("nrm_short");
  const add::AddCase* c = add::FindAdd("nrm_short");
  std::string what;
  try {
    if (c) add::RunOrThrow(*c, CopyMode::kFull);
  } catch (const std::exception& e) {
    what = e.what();
  }
  CHECKF(what == "struct.error: pack expected 8 items for packing (got 7)", "%s", what.c_str());
}

static void TestVertexLimitEdges() {
  // 65534 vertices on one copy node: built (both writers); 65535: refused (0x02 with a textured strip; 0x04 with an
  // untextured one, which group 0x02 skips)
  CheckAdd("lim2_65534", CopyMode::kFull);
  CheckAdd("lim2_65535", CopyMode::kFull);
  CheckAdd("lim4_65535", CopyMode::kFull);
  const auto o = add::OutOf("lim2_65534", CopyMode::kFull);
  CHECK(o && o->geo.size() > 15 && o->geo[6].decls[0].head[5] == 65534 && o->geo[15].decls[0].head[5] == 65534);
  CHECK(AddRow("lim2_65535/full") == "REFUSED lim2_65535: copy node 6 over 65534 vertices");
  CHECK(AddRow("lim4_65535/full") == "REFUSED lim4_65535: shadow node 15 over 65534 vertices");
}

static void TestSlimCopiesRefusalContract() {
  const add::AddCase* c = add::FindAdd("collapse");
  const auto f = c ? add::FilesOf(*c) : add::Files{};
  sl::Result out{{1, 2, 3}, {4, 5}};
  std::string err;
  const bool ok = sl::SlimCopies("collapse", f.tpr, f.cat, f.ref_tpr, f.ref_cat, CopyMode::kSlim, &out, &err);
  CHECK(!ok);
  CHECKF(err == "collapse: copy 0x02: node 13 had 1 triangles before the merge and none after (nodes [13])", "%s",
         err.c_str());
  CHECK(out.tpr == (Bytes{1, 2, 3}) && out.cat == (Bytes{4, 5}));
  // and a build: true, the AddCopies bytes, the error untouched
  std::string err2 = "untouched";
  sl::Result built;
  const bool ok2 = sl::SlimCopies("collapse", f.tpr, f.cat, f.ref_tpr, f.ref_cat, CopyMode::kFull, &built, &err2);
  std::optional<sl::Result> direct;
  if (c) add::Run(*c, CopyMode::kFull, &direct);
  CHECK(ok2 && err2 == "untouched" && direct && built.tpr == direct->tpr && built.cat == direct->cat);
}

// every refusal branch of the writers and the assembly, each by its text (Python's REFUSED rows; for a Python raise
// the C++ text that names the exception type, Python's type and text in expect4.log's comment lines)
static void TestAddRefusals() {
  const auto what_of = [](const char* name, CopyMode m) {
    const add::AddCase* c = add::FindAdd(name);
    try {
      if (c) add::RunOrThrow(*c, m);
    } catch (const std::exception& e) {
      return std::string(e.what());
    }
    return std::string("(built)");
  };
  for (const char* n : {"neg_target", "zero_target", "no_tex", "obj_count", "tmpl_bad", "empty_g2", "short_mesh"})
    CheckAddBoth(n);
  // fix round 1 (review findings 1, 2, 3, 6), each by its text:
  for (const char* n : {"lost2", "inf_pos", "nrm_short_rot", "id_overflow"}) CheckAddBoth(n);
  // 2: two lost nodes in first-fed order (13, fed 3 triangles by O0 and O4, before 9); by id the text would name node 9 and [9, 13]
  CHECK(AddRow("lost2/slim") ==
        "REFUSED lost2: copy 0x02: node 13 had 3 triangles before the merge and none after (nodes [13, 9])");
  // 1: merge's own OverflowError passes unwrapped (Python: RAISED OverflowError, no "<name>: copy 0x02:" prefix)
  const std::string inf_what = what_of("inf_pos", CopyMode::kSlim);
  CHECKF(inf_what.rfind("OverflowError", 0) == 0 && inf_what.find("inf_pos: copy 0x02:") == std::string::npos, "%s",
         inf_what.c_str());
  CHECK(AddRow("inf_pos/full").rfind("tpr=", 0) == 0);
  // 6: a short normal on a rotated node: _to_local's IndexError (the unrotated case stays struct.error, nrm_short)
  CHECKF(what_of("nrm_short_rot", CopyMode::kFull) == "IndexError: tuple index out of range", "%s",
         what_of("nrm_short_rot", CopyMode::kFull).c_str());
  // 3: a first copy id of 256: convert.t2_bytes' assert, as Python's text
  CHECKF(what_of("id_overflow", CopyMode::kFull) == "AssertionError: ('type-2 entry out of range', '0x9002', 256)", "%s",
         what_of("id_overflow", CopyMode::kFull).c_str());
  CHECK(AddRow("zero_target/slim") ==
        "REFUSED zero_target: copy 0x02: node 6 had 2 triangles before the merge and none after (nodes [6])");
  CHECK(AddRow("neg_target/slim") == "REFUSED neg_target: copy 0x02: count 0 at H_HI = 1.0 exceeds target -9");
  CHECK(AddRow("neg_target/full").rfind("tpr=", 0) == 0);
  CHECK(AddRow("no_tex/slim") == "REFUSED no_tex: copy 0x02: origin: no triangles");
  CHECK(AddRow("no_tex/full").rfind("tpr=", 0) == 0);
  CHECK(AddRow("obj_count/full") == "REFUSED obj_count: 6 objects for 7 nodes");
  CHECK(AddRow("tmpl_bad/full") == "REFUSED tmpl_bad: reference group-0x02 mesh not of the native 2-stage shape");
  // Python: ValueError: min() iterable argument is empty / AssertionError: ('mesh header shorter than 0xC0', 64)
  CHECKF(what_of("empty_g2", CopyMode::kFull) == "ValueError: min() iterable argument is empty", "%s",
         what_of("empty_g2", CopyMode::kFull).c_str());
  CHECKF(what_of("short_mesh", CopyMode::kFull) == "AssertionError: ('mesh header shorter than 0xC0', 64)", "%s",
         what_of("short_mesh", CopyMode::kFull).c_str());
  // no textured mesh, full mode: every group-0x02 node a placeholder of texture 0 (any_tex's default)
  const auto o = add::OutOf("no_tex", CopyMode::kFull);
  bool all0 = o && o->geo.size() == 19;
  for (size_t k = 6; all0 && k < 15; ++k) all0 = add::MeshTexOf(*o, k) == std::vector<uint32_t>{0};
  CHECK(all0);
}

static void TestAddRows() {
  // every case and mode has its Python row and every row its case
  size_t rows = 0, found = 0;
  for (const auto& e : kAddExpected) {
    ++rows;
    std::string n = e.name;
    const size_t sl_at = n.rfind('/');
    found += sl_at != std::string::npos && add::FindAdd(n.substr(0, sl_at)) != nullptr;
  }
  CHECKF(rows == found && rows == 2 * add::AddCases().size(), "rows %zu found %zu cases %zu", rows, found,
         add::AddCases().size());
  for (const auto& c : add::AddCases()) CheckAddBoth(c.name);
}

// --bench: the scan's time on a synthetic suit of about 25000 triangles (not a check)
static int Bench() {
  scan::Scene sc = scan::Base();
  for (size_t o = 0; o < sc.objs.size(); ++o) {
    scan::DeclDef d;
    for (uint16_t k = 0; k < 4200; ++k) {
      const double x = static_cast<double>(o) + 0.001 * (k / 2), y = 0.01 * (k % 2);
      d.verts.push_back(scan::V({x, y, 0.001 * (k % 7)}, {0.5, 0.25, 0.25, 0}));
      d.idx.push_back(k);
    }
    sc.objs[o].decls = {d};
    sc.objs[o].meshes = {synth::MeshSpec{.decl = 0, .istart = 0, .icount = 4200, .tex = static_cast<uint32_t>(o + 1)}};
    sc.objs[o].infos_short = 0;
  }
  const scan::ScanCase c{"bench", sc};
  const scan::Loaded ld = scan::Load(c.name, scan::BuildCase(c));
  const sl::Layout lay = sl::MakeLayout(ld.suit, ld.ref);
  double best = 1e9;
  size_t n = 0;
  for (int rep = 0; rep < 5; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    const sl::Scan a = sl::ScanKind(ld.suit, ld.ref, lay, 2), b = sl::ScanKind(ld.suit, ld.ref, lay, 4);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    best = std::min(best, ms);
    n = a.tris.size() + b.tris.size();
  }
  std::printf("bench: scan kinds 2 + 4 of %zu + %zu triangles: best of 5 %.2f ms\n", n / 2, n - n / 2, best);
  // AddCopies, slim mode, on the same suit with a reference whose copy objects hold 120 vertices each (targets 0x02:
  // 9 * 120 - 9 = 1071, 0x04: 4 * 120 - 4 = 476, near the real costumes' 916 / 707), so choose_h bisects in full
  // The ribbons are 0.2 wide here (the scan's are 0.01), so the merge at the cell size the targets need keeps every
  // node fed (a 0.01 ribbon collapses whole and is refused as a lost node).
  add::AddCase ac{"bench", add::AddBase(), 120};
  for (size_t o = 0; o < ac.sc.objs.size(); ++o) {
    scan::DeclDef d;
    for (uint16_t k = 0; k < 4200; ++k) {
      const double x = static_cast<double>(o) + 0.001 * (k / 2), y = 0.2 * (k % 2);
      d.verts.push_back(scan::V({x, y, 0.001 * (k % 7)}, {0.5, 0.25, 0.25, 0}));
      d.idx.push_back(k);
    }
    ac.sc.objs[o].decls = {d};
    ac.sc.objs[o].meshes = {synth::MeshSpec{.decl = 0, .istart = 0, .icount = 4200, .tex = static_cast<uint32_t>(o + 1)}};
    ac.sc.objs[o].infos_short = 0;
  }
  const add::Files f = add::FilesOf(ac);
  double best_add = 1e9;
  size_t out_bytes = 0;
  for (int rep = 0; rep < 5; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    sl::Result r;
    try {
      r = sl::AddCopies("bench", f.tpr, f.cat, f.ref_tpr, f.ref_cat, sl::CopyMode::kSlim);
    } catch (const std::exception& e) {
      std::printf("bench: AddCopies refused: %s\n", e.what());
      return 1;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    best_add = std::min(best_add, ms);
    out_bytes = r.tpr.size();
  }
  std::printf("bench: AddCopies slim mode (read, scan, choose_h, write, assemble), suit of %zu + %zu triangles: best of 5 "
              "%.2f ms (tpr %zu bytes)\n",
              n / 2, n - n / 2, best_add, out_bytes);
  return 0;
}

// ---------------------------------------------------------------- dump mode
static int Dump(const fs::path& dir) {
  fs::create_directories(dir);
  const auto put = [](const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  };
  int n = 0;
  for (const Case& c : Cases()) {
    if (c.is_cat) {
      put(dir / (c.name + ".cat"), c.cat);
      put(dir / (c.name + ".map"), c.map);
    } else {
      put(dir / (c.name + ".tpr"), c.tpr);
      put(dir / (c.name + ".cat"), c.cat);
    }
    ++n;
  }
  fs::create_directories(dir / "grid");
  for (const grid::GridCase& c : grid::GridCases()) {
    grid::WriteGridCase(dir / "grid", c);
    ++n;
  }
  fs::create_directories(dir / "scan");
  for (const scan::ScanCase& c : scan::ScanCases()) {
    const scan::Built b = scan::BuildCase(c);
    put(dir / "scan" / (c.name + ".tpr"), b.tpr);
    put(dir / "scan" / (c.name + ".cat"), b.cat);
    put(dir / "scan" / (c.name + ".ref.tpr"), b.ref_tpr);
    put(dir / "scan" / (c.name + ".ref.cat"), b.ref_cat);
    ++n;
  }
  {
    std::string t;
    for (const auto& c : scan::ToLocalCases()) {
      t += std::string(c.name) + " w";
      for (double v : c.w) t += " " + scan::Hex16(scan::Bits(v));
      t += " v";
      for (double v : c.v) t += " " + scan::Hex16(scan::Bits(v));
      t += std::string(" dir ") + (c.direction ? "1" : "0") + "\n";
      ++n;
    }
    put(dir / "scan" / "tolocal.txt", Bytes(t.begin(), t.end()));
  }
  fs::create_directories(dir / "add");
  for (const add::AddCase& c : add::AddCases()) {
    const add::Files f = add::FilesOf(c);
    put(dir / "add" / (c.name + ".tpr"), f.tpr);
    put(dir / "add" / (c.name + ".cat"), f.cat);
    put(dir / "add" / (c.name + ".ref.tpr"), f.ref_tpr);
    put(dir / "add" / (c.name + ".ref.cat"), f.ref_cat);
    if (c.hook_swap) {
      const std::string h = "swap_last_two\n";
      put(dir / "add" / (c.name + ".hook"), Bytes(h.begin(), h.end()));
    }
    ++n;
  }
  {  // one case per line: NAME, then per key 'K' and per vertex 'v VID XBITS YBITS ZBITS TRI CORNER'
    std::string t;
    for (const auto& c : add::NvCases()) {
      t += c.name;
      for (const auto& k : c.keys) {
        t += " K";
        for (const auto& v : k)
          t += " v " + std::to_string(v.vid) + " " + scan::Hex16(scan::Bits(v.x)) + " " + scan::Hex16(scan::Bits(v.y)) +
               " " + scan::Hex16(scan::Bits(v.z)) + " " + std::to_string(v.tri) + " " + std::to_string(v.corner);
      }
      t += "\n";
      ++n;
    }
    put(dir / "add" / "nodeverts.txt", Bytes(t.begin(), t.end()));
  }
  {  // one case per line: NAME, then per point 'p XBITS YBITS ZBITS'
    std::string t;
    for (const auto& c : add::BoundsCases()) {
      t += c.name;
      for (const auto& p : c.pts)
        t += " p " + scan::Hex16(scan::Bits(p[0])) + " " + scan::Hex16(scan::Bits(p[1])) + " " + scan::Hex16(scan::Bits(p[2]));
      t += "\n";
      ++n;
    }
    put(dir / "add" / "bounds.txt", Bytes(t.begin(), t.end()));
  }
  std::printf("dumped %d cases to %s\n", n, dir.string().c_str());
  return 0;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  doa4::suitconv::num::FpEnvGuard fp;
  if (argc == 3 && std::strcmp(argv[1], "--dump") == 0) return Dump(argv[2]);
  if (argc == 2 && std::strcmp(argv[1], "--bench") == 0) return Bench();
  RunGroup(TestSynthRoundTrip, "synth_round_trip");
  RunGroup(TestStripRemovesCopies, "strip_removes_copies");
  RunGroup(TestStripNotTrailing, "strip_not_trailing_in_list_order");
  RunGroup(TestStripRefusals, "strip_refusals");
  RunGroup(TestCatMapConverter, "cat_map_converter");
  RunGroup(TestCatMapInPlace, "cat_map_in_place");
  RunGroup(TestCatRefusals, "cat_refusals");
  RunGroup(TestMapGrammar, "map_grammar");
  RunGroup(TestBufSet, "buf_set");
  RunGroup(TestMergeIdentity, "merge_identity");
  RunGroup(TestMergeCellMean, "merge_cell_mean");
  RunGroup(TestMergeDegenerateAndDup, "merge_degenerate_and_dup");
  RunGroup(TestMergeAllCollapse, "merge_all_collapse");
  RunGroup(TestMergeKeysSeparate, "merge_keys_separate");
  RunGroup(TestMergeUnreferenced, "merge_unreferenced");
  RunGroup(TestChooseHFits, "choose_h_fits");
  RunGroup(TestChooseHTooSmall, "choose_h_too_small");
  RunGroup(TestDeterministic, "deterministic");
  RunGroup(TestEmptyKey, "empty_key");
  RunGroup(TestBisectLeEdge, "bisect_le_edge");
  RunGroup(TestCellMeanIsSequential, "cell_mean_is_sequential");
  RunGroup(TestVidMembership, "vid_membership");
  RunGroup(TestGridRefusals, "grid_refusals");
  RunGroup(TestChannelLowestCode, "channel_lowest_code");
  RunGroup(TestShallowestThenLowestId, "shallowest_then_lowest_id");
  RunGroup(TestIdsFromOwnCount, "ids_from_own_count");
  RunGroup(TestHeaviestTieLowestBone, "heaviest_tie_lowest_bone");
  RunGroup(TestFirstVertexAfterSwap, "first_vertex_after_swap");
  RunGroup(TestStripTrisRestart, "strip_tris_restart");
  RunGroup(TestCornerAddsWorldTranslation, "corner_adds_world_translation");
  RunGroup(TestWorldMatsCompensated, "world_mats_compensated");
  RunGroup(TestT1FirstRowPerCode, "t1_first_row_per_code");
  RunGroup(TestNeckN2, "neck_n2");
  RunGroup(TestKind2SkipsUntextured, "kind2_skips_untextured");
  RunGroup(TestMatsrcFirstMesh, "matsrc_first_mesh");
  RunGroup(TestToLocalCompensated, "to_local_compensated");
  RunGroup(TestFrameDevThreshold, "frame_dev_threshold");
  RunGroup(TestVidsOneToOne, "vids_one_to_one");
  RunGroup(TestKeyFieldsSeparate, "key_fields_separate");
  RunGroup(TestIndexSliceClamps, "index_slice_clamps");
  RunGroup(TestOutOfRangeRefuses, "out_of_range_refuses");
  RunGroup(TestKind4PayloadHasNoAttributes, "kind4_payload_has_no_attributes");
  RunGroup(TestScanRefusals, "scan_refusals");
  RunGroup(TestSamePassKeysKeepOrder, "same_pass_keys_keep_order");
  RunGroup(TestFusionByVidAndPosition, "fusion_by_vid_and_position");
  RunGroup(TestVertexOrderByFirstAppearance, "vertex_order_by_first_appearance");
  RunGroup(TestReflectionFrameRotatesOnlyPast1e6, "reflection_frame_rotates_only_past_1e-6");
  RunGroup(TestShadowMinusJoint, "shadow_minus_joint");
  RunGroup(TestBoundsSignedZeroTie, "bounds_signed_zero_tie");
  RunGroup(TestBoundsRadiusCompensated, "bounds_radius_compensated");
  RunGroup(TestPlaceholderBytes, "placeholder_bytes");
  RunGroup(TestPlaceholderTextureFromMergedOrder, "placeholder_texture_from_merged_order");
  RunGroup(TestCollapseRefused, "collapse_refused");
  RunGroup(TestFullModeNeverRefuses, "full_mode_never_refuses");
  RunGroup(TestMapRuleI1, "map_rule_i1");
  RunGroup(TestKindOrderFollowsMarkerOrder, "kind_order_follows_marker_order");
  RunGroup(TestBuffersAt4k, "buffers_at_4k");
  RunGroup(TestUser0Recomputed, "user0_recomputed");
  RunGroup(TestIdsOutOfStepRefused, "ids_out_of_step_refused");
  RunGroup(TestTwiceEqual, "twice_equal");
  RunGroup(TestSelfReference, "self_reference");
  RunGroup(TestNrmUvShortRefused, "nrm_uv_short_refused");
  RunGroup(TestVertexLimitEdges, "vertex_limit_edges");
  RunGroup(TestSlimCopiesRefusalContract, "slim_copies_refusal_contract");
  RunGroup(TestAddRefusals, "add_refusals");
  RunGroup(TestAddRows, "add_rows");
  if (g_failures) {
    std::printf("FAILED: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }
  std::printf("PASSED: %d checks, 0 failures\n", g_checks);
  return 0;
}
