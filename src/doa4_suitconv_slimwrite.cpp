// Suit slim copies, Task 6: the copy writers and the assembly (see doa4_suitconv_slim.h). Each function names the Python
// function it ports (slim_addon.py: bounds via core::Bounds, _node_verts, _write_lod, _write_shadow, _ref_count,
// _new_map, add_copies; lod_chain.py: _record; convert.py: main_chain's orphans; tpr.py: Decl, ObjGeo, ObjInfo,
// mesh_patch; found by name in doax2_work/physics_spike/scripts, the line numbers move). Every SlimError text is built
// here exactly as the Python f-string builds it ({kind:#04x} as 0x02 / 0x04, ints in decimal, a list as Python's repr);
// any other Python raise is a ConvertError naming the exception type.

#include "doa4_suitconv_fp.h"

#include "doa4_suitconv_slim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <new>
#include <set>
#include <tuple>

namespace doa4::suitconv::slim {

using be::Reader;
using be::Writer;
using tpr::Al;
using tpr::Fourcc;
using tpr::MakeFourcc;

namespace {

constexpr Fourcc kVbuf = MakeFourcc("VBUF");
constexpr Fourcc kIbuf = MakeFourcc("IBUF");
constexpr uint32_t kF3 = 0x2A23B9;  // tpr.DT_BY_NAME['F3']
constexpr uint32_t kF2 = 0x2C23A5;  // tpr.DT_BY_NAME['F2']
constexpr uint32_t kRestart = 0xFFFF;

// convert.py DOA4_VB_HEAD / DOA4_IB_HEAD (bytes.fromhex('0001000100000000ffff0000') / ('0002000100000000ffff0000')),
// copied here (core.cpp's kDoa4VbHead / kDoa4IbHead have internal linkage; core is not edited)
constexpr uint8_t kVbHead[12] = {0, 1, 0, 1, 0, 0, 0, 0, 0xff, 0xff, 0, 0};
constexpr uint8_t kIbHead[12] = {0, 2, 0, 1, 0, 0, 0, 0, 0xff, 0xff, 0, 0};

[[noreturn]] void Fail(const std::string& what) { throw ConvertError(what); }

std::span<const uint8_t> S(const Bytes& b) { return std::span<const uint8_t>(b.data(), b.size()); }

std::string Dec(int64_t v) { return std::to_string(v); }
std::string Kind04(int kind) {  // f'{kind:#04x}'
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%02x", kind);
  return buf;
}
std::string Hex(int64_t v) {  // hex(v) for a non-negative int
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

// Python's list[i] for a non-negative index (IndexError otherwise)
template <class T>
const T& At(const std::vector<T>& v, int64_t i) {
  if (i < 0 || i >= static_cast<int64_t>(v.size())) Fail("IndexError: list index out of range");
  return v[static_cast<size_t>(i)];
}

void PutF32(Writer& w, double v) { w.F32(v); }  // struct.pack('>f', v)

// tpr.ObjGeo(struct.pack('>IIII', 0x00020000, s, 0, 0), ...)'s pre
Bytes GeoPre(int64_t s) {
  Writer w;
  w.U32(0x00020000);
  w.U32(be::CheckU32S(s));
  w.U32(0);
  w.U32(0);
  return w.Take();
}

// struct.pack('>8I', ...)
Bytes Pack8(std::array<uint64_t, 8> v) {
  Writer w;
  for (uint64_t x : v) w.U32(x);
  return w.Take();
}

// struct.pack(f'>{n}H', *ib): every value is a vertex index below 0xFFFF or the restart 0xFFFF
Bytes PackIndices(const std::vector<uint32_t>& ib) {
  Writer w;
  for (uint32_t v : ib) w.U16(v);
  return w.Take();
}

// lod_chain._record(src, tmpl, tex, index, istart, icount)
Bytes Record(std::span<const uint8_t> src, std::span<const uint8_t> tmpl, uint32_t tex, uint64_t index, uint64_t istart,
             uint64_t icount) {
  const auto [base, nst] = tpr::MeshStageBase(src);
  (void)nst;
  if (!(base >= 0xC0)) Fail("AssertionError: ('mesh header shorter than 0xC0', " + Dec(base) + ")");
  Bytes hdr(src.begin(), src.begin() + 0xC0);
  const std::pair<int64_t, uint64_t> fields[] = {{0x00, index}, {0x08, 0},          {0x10, istart}, {0x14, icount},
                                                 {0x18, 2},     {0x1C, 0xC0},       {0x24, 0},      {0x30, 0x20},
                                                 {0x3C, 0xFFFFFFFFu}, {0x60, 0},    {0x64, 0}};
  for (const auto& [off, val] : fields) be::PackIntoU32(hdr, off, val);
  const auto st = Reader(tmpl).Slice(0xC0, 0x140);  // tmpl[0xC0:0x140]
  Bytes stb(st.begin(), st.end());
  be::PackIntoU32(stb, 8, tex);
  hdr.insert(hdr.end(), stb.begin(), stb.end());
  return hdr;
}

// One built copy object: (ObjGeo, ObjInfo, vb, ib) of _write_lod / _write_shadow
struct Built {
  tpr::ObjGeo geo;
  tpr::ObjInfo info;
  Bytes vb, ib;
};

// _ref_count(ref, ids): the reference's vertex count (decl head word 5) over the group's objects
int64_t RefCount(const core::Model& ref, const std::vector<int64_t>& ids) {
  int64_t n = 0;
  for (int64_t i : ids)
    for (const auto& dc : At(ref.geo, i).decls) n += dc.head[5];
  return n;
}

// The merged keys whose scan key names copy node s, in merged (first-traversal) order
std::vector<uint32_t> KeysOf(const Merged& merged, const Scan& sc, int64_t s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < merged.size(); ++i)
    if (At(sc.keys, merged[i].key).node == s) out.push_back(static_cast<uint32_t>(i));
  return out;
}

// slim_addon._write_lod: the group 0x02 objects in the shapes lod_chain.append writes
std::vector<Built> WriteLod(const Suit& suit, const core::Model& ref, const Layout& lay, const Merged& merged,
                            const Scan& sc) {
  const CopyGroup& g = lay.g2;
  const std::vector<int64_t>& ids = g.ids;
  const Bytes& t_mesh = At(At(ref.geo, At(ids, 0)).meshes, 0);
  bool shape = t_mesh.size() == 0x140;
  if (shape) {  // [(a, b) for a, b, c in tpr.mesh_textures(t_mesh)] != [(0, 0), (1, 1)]
    const auto texs = tpr::MeshTextures(S(t_mesh));
    shape = texs.size() == 2 && texs[0].stage == 0 && texs[0].kind == 0 && texs[1].stage == 1 && texs[1].kind == 1;
  }
  if (!shape) Fail(suit.name + ": reference group-0x02 mesh not of the native 2-stage shape");
  const auto& t_words = At(ref.info, ids[0]).words;
  std::vector<int64_t> nodes;
  for (int64_t i : ids) nodes.push_back(g.new_id.at(i));
  std::vector<std::vector<uint32_t>> keys_of;
  for (int64_t s : nodes) keys_of.push_back(KeysOf(merged, sc, s));
  // any_tex: the first key in MERGED order of the first node (in nodes order) that has keys, else 0
  uint32_t any_tex = 0;
  for (const auto& ks : keys_of)
    if (!ks.empty()) {
      any_tex = sc.keys[merged[ks[0]].key].tex;
      break;
    }
  std::vector<Built> out;
  for (size_t si = 0; si < nodes.size(); ++si) {
    const int64_t s = nodes[si];
    const auto fr = lay.frame.find(s);
    if (fr == lay.frame.end()) Fail("KeyError: " + Dec(s));
    const Frame& frame = fr->second;
    // stable sort by pass: same-pass keys keep their merged order (port contract (a))
    std::vector<uint32_t> order = keys_of[si];
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      return sc.keys[merged[a].key].pass < sc.keys[merged[b].key].pass;
    });
    struct LVert {
      std::array<double, 3> p, n;
      std::array<double, 2> uv;
      uint8_t nn = 3, un = 2;  // the value counts Python's tuples hold
    };
    struct Mat {
      std::span<const uint8_t> src;
      uint32_t tex, lp, d10;
      std::vector<std::array<uint32_t, 3>> faces;
    };
    std::vector<LVert> lverts;
    std::vector<Mat> mats;
    if (order.empty()) {
      // no triangle reached this node: one degenerate triangle, the first copy texture (never drawn)
      lverts.push_back({{0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0}});
      mats.push_back({S(t_mesh), any_tex, 2, 0, {{0, 0, 0}}});
    } else {
      const NodeVerts nv = NodeVertsOf(order, merged, sc.payload);
      for (const auto& [p, pl] : nv.verts) {
        const ScanPayload& py = sc.payload[pl];
        LVert lv;
        lv.p = ToLocal(p, frame, false);
        // lod_chain._to_local on a short normal tuple: a rotated frame reads q[2] (IndexError here, before the vertex
        // limit and _record); an unrotated one returns the short tuple and the pack below refuses it (struct.error)
        if (!(frame.dev < 1e-6) && py.nrm_n < 3) Fail("IndexError: tuple index out of range");
        lv.n = ToLocal(py.nrm, frame, true);
        lv.uv = py.uv;
        lv.nn = py.nrm_n;
        lv.un = py.uv_n;
        lverts.push_back(lv);
      }
      for (size_t oi = 0; oi < order.size(); ++oi) {
        const MergedKey& mk = merged[order[oi]];
        const ScanKey& k = sc.keys[mk.key];
        Mat m{S(k.matsrc), k.tex, k.pass, k.flag, {}};
        for (const auto& f : mk.faces) m.faces.push_back({nv.remap[oi][f[0]], nv.remap[oi][f[1]], nv.remap[oi][f[2]]});
        mats.push_back(std::move(m));
      }
    }
    if (lverts.size() >= 0xFFFF) Fail(suit.name + ": copy node " + Dec(s) + " over 65534 vertices");
    std::vector<uint32_t> ib;
    std::vector<Bytes> meshes, minfos;
    uint32_t mask8 = 0, mask10 = 0;
    for (size_t n = 0; n < mats.size(); ++n) {
      const Mat& m = mats[n];
      const size_t i0 = ib.size();
      for (size_t jj = 0; jj < m.faces.size(); ++jj) {
        if (jj) ib.push_back(kRestart);
        ib.insert(ib.end(), m.faces[jj].begin(), m.faces[jj].end());
      }
      meshes.push_back(Record(m.src, S(t_mesh), m.tex, n, i0, ib.size() - i0));
      std::set<uint32_t> used_set;
      for (const auto& f : m.faces) used_set.insert(f.begin(), f.end());
      std::vector<core::Vec3> used;  // ascending vertex index (port contract (f))
      for (uint32_t v : used_set) used.push_back(lverts[v].p);
      Bytes info = Pack8({n, 1, m.lp, 0, m.d10, 0, 0, 0});
      const Bytes b = core::Bounds(used);
      info.insert(info.end(), b.begin(), b.end());
      minfos.push_back(std::move(info));
      mask8 |= m.lp;
      mask10 |= m.d10;
    }
    Writer vb;
    for (const LVert& q : lverts) {
      // struct.pack('>3f3f2f', *q[0], *q[1], *q[2]) needs 8 values (a short normal or uv: struct.error)
      if (q.nn != 3 || q.un != 2)
        Fail("struct.error: pack expected 8 items for packing (got " + Dec(3 + q.nn + q.un) + ")");
      for (double v : q.p) PutF32(vb, v);
      for (double v : q.n) PutF32(vb, v);
      for (double v : q.uv) PutF32(vb, v);
    }
    Built bt;
    tpr::Decl dcl;
    dcl.head = {0, 1, 0, be::CheckU32(ib.size()), 4, static_cast<uint32_t>(lverts.size()), 1, 0, 0, 0, 0, 0};
    dcl.elems = {tpr::DeclElem{0, 0, kF3, 0, 0, 0, 0}, tpr::DeclElem{0, 12, kF3, 0, 3, 0, 0},
                 tpr::DeclElem{0, 24, kF2, 0, 5, 0, 0}, tpr::DeclElem{0xFF, 0, 0xFFFFFFFFu, 0, 0, 0, 0}};
    {
      Writer w;
      w.U32(0), w.U32(32), w.U32(0), w.U32(0);
      dcl.tails = {w.Take()};
    }
    auto words = t_words;
    words[1] = be::CheckU32S(s);
    words[3] = 0;
    words[7] = mask8;
    words[8] = mask10;
    bt.geo.pre = GeoPre(s);
    bt.geo.decls = {dcl};
    bt.geo.meshes = std::move(meshes);
    std::vector<core::Vec3> all;
    for (const LVert& q : lverts) all.push_back(q.p);
    bt.info.words = words;
    bt.info.bounds = core::Bounds(all);
    bt.info.infos = std::move(minfos);
    bt.vb = vb.Take();
    bt.ib = PackIndices(ib);
    out.push_back(std::move(bt));
  }
  return out;
}

// slim_addon._write_shadow: the group 0x04 objects in the shapes convert.shadow_chain writes
std::vector<Built> WriteShadow(const Suit& suit, const core::Model& ref, const Layout& lay, const Merged& merged,
                               const Scan& sc) {
  const CopyGroup& g = lay.g4;
  const std::vector<int64_t>& ids = g.ids;
  const Bytes& t_mesh = At(At(ref.geo, At(ids, 0)).meshes, 0);
  const auto& t_words = At(ref.info, ids[0]).words;
  std::vector<Built> out;
  for (int64_t old : ids) {
    const int64_t s = g.new_id.at(old);
    const auto fr = lay.frame.find(s);
    if (fr == lay.frame.end()) Fail("KeyError: " + Dec(s));
    const std::array<double, 3> j = fr->second.t;
    const std::vector<uint32_t> keys = KeysOf(merged, sc, s);  // kind 4: one key per node at most
    std::vector<core::Vec3> pos;
    std::vector<uint32_t> ib;
    if (!keys.empty()) {
      const NodeVerts nv = NodeVertsOf(std::span<const uint32_t>(keys.data(), 1), merged, sc.payload);
      for (const auto& [p, pl] : nv.verts) pos.push_back({p[0] - j[0], p[1] - j[1], p[2] - j[2]});
      for (const auto& f : merged[keys[0]].faces) {
        for (uint32_t i : f) ib.push_back(nv.remap[0][i]);
        ib.push_back(kRestart);
      }
    } else {
      pos = {{0.0, 0.0, 0.0}};
      ib = {0, 0, 0, kRestart};
    }
    if (pos.size() >= 0xFFFF) Fail(suit.name + ": shadow node " + Dec(s) + " over 65534 vertices");
    ib.pop_back();  // no trailing restart
    Writer vb;
    for (const auto& q : pos)
      for (double v : q) PutF32(vb, v);
    Built bt;
    tpr::Decl dcl;
    dcl.head = {0, 1, 0, be::CheckU32(ib.size()), 2, static_cast<uint32_t>(pos.size()), 1, 0, 0, 0, 0, 0};
    dcl.elems = {tpr::DeclElem{0, 0, kF3, 0, 0, 0, 0}, tpr::DeclElem{0xFF, 0, 0xFFFFFFFFu, 0, 0, 0, 0}};
    {
      Writer w;
      w.U32(0), w.U32(12), w.U32(0), w.U32(0);
      dcl.tails = {w.Take()};
    }
    const Bytes mesh = tpr::MeshPatch(S(t_mesh), 0, 0, 0, ib.size());
    const Bytes b = core::Bounds(pos);
    auto words = t_words;
    words[1] = be::CheckU32S(s);
    words[3] = 0;
    bt.geo.pre = GeoPre(s);
    bt.geo.decls = {dcl};
    bt.geo.meshes = {mesh};
    bt.info.words = words;
    bt.info.bounds = b;
    Bytes info = Pack8({0, 1, 2, 0, 0, 0, 0, 0});
    info.insert(info.end(), b.begin(), b.end());
    bt.info.infos = {info};
    bt.vb = vb.Take();
    bt.ib = PackIndices(ib);
    out.push_back(std::move(bt));
  }
  return out;
}

// convert.main_chain(ref, [])'s orphans, as a membership test: ids 0..main_max that no root holding a main-group
// channel reaches (the main group read with ParseMap, convert.parse_t2's current grammar; never core::MainChainOf)
struct Orphans {
  int64_t main_max = -1;
  std::set<int64_t> keep;
  bool Has(int64_t i) const { return i >= 0 && i <= main_max && !keep.count(i); }
};
Orphans MainChainOrphans(const core::Model& ref, const std::vector<MapGroup>& groups) {
  std::set<int64_t> main_ids;
  for (const auto& [c, i] : groups.at(0).items) main_ids.insert(i);
  std::map<int64_t, std::set<int64_t>> subtree;  // root id -> its subtree's ids (a later root of one id wins)
  const size_t n = ref.hie.size();
  for (size_t k = 0; k < n; ++k) {
    if (ref.hie[k].parent >= 0) continue;
    std::set<int64_t> ids;
    std::vector<int64_t> stack = {static_cast<int64_t>(k)};
    size_t steps = 0;
    while (!stack.empty()) {
      const int64_t j = stack.back();
      stack.pop_back();
      if (++steps > n * n + n + 1) Fail("RecursionError: the reference's Hie children loop");  // Python: never ends
      const tpr::Node& nd = ref.Hie(j);
      ids.insert(nd.id);
      stack.insert(stack.end(), nd.children.begin(), nd.children.end());
    }
    subtree[ref.hie[k].id] = std::move(ids);
  }
  Orphans o;
  for (const auto& [r, ids] : subtree) {
    bool hit = false;
    for (int64_t i : ids) hit = hit || main_ids.count(i);
    if (hit) o.keep.insert(ids.begin(), ids.end());
  }
  if (o.keep.empty()) Fail("ValueError: max() iterable argument is empty");
  o.main_max = *o.keep.rbegin();
  return o;
}

// slim_addon._new_map (rule I1): the suit's main group, the reference's state groups whose ids are all kept and below
// the first built id, then the built groups in the reference's marker order.
std::vector<MapGroup> NewMap(const Suit& suit, const core::Model& ref, const Layout& lay) {
  const auto groups = RefGroups(ref);
  const Orphans orphans = MainChainOrphans(ref, groups);
  const int64_t first = static_cast<int64_t>(suit.nodes.size());
  std::set<int64_t> kept;
  for (const auto& nd : suit.nodes) kept.insert(nd.id);
  for (const auto& [o, nid] : lay.g2.new_id) kept.insert(nid);
  for (const auto& [o, nid] : lay.g4.new_id) kept.insert(nid);
  for (auto it = kept.begin(); it != kept.end();) it = orphans.Has(*it) ? kept.erase(it) : std::next(it);
  std::vector<MapGroup> out;
  MapGroup main;
  main.kind = MapGroup::Kind::kMain;
  main.items = suit.main;
  out.push_back(std::move(main));
  for (size_t gi = 1; gi < groups.size(); ++gi) {
    const MapGroup& g = groups[gi];
    if (g.kind != MapGroup::Kind::kState) continue;
    bool all = true;
    for (const auto& [c, i] : g.items) all = all && kept.count(i) && static_cast<int64_t>(i) < first;
    if (all) out.push_back(g);
  }
  // built groups, ordered by the position of their marker in lay.kinds (the first such position)
  std::vector<std::pair<size_t, int>> built;
  for (int k : {2, 4}) {
    size_t at = lay.kinds.size();
    for (size_t p = 0; p < lay.kinds.size(); ++p)
      if (lay.kinds[p].first == MapGroup::Kind::kMarker && lay.kinds[p].second == k) {
        at = p;
        break;
      }
    if (at == lay.kinds.size()) Fail("ValueError: " + Dec(k) + " is not in list");
    built.emplace_back(at, k);
  }
  std::stable_sort(built.begin(), built.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [at, k] : built) {
    const CopyGroup& cg = lay.Of(k);
    MapGroup bg;
    bg.kind = MapGroup::Kind::kMarker;
    bg.value = static_cast<uint8_t>(k);
    for (const auto& [c, i] : cg.rows) {
      const int64_t nid = cg.new_id.at(i);
      // convert.t2_bytes' assert on the entry (an id past one byte); the code came from the parsed map
      if (nid > 0xFF) Fail("AssertionError: ('type-2 entry out of range', '" + Hex(c) + "', " + Dec(nid) + ")");
      bg.items.emplace_back(c, static_cast<uint8_t>(nid));
    }
    out.push_back(std::move(bg));
  }
  return out;
}

// slim.choose_h(tris, target) for a Python int target (a negative one is refused as Python refuses it)
Chosen ChooseFor(std::span<const Tri> tris, int64_t target) {
  if (target >= 0) return ChooseH(tris, static_cast<size_t>(target));
  const auto org = Origin(tris);
  const size_t n_hi = Count(Merge(tris, kHHi, org));
  Fail("count " + std::to_string(n_hi) + " at H_HI = 1.0 exceeds target " + Dec(target));
}

// Python's repr of a list of ints
std::string ListRepr(const std::vector<int64_t>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + Dec(v[i]);
  return s + "]";
}

Result AddCopiesImpl(std::string_view name_in, std::span<const uint8_t> tpr_bytes, std::span<const uint8_t> cat_bytes,
                     std::span<const uint8_t> ref_tpr, std::span<const uint8_t> ref_cat, CopyMode mode,
                     const TestHooks& hooks) {
  const Suit suit = ReadSuit(name_in, tpr_bytes, cat_bytes);
  const core::Model ref = core::LoadModel(std::string(name_in) + ".ref.tpr", ref_tpr,
                                          std::make_shared<const Bytes>(ref_cat.begin(), ref_cat.end()));
  const std::string& name = suit.name;
  if (suit.geo.size() != suit.nodes.size() || suit.info.size() != suit.nodes.size())
    Fail(name + ": " + Dec(static_cast<int64_t>(suit.geo.size())) + " objects for " +
         Dec(static_cast<int64_t>(suit.nodes.size())) + " nodes");
  BindGuard(suit, ref);
  Layout lay = MakeLayout(suit, ref);
  if (hooks.layout) hooks.layout(lay);
  // the report line's min()/max() over each group's new ids (an empty group raises there)
  if (lay.g2.new_id.empty() || lay.g4.new_id.empty()) Fail("ValueError: min() iterable argument is empty");
  std::vector<Built> built2, built4;
  for (int kind : {2, 4}) {
    const Scan sc = ScanKind(suit, ref, lay, kind);
    Merged merged;
    if (mode == CopyMode::kFull) {
      merged = Merge(sc.tris, std::nullopt, {0.0, 0.0, 0.0});  // merge(tris, None, None): identity mode
    } else {
      const CopyGroup& cg = lay.Of(kind);
      const int64_t target = RefCount(ref, cg.ids) - static_cast<int64_t>(cg.ids.size());
      try {
        merged = ChooseFor(sc.tris, target).merged;
      } catch (const ConvertError& e) {
        const std::string w = e.what();
        // slim.SlimError ('origin: no triangles', 'count ... exceeds target ...') is re-raised with the suit's name;
        // merge's own ValueError / OverflowError / ZeroDivisionError pass unchanged, as in Python
        if (w.rfind("origin: ", 0) == 0 || w.rfind("count ", 0) == 0) Fail(name + ": copy " + Kind04(kind) + ": " + w);
        throw;
      }
      // a node fed triangles that the merge collapsed to none (first-fed order)
      std::vector<int64_t> fed_order;
      std::map<int64_t, int64_t> fed;
      for (const Tri& t : sc.tris) {
        const int64_t n = sc.keys[t.key].node;
        if (!fed.count(n)) fed_order.push_back(n);
        ++fed[n];
      }
      std::set<int64_t> kept;
      for (const MergedKey& mk : merged) kept.insert(sc.keys[mk.key].node);
      std::vector<int64_t> lost;
      for (int64_t n : fed_order)
        if (!kept.count(n)) lost.push_back(n);
      if (!lost.empty())
        Fail(name + ": copy " + Kind04(kind) + ": node " + Dec(lost[0]) + " had " + Dec(fed[lost[0]]) +
             " triangles before the merge and none after (nodes " + ListRepr(lost) + ")");
    }
    if (kind == 2) {
      built2 = WriteLod(suit, ref, lay, merged, sc);
    } else {
      built4 = WriteShadow(suit, ref, lay, merged, sc);
    }
  }
  // container: new VBUF after the last VBUF, new IBUF after the last IBUF; data appended, each at 4 KiB
  tpr::Xpr x;
  x.res = suit.x.res;
  x.data = suit.x.data;
  const auto vbr = x.OfType(kVbuf), ibr = x.OfType(kIbuf);
  if (vbr.empty() || ibr.empty()) Fail(name + ": no vertex or index buffers");
  const size_t nvb = vbr.size(), nib = ibr.size();
  std::vector<Built> objs = std::move(built2);
  for (auto& b : built4) objs.push_back(std::move(b));
  std::vector<tpr::ObjGeo> geo = suit.geo;
  std::vector<tpr::ObjInfo> info = suit.info;
  for (size_t k = 0; k < objs.size(); ++k) {
    tpr::Decl& dc = objs[k].geo.decls[0];
    dc.head[2] = be::CheckU32(nib + k);
    Writer w;
    w.U32(nvb + k);
    Bytes t = w.Take();
    t.insert(t.end(), dc.tails[0].begin() + 4, dc.tails[0].end());
    dc.tails[0] = std::move(t);
    geo.push_back(objs[k].geo);
    info.push_back(objs[k].info);
  }
  Bytes& data = x.data;
  std::vector<Bytes> vdesc, idesc;
  for (int pass = 0; pass < 2; ++pass) {
    const uint8_t* head = pass == 0 ? kVbHead : kIbHead;
    const uint32_t low0 = pass == 0 ? 3 : 0, low1 = pass == 0 ? 2 : 0;
    for (const Built& o : objs) {
      const Bytes& b = pass == 0 ? o.vb : o.ib;
      const uint64_t pos = Al(data.size(), 0x1000);
      data.resize(pos, 0);
      const uint64_t size = Al(b.size(), 16);
      data.insert(data.end(), b.begin(), b.end());
      data.resize(pos + size, 0);
      Bytes d(head, head + 12);
      Writer w;
      w.U32(pos | low0);
      w.U32(size | low1);
      d.insert(d.end(), w.bytes().begin(), w.bytes().end());
      (pass == 0 ? vdesc : idesc).push_back(std::move(d));
    }
  }
  data.resize(Al(data.size(), 0x1000), 0);
  const size_t at_v = vbr.back() + 1;
  {
    std::vector<tpr::Res> ins;
    for (auto& d : vdesc) ins.push_back({kVbuf, std::move(d)});
    x.res.insert(x.res.begin() + static_cast<std::ptrdiff_t>(at_v), ins.begin(), ins.end());
  }
  const size_t at_i = x.OfType(kIbuf).back() + 1;  // found after the VBUF insert
  {
    std::vector<tpr::Res> ins;
    for (auto& d : idesc) ins.push_back({kIbuf, std::move(d)});
    x.res.insert(x.res.begin() + static_cast<std::ptrdiff_t>(at_i), ins.begin(), ins.end());
  }
  SetUser0(x);
  const std::vector<tpr::Node>& nodes = lay.allnodes;
  bool step = nodes.size() >= suit.nodes.size() && nodes.size() - suit.nodes.size() == geo.size() - suit.nodes.size();
  for (size_t k = suit.nodes.size(); step && k < nodes.size(); ++k) step = nodes[k].id == static_cast<int64_t>(k);
  if (!step) Fail(name + ": copy ids out of step with the objects");
  x.res[1].blob = tpr::BuildMdlGeo(geo);
  x.res[2].blob = tpr::BuildMdlInfo(info);
  x.res[3].blob = tpr::BuildHie(nodes);
  Result r;
  r.tpr = x.Build(1);
  const auto map = NewMap(suit, ref, lay);
  r.cat = CatWithMap(S(suit.cat), map);
  return r;
}

}  // namespace

// ---------------------------------------------------------------- _node_verts
NodeVerts NodeVertsOf(std::span<const uint32_t> keys, const Merged& merged, std::span<const ScanPayload> payload) {
  struct Ent {
    uint64_t at;  // (triangle, corner) as one ordered number
    std::array<double, 3> pos;
    uint32_t pl;
  };
  // identity (vid, x, y, z): std::tuple's < on doubles is by value, so 0.0 and -0.0 are one key (NaN refused first)
  std::map<std::tuple<uint32_t, double, double, double>, uint32_t> slot;
  std::vector<Ent> ents;
  std::vector<std::vector<uint32_t>> ent_of(keys.size());
  for (size_t ki = 0; ki < keys.size(); ++ki) {
    const MergedKey& mk = At(merged, keys[ki]);
    for (size_t mi = 0; mi < mk.pos.size(); ++mi) {
      const auto& pos = mk.pos[mi];
      if (std::isnan(pos[0]) || std::isnan(pos[1]) || std::isnan(pos[2]))
        Fail("ValueError: a NaN position in a copy node's vertex list (Python's fusion there depends on object identity)");
      const uint32_t pi = At(mk.payload, static_cast<int64_t>(mi));
      if (pi >= payload.size()) Fail("IndexError: list index out of range");
      const ScanPayload& p = payload[pi];
      const uint64_t at = (static_cast<uint64_t>(p.tri) << 8) | p.corner;
      const auto [it, added] = slot.emplace(std::make_tuple(p.vid, pos[0], pos[1], pos[2]), uint32_t(ents.size()));
      if (added) {
        ents.push_back({at, pos, pi});
      } else if (at < ents[it->second].at) {
        ents[it->second] = {at, pos, pi};
      }
      ent_of[ki].push_back(it->second);
    }
  }
  std::vector<uint32_t> order(ents.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return ents[a].at < ents[b].at; });
  std::vector<uint32_t> index(ents.size());
  NodeVerts out;
  for (uint32_t n = 0; n < order.size(); ++n) {
    index[order[n]] = n;
    out.verts.emplace_back(ents[order[n]].pos, ents[order[n]].pl);
  }
  for (auto& v : ent_of) {
    for (auto& e : v) e = index[e];
    out.remap.push_back(std::move(v));
  }
  return out;
}

// ---------------------------------------------------------------- add_copies
Result AddCopies(std::string_view name, std::span<const uint8_t> tpr, std::span<const uint8_t> cat,
                 std::span<const uint8_t> ref_tpr, std::span<const uint8_t> ref_cat, CopyMode mode) {
  return AddCopiesImpl(name, tpr, cat, ref_tpr, ref_cat, mode, TestHooks{});
}

Result AddCopies(std::string_view name, std::span<const uint8_t> tpr, std::span<const uint8_t> cat,
                 std::span<const uint8_t> ref_tpr, std::span<const uint8_t> ref_cat, CopyMode mode,
                 const TestHooks& hooks) {
  return AddCopiesImpl(name, tpr, cat, ref_tpr, ref_cat, mode, hooks);
}

// ---------------------------------------------------------------- the public entry
namespace {
struct SlimCall {
  std::string_view name;
  std::span<const uint8_t> tpr, cat, ref_tpr, ref_cat;
  CopyMode mode;
  Result result;
  bool ok = false;
  std::string error;
};
void RunSlim(void* p) {
  SlimCall& c = *static_cast<SlimCall*>(p);
  try {
    c.result = AddCopies(c.name, c.tpr, c.cat, c.ref_tpr, c.ref_cat, c.mode);
    c.ok = true;
  } catch (const ConvertError& e) {
    c.error = e.what();
  } catch (const std::bad_alloc&) {
    c.error = "out of memory (std::bad_alloc)";
  } catch (const std::exception& e) {
    c.error = std::string("unexpected error: ") + e.what();
  } catch (...) {
    c.error = "unexpected non-standard exception";
  }
}
}  // namespace

bool SlimCopies(std::string_view name, std::span<const uint8_t> tpr, std::span<const uint8_t> cat,
                std::span<const uint8_t> ref_tpr, std::span<const uint8_t> ref_cat, CopyMode mode, Result* out,
                std::string* error) {
  SlimCall c{name, tpr, cat, ref_tpr, ref_cat, mode, {}, false, {}};
  try {
    num::RunUnderConvertEnv(&RunSlim, &c);  // RunSlim catches everything; nothing crosses the call
  } catch (...) {
    c.ok = false;
    c.error = "unexpected non-standard exception";
  }
  if (!c.ok) {
    if (error) *error = std::move(c.error);
    return false;
  }
  if (out) *out = std::move(c.result);
  return true;
}

}  // namespace doa4::suitconv::slim
