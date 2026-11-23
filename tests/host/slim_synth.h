// Synthetic suit models for the slim-copies host test (suitconv_slim_host_test.cpp). Test only. Every byte comes from the
// C++ writers (tpr::Xpr::Build, BuildMdlGeo, BuildMdlInfo, BuildHie, slim::MapBytes) fed with literal numbers that the
// test types; no byte of any game file. A model is a list of Hie nodes (in file order, links as list positions), one
// object per node id (one decl, one mesh), textures and vertex / index buffers placed at literal offsets of the data
// section, and a type-2 map; MakeTpr() / MakeCat() write it.
//
// Resource order in the container: USER0..USER3, then every TX2D, every VBUF, every IBUF (so USER0 is
// [7, 4, ntex, 4, nvb, 4 + ntex, nib, 4 + ntex + nvb, 1, 2, 3, 16]). Data bytes of a buffer or texture are a pattern
// seeded by its `fill` byte; the bytes between them are zero.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../src/doa4_suitconv_be.h"
#include "../../src/doa4_suitconv_slim.h"
#include "../../src/doa4_suitconv_tpr.h"

namespace synth {

namespace tp = doa4::suitconv::tpr;
namespace sl = doa4::suitconv::slim;
using Bytes = tp::Bytes;

struct Node {
  int64_t id;
  int64_t parent;                  // list position, -1 for a root
  std::vector<int64_t> children;   // list positions
  double tx = 0, ty = 0, tz = 0;   // translation row of an identity matrix
};

// One object per node id: objects[k] is object k (MdlGeo / MdlInfo index k).
struct Object {
  uint32_t node_word;   // MdlGeo pre word 1 (node index)
  uint32_t info_word1;  // MdlInfo word 1
  uint32_t vb, ib;      // buffer indices (among VBUFs / IBUFs)
  uint32_t nverts, nidx;
  uint32_t tex = 0;     // texture index named by the mesh's stage
};

struct Buf {
  uint32_t base, size;  // in the data section (multiples of 4)
  uint8_t fill;
};

struct Tex {
  uint32_t base, mip;  // mip 0 = no mip address
  uint32_t size;       // bytes filled from base (and from mip when present)
  uint8_t fill;
};

inline void Fill(Bytes& data, uint32_t at, uint32_t n, uint8_t seed) {
  for (uint32_t k = 0; k < n; ++k) data.at(at + k) = static_cast<uint8_t>((k * 7u + seed) % 251u + 1u);
}

struct Model {
  std::vector<Node> nodes;
  std::vector<Object> objects;
  std::vector<Tex> textures;
  std::vector<Buf> vbufs, ibufs;
  uint32_t data_end = 0;
  std::vector<tp::T2Group> groups;              // the type-2 map
  std::optional<std::vector<uint32_t>> user0;   // when set, USER0's 12 words as given (a refusal fixture)

  void SetNodes(std::vector<Node> v) { nodes = std::move(v); }
  void SetObjects(std::vector<Object> v) { objects = std::move(v); }
  void SetTextures(std::vector<Tex> v) { textures = std::move(v); }
  void SetVbufs(std::vector<Buf> v) { vbufs = std::move(v); }
  void SetIbufs(std::vector<Buf> v) { ibufs = std::move(v); }
  void SetDataEnd(uint32_t v) { data_end = v; }
  void SetGroups(std::vector<tp::T2Group> v) { groups = std::move(v); }

  std::vector<tp::Node> HieNodes() const {
    std::vector<tp::Node> out;
    for (const Node& n : nodes) {
      std::vector<double> m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, n.tx, n.ty, n.tz, 1};
      out.push_back(tp::Node::FromFloats(m, n.id, n.parent, n.children));
    }
    return out;
  }

  std::vector<tp::ObjGeo> Geo() const {
    std::vector<tp::ObjGeo> out;
    for (size_t k = 0; k < objects.size(); ++k) {
      const Object& o = objects[k];
      tp::ObjGeo g;
      {
        doa4::suitconv::be::Writer w;
        w.U32(0x00020000);
        w.U32(o.node_word);
        w.U32(0);
        w.U32(0);
        g.pre = w.Take();
      }
      tp::Decl d;
      d.head = {0, 1, o.ib, o.nidx, 2, o.nverts, 1, 0, 0, 0, 0, 0};
      d.elems = {tp::DeclElem{0, 0, 0x2A23B9, 0, 0, 0, 0}, tp::DeclElem{0x00FF, 0, 0xFFFFFFFF, 0, 0, 0, 0}};
      {
        doa4::suitconv::be::Writer w;
        w.U32(o.vb);
        w.U32(12);
        w.U32(0);
        w.U32(0);
        d.tails = {w.Take()};
      }
      g.decls = {d};
      {
        doa4::suitconv::be::Writer w;  // mesh words: index, 0, decl, 0, index start, index count, stages, 0
        w.U32(0);
        w.U32(0);
        w.U32(0);
        w.U32(0);
        w.U32(0);
        w.U32(o.nidx);
        w.U32(1);
        w.U32(0);
        w.U32(0);  // stage 0: {stage, kind, texture, ...} 0x40 bytes
        w.U32(0);
        w.U32(o.tex);
        w.Zeros(0x40 - 12);
        g.meshes = {w.Take()};
      }
      out.push_back(std::move(g));
    }
    return out;
  }

  std::vector<tp::ObjInfo> Info() const {
    std::vector<tp::ObjInfo> out;
    for (size_t k = 0; k < objects.size(); ++k) {
      tp::ObjInfo inf;
      inf.words = {0x00010001, objects[k].info_word1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
      inf.bounds.assign(48, 0);
      inf.bounds[3] = static_cast<uint8_t>(k + 1);
      inf.infos = {Bytes(0x50, static_cast<uint8_t>(0x10 + k))};
      out.push_back(std::move(inf));
    }
    return out;
  }

  Bytes MakeTpr() const {
    namespace be = doa4::suitconv::be;
    tp::Xpr x;
    const uint32_t ntex = static_cast<uint32_t>(textures.size());
    const uint32_t nvb = static_cast<uint32_t>(vbufs.size()), nib = static_cast<uint32_t>(ibufs.size());
    {
      be::Writer w;
      if (user0) {
        for (uint32_t v : *user0) w.U32(v);
      } else {
        for (uint32_t v : {7u, 4u, ntex, 4u, nvb, 4u + ntex, nib, 4u + ntex + nvb, 1u, 2u, 3u, 16u}) w.U32(v);
      }
      x.res.push_back({tp::MakeFourcc("USER"), w.Take()});
    }
    x.res.push_back({tp::MakeFourcc("USER"), tp::BuildMdlGeo(Geo())});
    x.res.push_back({tp::MakeFourcc("USER"), tp::BuildMdlInfo(Info())});
    x.res.push_back({tp::MakeFourcc("USER"), tp::BuildHie(HieNodes())});
    x.data.assign(data_end, 0);
    for (const Tex& t : textures) {
      be::Writer w;  // 0x34-byte descriptor: 7 words, then the six fetch-constant words
      for (int k = 0; k < 7; ++k) w.U32(0x100 + k);
      w.U32(0x00000002);
      w.U32(t.base | 0x52);  // low 12 bits: a format
      w.U32(0x003FE0FF);
      w.U32(0);
      w.U32(0);
      w.U32(t.mip | (t.mip ? 0x200 : 0));
      x.res.push_back({tp::MakeFourcc("TX2D"), w.Take()});
      Fill(x.data, t.base, t.size, t.fill);
      if (t.mip) Fill(x.data, t.mip, t.size / 4, static_cast<uint8_t>(t.fill + 1));
    }
    for (const Buf& b : vbufs) {
      be::Writer w;
      w.U32(0x00000003);
      w.U32(0);
      w.U32(b.base | 3);
      w.U32(b.size | 2);
      x.res.push_back({tp::MakeFourcc("VBUF"), w.Take()});
      Fill(x.data, b.base, b.size, b.fill);
    }
    for (const Buf& b : ibufs) {
      be::Writer w;
      w.U32(0x00000001);
      w.U32(0x00000002);
      w.U32(0);
      w.U32(b.base | 1);
      w.U32(b.size);
      x.res.push_back({tp::MakeFourcc("IBUF"), w.Take()});
      Fill(x.data, b.base, b.size, b.fill);
    }
    return x.Build(1);
  }

  // A cat of the converter's shape: header, the map at 0x18, a type-4 part, the first directory, the apa block (one
  // entry with one rule), the second directory [(2, 0x18), (4, T4), (5, B)]; header word 8 points into the apa block.
  Bytes MakeCat() const { return MakeCatWith(sl::MapBytes(groups)); }

  static Bytes MakeCatWith(const Bytes& map) {
    namespace be = doa4::suitconv::be;
    Bytes c = {'C', 'A', 'T', 0};
    c.resize(0x18, 0);
    c.insert(c.end(), map.begin(), map.end());
    c.resize(tp::Al(c.size(), 8), 0);
    const uint32_t t4 = static_cast<uint32_t>(c.size());
    for (int k = 0; k < 16; ++k) c.push_back(static_cast<uint8_t>(0xA0 + k));  // the type-4 part
    const uint32_t p = static_cast<uint32_t>(c.size());
    const uint32_t B = static_cast<uint32_t>(tp::Al(p + 24, 16));
    {
      be::Writer w;  // the first directory: the second's first two entries and a zero entry
      w.U8(1), w.U8(0), w.U8(2), w.U8(0), w.U32(0x18);
      w.U8(1), w.U8(0), w.U8(4), w.U8(0), w.U32(t4);
      w.Zeros(8);
      const auto& b = w.bytes();
      c.insert(c.end(), b.begin(), b.end());
    }
    c.resize(B, 0);
    {
      be::Writer w;  // apa block at B: magic, count 1, entry at B+12 {nmap 1, map B+28, rules 1, rules at B+32}
      w.U32(0x617061);
      w.U32(1);
      w.U32(B + 12);
      w.U32(1);
      w.U32(B + 28);
      w.U32(1);
      w.U32(B + 32);
      w.U32(0x11223344);  // map data
      w.U32(0x55);        // rule 0: {word, absolute offset B+40}
      w.U32(B + 40);
      w.U32(0x66778899);  // rule target data
      w.U32(0xAABBCCDD);
      const auto& b = w.bytes();
      c.insert(c.end(), b.begin(), b.end());
    }
    const uint32_t dir2 = static_cast<uint32_t>(c.size());
    {
      be::Writer w;
      w.U8(1), w.U8(0), w.U8(2), w.U8(0), w.U32(0x18);
      w.U8(1), w.U8(0), w.U8(4), w.U8(0), w.U32(t4);
      w.U8(1), w.U8(0), w.U8(5), w.U8(0), w.U32(B);
      w.Zeros(8);
      const auto& b = w.bytes();
      c.insert(c.end(), b.begin(), b.end());
    }
    be::PackIntoU32(c, 4, dir2);
    be::PackIntoU32(c, 8, B + 28);
    return c;
  }

  // A cat NOT of the converter's shape (retail hel03/hak03/hak08 style): header, the map at 0x18, 0x40 non-zero
  // structure bytes right after the map (unlisted), padding to 16, an 'apa' type-5 part X, a type-4 part at X + 0x10,
  // the directory [(2, 0x18), (5, X), (4, X + 0x10)].
  static Bytes MakeRetailCat(const Bytes& map) {
    namespace be = doa4::suitconv::be;
    Bytes c = {'C', 'A', 'T', 0};
    c.resize(0x18, 0);
    c.insert(c.end(), map.begin(), map.end());
    for (int k = 0; k < 0x40; ++k) c.push_back(static_cast<uint8_t>((k * 13 + 5) % 250 + 1));
    c.resize(tp::Al(c.size(), 16), 0);
    const uint32_t X = static_cast<uint32_t>(c.size());
    c.push_back(0), c.push_back('a'), c.push_back('p'), c.push_back('a');
    for (int k = 1; k < 13; ++k) c.push_back(static_cast<uint8_t>(k));
    for (int k = 20; k < 36; ++k) c.push_back(static_cast<uint8_t>(k));
    const uint32_t D = static_cast<uint32_t>(c.size());
    be::Writer w;
    w.U8(1), w.U8(0), w.U8(2), w.U8(0), w.U32(0x18);
    w.U8(1), w.U8(0), w.U8(5), w.U8(0), w.U32(X);
    w.U8(1), w.U8(0), w.U8(4), w.U8(0), w.U32(X + 0x10);
    w.Zeros(8);
    c.insert(c.end(), w.bytes().begin(), w.bytes().end());
    be::PackIntoU32(c, 4, D);
    return c;
  }
};

inline tp::T2Group Main(std::vector<std::pair<uint16_t, uint8_t>> items) {
  tp::T2Group g;
  g.is_main = true;
  g.items = std::move(items);
  return g;
}
inline tp::T2Group Group(uint8_t kind, std::vector<std::pair<uint16_t, uint8_t>> items) {
  tp::T2Group g;
  g.is_main = false;
  g.kind = kind;
  g.items = std::move(items);
  return g;
}

}  // namespace synth
