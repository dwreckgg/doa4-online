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
// cat_map_in_place, cat_refusals, map_grammar, buf_set. Last line: "PASSED: N checks, 0 failures" or "FAILED: N checks, M failures".

#include "../../src/doa4_suitconv_slim.h"
#include "slim_synth.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
  m.SetGroups({Main({{0x9001, 0}, {0x9002, 1}, {0xD41410, 0}, {0x9003, 2}, {0x9004, 3}}),
               synth::State(0x10, {{0x9003, 2}}), synth::State(0x11, {{0x9004, 3}, {0xD01410, 1}}),
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
  // copy groups (as Helena's reference hel05 holds them)
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
    {"cat_states_changed", "OK fnv=9b3fb8f004c39145 len=208"},
    {"cat_states_same", "OK fnv=6897cbd0931fd329 len=192"},
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
    {"strip_states", "OK fnv=2b5cfef084a35e7a len=45056 stripped=old copies ids 4..6, 6 buffers ids=[0, 1, 2, 3] groups=main[9001:0,9002:1,d41410:0,9003:2,9004:3] s10[9003:2] s11[9004:3,d01410:1] 02[9002:4,9003:5] 04[9002:6]"},
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
  std::printf("dumped %d cases to %s\n", n, dir.string().c_str());
  return 0;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  doa4::suitconv::num::FpEnvGuard fp;
  if (argc == 3 && std::strcmp(argv[1], "--dump") == 0) return Dump(argv[2]);
  RunGroup(TestSynthRoundTrip, "synth_round_trip");
  RunGroup(TestStripRemovesCopies, "strip_removes_copies");
  RunGroup(TestStripNotTrailing, "strip_not_trailing_in_list_order");
  RunGroup(TestStripRefusals, "strip_refusals");
  RunGroup(TestCatMapConverter, "cat_map_converter");
  RunGroup(TestCatMapInPlace, "cat_map_in_place");
  RunGroup(TestCatRefusals, "cat_refusals");
  RunGroup(TestMapGrammar, "map_grammar");
  RunGroup(TestBufSet, "buf_set");
  if (g_failures) {
    std::printf("FAILED: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }
  std::printf("PASSED: %d checks, 0 failures\n", g_checks);
  return 0;
}
