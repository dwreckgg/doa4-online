// Suit slim copies, Task 5: the bind guard, the copy layout and the triangle scan (see doa4_suitconv_slim.h). Each
// function names the Python function it ports (slim_addon.py, lod_chain.py, convert.py, tpr.py in
// doax2_work/physics_spike/scripts; found by name, the line numbers move). Every SlimError text is built here exactly as
// the Python f-string builds it ({:#x} as 0x... lower case, ints in decimal, {d * 1000:.3f} as %.3f).
//
// Player-file bytes: every read is bounds-checked (be::Reader). Where Python slices, the slice is clamped the same way
// (idx[st:st + cnt], m[0x70:0x74], m[0x80:0xC0]); where Python raises, the port throws ConvertError naming the Python
// exception type. Vertex records are read from the data section as Python reads them (vb + v * stride + offset), so a
// vertex past its VBUF's size but inside the data section is read, as in Python, and one past the data end refuses.
// Where Python would loop forever (a parent chain that cycles through duplicated ids), the port refuses.

#include "doa4_suitconv_fp.h"

#include "doa4_suitconv_slim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <tuple>
#include <unordered_map>

namespace doa4::suitconv::slim {

using be::Reader;
using tpr::Fourcc;
using tpr::MakeFourcc;

namespace {

constexpr Fourcc kVbuf = MakeFourcc("VBUF");
constexpr Fourcc kIbuf = MakeFourcc("IBUF");
constexpr uint32_t kNotTex = 0xFFFFFFFFu;  // slim_addon.NOT_TEX

// slim_addon.T1_SIDES: (forearm draw code after T1, hand, upper arm)
constexpr std::array<std::array<uint32_t, 3>, 2> kT1Sides = {{{0x9120, 0x9005, 0x9008}, {0x9121, 0x900B, 0x900E}}};

// convert.py CHANNEL_ALIAS (a copy: core.cpp's kChannelAlias has internal linkage), in sorted code order
struct Alias {
  uint32_t code;
  uint32_t alt;
};
constexpr Alias kChannelAlias[] = {{0x9004, 0x9120}, {0x900A, 0x9121}, {0x9103, 0x9003}, {0x9109, 0x9009}};

[[noreturn]] void Fail(const std::string& what) { throw ConvertError(what); }

std::span<const uint8_t> S(const Bytes& b) { return std::span<const uint8_t>(b.data(), b.size()); }

std::string Hex(int64_t v) {  // f'{v:#x}'
  char buf[32];
  if (v < 0) {
    std::snprintf(buf, sizeof buf, "-0x%llx", static_cast<unsigned long long>(-v));
  } else {
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  }
  return buf;
}
std::string Dec(int64_t v) { return std::to_string(v); }

// Python's list[i] for a non-negative or negative index (IndexError otherwise)
template <class T>
const T& At(const std::vector<T>& v, int64_t i) {
  const int64_t n = static_cast<int64_t>(v.size());
  if (i < 0) i += n;
  if (i < 0 || i >= n) Fail("IndexError: list index out of range");
  return v[static_cast<size_t>(i)];
}

// Python's max(iterable) over doubles: the first item, replaced by each later item that is greater (`>`)
template <size_t N>
double PyMax(const std::array<double, N>& v) {
  double m = v[0];
  for (size_t i = 1; i < N; ++i)
    if (v[i] > m) m = v[i];
  return m;
}

// slim_addon._world_t: node id -> world translation (a later node of the same id wins, as in a dict)
std::unordered_map<int64_t, std::array<double, 3>> WorldT(const std::vector<tpr::Node>& nodes) {
  const auto W = tpr::WorldMats(nodes);
  std::unordered_map<int64_t, std::array<double, 3>> out;
  for (size_t i = 0; i < nodes.size(); ++i) out[nodes[i].id] = {W[i][12], W[i][13], W[i][14]};
  return out;
}

// repr([x[0] for x in groups]): 'main', an int, or ('state', NN)
std::string KindsRepr(const std::vector<MapGroup>& groups) {
  std::string s = "[";
  for (size_t i = 0; i < groups.size(); ++i) {
    if (i) s += ", ";
    const MapGroup& g = groups[i];
    if (g.kind == MapGroup::Kind::kMain) {
      s += "'main'";
    } else if (g.kind == MapGroup::Kind::kState) {
      s += "('state', " + Dec(g.value) + ")";
    } else {
      s += Dec(g.value);
    }
  }
  return s + "]";
}

// slim_addon._elem(dc, usage): the first element of stream 0 with this usage and usage index 0, END excluded
const tpr::DeclElem* ElemOf(const tpr::Decl& dc, uint8_t usage) {
  for (size_t i = 0; i + 1 < dc.elems.size(); ++i) {
    const tpr::DeclElem& e = dc.elems[i];
    if (e.a == 0 && e.e == usage && e.f == 0) return &e;
  }
  return nullptr;
}

// slim_addon._heaviest(slots, ws): sum per bone over the palette slots (weight slots past the palette name no bone;
// plain sequential w.get(b, 0.0) + ws[k]), then max(sorted(w), key=w): the first maximum in ascending bone order.
int64_t Heaviest(const std::vector<int64_t>& slots, const tpr::ElemValue& ws) {
  std::vector<std::pair<int64_t, double>> w;  // few slots: a linear map
  for (size_t k = 0; k < slots.size(); ++k) {
    if (static_cast<int>(k) >= ws.n) continue;
    const int64_t b = slots[k];
    auto it = std::find_if(w.begin(), w.end(), [b](const auto& p) { return p.first == b; });
    if (it == w.end()) {
      w.emplace_back(b, 0.0 + ws.v[k]);
    } else {
      it->second = it->second + ws.v[k];
    }
  }
  if (w.empty()) Fail("ValueError: max() iterable argument is empty");
  std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  size_t best = 0;
  for (size_t i = 1; i < w.size(); ++i)
    if (w[i].second > w[best].second) best = i;
  return w[best].first;
}

// slim_addon._t1_objects(suit): the wrist-T1 forearm objects
std::vector<int64_t> T1Objects(const Suit& suit) {
  std::map<uint32_t, int64_t> code_obj;  // setdefault: the first row per code
  for (const auto& [c, i] : suit.main) code_obj.emplace(c, i);
  std::vector<int64_t> out;
  for (const auto& side : kT1Sides) {
    const auto fa = code_obj.find(side[0]);
    if (fa == code_obj.end() || !code_obj.count(side[1]) || !code_obj.count(side[2])) continue;
    const auto& pal = At(suit.info, fa->second).palette;
    if (pal.size() == 2 && pal[0] == code_obj[side[1]] && pal[1] == code_obj[side[2]]) out.push_back(fa->second);
  }
  return out;
}

// slim_addon._slot_bones(suit, oid, t1, neck): palette slot -> bone id; empty optional for no palette
std::optional<std::vector<int64_t>> SlotBones(const Suit& suit, int64_t oid, const std::vector<int64_t>& t1,
                                              std::optional<int64_t> neck) {
  const auto& pal = At(suit.info, oid).palette;
  if (pal.empty()) return std::nullopt;
  if (std::find(t1.begin(), t1.end(), oid) != t1.end()) return std::vector<int64_t>{oid, oid};  // rule T1
  if (neck && oid == *neck && pal.size() == 2 && pal[0] == pal[1]) return std::vector<int64_t>{oid, pal[1]};  // rule N2
  return std::vector<int64_t>(pal.begin(), pal.end());
}

Bytes SliceBytes(std::span<const uint8_t> m, uint64_t a, uint64_t b) {  // m[a:b], clamped
  const auto s = Reader(m).Slice(a, b);
  return Bytes(s.begin(), s.end());
}

}  // namespace

// ---------------------------------------------------------------- reference
// slim_addon.ref_groups
std::vector<MapGroup> RefGroups(const core::Model& ref) {
  const Bytes& cat = ref.Cat();
  std::optional<uint32_t> t2;
  for (const auto& [t, v] : tpr::CatDir(S(cat)))
    if (t == 2) t2 = v;  // dict(cat_dir(...)): the last value of a type wins
  if (!t2) Fail("KeyError: 2");
  return ParseMap(S(cat), *t2).first;
}

// convert.channel_map
std::map<uint32_t, int64_t> ChannelMap(const core::Model& ref) {
  const auto groups = RefGroups(ref);
  const auto& main = groups.at(0).items;
  std::unordered_map<int64_t, int64_t> depth;
  const int64_t n = static_cast<int64_t>(ref.hie.size());
  for (int64_t k = 0; k < n; ++k) {
    int64_t d = 0, j = k;
    while (ref.Hie(j).parent >= 0) {
      j = ref.Hie(j).parent;
      if (++d > n) Fail("RecursionError: the reference's Hie parent chain loops");  // Python: never ends
    }
    depth[ref.hie[static_cast<size_t>(k)].id] = d;
  }
  const auto depth_of = [&depth](int64_t nid) {
    const auto it = depth.find(nid);
    if (it == depth.end()) Fail("KeyError: " + Dec(nid));
    return it->second;
  };
  std::map<uint32_t, int64_t> out;
  for (const auto& [code, nid] : main) {
    // a channel on two nodes: the shallowest wins, then the lowest id
    const auto it = out.find(code);
    if (it == out.end() || std::make_pair(depth_of(nid), int64_t(nid)) < std::make_pair(depth_of(it->second), it->second))
      out[code] = nid;
  }
  for (const Alias& a : kChannelAlias)
    if (!out.count(a.code)) {
      const auto it = out.find(a.alt);
      if (it != out.end()) out[a.code] = it->second;
    }
  return out;
}

// ---------------------------------------------------------------- bind guard
// slim_addon.bind_guard
void BindGuard(const Suit& suit, const core::Model& ref) {
  const auto st = WorldT(suit.nodes), rt = WorldT(ref.hie);
  const auto groups = RefGroups(ref);
  for (const auto& [code, nid] : groups.at(0).items) {
    const auto s = st.find(nid);
    if (s == st.end())
      Fail(suit.name + ": reference node " + Dec(nid) + " (" + Hex(code) + ") missing from the suit");
    const auto r = rt.find(nid);
    if (r == rt.end()) Fail("KeyError: " + Dec(nid));
    std::array<double, 3> diff{};
    for (int a = 0; a < 3; ++a) diff[a] = std::fabs(s->second[a] - r->second[a]);
    const double d = PyMax(diff);
    if (d > 1e-5) {
      char mm[64];
      std::snprintf(mm, sizeof mm, "%.3f", d * 1000);
      Fail(suit.name + ": node " + Dec(nid) + " (" + Hex(code) + ") is " + mm + " mm from the reference bind");
    }
  }
}

// ---------------------------------------------------------------- frames
// lod_chain._frame
Frame FrameOf(const std::vector<double>& w) {
  if (w.size() < 15) Fail("IndexError: list index out of range");
  Frame f;
  f.R = {{{w[0], w[1], w[2]}, {w[4], w[5], w[6]}, {w[8], w[9], w[10]}}};
  std::array<double, 9> dev{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) dev[3 * r + c] = std::fabs(f.R[r][c] - (r == c ? 1.0 : 0.0));
  f.t = {w[12], w[13], w[14]};
  f.dev = PyMax(dev);
  return f;
}

// lod_chain._to_local
std::array<double, 3> ToLocal(const std::array<double, 3>& v, const Frame& f, bool direction) {
  std::array<double, 3> q = v;
  if (!direction)
    for (int a = 0; a < 3; ++a) q[a] = v[a] - f.t[a];
  if (f.dev < 1e-6) return q;
  std::array<double, 3> out{};
  for (int j = 0; j < 3; ++j) {
    num::PySum s;  // builtin sum() from the int 0
    for (int i = 0; i < 3; ++i) {
      const double prod = q[i] * f.R[j][i];
      s.Add(prod);
    }
    out[j] = s.Result();
  }
  return out;
}

// ---------------------------------------------------------------- layout
const CopyGroup& Layout::Of(int kind) const {
  if (kind == 2) return g2;
  if (kind == 4) return g4;
  Fail("KeyError: " + Dec(kind));
}

// slim_addon.Layout.__init__
Layout MakeLayout(const Suit& suit, const core::Model& ref) {
  const auto groups = RefGroups(ref);
  Layout lay;
  int64_t first = static_cast<int64_t>(suit.nodes.size());
  for (int kind : {2, 4}) {
    CopyGroup& cg = kind == 2 ? lay.g2 : lay.g4;
    const MapGroup* g = nullptr;
    int count = 0;
    for (size_t i = 1; i < groups.size(); ++i)
      if (groups[i].kind == MapGroup::Kind::kMarker && groups[i].value == kind) {
        if (!count) g = &groups[i];
        ++count;
      }
    if (count != 1)
      Fail(suit.name + ": reference has no single group " + Hex(kind) + " (" + KindsRepr(groups) + ")");
    cg.rows = g->items;
    std::set<int64_t> idset;
    for (const auto& [c, i] : cg.rows) idset.insert(i);
    cg.ids.assign(idset.begin(), idset.end());
    if (cg.ids.size() != cg.rows.size()) Fail(suit.name + ": a group-" + Hex(kind) + " node named twice in the reference");
    for (size_t k = 0; k < cg.ids.size(); ++k) cg.new_id[cg.ids[k]] = first + static_cast<int64_t>(k);
    first += static_cast<int64_t>(cg.ids.size());
  }
  for (size_t i = 1; i < groups.size(); ++i) lay.kinds.emplace_back(groups[i].kind, groups[i].value);
  lay.allnodes = suit.nodes;
  for (int kind : {2, 4}) {
    CopyGroup& cg = kind == 2 ? lay.g2 : lay.g4;
    const auto& nid = cg.new_id;
    for (int64_t old : cg.ids) {
      const int64_t* at = ref.id2node.Find(old);
      if (!at) Fail("KeyError: " + Dec(old));
      const tpr::Node& nd = ref.Hie(*at);
      int64_t par = -1;
      if (nd.parent >= 0) {
        const int64_t pid = ref.Hie(nd.parent).id;
        const auto p = nid.find(pid);
        if (p == nid.end())
          Fail(suit.name + ": group-" + Hex(kind) + " node " + Dec(old) + " has its parent " + Dec(pid) +
               " outside the group");
        par = p->second;  // appended in id order: Hie index = new id
      }
      std::vector<int64_t> ch;
      for (int64_t c : nd.children) {
        const int64_t cid = ref.Hie(c).id;
        const auto ci = nid.find(cid);
        if (ci != nid.end()) {
          ch.push_back(ci->second);
        } else if (kind == 4) {  // shadow_chain takes every child (its subtree is exactly the group)
          Fail(suit.name + ": shadow node " + Dec(old) + " has a child " + Dec(cid) + " outside the group");
        }
        // group 0x02: children outside the group are left out
      }
      tpr::Node out = nd;  // tpr.Node(nd.mat, nid[old], par, ch): the matrix kept, the tail empty
      out.id = nid.at(old);
      out.parent = par;
      out.children = std::move(ch);
      out.tail.clear();
      cg.nodes.push_back(std::move(out));
    }
    lay.allnodes.insert(lay.allnodes.end(), cg.nodes.begin(), cg.nodes.end());
  }
  const auto W = tpr::WorldMats(lay.allnodes);
  for (size_t k = 0; k < lay.allnodes.size(); ++k) lay.frame[lay.allnodes[k].id] = FrameOf(W[k]);
  // a channel on several copy nodes: the shallowest node, then the lowest id
  for (int kind : {2, 4}) {
    CopyGroup& cg = kind == 2 ? lay.g2 : lay.g4;
    std::map<int64_t, std::optional<int64_t>> par_of;
    for (const auto& nd : cg.nodes)
      par_of[nd.id] = nd.parent >= 0 ? std::optional<int64_t>(At(lay.allnodes, nd.parent).id) : std::nullopt;
    const int64_t limit = static_cast<int64_t>(cg.nodes.size());
    const auto depth = [&](int64_t n) {
      int64_t d = 0;
      for (;;) {
        const auto it = par_of.find(n);
        if (it == par_of.end()) Fail("KeyError: " + Dec(n));
        if (!it->second) return d;
        n = *it->second;
        if (++d > limit) Fail(suit.name + ": group-" + Hex(kind) + " parent chain loops");  // Python: never ends
      }
    };
    for (const auto& [c, i] : cg.rows) {
      const int64_t n = cg.new_id.at(i);
      const auto it = cg.node_of_code.find(c);
      if (it == cg.node_of_code.end() || std::make_pair(depth(n), n) < std::make_pair(depth(it->second), it->second))
        cg.node_of_code[c] = n;
    }
  }
  return lay;
}

// ---------------------------------------------------------------- scan
// slim_addon._scan
Scan ScanKind(const Suit& suit, const core::Model& ref, const Layout& lay, int kind) {
  const std::string& name = suit.name;
  const auto chan2id = ChannelMap(ref);
  std::unordered_map<int64_t, uint32_t> id2chan;  // the lowest code per node id
  for (const auto& [code, nid] : chan2id) id2chan.emplace(nid, code);
  std::unordered_map<int64_t, std::optional<int64_t>> parent;
  for (const auto& nd : suit.nodes) {
    std::optional<int64_t> p;
    if (nd.parent >= 0) {
      if (static_cast<uint64_t>(nd.parent) >= suit.nodes.size()) Fail("IndexError: list index out of range");
      p = suit.nodes[static_cast<size_t>(nd.parent)].id;
    }
    parent[nd.id] = p;
  }
  const auto& noc = lay.Of(kind).node_of_code;
  if (!noc.count(0x9002)) Fail(name + ": reference group " + Hex(kind) + " has no 0x9002 row");
  const int64_t root_node = noc.at(0x9002);
  const size_t walk_limit = suit.nodes.size() + 1;
  const auto target = [&](int64_t d) -> int64_t {
    std::optional<int64_t> cur = d;
    for (size_t steps = 0; cur; ++steps) {
      if (steps > walk_limit) Fail(name + ": the suit's parent chain from node " + Dec(d) + " loops");  // Python: never ends
      const auto c = id2chan.find(*cur);
      if (c != id2chan.end()) {
        const auto n = noc.find(c->second);
        if (n != noc.end()) return n->second;
      }
      const auto p = parent.find(*cur);
      cur = p == parent.end() ? std::nullopt : p->second;
    }
    return root_node;
  };
  const auto t1 = T1Objects(suit);
  std::optional<int64_t> neck;
  for (const auto& [c, i] : suit.main)
    if (c == 0x9101) {
      neck = i;
      break;
    }
  const auto own = WorldT(suit.nodes);
  std::vector<size_t> vbr = suit.x.OfType(kVbuf), ibr = suit.x.OfType(kIbuf);
  const Bytes& data = suit.x.data;
  const Reader rd(S(data));

  Scan out;
  // matkey -> dense id, then (node, matkey id) -> key id (kind 4: matkey id 0)
  using MatKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, Bytes, Bytes>;
  std::map<MatKey, uint32_t> matkeys;
  std::vector<const MatKey*> matkey_of;  // matkey id -> its tuple (std::map nodes do not move)
  std::map<std::pair<int64_t, uint32_t>, uint32_t> key_ids;
  // per-decl vertex caches (Python's `cache`), stamped by decl so nothing is cleared between decls
  std::vector<uint32_t> vstamp(0x10000, 0), vcache(0x10000, 0), bstamp(0x10000, 0);
  std::vector<int64_t> bcache(0x10000, 0);
  uint32_t stamp = 0;
  std::vector<std::array<double, 3>> vpos, vnrm;
  std::vector<std::array<double, 2>> vuv;
  std::vector<std::pair<uint8_t, uint8_t>> vn;  // (nrm_n, uv_n) per vid
  uint32_t t = 0;

  for (size_t oid = 0; oid < suit.geo.size(); ++oid) {
    const tpr::ObjGeo& g = suit.geo[oid];
    const tpr::ObjInfo& inf = At(suit.info, static_cast<int64_t>(oid));
    const auto jo = own.find(static_cast<int64_t>(oid));
    if (jo == own.end()) Fail(name + ": object " + Dec(static_cast<int64_t>(oid)) + " has no node of that id");
    const std::array<double, 3> j = jo->second;
    const auto slots = SlotBones(suit, static_cast<int64_t>(oid), t1, neck);
    for (size_t k = 0; k < g.decls.size(); ++k) {
      const tpr::Decl& dc = g.decls[k];
      std::vector<size_t> meshes;
      for (size_t mi = 0; mi < g.meshes.size(); ++mi)
        if (Reader(S(g.meshes[mi])).U32(8) == k) meshes.push_back(mi);
      if (meshes.empty()) continue;
      const int64_t vb = tpr::BufSpan(S(suit.x.res[At(vbr, dc.Vb())].blob)).first;
      const int64_t stride = dc.Stride();
      const int64_t ib0 = tpr::BufSpan(S(suit.x.res[At(ibr, dc.head[2])].blob)).first;
      if (static_cast<uint64_t>(ib0) > data.size()) Fail("struct.error: offset out of range for the data");
      const std::vector<uint16_t> idx = rd.U16s(ib0, dc.head[3]);
      const tpr::DeclElem *ep = ElemOf(dc, 0), *en = ElemOf(dc, 3), *eu = ElemOf(dc, 5), *ew = ElemOf(dc, 1);
      ++stamp;
      const auto corner = [&](uint16_t v) -> uint32_t {
        if (vstamp[v] == stamp) return vcache[v];
        const int64_t o = vb + static_cast<int64_t>(v) * stride;
        if (!ep) Fail("TypeError: 'NoneType' object is not subscriptable");
        const tpr::ElemValue p = tpr::DecodeElem(S(data), o + ep->b, ep->c);
        if (p.n < 3) Fail("IndexError: tuple index out of range");
        std::array<double, 3> pos;
        for (int a = 0; a < 3; ++a) pos[a] = p.v[a] + j[a];
        std::array<double, 3> nrm{};
        std::array<double, 2> uv{};
        uint8_t nn = 0, un = 0;
        if (kind == 2) {
          if (!en || !eu)
            Fail(name + ": object " + Dec(static_cast<int64_t>(oid)) + " decl " + Dec(static_cast<int64_t>(k)) +
                 " has no normal or uv0");
          const tpr::ElemValue en_v = tpr::DecodeElem(S(data), o + en->b, en->c);
          const tpr::ElemValue eu_v = tpr::DecodeElem(S(data), o + eu->b, eu->c);
          nn = static_cast<uint8_t>(std::min(en_v.n, 3));
          un = static_cast<uint8_t>(std::min(eu_v.n, 2));
          for (int a = 0; a < nn; ++a) nrm[a] = en_v.v[a];
          for (int a = 0; a < un; ++a) uv[a] = eu_v.v[a];
        }
        const uint32_t vid = static_cast<uint32_t>(out.vid_src.size());
        out.vid_src.push_back({static_cast<uint32_t>(oid), static_cast<uint32_t>(k), v});
        vpos.push_back(pos);
        vnrm.push_back(nrm);
        vuv.push_back(uv);
        vn.emplace_back(nn, un);
        vstamp[v] = stamp;
        vcache[v] = vid;
        return vid;
      };
      const auto bone = [&](uint16_t v) -> int64_t {
        if (!slots || !ew) return static_cast<int64_t>(oid);
        if (bstamp[v] == stamp) return bcache[v];
        const tpr::ElemValue ws = tpr::DecodeElem(S(data), vb + static_cast<int64_t>(v) * stride + ew->b, ew->c);
        const int64_t b = Heaviest(*slots, ws);
        bstamp[v] = stamp;
        bcache[v] = b;
        return b;
      };
      for (size_t mi : meshes) {
        const Bytes& m = g.meshes[mi];
        const auto texs = tpr::MeshTextures(S(m));
        if (texs.empty()) Fail("IndexError: list index out of range");
        const uint32_t tex = texs[0].texture;
        if (mi >= inf.infos.size()) Fail("IndexError: list index out of range");
        const auto mf = Reader(S(inf.infos[mi])).U32s(0, 8);
        uint32_t mk = 0;
        if (kind == 2) {
          if (tex == kNotTex) continue;  // lod_chain.py: no stage-0 texture, not in the reflection
          if (mf[2] != 2 && mf[2] != 4 && mf[2] != 8)  // lod_chain.LOD_PASS = {2: 2, 4: 4, 8: 8}
            Fail(name + ": object " + Dec(static_cast<int64_t>(oid)) + " mesh " + Dec(static_cast<int64_t>(mi)) +
                 " draw pass " + Dec(mf[2]));
          MatKey key{tex, mf[2], mf[4], Reader(S(m)).U32(4), SliceBytes(S(m), 0x70, 0x74), SliceBytes(S(m), 0x80, 0xC0)};
          const auto it = matkeys.find(key);
          if (it == matkeys.end()) {
            mk = static_cast<uint32_t>(matkeys.size());
            matkey_of.push_back(&matkeys.emplace(std::move(key), mk).first->first);
          } else {
            mk = it->second;
          }
        }
        const Reader rm(S(m));
        const uint64_t st = rm.U32(0x10), cnt = rm.U32(0x14);
        const uint64_t a = std::min<uint64_t>(st, idx.size()), b = std::min<uint64_t>(st + cnt, idx.size());
        const std::span<const uint16_t> run(idx.data() + a, b > a ? static_cast<size_t>(b - a) : 0);
        for (const auto& tri : core::StripTris(run)) {
          const int64_t node = target(bone(tri[0]));
          const auto kk = std::make_pair(node, mk);
          auto ki = key_ids.find(kk);
          if (ki == key_ids.end()) {
            ki = key_ids.emplace(kk, static_cast<uint32_t>(out.keys.size())).first;
            ScanKey sk;
            sk.node = node;
            if (kind == 2) {
              const MatKey* mkp = matkey_of[mk];
              sk.tex = std::get<0>(*mkp);
              sk.pass = std::get<1>(*mkp);
              sk.flag = std::get<2>(*mkp);
              sk.word4 = std::get<3>(*mkp);
              sk.m70 = std::get<4>(*mkp);
              sk.m80 = std::get<5>(*mkp);
              sk.matsrc = m;  // matsrc[key] = m only when the key is new: the first mesh
            }
            out.keys.push_back(std::move(sk));
          }
          Tri tr;
          tr.key = ki->second;
          for (int ci = 0; ci < 3; ++ci) {
            const uint32_t vid = corner(tri[ci]);
            tr.vid[ci] = vid;
            tr.pos[ci] = vpos[vid];
            tr.payload[ci] = 3 * t + static_cast<uint32_t>(ci);
            ScanPayload pl;
            pl.vid = vid;
            pl.tri = t;
            pl.corner = static_cast<uint8_t>(ci);
            pl.nrm_n = vn[vid].first;
            pl.uv_n = vn[vid].second;
            pl.nrm = vnrm[vid];
            pl.uv = vuv[vid];
            out.payload.push_back(pl);
          }
          out.tris.push_back(tr);
          ++t;
        }
      }
    }
  }
  return out;
}

}  // namespace doa4::suitconv::slim
