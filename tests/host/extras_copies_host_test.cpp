// Host test for src/doa4_extras_copies.cpp (Task 8 of the extras-copies plan): the start-up builder Build, the game's
// costume table Costumes() and the archive reader ReadEntryFrom / ReadEntryFromSource. Built and run by
// run_extras_copies_test.bat with the converter's compile flags (Build runs the slim core).
//
// Every model is synthetic: the slim test's "self" add case (tests/host/suitconv_slim_host_test.cpp, scan::Base and
// add::SelfRef, copied below as SelfScene; a suit that carries stand-in copies and is its own reference, as the game's
// costumes are) built by slim_synth.h's writers from literal numbers. Variant 0 is that case byte for byte: its slim
// output is pinned to the Python tool's row for "self/slim" (kAddExpected in the slim test), which anchors the copy.
// Variants 1 and 2 move one vertex of object 0, so the three test costumes have different bytes. The test table's
// SHA-1s are computed here from those bytes (an own FIPS 180-4 SHA-1, independent of the Windows CNG one Build uses).
//
// Groups: fixture_anchor, costumes_table, boxes_off_reads_nothing, opera_only_reads_two_entries, built_replaces_both_entries,
// hash_mismatch_keeps_standin, core_refusal_keeps_standin, over_limit_refused, unreadable_archive,
// read_callback_throws, sha1_of_built_is_stable_and_ordered, boxes_at_start_bits, unknown_cvar_row, read_entry_from,
// reader_promises, toc_refusal_reason, allocation_failures (a replaced global operator new, armed only in that group);
// then the gated real-archive check "extras real archive": DOA4_EXTRAS_GAME_DIR (a game folder holding chrtwo.afs, read
// only) runs Build on the real archive and expects all three built with G0's SHA-1s (g0_sha1.txt); unset, it prints
// "extras real archive: SKIPPED (DOA4_EXTRAS_GAME_DIR unset)" and counts nothing. DOA4_EXTRAS_TEST_FLIP=1 (with the
// game folder) flips one byte of what the reader returns for entry 46: the group must then fail on hak03 alone.
// Last line: "PASSED: N checks, 0 failures" or "FAILED: N checks, M failures".

#include "../../src/doa4_extras_copies.h"
#include "../../src/doa4_gamefiles.h"
#include "../../src/doa4_suitconv_slim.h"
#include "slim_synth.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <list>
#include <fstream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace tp = doa4::suitconv::tpr;
namespace sl = doa4::suitconv::slim;
namespace xc = doa4::extras_copies;
namespace afs = doa4::afs;
using tp::Bytes;

static int g_checks = 0;
static int g_failures = 0;

// ---------------------------------------------------------------- allocation-failure injection (group
// allocation_failures only; the reviewer's probe P3 as a test): a replaced global operator new that, while armed, throws
// std::bad_alloc once at the K-th allocation and disarms itself (single-shot), or at the K-th and every one after it
// while armed (sticky). Unarmed (every other group) it is malloc. Only these forms exist: the code on Build's path has
// no aligned (align_val_t) and no nothrow new (fix wave 2 M5, grep in the hand-back).
static std::atomic<long long> g_alloc_count{0};
static std::atomic<long long> g_alloc_fail_at{-1};
static std::atomic<bool> g_alloc_armed{false};
static std::atomic<bool> g_alloc_fired{false};
static std::atomic<bool> g_alloc_sticky{false};

static void* TestAlloc(size_t n) {
  if (g_alloc_armed.load(std::memory_order_relaxed)) {
    const long long c = ++g_alloc_count;
    const long long k = g_alloc_fail_at.load(std::memory_order_relaxed);
    if (g_alloc_sticky.load(std::memory_order_relaxed) && k > 0 && c >= k) {
      g_alloc_fired = true;
      throw std::bad_alloc();
    }
    if (c == k) {
      g_alloc_armed = false;
      g_alloc_fired = true;
      throw std::bad_alloc();
    }
  }
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new(size_t n) { return TestAlloc(n); }
void* operator new[](size_t n) { return TestAlloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

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
  } catch (...) {
    Check(false, __FILE__, __LINE__, "group threw", "%s: a non-standard exception", name);
  }
  std::printf("%s: %d checks, %d failures\n", name, g_checks - before, g_failures - fb);
}

// ---------------------------------------------------------------- SHA-1 (FIPS 180-4), the test's own
static std::string Sha1(const Bytes& msg) {
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

static Bytes Concat(std::initializer_list<const Bytes*> parts) {
  Bytes out;
  for (const Bytes* p : parts) out.insert(out.end(), p->begin(), p->end());
  return out;
}

// ---------------------------------------------------------------- the synthetic costumes (copied from the slim test)
// scan::NodeDef / DeclDef / ObjDef / Scene, scan::Tree, scan::SuitModel, scan::V, scan::Base, add::SelfRef and the
// self branch of add::FilesOf, unchanged apart from names (suitconv_slim_host_test.cpp:1262-1405, 2366-2384,
// 2517-2546).
namespace fx {

using MS = synth::MeshSpec;
struct NodeDef {
  int64_t id, parent_id;
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

static Bytes V(std::array<double, 3> p, std::array<double, 4> w = {1, 0, 0, 0}) {
  return synth::Vert(p, {p[1] * 0.5, 0.75, p[0] - 0.125}, {p[0] + 0.0625, 1.0 - p[2]}, w);
}

static Scene Base() {
  Scene sc;
  sc.suit_nodes = {{0, -1, 0.5, 1.0, 0.25}, {1, 0, 0.125, 0.2, 0.3}, {2, 1, 0.0625, -0.125, 0.75},
                   {3, 0, -0.3, 0.1, 0.05},  {4, 3, 0.01, 0.02, 0.03},  {5, 2, 0.3, 0.3, -0.2}};
  sc.main = {{0x9002, 0}, {0x9003, 1}, {0x9004, 2}, {0x9006, 3}, {0x9005, 3}, {0x9101, 4}, {0x9007, 5}};
  sc.ref_main = sc.main;
  const double c = 0.0, s = 1.0;
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
  sc.objs.push_back({{},
                     {{{V({0, 0, 0}), V({0.25, 0, 0}), V({0, 0.25, 0}), V({0.25, 0.25, 0.125})}, {0, 1, 2, 3}}},
                     {MS{.decl = 0, .istart = 0, .icount = 4, .tex = 1}}});
  sc.objs.push_back({{2, 1, 2},
                     {{{V({0, 0, 0.5}, {0.5, 0.5, 0, 0}), V({0.5, 0, 0.5}, {0.3, 0.4, 0.3, 0}),
                        V({0, 0.5, 0.5}, {0.125, 0.75, 0.125, 0}), V({0.5, 0.5, 0.5}, {0, 0.25, 0.5, 0.25})},
                       {0, 1, 2, 3}}},
                     {MS{.decl = 0, .istart = 0, .icount = 4, .tex = 2}}});
  sc.objs.push_back({{},
                     {{{V({0, 0, 1}), V({0.5, 0, 1}), V({0, 0.5, 1}), V({0.5, 0.5, 1.25})}, {0, 1, 2, 1, 2, 3, 0, 2, 3}}},
                     {MS{.decl = 0, .istart = 0, .icount = 3, .tex = 3, .pass = 4, .flag = 1, .word4 = 0x11},
                      MS{.decl = 0, .istart = 3, .icount = 3, .tex = 0xFFFFFFFF},
                      MS{.decl = 0, .istart = 6, .icount = 3, .tex = 3, .pass = 4, .flag = 1, .word4 = 0x11,
                         .index_word = 2, .other = 0x55}}});
  sc.objs.push_back({{},
                     {{{V({1, 0, 0}), V({1.25, 0, 0}), V({1, 0.25, 0})}, {0, 1, 2}},
                      {{V({2, 0, 0}), V({2.25, 0, 0}), V({2, 0.25, 0})}, {0, 1, 2}}},
                     {MS{.decl = 0, .istart = 0, .icount = 3, .tex = 4}, MS{.decl = 0, .istart = 0, .icount = 3, .tex = 5},
                      MS{.decl = 1, .istart = 0, .icount = 3, .tex = 4}}});
  sc.objs.push_back({{3, 3},
                     {{{V({0, 1, 0}, {0.625, 0.375, 0, 0}), V({0.25, 1, 0}, {0.625, 0.375, 0, 0}),
                        V({0, 1.25, 0}, {0.625, 0.375, 0, 0})},
                       {0, 1, 2}}},
                     {MS{.decl = 0, .istart = 0, .icount = 3, .tex = 6}}});
  sc.objs.push_back({{},
                     {{{V({3, 0, 0}), V({3.25, 0, 0}), V({3, 0.25, 0}), V({3.5, 0, 0}), V({3.5, 0.25, 0}),
                        V({3.75, 0, 0}), V({3.75, 0.25, 0}), V({4, 0.5, 0})},
                       {0, 1, 2, 0xFFFF, 3, 4, 5, 6, 7, 7, 0, 1, 3}}},
                     {MS{.decl = 0, .istart = 0, .icount = 8, .tex = 7}, MS{.decl = 0, .istart = 8, .icount = 100, .tex = 7}}});
  return sc;
}

static Scene SelfScene() {
  const Scene b = Base();
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

struct Files {
  Bytes tpr, cat;
};

static Files SelfFiles(const Scene& scene) {
  synth::Model m = SuitModel(scene);
  uint32_t at = static_cast<uint32_t>(tp::Al(m.data_end, 0x1000)), end = m.data_end;
  for (size_t k = 7; k < m.vbufs.size(); ++k)
    for (synth::Buf* b : {&m.vbufs[k], &m.ibufs[k]}) {
      b->base = at;
      end = at + b->size;
      at = static_cast<uint32_t>(tp::Al(end, 0x1000));
    }
  m.SetDataEnd(static_cast<uint32_t>(tp::Al(end, 16)));
  m.SetGroups({synth::Main(scene.main), synth::Group(2, scene.g2), synth::State(0x10, {{0x9001, 0}}),
               synth::Group(4, scene.g4)});
  return {m.MakeTpr(), m.MakeCat()};
}

// Variant 0: the slim test's "self" case. Variants 1, 2: object 0's last vertex moved up by k/32 in z. Variant 3: the
// neck shrunk (a model the core refuses in slim mode).
static const Files& Variant(int k) {
  static std::map<int, Files> cache;
  auto it = cache.find(k);
  if (it != cache.end()) return it->second;
  Scene sc = SelfScene();
  if (k == 1 || k == 2) sc.objs[0].decls[0].verts[3] = V({0.25, 0.25, 0.125 + k / 32.0});
  if (k == 3)  // the slim test's "collapse" edit: object 4 (the neck) shrunk to a triangle of 1e-5, which slim collapses
    sc.objs[4].decls[0].verts = {V({0.0004, 1.0004, 0.0004}, {0.625, 0.375, 0, 0}),
                                 V({0.00041, 1.0004, 0.0004}, {0.625, 0.375, 0, 0}),
                                 V({0.0004, 1.00041, 0.0004}, {0.625, 0.375, 0, 0})};
  return cache.emplace(k, SelfFiles(sc)).first->second;
}

}  // namespace fx

// ---------------------------------------------------------------- the test table and the fake archive
struct Direct {
  bool ok = false;
  sl::Result out;
  std::string error;
};
static Direct DirectSlim(const char* name, const Bytes& tpr, const Bytes& cat) {
  Direct d;
  d.ok = sl::SlimCopies(name, tpr, cat, tpr, cat, sl::CopyMode::kSlim, &d.out, &d.error);
  return d;
}

// one test costume's input files
struct Input {
  const char* cvar;
  const char* name;
  uint32_t tpr_entry, cat_entry;
  Bytes tpr, cat;
  uint32_t tpr_limit = 0xFA0000, cat_limit = 0x300000;
};

static std::vector<Input> DefaultInputs() {
  return {{"doa4_helena_opera", "hel03", 16, 17, fx::Variant(0).tpr, fx::Variant(0).cat},
          {"doa4_kokoro_kimonos", "hak03", 46, 47, fx::Variant(1).tpr, fx::Variant(1).cat},
          {"doa4_kokoro_kimonos", "hak08", 56, 57, fx::Variant(2).tpr, fx::Variant(2).cat}};
}

// A costume table whose strings live as long as it does; SHA-1s from the inputs' bytes.
struct Table {
  std::list<std::string> strs;
  std::vector<xc::Costume> rows;
  const char* Keep(std::string s) { return strs.emplace_back(std::move(s)).c_str(); }
  std::span<const xc::Costume> span() const { return rows; }
};
static Table TableOf(const std::vector<Input>& in) {
  Table t;
  for (const Input& i : in)
    t.rows.push_back(xc::Costume{i.cvar, t.Keep(std::string(i.name) + " (extras copy)"), i.name, i.tpr_entry,
                                 i.cat_entry, t.Keep(Sha1(i.tpr)), t.Keep(Sha1(i.cat)), i.tpr_limit, i.cat_limit});
  return t;
}

struct Archive {
  std::map<uint32_t, Bytes> entries;
  std::set<uint32_t> fail, throw_std, throw_other;
  std::vector<uint32_t> reads;
  std::function<void(uint32_t)> on_read;  // the allocation group records its counter here
  xc::ReadEntry Fn() {
    return [this](uint32_t entry, std::vector<uint8_t>* out) -> bool {
      if (on_read) on_read(entry);
      reads.push_back(entry);
      if (throw_std.count(entry)) throw std::runtime_error("archive gone");
      if (throw_other.count(entry)) throw 42;
      if (fail.count(entry)) return false;
      const auto it = entries.find(entry);
      if (it == entries.end()) return false;
      *out = it->second;
      return true;
    };
  }
};
static Archive ArchiveOf(const std::vector<Input>& in) {
  Archive a;
  for (const Input& i : in) a.entries[i.tpr_entry] = i.tpr, a.entries[i.cat_entry] = i.cat;
  return a;
}

static Bytes ReadBack(const afs::Replacement& r) {
  if (!r.data) return {};
  Bytes b(static_cast<size_t>(r.data->size()));
  if (!r.data->Read(0, b.data(), b.size())) return {};
  return b;
}

static std::vector<uint32_t> Entries(const xc::Built& b) {
  std::vector<uint32_t> v;
  for (const auto& r : b.chrtwo) v.push_back(r.entry);
  return v;
}

static std::string BuiltLine(const char* name, const sl::Result& r) {
  return std::string(name) + ": built (tpr " + std::to_string(r.tpr.size()) + " bytes, cat " +
         std::to_string(r.cat.size()) + " bytes)";
}
static std::string KeptLine(const char* name, const std::string& reason) {
  return std::string(name) + ": kept the stand-in: " + reason;
}

// "extras copies: <k> built of <n> selected in <digits> ms"; with `unhashed`, "extras copies: 0 built of <n> selected
// (the copies could not be hashed; none installed) in <digits> ms" (fix wave 2 I1)
static bool ClosingLine(const std::string& s, int k, int n, bool unhashed = false) {
  const std::string head = "extras copies: " + std::to_string(k) + " built of " + std::to_string(n) + " selected" +
                           (unhashed ? " (the copies could not be hashed; none installed)" : "") + " in ";
  if (s.size() <= head.size() + 3 || s.compare(0, head.size(), head) != 0) return false;
  if (s.compare(s.size() - 3, 3, " ms") != 0) return false;
  const std::string mid = s.substr(head.size(), s.size() - head.size() - 3);
  return !mid.empty() && std::all_of(mid.begin(), mid.end(), [](char c) { return c >= '0' && c <= '9'; });
}

static std::string LogAt(const xc::Built& b, size_t i) { return i < b.log.size() ? b.log[i] : std::string("<none>"); }

// the direct core outputs of the default inputs (the oracle for every built costume)
static const std::vector<Direct>& Directs() {
  static const std::vector<Direct> d = [] {
    std::vector<Direct> v;
    for (const Input& i : DefaultInputs()) v.push_back(DirectSlim(i.name, i.tpr, i.cat));
    return v;
  }();
  return d;
}

// ---------------------------------------------------------------- the groups

static void TestCostumesTable() {
  // the game's table, pinned to G0's g0_sha1.txt (ou_runs/extras_refl/g0) and the box names of doa4_tweaks.cpp:112-113
  const auto t = xc::Costumes();
  CHECKF(t.size() == 3, "%zu rows", t.size());
  if (t.size() != 3) return;
  const auto row = [&](size_t i, const char* cvar, const char* label, const char* name, uint32_t te, uint32_t ce,
                       const char* ts, const char* cs, uint32_t tl, uint32_t cl) {
    const xc::Costume& c = t[i];
    CHECKF(std::string(c.tweak_cvar) == cvar && std::string(c.label) == label && std::string(c.name) == name &&
               c.tpr_entry == te && c.cat_entry == ce && std::string(c.tpr_sha1) == ts && std::string(c.cat_sha1) == cs &&
               c.tpr_limit == tl && c.cat_limit == cl,
           "row %zu (%s)", i, c.name ? c.name : "?");
  };
  row(0, "doa4_helena_opera", "hel03 (extras copy)", "hel03", 16, 17, "46aa6d42e34d093e1cdc198b9e78669a65bb0f5c",
      "6a7135bff35e25da97a47b77f83d7f26ddcc9a54", 0xFA0000, 0x200000);
  row(1, "doa4_kokoro_kimonos", "hak03 (extras copy)", "hak03", 46, 47, "00408e2162b8c3dcc6e6bb85a3e22e0ef312ea65",
      "6b91dc27df6b8190b92cbed6c08a822b1e0300b2", 0xCB0000, 0x300000);
  row(2, "doa4_kokoro_kimonos", "hak08 (extras copy)", "hak08", 56, 57, "e81b060734d8e908e19dec828c9f9d319f384e71",
      "de0335bd97423465f078421bab0f489642e195ed", 0xCB0000, 0x300000);
}

static void TestFixtureAnchor() {
  // the own SHA-1 against a FIPS 180-4 vector ("abc"), so the test tables' hashes are right
  CHECK(Sha1(Bytes{'a', 'b', 'c'}) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  // the fixture anchor: variant 0 is the slim test's "self" case (Python row "self/slim"); variants differ
  const auto& d = Directs();
  CHECK(d.size() == 3 && d[0].ok && d[1].ok && d[2].ok);
  if (d.size() == 3) {
    CHECKF(Sha1(d[0].out.tpr) == "827d0705f45a2d189b8f882384a91a4e78739236" && d[0].out.tpr.size() == 135168 &&
               Sha1(d[0].out.cat) == "9ebc22d3e9be63981f5bbbfea39ef001a51cd270" && d[0].out.cat.size() == 224,
           "variant 0 slim: tpr %s:%zu", Sha1(d[0].out.tpr).c_str(), d[0].out.tpr.size());
    CHECK(d[0].out.tpr != d[1].out.tpr && d[1].out.tpr != d[2].out.tpr && d[0].out.tpr != d[2].out.tpr);
    // the build changes the tpr (so a replacement holding the retail bytes is told apart)
    CHECK(d[0].out.tpr != fx::Variant(0).tpr && d[1].out.tpr != fx::Variant(1).tpr);
    // tpr and cat differ in size (so a swap in the replacement is told apart)
    CHECK(d[0].out.tpr.size() != d[0].out.cat.size());
  }
}

static void TestBoxesOffReadsNothing() {
  // wrong version: reading (or hashing) the rows before the box test; it calls the callback here
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a = ArchiveOf(in);
  const xc::Built b = xc::Build(false, false, a.Fn(), t.span());
  CHECKF(a.reads.empty(), "%zu reads", a.reads.size());
  CHECK(b.chrtwo.empty() && b.sha1.empty() && b.boxes_at_start == 0);
  CHECKF(b.log.size() <= 1, "%zu lines", b.log.size());
  CHECKF(b.log.size() == 1 && ClosingLine(b.log[0], 0, 0), "%s", LogAt(b, 0).c_str());
  // the game's table, through the defaulted argument (as the game calls it)
  Archive g = ArchiveOf(in);
  const xc::Built bg = xc::Build(false, false, g.Fn());
  CHECKF(g.reads.empty() && bg.chrtwo.empty() && bg.sha1.empty() && bg.log.size() <= 1, "%zu reads, %zu lines",
         g.reads.size(), bg.log.size());
}

static void TestOperaOnlyReadsTwoEntries() {
  // wrong versions: the kimono rows read too (any read of 46, 47, 56, 57), or a box bit swapped (kimonos built)
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a = ArchiveOf(in);
  const xc::Built b = xc::Build(true, false, a.Fn(), t.span());
  CHECKF(a.reads == (std::vector<uint32_t>{16, 17}), "%zu reads", a.reads.size());
  CHECK(Entries(b) == (std::vector<uint32_t>{16, 17}));
  CHECKF(b.log.size() == 2 && b.log[0] == BuiltLine("hel03", Directs()[0].out) && ClosingLine(b.log[1], 1, 1), "%s | %s",
         LogAt(b, 0).c_str(), LogAt(b, 1).c_str());
  // and the kimonos alone: their four entries, in table order, hel03 untouched
  Archive k = ArchiveOf(in);
  const xc::Built bk = xc::Build(false, true, k.Fn(), t.span());
  CHECK(k.reads == (std::vector<uint32_t>{46, 47, 56, 57}));
  CHECK(Entries(bk) == (std::vector<uint32_t>{46, 47, 56, 57}));
}

static void TestBuiltReplacesBothEntries() {
  // wrong versions: tpr and cat swapped (entry 16 would hold 224 bytes), the retail bytes kept as the replacement, the
  // rows out of table order, the core called with another name or mode (other bytes than the direct call's)
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a = ArchiveOf(in);
  const xc::Built b = xc::Build(true, true, a.Fn(), t.span());
  CHECK(Entries(b) == (std::vector<uint32_t>{16, 17, 46, 47, 56, 57}));
  CHECK(a.reads == (std::vector<uint32_t>{16, 17, 46, 47, 56, 57}));
  const auto& d = Directs();
  if (b.chrtwo.size() == 6 && d.size() == 3) {
    for (size_t i = 0; i < 3; ++i) {
      CHECKF(ReadBack(b.chrtwo[2 * i]) == d[i].out.tpr, "costume %zu tpr", i);
      CHECKF(ReadBack(b.chrtwo[2 * i + 1]) == d[i].out.cat, "costume %zu cat", i);
      CHECKF(ReadBack(b.chrtwo[2 * i]) != in[i].tpr, "costume %zu tpr is not the retail bytes", i);
      // each replacement owns its bytes: a MemorySource
      CHECK(dynamic_cast<const afs::MemorySource*>(b.chrtwo[2 * i].data.get()) != nullptr &&
            dynamic_cast<const afs::MemorySource*>(b.chrtwo[2 * i + 1].data.get()) != nullptr);
    }
  } else {
    CHECKF(false, "%zu replacements", b.chrtwo.size());
  }
  CHECKF(b.log.size() == 4, "%zu lines", b.log.size());
  if (b.log.size() == 4 && d.size() == 3) {
    CHECKF(b.log[0] == BuiltLine("hel03", d[0].out), "%s", b.log[0].c_str());
    CHECKF(b.log[1] == BuiltLine("hak03", d[1].out), "%s", b.log[1].c_str());
    CHECKF(b.log[2] == BuiltLine("hak08", d[2].out), "%s", b.log[2].c_str());
    CHECKF(ClosingLine(b.log[3], 3, 3), "%s", b.log[3].c_str());
  }
  // labels: the test table carries them through unchanged
  CHECK(t.rows.size() == 3 && std::string(t.rows[1].label) == "hak03 (extras copy)");
}

static void TestHashMismatchKeepsStandin() {
  // one flipped byte in hak03's tpr (the table keeps the retail SHA-1). Wrong versions: no hash check (hak03 built, or
  // refused with the core's text), a failure that drops the whole kimono box (hak08 kept too), the hash taken over
  // other bytes than the callback's
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a = ArchiveOf(in);
  {
    Bytes& b46 = a.entries[46];
    b46[b46.size() - 1] ^= 0x01;
  }
  const xc::Built b = xc::Build(true, true, a.Fn(), t.span());
  CHECK(Entries(b) == (std::vector<uint32_t>{16, 17, 56, 57}));
  CHECKF(LogAt(b, 1) == KeptLine("hak03", "hak03.tpr (chrtwo.afs entry 46) is not the retail file"), "%s",
         LogAt(b, 1).c_str());
  CHECKF(LogAt(b, 2) == BuiltLine("hak08", Directs()[2].out), "%s", LogAt(b, 2).c_str());
  CHECKF(b.log.size() == 4 && ClosingLine(LogAt(b, 3), 2, 3), "%s", LogAt(b, 3).c_str());
  if (b.chrtwo.size() == 4) CHECK(ReadBack(b.chrtwo[2]) == Directs()[2].out.tpr);
  // and a flipped byte in hel03's cat: hel03 kept, naming its cat
  Archive c = ArchiveOf(in);
  c.entries[17][0] ^= 0x80;
  const xc::Built bc = xc::Build(true, false, c.Fn(), t.span());
  CHECK(bc.chrtwo.empty() && bc.sha1.empty());
  CHECKF(LogAt(bc, 0) == KeptLine("hel03", "hel03.cat (chrtwo.afs entry 17) is not the retail file"), "%s",
         LogAt(bc, 0).c_str());
}

static void TestCoreRefusalKeepsStandin() {
  // hak03 given variant 3 (its neck collapses in slim mode; the table SHA-1s are its own, so the core is what
  // refuses). Wrong versions: a refusal that drops the box (hak08 kept), a line without the core's text, a refused
  // build installed, the core run in full mode (which never refuses)
  auto in = DefaultInputs();
  in[1].tpr = fx::Variant(3).tpr;
  in[1].cat = fx::Variant(3).cat;
  const Direct d = DirectSlim("hak03", in[1].tpr, in[1].cat);
  CHECKF(!d.ok && d.error.rfind("hak03: ", 0) == 0 && d.error.find("none after") != std::string::npos,
         "direct: ok %d, '%s'", d.ok, d.error.c_str());
  Table t = TableOf(in);
  Archive a = ArchiveOf(in);
  const xc::Built b = xc::Build(true, true, a.Fn(), t.span());
  CHECK(Entries(b) == (std::vector<uint32_t>{16, 17, 56, 57}));
  CHECKF(LogAt(b, 1) == KeptLine("hak03", d.error), "%s", LogAt(b, 1).c_str());
  CHECKF(LogAt(b, 2) == BuiltLine("hak08", Directs()[2].out), "%s", LogAt(b, 2).c_str());
}

static void TestOverLimitRefused() {
  // A1: limit = size - 1 refused, = size built, = size + 1 built; for the tpr and for the cat. Wrong versions: `>=`
  // (the middle case refused), `> limit + 1` (the first built), `> limit - 1` (the middle refused), the cat compared
  // with the tpr limit or the tpr size (the cat middle case refused: the tpr is far larger), the limit read from
  // another row
  const auto& d = Directs();
  if (d.size() != 3) return CHECK(false);
  const size_t ts = d[1].out.tpr.size(), cs = d[1].out.cat.size();
  CHECK(ts > cs + 2);  // precondition: the cross-wired mutants see a different size
  for (int which = 0; which < 2; ++which) {
    for (int delta = -1; delta <= 1; ++delta) {
      auto in = DefaultInputs();
      const size_t size = which == 0 ? ts : cs;
      const uint32_t limit = static_cast<uint32_t>(static_cast<int64_t>(size) + delta);
      (which == 0 ? in[1].tpr_limit : in[1].cat_limit) = limit;
      Table t = TableOf(in);
      Archive a = ArchiveOf(in);
      const xc::Built b = xc::Build(true, true, a.Fn(), t.span());
      const char* file = which == 0 ? "tpr" : "cat";
      if (delta < 0) {
        CHECKF(Entries(b) == (std::vector<uint32_t>{16, 17, 56, 57}), "%s limit size-1: hak03 refused, others built", file);
        const std::string reason = std::string("the built hak03.") + file + " is " + std::to_string(size) +
                                   " bytes, over its limit of " + std::to_string(limit);
        CHECKF(LogAt(b, 1) == KeptLine("hak03", reason), "%s limit size-1: %s", file, LogAt(b, 1).c_str());
      } else {
        CHECKF(Entries(b) == (std::vector<uint32_t>{16, 17, 46, 47, 56, 57}), "%s limit size%+d: built", file, delta);
        CHECKF(LogAt(b, 1) == BuiltLine("hak03", d[1].out), "%s limit size%+d: %s", file, delta, LogAt(b, 1).c_str());
      }
    }
  }
}

static void TestUnreadableArchive() {
  // callback false for every entry: nothing built, one line each naming the entry, sha1 empty, no crash. Wrong
  // versions: the cat entry's failure reported as the tpr entry, a box dropped at its first failure
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a;  // no entries: every read returns false
  const xc::Built b = xc::Build(true, true, a.Fn(), t.span());
  CHECK(b.chrtwo.empty() && b.sha1.empty() && b.boxes_at_start == 3);
  CHECKF(b.log.size() == 4, "%zu lines", b.log.size());
  CHECKF(LogAt(b, 0) == KeptLine("hel03", "could not read chrtwo.afs entry 16"), "%s", LogAt(b, 0).c_str());
  CHECKF(LogAt(b, 1) == KeptLine("hak03", "could not read chrtwo.afs entry 46"), "%s", LogAt(b, 1).c_str());
  CHECKF(LogAt(b, 2) == KeptLine("hak08", "could not read chrtwo.afs entry 56"), "%s", LogAt(b, 2).c_str());
  CHECKF(ClosingLine(LogAt(b, 3), 0, 3), "%s", LogAt(b, 3).c_str());
  // only hak03's cat unreadable: the line names entry 47, hak08 still built
  Archive c = ArchiveOf(in);
  c.fail = {47};
  const xc::Built bc = xc::Build(true, true, c.Fn(), t.span());
  CHECK(Entries(bc) == (std::vector<uint32_t>{16, 17, 56, 57}));
  CHECKF(LogAt(bc, 1) == KeptLine("hak03", "could not read chrtwo.afs entry 47"), "%s", LogAt(bc, 1).c_str());
}

static void TestReadCallbackThrows() {
  // A5 M1: a callback that throws a std::exception (entry 46) and one that throws a non-standard value (entry 57):
  // those two kept with the read line, hel03 built, Build returns. Wrong versions: no per-costume catch (Build throws),
  // a catch around the whole loop (hel03's line or the closing line lost, or hak08 never tried)
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a = ArchiveOf(in);
  a.throw_std = {46};
  a.throw_other = {57};
  bool threw = false;
  xc::Built b;
  try {
    b = xc::Build(true, true, a.Fn(), t.span());
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK(Entries(b) == (std::vector<uint32_t>{16, 17}));
  CHECK(a.reads == (std::vector<uint32_t>{16, 17, 46, 56, 57}));
  CHECKF(LogAt(b, 0) == BuiltLine("hel03", Directs()[0].out), "%s", LogAt(b, 0).c_str());
  // F5: a std::exception's text follows the entry; a non-standard throw adds nothing
  CHECKF(LogAt(b, 1) == KeptLine("hak03", "could not read chrtwo.afs entry 46: archive gone"), "%s", LogAt(b, 1).c_str());
  CHECKF(LogAt(b, 2) == KeptLine("hak08", "could not read chrtwo.afs entry 57"), "%s", LogAt(b, 2).c_str());
  CHECKF(b.log.size() == 4 && ClosingLine(LogAt(b, 3), 1, 3), "%s", LogAt(b, 3).c_str());
}

static void TestSha1OfBuiltIsStableAndOrdered() {
  // the expected values: the own SHA-1 over the direct core outputs, concatenated tpr_1 cat_1 tpr_2 ... Wrong
  // versions: over the selected (not the built) costumes, in reverse table order, over the tpr files only, cat before tpr
  const auto& d = Directs();
  if (d.size() != 3) return CHECK(false);
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a1 = ArchiveOf(in), a2 = ArchiveOf(in);
  const xc::Built b1 = xc::Build(true, true, a1.Fn(), t.span());
  const xc::Built b2 = xc::Build(true, true, a2.Fn(), t.span());
  const std::string all =
      Sha1(Concat({&d[0].out.tpr, &d[0].out.cat, &d[1].out.tpr, &d[1].out.cat, &d[2].out.tpr, &d[2].out.cat}));
  CHECKF(b1.sha1 == all, "%s want %s", b1.sha1.c_str(), all.c_str());
  CHECK(b1.sha1 == b2.sha1 && !b1.sha1.empty());
  // one costume drops out (hak03's tpr flipped): the hash of hel03 and hak08 only
  Archive a3 = ArchiveOf(in);
  a3.entries[46][100] ^= 0x10;
  const xc::Built b3 = xc::Build(true, true, a3.Fn(), t.span());
  const std::string two = Sha1(Concat({&d[0].out.tpr, &d[0].out.cat, &d[2].out.tpr, &d[2].out.cat}));
  CHECKF(b3.sha1 == two && b3.sha1 != all, "%s want %s", b3.sha1.c_str(), two.c_str());
  // the opera alone
  Archive a4 = ArchiveOf(in);
  const xc::Built b4 = xc::Build(true, false, a4.Fn(), t.span());
  CHECK(b4.sha1 == Sha1(Concat({&d[0].out.tpr, &d[0].out.cat})));
  // two rows swapped (hak08 before hak03): the hash follows the table order
  std::vector<Input> sw = {in[0], in[2], in[1]};
  Table ts = TableOf(sw);
  Archive a5 = ArchiveOf(sw);
  const xc::Built b5 = xc::Build(true, true, a5.Fn(), ts.span());
  const std::string swapped =
      Sha1(Concat({&d[0].out.tpr, &d[0].out.cat, &d[2].out.tpr, &d[2].out.cat, &d[1].out.tpr, &d[1].out.cat}));
  CHECKF(b5.sha1 == swapped && b5.sha1 != all, "%s want %s", b5.sha1.c_str(), swapped.c_str());
  CHECK(Entries(b5) == (std::vector<uint32_t>{16, 17, 56, 57, 46, 47}));
}

static void TestBoxesAtStartBits() {
  // all four combinations, with an archive that builds and with one that builds nothing (the bits are the inputs,
  // never what was built). Wrong versions: bits swapped, bits from what was built
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  for (int bits = 0; bits < 4; ++bits) {
    const bool opera = bits & 1, kimonos = bits & 2;
    Archive good = ArchiveOf(in), none;
    const xc::Built bg = xc::Build(opera, kimonos, good.Fn(), t.span());
    const xc::Built bn = xc::Build(opera, kimonos, none.Fn(), t.span());
    CHECKF(bg.boxes_at_start == static_cast<unsigned>(bits) && bn.boxes_at_start == static_cast<unsigned>(bits),
           "bits %d: built %u, nothing built %u", bits, bg.boxes_at_start, bn.boxes_at_start);
    const size_t want = (opera ? 2 : 0) + (kimonos ? 4 : 0);
    CHECKF(bg.chrtwo.size() == want && bn.chrtwo.empty(), "bits %d: %zu replacements", bits, bg.chrtwo.size());
  }
}

static void TestUnknownCvarRow() {
  // R3: a row whose box is neither name is never built nor read, with one line naming it, whichever boxes are on.
  // Wrong versions: an unknown box treated as on (built), as the opera or kimono box, or read
  auto in = DefaultInputs();
  in.push_back({"doa4_christie_costume", "chr01", 70, 71, fx::Variant(1).tpr, fx::Variant(1).cat});
  Table t = TableOf(in);
  for (int bits = 0; bits < 4; ++bits) {
    Archive a = ArchiveOf(in);
    const xc::Built b = xc::Build(bits & 1, bits & 2, a.Fn(), t.span());
    CHECKF(std::count(a.reads.begin(), a.reads.end(), 70u) == 0 && std::count(a.reads.begin(), a.reads.end(), 71u) == 0,
           "bits %d: chr01 read", bits);
    const auto e = Entries(b);
    CHECKF(std::count(e.begin(), e.end(), 70u) == 0, "bits %d: chr01 built", bits);
    const std::string line = "chr01: never built: unknown box doa4_christie_costume";
    CHECKF(std::count(b.log.begin(), b.log.end(), line) == 1, "bits %d: the line", bits);
    const int sel = (bits & 1 ? 1 : 0) + (bits & 2 ? 2 : 0);
    CHECKF(!b.log.empty() && ClosingLine(b.log.back(), sel, sel), "bits %d: %s", bits,
           b.log.empty() ? "<none>" : b.log.back().c_str());
  }
}

// a tiny AFS archive (little-endian TOC): `n` entries of the given contents, laid out from 0x800, then the name table
static Bytes Afs(const std::vector<Bytes>& entries) {
  Bytes out(0x800, 0);
  const auto put32 = [&out](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  std::memcpy(out.data(), "AFS\0", 4);
  put32(4, static_cast<uint32_t>(entries.size()));
  for (size_t i = 0; i < entries.size(); ++i) {
    const size_t at = out.size();
    out.insert(out.end(), entries[i].begin(), entries[i].end());
    put32(8 + 8 * i, static_cast<uint32_t>(at));
    put32(12 + 8 * i, static_cast<uint32_t>(entries[i].size()));
    out.resize((out.size() + 0x7FF) & ~size_t(0x7FF), 0xEE);  // padding that is not zero: a read past the size shows
  }
  const size_t names = out.size();
  out.resize(names + 0x30 * entries.size(), 0);
  put32(8 + 8 * entries.size(), static_cast<uint32_t>(names));
  put32(12 + 8 * entries.size(), static_cast<uint32_t>(0x30 * entries.size()));
  return out;
}

static void TestReadEntryFrom() {
  // ReadEntryFromSource: exactly sizes[entry] bytes at offsets[entry]; false past the count and on a refused TOC; the
  // TOC is read once. ReadEntryFrom: false (never a throw) for a file that does not exist. Wrong versions: a read of
  // the padded span, of the next entry, from offset 0, a Build-time open
  const std::vector<Bytes> e = {{1, 2, 3}, Bytes(0x900, 0x42), {}, {9, 8, 7, 6, 5}};
  auto src = std::make_shared<afs::MemorySource>(Afs(e));
  xc::ReadEntry r = xc::ReadEntryFromSource(src);
  for (uint32_t i = 0; i < e.size(); ++i) {
    std::vector<uint8_t> out = {0xAA};
    const bool ok = r(i, &out);
    CHECKF(ok && out == e[i], "entry %u: ok %d, %zu bytes", i, ok, out.size());
  }
  std::vector<uint8_t> out;
  CHECK(!r(4, &out));
  CHECK(!r(0xFFFFFFFFu, &out));
  // a refused TOC (not an AFS archive), a null source
  Bytes bad = Afs(e);
  bad[0] = 'X';
  xc::ReadEntry rb = xc::ReadEntryFromSource(std::make_shared<afs::MemorySource>(bad));
  std::string toc_what = "<no throw>";
  try {
    (void)rb(0, &out);
  } catch (const std::runtime_error& e) {
    toc_what = e.what();
  }
  CHECKF(toc_what == "not an AFS archive", "%s", toc_what.c_str());
  xc::ReadEntry rn = xc::ReadEntryFromSource(nullptr);
  CHECK(!rn(0, &out));
  // a path that does not exist: false, no throw
  bool threw = false, ok = true;
  try {
    xc::ReadEntry rf = xc::ReadEntryFrom(fs::path("Z:/no/such/folder/chrtwo.afs"));
    ok = rf(0, &out);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw && !ok);
}

// a Source that counts its reads (offset 0 is ReadToc's header read: the archive's first entry starts at 0x800)
struct CountingSource final : afs::Source {
  std::shared_ptr<const afs::Source> inner;
  mutable int reads = 0, reads_at_0 = 0;
  uint64_t size() const override { return inner->size(); }
  bool Read(uint64_t offset, void* dst, size_t n) const override {
    ++reads;
    if (offset == 0) ++reads_at_0;
    return inner->Read(offset, dst, n);
  }
};

static void TestReaderPromises() {
  // F4: ReadEntryFrom opens on the first call only (wrong version: an open at construction), and the TOC is read once
  // (wrong version: ReadToc on every call); counted through the opener seam, no file written
  const std::vector<Bytes> e = {{1, 2, 3}, {4, 5}, {6}};
  auto counting = std::make_shared<CountingSource>();
  counting->inner = std::make_shared<afs::MemorySource>(Afs(e));
  int opens = 0;
  fs::path seen;
  const xc::Opener opener = [&](const fs::path& p) -> std::shared_ptr<const afs::Source> {
    ++opens;
    seen = p;
    return counting;
  };
  {
    const auto in = DefaultInputs();
    Table t = TableOf(in);
    const xc::ReadEntry r = xc::ReadEntryFrom(fs::path("X:/game/chrtwo.afs"), opener);
    const xc::Built b = xc::Build(false, false, r, t.span());
    const xc::Built bg = xc::Build(false, false, r);
    CHECKF(opens == 0 && b.chrtwo.empty() && bg.chrtwo.empty(), "boxes off: %d opens", opens);
  }
  const xc::ReadEntry r = xc::ReadEntryFrom(fs::path("X:/game/chrtwo.afs"), opener);
  CHECKF(opens == 0, "%d opens before the first read", opens);
  std::vector<uint8_t> o0, o2, o1;
  const bool a0 = r(0, &o0), a2 = r(2, &o2), a1 = r(1, &o1);
  CHECKF(opens == 1 && seen == fs::path("X:/game/chrtwo.afs"), "%d opens", opens);
  CHECK(a0 && a2 && a1 && o0 == e[0] && o2 == e[2] && o1 == e[1]);
  // ReadToc reads the header (offset 0) and the table (offset 8): 2 reads, then one per entry
  CHECKF(counting->reads_at_0 == 1 && counting->reads == 5, "%d reads at 0, %d reads", counting->reads_at_0,
         counting->reads);
}

static void TestTocRefusalReason() {
  // F5: a refused TOC's reason reaches the costume's line; a missing file gives the bare line. Wrong version: the
  // reason dropped (the bare line for both)
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Bytes bad = Afs({{1, 2, 3}});
  bad[0] = 'X';
  const xc::Built b = xc::Build(true, false, xc::ReadEntryFromSource(std::make_shared<afs::MemorySource>(bad)), t.span());
  CHECKF(LogAt(b, 0) == KeptLine("hel03", "could not read chrtwo.afs entry 16: not an AFS archive"), "%s",
         LogAt(b, 0).c_str());
  CHECK(b.chrtwo.empty() && b.sha1.empty());
  const xc::Built bm = xc::Build(true, false, xc::ReadEntryFrom(fs::path("Z:/no/such/folder/chrtwo.afs")), t.span());
  CHECKF(LogAt(bm, 0) == KeptLine("hel03", "could not read chrtwo.afs entry 16"), "%s", LogAt(bm, 0).c_str());
  // M2: a REFUSED TOC is read once too (wrong version: tried only when the TOC was accepted, so the header is read
  // again on every call), and every call gives the same reason
  auto counting = std::make_shared<CountingSource>();
  counting->inner = std::make_shared<afs::MemorySource>(bad);
  const xc::ReadEntry rc = xc::ReadEntryFromSource(counting);
  std::string what[2] = {"<no throw>", "<no throw>"};
  for (int i = 0; i < 2; ++i) {
    std::vector<uint8_t> out;
    try {
      (void)rc(0, &out);
    } catch (const std::runtime_error& e) {
      what[i] = e.what();
    }
  }
  CHECKF(counting->reads_at_0 == 1, "%d header reads over two calls", counting->reads_at_0);
  CHECKF(what[0] == "not an AFS archive" && what[1] == what[0], "%s / %s", what[0].c_str(), what[1].c_str());
}

// The folder the runner writes its output to (the bat's %OUT%, or DOA4_SUITCONV_OUT, or a probe's row folder): the
// test executable's own folder, from argv[0]. Never %TEMP%.
static fs::path g_out_dir;

static void TestDefaultOpener() {
  // M1: ReadEntryFrom with NO opener reads a real file through the production default (afs::FileSource::Open). Wrong
  // version: the default opener gives no source (every read false). The file is written here and removed after.
  if (g_out_dir.empty() || !fs::is_directory(g_out_dir)) return CHECKF(false, "no output folder from argv[0]");
  const fs::path p = g_out_dir / "extras_default_opener.afs";
  const std::vector<Bytes> e = {{1, 2, 3}, Bytes(0x900, 0x42), {9, 8, 7, 6, 5}};
  const Bytes file = Afs(e);
  {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!f) return CHECKF(false, "could not write %s", p.string().c_str());
  }
  {
    const xc::ReadEntry r = xc::ReadEntryFrom(p);
    for (uint32_t i : {2u, 0u, 1u}) {
      std::vector<uint8_t> out = {0xAA};
      bool ok = false;
      try {
        ok = r(i, &out);
      } catch (...) {
      }
      CHECKF(ok && out == e[i], "entry %u: ok %d, %zu bytes", i, ok, out.size());
    }
  }  // the reader (and the file handle) is gone before the remove
  std::error_code ec;
  fs::remove(p, ec);
  CHECKF(!fs::exists(p), "%s left behind", p.string().c_str());
}

static void TestHashFailureInstallsNothing() {
  // I1 route (b): the hash is not a 40-hex digest (the CNG provider missing gives ""), with no throw: nothing is
  // installed, sha1 is "", the closing line says why, the "built" lines stay. Wrong versions: no rollback (copies
  // installed with sha1 ""), the rollback keyed on a throw (never runs here), the check `sha1.empty()` (a short or
  // non-hex answer installs). Expected values from the rule and the direct core runs, never from Build's output.
  const auto& d = Directs();
  if (d.size() != 3) return CHECK(false);
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a = ArchiveOf(in);
  const xc::ReadEntry fn = a.Fn();
  const char* names[3] = {"hel03", "hak03", "hak08"};
  const std::string good = "0123456789abcdef0123456789abcdef01234567";
  for (const std::string& answer : {std::string(), std::string("abc"), std::string(40, 'z'),
                                    std::string("0123456789ABCDEF0123456789ABCDEF01234567"), good + "0"}) {
    int calls = 0;
    const xc::Built b = xc::Build(true, true, fn, t.span(), [&](const std::vector<afs::Replacement>& r) {
      ++calls;
      return r.size() == 6 ? answer : std::string("<wrong set>");
    });
    bool lines = b.log.size() == 4;
    for (int i = 0; lines && i < 3; ++i) lines = b.log[i] == BuiltLine(names[i], d[i].out);
    CHECKF(b.chrtwo.empty() && b.sha1.empty() && calls == 1, "answer '%s': %zu replacements, sha1 '%s', %d calls",
           answer.c_str(), b.chrtwo.size(), b.sha1.c_str(), calls);
    CHECKF(lines && ClosingLine(LogAt(b, 3), 0, 3, true), "answer '%s': %s", answer.c_str(), LogAt(b, 3).c_str());
  }
  // positive control: a well-formed answer is the sha1 and everything is installed (the seam is the one used)
  {
    int calls = 0;
    const xc::Built b = xc::Build(true, true, fn, t.span(), [&](const std::vector<afs::Replacement>&) {
      ++calls;
      return good;
    });
    CHECKF(b.chrtwo.size() == 6 && b.sha1 == good && calls == 1 && ClosingLine(LogAt(b, 3), 3, 3), "%zu, %s, %s",
           b.chrtwo.size(), b.sha1.c_str(), LogAt(b, 3).c_str());
  }
  // route (a) without an allocator: the hasher throws twice (nothing installed), or once (the retry's real digest)
  for (int throws : {2, 1}) {
    int calls = 0;
    const xc::Built b = xc::Build(true, true, fn, t.span(), [&](const std::vector<afs::Replacement>&) -> std::string {
      if (++calls <= throws) throw std::runtime_error("hash failed");
      return good;
    });
    if (throws == 2)
      CHECKF(b.chrtwo.empty() && b.sha1.empty() && calls == 2 && ClosingLine(LogAt(b, 3), 0, 3, true), "%zu, %d, %s",
             b.chrtwo.size(), calls, LogAt(b, 3).c_str());
    else
      CHECKF(b.chrtwo.size() == 6 && b.sha1 == good && calls == 2 && ClosingLine(LogAt(b, 3), 3, 3), "%zu, %d, %s",
             b.chrtwo.size(), calls, LogAt(b, 3).c_str());
  }
  // nothing built: the hasher is not asked, and the closing line is the plain one
  {
    int calls = 0;
    const xc::Built b = xc::Build(false, false, fn, t.span(), [&](const std::vector<afs::Replacement>&) {
      ++calls;
      return std::string();
    });
    CHECKF(calls == 0 && b.chrtwo.empty() && b.sha1.empty() && b.log.size() == 1 && ClosingLine(LogAt(b, 0), 0, 0),
           "%d calls, %s", calls, LogAt(b, 0).c_str());
  }
}

static void AllocationSweep(bool sticky) {
  // Fix wave 2 I1: run in two modes. Single-shot (one failure at K) as below; sticky (every allocation from K on fails:
  // reserve failing twice, sha1 failing twice, the closing line failing twice). Both modes, every sampled K:
  // sha1.empty() == chrtwo.empty(), sha1 is the hash of the installed set (40 hex when not empty), and a closing line,
  // when there is one, counts chrtwo.size() / 2. In sticky mode a missing closing line is allowed (a log line only),
  // and the "built" lines may outnumber the pairs only when nothing is installed (the copies could not be hashed).
  // F3 (the reviewer's P3): one std::bad_alloc at the K-th allocation of a Build(true, true) on the test table. For
  // every sampled K: Build does not throw; sha1 is the hash of the installed bytes in order ("" when none); the
  // "built" lines are chrtwo.size() / 2; the closing line is there with those counts. Wrong versions: the hash taken
  // before the commit (sha1 holds a kept costume's bytes), a commit that is not rolled back (a pair without its line),
  // no per-costume catch (Build throws)
  const auto& d = Directs();
  if (d.size() != 3) return CHECK(false);
  const auto in = DefaultInputs();
  Table t = TableOf(in);
  Archive a = ArchiveOf(in);
  a.reads.reserve(64);  // so the fake's own bookkeeping allocates nothing while armed
  std::vector<long long> at_read;
  at_read.reserve(64);
  a.on_read = [&](uint32_t) { at_read.push_back(g_alloc_count.load()); };
  const xc::ReadEntry fn = a.Fn();
  // expected sha1 per installed subset (bit i = costume i), the own SHA-1
  std::map<int, std::string> want;
  for (int s = 1; s < 8; ++s) {
    Bytes cat;
    for (int i = 0; i < 3; ++i)
      if (s & (1 << i)) cat = Concat({&cat, &d[i].out.tpr, &d[i].out.cat});
    want[s] = Sha1(cat);
  }
  want[0] = "";
  // the clean run: the count and the costume boundaries
  g_alloc_count = 0;
  g_alloc_fail_at = -1;
  g_alloc_fired = false;
  g_alloc_armed = true;
  const auto t0 = std::chrono::steady_clock::now();
  const xc::Built clean = xc::Build(true, true, fn, t.span());
  const double clean_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  g_alloc_armed = false;
  a.on_read = nullptr;
  const long long total = g_alloc_count;
  CHECKF(clean.chrtwo.size() == 6 && at_read.size() == 6 && total > 100, "clean: %zu replacements, %zu reads, %lld",
         clean.chrtwo.size(), at_read.size(), total);
  if (at_read.size() != 6) return;
  const long long b2 = at_read[2], b3 = at_read[4];  // the counter when hak03's and hak08's reads begin
  // Every K would take about 45 s (about 24,000 K at about 2 ms), so: a fixed stride of 7, plus every K in the first
  // 64, near each costume's end (the 64 before and 8 after the next costume's first read: the reviewer's K = 7970 ..
  // 7975 were hel03's commit, just before hak03's first read) and in the last 200 (hak08's commit, sha1, closing line)
  // DOA4_EXTRAS_ALLOC_FULL=1: every K (stride 1; about 45 s a mode)
  const char* full_env = std::getenv("DOA4_EXTRAS_ALLOC_FULL");
  const bool full = full_env && std::string(full_env) == "1";
  const long long stride = full ? 1 : 7;
  std::vector<long long> ks;
  for (long long k = 1; k <= total; ++k)
    if (k % stride == 0 || k <= 64 || (k > b2 - 64 && k <= b2 + 8) || (k > b3 - 64 && k <= b3 + 8) || k > total - 200)
      ks.push_back(k);
  long long escaped = 0, sha_bad = 0, lines_bad = 0, closing_bad = 0, not_fired = 0, kept_runs = 0, bytes_bad = 0;
  long long first_bad = -1, inv_bad = 0, unhashed_runs = 0, no_closing = 0;
  const auto ts = std::chrono::steady_clock::now();
  for (size_t n = 0; n < ks.size(); ++n) {
    const long long k = ks[n];
    a.reads.clear();
    xc::Built b;
    bool threw = false;
    g_alloc_count = 0;
    g_alloc_fail_at = k;
    g_alloc_fired = false;
    g_alloc_sticky = sticky;
    g_alloc_armed = true;
    try {
      b = xc::Build(true, true, fn, t.span());
    } catch (...) {
      threw = true;
    }
    g_alloc_armed = false;
    g_alloc_sticky = false;
    bool bad = false;
    if (!g_alloc_fired) ++not_fired, bad = true;
    if (threw) {
      ++escaped;
      if (first_bad < 0) first_bad = k;
      continue;
    }
    // the installed subset: whole pairs, in table order
    int subset = 0;
    bool pairs = b.chrtwo.size() % 2 == 0;
    const uint32_t te[3] = {16, 46, 56};
    int last = -1;
    for (size_t i = 0; pairs && i + 1 < b.chrtwo.size(); i += 2) {
      int c = -1;
      for (int j = 0; j < 3; ++j)
        if (b.chrtwo[i].entry == te[j] && b.chrtwo[i + 1].entry == te[j] + 1) c = j;
      if (c <= last) pairs = false;
      else subset |= 1 << c, last = c;
      if (pairs && (!b.chrtwo[i].data || b.chrtwo[i].data->size() != d[c].out.tpr.size() || !b.chrtwo[i + 1].data ||
                    b.chrtwo[i + 1].data->size() != d[c].out.cat.size()))
        pairs = false;
    }
    if (!pairs || b.sha1 != want[subset]) ++sha_bad, bad = true;
    if (n % 50 == 0 && pairs) {  // spot check: the installed bytes themselves
      Bytes all;
      for (const auto& r : b.chrtwo) {
        const Bytes x = ReadBack(r);
        all.insert(all.end(), x.begin(), x.end());
      }
      if ((all.empty() ? std::string() : Sha1(all)) != b.sha1) ++bytes_bad, bad = true;
    }
    size_t built_lines = 0;
    for (const auto& l : b.log)
      if (l.find(": built (") != std::string::npos) ++built_lines;
    const bool hex40 = b.sha1.size() == 40 && std::all_of(b.sha1.begin(), b.sha1.end(), [](char c) {
                         return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                       });
    if (b.sha1.empty() != b.chrtwo.empty() || (!b.sha1.empty() && !hex40)) ++inv_bad, bad = true;
    const int pairs_n = static_cast<int>(b.chrtwo.size() / 2);
    const bool unhashed_line = !b.log.empty() && ClosingLine(b.log.back(), 0, 3, true);
    const bool plain_line = !b.log.empty() && ClosingLine(b.log.back(), pairs_n, 3);
    const bool any_closing = !b.log.empty() && b.log.back().rfind("extras copies: ", 0) == 0;
    if (unhashed_line || (sticky && b.chrtwo.empty() && built_lines > 0)) ++unhashed_runs;
    if (sticky ? (built_lines * 2 != b.chrtwo.size() && !b.chrtwo.empty())
               : (built_lines * 2 != b.chrtwo.size()))
      ++lines_bad, bad = true;
    if (!any_closing) ++no_closing;
    if (sticky ? (any_closing && !plain_line && !(unhashed_line && b.chrtwo.empty())) : !plain_line)
      ++closing_bad, bad = true;
    if (b.chrtwo.size() < 6) ++kept_runs;
    if (bad && first_bad < 0) first_bad = k;
  }
  const double sweep_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
  std::printf("allocation_failures (%s): %lld allocations in a clean Build (%.2f ms; hak03 read at %lld, hak08 at %lld); %s; "
              "%zu K run in %.1f s; %lld kept a costume; escaped %lld, sha1 %lld, bytes %lld, lines %lld, closing %lld, "
              "not fired %lld; invariant %lld; unhashed %lld, no closing line %lld; first bad K %lld\n",
              sticky ? "sticky" : "single-shot", total, clean_ms, b2, b3, full ? "every K" : ("stride " + std::to_string(stride) + " plus windows").c_str(),
              ks.size(), sweep_s, kept_runs, escaped, sha_bad, bytes_bad, lines_bad, closing_bad, not_fired, inv_bad,
              unhashed_runs, no_closing, first_bad);
  CHECKF(inv_bad == 0, "%lld runs broke sha1.empty() == chrtwo.empty() or 40 hex (first bad K %lld)", inv_bad, first_bad);
  // evidence the sticky sweep reached the hash failing twice (copies built, none installed); single-shot never does
  if (sticky) CHECKF(unhashed_runs > 0 && no_closing > 0, "%lld unhashed, %lld without a closing line", unhashed_runs,
                     no_closing);
  else CHECKF(unhashed_runs == 0, "%lld unhashed runs in single-shot mode", unhashed_runs);
  CHECKF(escaped == 0, "%lld runs threw (first bad K %lld)", escaped, first_bad);
  CHECKF(sha_bad == 0 && bytes_bad == 0, "%lld sha1, %lld bytes (first bad K %lld)", sha_bad, bytes_bad, first_bad);
  CHECKF(lines_bad == 0, "%lld runs (first bad K %lld)", lines_bad, first_bad);
  CHECKF(closing_bad == 0, "%lld runs (first bad K %lld)", closing_bad, first_bad);
  CHECKF(not_fired == 0, "%lld runs never reached K", not_fired);
  // evidence the injection acted: some K kept a costume, and the windows were sampled
  CHECKF(kept_runs > 0 && !ks.empty() && ks.size() >= std::min<long long>(total, 200), "%lld kept, %zu K", kept_runs,
         ks.size());
}
static void TestAllocationFailures() { AllocationSweep(false); }
static void TestAllocationFailuresSticky() { AllocationSweep(true); }

// ---------------------------------------------------------------- the gated real-archive check (A5 M3)
static void RunRealArchive() {
  const char* dir = std::getenv("DOA4_EXTRAS_GAME_DIR");
  if (!dir || !*dir) {
    std::printf("extras real archive: SKIPPED (DOA4_EXTRAS_GAME_DIR unset)\n");
    return;
  }
  const int before = g_checks, fb = g_failures;
  const char* flip_env = std::getenv("DOA4_EXTRAS_TEST_FLIP");
  const bool flip = flip_env && std::strcmp(flip_env, "1") == 0;
  xc::ReadEntry base = xc::ReadEntryFrom(fs::path(dir) / "chrtwo.afs");
  xc::ReadEntry read = [&](uint32_t entry, std::vector<uint8_t>* out) {
    const bool ok = base(entry, out);
    if (ok && flip && entry == 46 && !out->empty()) (*out)[out->size() / 2] ^= 0x01;
    return ok;
  };
  const xc::Built b = xc::Build(true, true, read);
  for (const std::string& l : b.log) std::printf("extras real archive: log: %s\n", l.c_str());
  struct Want {
    const char* name;
    uint32_t tpr_entry, cat_entry;
    const char* tpr_built;
    size_t tpr_size;
    const char* cat_retail;
    size_t cat_size;
  };
  // g0_sha1.txt's "built" lines (the Python tool's output) and the retail cat files
  const Want want[3] = {
      {"hel03", 16, 17, "00ecdbd3b76ae88d8dd3877248554141f00fe28b", 3198976, "6a7135bff35e25da97a47b77f83d7f26ddcc9a54", 682928},
      {"hak03", 46, 47, "c7a9bfa560209c332c35147517445f4006fac0c9", 7806976, "6b91dc27df6b8190b92cbed6c08a822b1e0300b2",
       1722896},
      {"hak08", 56, 57, "f37f2837f76a738b4b1de3ddf6010b39bf4a9d7c", 7819264, "de0335bd97423465f078421bab0f489642e195ed",
       1722768}};
  std::map<uint32_t, const afs::Replacement*> by_entry;
  for (const auto& r : b.chrtwo) by_entry[r.entry] = &r;
  for (const Want& w : want) {
    const auto t = by_entry.find(w.tpr_entry), c = by_entry.find(w.cat_entry);
    const bool have = t != by_entry.end() && c != by_entry.end();
    CHECKF(have, "%s: built", w.name);
    if (!have) continue;
    const Bytes tb = ReadBack(*t->second), cb = ReadBack(*c->second);
    const std::string ts = doa4::gamefiles::Sha1Hex(tb.data(), tb.size());
    const std::string cs = doa4::gamefiles::Sha1Hex(cb.data(), cb.size());
    std::printf("extras real archive: %s tpr %zu bytes sha1 %s (want %s), cat %zu bytes sha1 %s (want %s)\n", w.name,
                tb.size(), ts.c_str(), w.tpr_built, cb.size(), cs.c_str(), w.cat_retail);
    CHECKF(ts == w.tpr_built && tb.size() == w.tpr_size, "%s: built tpr", w.name);
    CHECKF(cs == w.cat_retail && cb.size() == w.cat_size, "%s: cat", w.name);
  }
  CHECKF(Entries(b) == (std::vector<uint32_t>{16, 17, 46, 47, 56, 57}), "%zu replacements", b.chrtwo.size());
  CHECKF(b.log.size() == 4 && ClosingLine(LogAt(b, 3), 3, 3), "%s", LogAt(b, 3).c_str());
  // F7: Built::sha1 against the own FIPS SHA-1 over the installed bytes, tpr_1 cat_1 tpr_2 cat_2 tpr_3 cat_3
  Bytes all;
  for (const auto& r : b.chrtwo) {
    const Bytes x = ReadBack(r);
    all.insert(all.end(), x.begin(), x.end());
  }
  const std::string own = b.chrtwo.empty() ? std::string() : Sha1(all);
  std::printf("extras real archive: Built::sha1 %s (own SHA-1 over the %zu installed bytes: %s)\n",
              b.sha1.empty() ? "<empty>" : b.sha1.c_str(), all.size(), own.empty() ? "<empty>" : own.c_str());
  CHECKF(!b.sha1.empty() && b.sha1 == own, "Built::sha1 %s", b.sha1.c_str());
  std::printf("extras real archive: %d checks, %d failures%s\n", g_checks - before, g_failures - fb,
              flip ? " (entry 46 flipped)" : "");
}

int main(int argc, char** argv) {
  if (argc > 0 && argv[0]) g_out_dir = fs::absolute(fs::path(argv[0])).parent_path();
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  RunGroup(TestFixtureAnchor, "fixture_anchor");
  RunGroup(TestCostumesTable, "costumes_table");
  RunGroup(TestBoxesOffReadsNothing, "boxes_off_reads_nothing");
  RunGroup(TestOperaOnlyReadsTwoEntries, "opera_only_reads_two_entries");
  RunGroup(TestBuiltReplacesBothEntries, "built_replaces_both_entries");
  RunGroup(TestHashMismatchKeepsStandin, "hash_mismatch_keeps_standin");
  RunGroup(TestCoreRefusalKeepsStandin, "core_refusal_keeps_standin");
  RunGroup(TestOverLimitRefused, "over_limit_refused");
  RunGroup(TestUnreadableArchive, "unreadable_archive");
  RunGroup(TestReadCallbackThrows, "read_callback_throws");
  RunGroup(TestSha1OfBuiltIsStableAndOrdered, "sha1_of_built_is_stable_and_ordered");
  RunGroup(TestBoxesAtStartBits, "boxes_at_start_bits");
  RunGroup(TestUnknownCvarRow, "unknown_cvar_row");
  RunGroup(TestReadEntryFrom, "read_entry_from");
  RunGroup(TestReaderPromises, "reader_promises");
  RunGroup(TestTocRefusalReason, "toc_refusal_reason");
  RunGroup(TestAllocationFailures, "allocation_failures");
  RunGroup(TestAllocationFailuresSticky, "allocation_failures_sticky");
  RunGroup(TestDefaultOpener, "default_opener");
  RunGroup(TestHashFailureInstallsNothing, "hash_failure_installs_nothing");
  RunRealArchive();
  if (g_failures) {
    std::printf("FAILED: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }
  std::printf("PASSED: %d checks, 0 failures\n", g_checks);
  return 0;
}
