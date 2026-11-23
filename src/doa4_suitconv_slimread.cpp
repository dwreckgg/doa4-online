// Suit slim copies, Task 3: the container layer (see doa4_suitconv_slim.h). Each function names the Python function it
// ports (slim_addon.py, tpr.py, convert.py in doax2_work/physics_spike/scripts; found by name, the line numbers move).
// Every SlimError text is built here exactly as the Python f-string builds it (ints in decimal, {:#x} as 0x..., a list
// as Python's repr).

#include "doa4_suitconv_fp.h"

#include "doa4_suitconv_slim.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace doa4::suitconv::slim {

using be::Reader;
using be::Writer;
using tpr::Al;
using tpr::Fourcc;
using tpr::MakeFourcc;

namespace {

constexpr Fourcc kUser = MakeFourcc("USER");
constexpr Fourcc kTx2d = MakeFourcc("TX2D");
constexpr Fourcc kVbuf = MakeFourcc("VBUF");
constexpr Fourcc kIbuf = MakeFourcc("IBUF");

[[noreturn]] void Fail(const std::string& what) { throw ConvertError(what); }

std::span<const uint8_t> S(const Bytes& b) { return std::span<const uint8_t>(b.data(), b.size()); }

// Python's f'{v:#x}'
std::string Hex(int64_t v) {
  char buf[32];
  if (v < 0) {
    std::snprintf(buf, sizeof buf, "-0x%llx", static_cast<unsigned long long>(-v));
  } else {
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  }
  return buf;
}
std::string Dec(int64_t v) { return std::to_string(v); }

// Python's repr of a list of ints
template <class T>
std::string ListRepr(const std::vector<T>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ", ";
    s += Dec(static_cast<int64_t>(v[i]));
  }
  return s + "]";
}

// Python's repr of the cat directory, [(type, value), ...]
std::string DirRepr(const std::vector<std::pair<uint8_t, uint32_t>>& ents) {
  std::string s = "[";
  for (size_t i = 0; i < ents.size(); ++i) {
    if (i) s += ", ";
    s += "(" + Dec(ents[i].first) + ", " + Dec(ents[i].second) + ")";
  }
  return s + "]";
}

bool AnyNonZero(std::span<const uint8_t> s) {
  for (uint8_t b : s)
    if (b) return true;
  return false;
}

// struct.unpack('>12I', blob): exactly 48 bytes
std::array<uint32_t, 12> Unpack12(const Bytes& blob) {
  if (blob.size() != 48) Fail("unpack requires a buffer of 48 bytes");
  const Reader r(S(blob));
  std::array<uint32_t, 12> w{};
  for (int i = 0; i < 12; ++i) w[i] = r.U32(4 * i);
  return w;
}

// ---------------------------------------------------------------- reader helpers
// _users (slim_addon.py)
std::vector<size_t> Users(const tpr::Xpr& x) {
  const auto u = x.OfType(kUser);
  if (u.size() != 4 || u != std::vector<size_t>{0, 1, 2, 3}) Fail("USER resources at " + ListRepr(u) + ", expected 0..3");
  return u;
}

struct User0 {
  uint32_t ntex, nvb, nib, first_tx, first_vb, first_ib;
};
// _user0
User0 ComputeUser0(const tpr::Xpr& x) {
  User0 u{0, 0, 0, 0, 0, 0};
  bool seen_tx = false, seen_vb = false, seen_ib = false;
  for (size_t i = 0; i < x.res.size(); ++i) {
    const Fourcc& t = x.res[i].type;
    if (t == kTx2d) {
      if (!seen_tx) u.first_tx = static_cast<uint32_t>(i), seen_tx = true;
      ++u.ntex;
    } else if (t == kVbuf) {
      if (!seen_vb) u.first_vb = static_cast<uint32_t>(i), seen_vb = true;
      ++u.nvb;
    } else if (t == kIbuf) {
      if (!seen_ib) u.first_ib = static_cast<uint32_t>(i), seen_ib = true;
      ++u.nib;
    }
  }
  return u;
}

// _check_user0
void CheckUser0(const tpr::Xpr& x, const std::string& who) {
  const auto w = Unpack12(x.res[0].blob);
  const User0 u = ComputeUser0(x);
  const std::array<uint32_t, 12> want = {7, 4, u.ntex, u.ntex ? u.first_tx : 4u, u.nvb, u.first_vb,
                                          u.nib, u.first_ib, 1, 2, 3, 16};
  if (w != want) {
    Fail(who + ": USER0 " + ListRepr(std::vector<uint32_t>(w.begin(), w.end())) + " does not describe its resource list " +
         ListRepr(std::vector<uint32_t>(want.begin(), want.end())));
  }
}

// _set_user0
void SetUser0(tpr::Xpr& x) {
  auto w = Unpack12(x.res[0].blob);
  const User0 u = ComputeUser0(x);
  w[2] = u.ntex;
  w[4] = u.nvb;
  w[5] = u.first_vb;
  w[6] = u.nib;
  w[7] = u.first_ib;
  Writer wr;
  for (uint32_t v : w) wr.U32(v);
  x.res[0].blob = wr.Take();
}

// _res_bases: (resource index, base) of every texture and buffer
std::vector<std::pair<size_t, uint32_t>> ResBases(const tpr::Xpr& x) {
  std::vector<std::pair<size_t, uint32_t>> out;
  for (size_t i = 0; i < x.res.size(); ++i) {
    const auto& r = x.res[i];
    if (r.type == kTx2d) {
      out.emplace_back(i, TexAddrs(S(r.blob)).first);
    } else if (r.type == kVbuf || r.type == kIbuf) {
      out.emplace_back(i, BufSpan(S(r.blob)).first);
    }
  }
  return out;
}

// _remove_resources(x, drop, who)
void RemoveResources(tpr::Xpr& x, const std::set<size_t>& drop, const std::string& who) {
  std::set<int64_t> base_set;
  for (const auto& [i, b] : ResBases(x)) base_set.insert(b);
  const int64_t data_len = static_cast<int64_t>(x.data.size());
  base_set.insert(data_len);
  std::vector<std::pair<int64_t, int64_t>> spans;
  for (size_t i : drop) {  // sorted(drop)
    if (i >= x.res.size()) Fail("IndexError: list index out of range");
    const int64_t base = BufSpan(S(x.res[i].blob)).first;
    const auto it = base_set.upper_bound(base);
    if (it == base_set.end()) Fail("ValueError: min() arg is an empty sequence");
    const int64_t nxt = *it;
    if (base % 0x1000 || (nxt % 0x1000 && nxt != data_len))
      Fail(who + ": copy buffer span " + Hex(base) + ".." + Hex(nxt) + " not on 4 KiB boundaries");
    spans.emplace_back(base, nxt);
  }
  for (size_t i = 0; i < x.res.size(); ++i) {
    const Fourcc& t = x.res[i].type;
    if (drop.count(i) || !(t == kVbuf || t == kIbuf || t == kTx2d)) continue;
    const int64_t base = t != kTx2d ? BufSpan(S(x.res[i].blob)).first : TexAddrs(S(x.res[i].blob)).first;
    for (const auto& [a, e] : spans)
      if (a <= base && base < e) Fail(who + ": resource " + Dec(static_cast<int64_t>(i)) + " shares a span with a copy buffer");
  }
  std::sort(spans.begin(), spans.end());
  const auto moved = [&spans](int64_t v) {
    int64_t s = 0;
    for (const auto& [a, e] : spans)
      if (e <= v) s += e - a;
    return v - s;
  };
  // x.data[p:a] and x.data[p:] as Python slices them: both ends clamped to the data's length. A span's base and end
  // come from descriptors and may lie past the data end (a base above len(data) whose next base is higher still).
  const auto slice = [&x, data_len](Bytes& out, int64_t from, int64_t to) {
    from = std::min(from, data_len);
    to = std::min(to, data_len);
    if (to > from) out.insert(out.end(), x.data.begin() + from, x.data.begin() + to);
  };
  Bytes data;
  int64_t p = 0;
  for (const auto& [a, e] : spans) {
    slice(data, p, a);
    p = e;
  }
  slice(data, p, data_len);
  for (size_t i = 0; i < x.res.size(); ++i) {
    if (drop.count(i)) continue;
    auto& r = x.res[i];
    if (r.type == kVbuf || r.type == kIbuf) {
      const auto [base, size] = BufSpan(S(r.blob));
      r.blob = BufSet(S(r.blob), moved(base), size);
    } else if (r.type == kTx2d) {
      const auto [base, mip] = TexAddrs(S(r.blob));
      r.blob = TexSet(S(r.blob), moved(base), mip ? moved(mip) : 0);
    }
  }
  std::vector<tpr::Res> kept;
  for (size_t i = 0; i < x.res.size(); ++i)
    if (!drop.count(i)) kept.push_back(std::move(x.res[i]));
  x.res = std::move(kept);
  x.data = std::move(data);
}

// ---------------------------------------------------------------- cat helpers
uint32_t U32(std::span<const uint8_t> c, int64_t o) { return Reader(c).U32(o); }

// _apa_fields(c, apa): (start of the apa block's span, positions of its absolute offsets)
std::pair<int64_t, std::vector<int64_t>> ApaFields(std::span<const uint8_t> c, int64_t apa) {
  if (U32(c, apa) != 0x617061) Fail("type-5 block at " + Hex(apa) + " is not an apa block");
  const uint32_t count = U32(c, apa + 4);
  std::vector<int64_t> entry_at, fields, lows{apa};
  for (uint32_t i = 0; i < count; ++i) entry_at.push_back(U32(c, apa + 8 + 4 * static_cast<int64_t>(i)));
  for (uint32_t i = 0; i < count; ++i) fields.push_back(apa + 8 + 4 * static_cast<int64_t>(i));
  for (int64_t e : entry_at) {
    U32(c, e + 12);  // unpack_from('>4I') needs all 16 bytes
    const int64_t mp = U32(c, e + 4), rp = U32(c, e + 12);
    const uint32_t nrules = U32(c, e + 8);
    lows.push_back(e);
    lows.push_back(mp);
    if (rp) {
      lows.push_back(rp);
      for (uint32_t k = 0; k < nrules; ++k) {
        lows.push_back(U32(c, rp + 8 * static_cast<int64_t>(k) + 4));
        fields.push_back(rp + 8 * static_cast<int64_t>(k) + 4);
      }
    }
    fields.push_back(e + 4);
    fields.push_back(e + 12);
  }
  return {*std::min_element(lows.begin(), lows.end()), fields};
}

// _map_in_place(c, new_map, ents)
Bytes MapInPlace(std::span<const uint8_t> c, const Bytes& new_map, const std::vector<std::pair<uint8_t, uint32_t>>& ents) {
  const uint64_t end = ParseMap(c, 0x18).second;
  const int64_t old = static_cast<int64_t>(end) - 0x18;
  if (static_cast<int64_t>(new_map.size()) > old) {
    Fail("cat directory " + DirRepr(ents) + " not of the converter shape: new type-2 map " +
         Dec(static_cast<int64_t>(new_map.size())) + " bytes is longer than the old map " + Dec(old) + " bytes");
  }
  const auto head18 = Reader(c).Slice(0, 0x18);  // c[:0x18], clamped
  Bytes out(head18.begin(), head18.end());
  out.insert(out.end(), new_map.begin(), new_map.end());
  out.insert(out.end(), static_cast<size_t>(old - static_cast<int64_t>(new_map.size())), uint8_t(0));
  const auto rest = Reader(c).From(end);
  out.insert(out.end(), rest.begin(), rest.end());
  return out;
}

Bytes CatWithMapBytes(std::span<const uint8_t> c, const Bytes& new_map) {
  const Reader r(c);
  const auto head = r.Slice(0, 4);
  static constexpr uint8_t kCat[4] = {'C', 'A', 'T', 0};
  if (head.size() != 4 || !std::equal(head.begin(), head.end(), kCat) || r.U32(0x10) != 0)
    Fail("cat header not of the converter shape");
  const int64_t dir2 = r.U32(4);
  const auto ents = tpr::CatDir(c);
  std::vector<uint8_t> types;
  std::map<uint8_t, uint32_t> d;  // dict(ents): the last value of a type wins
  for (const auto& [t, v] : ents) {
    types.push_back(t);
    d[t] = v;
  }
  const auto get = [&d](uint8_t t) -> std::optional<uint32_t> {
    const auto it = d.find(t);
    return it == d.end() ? std::nullopt : std::optional<uint32_t>(it->second);
  };
  bool shape_bad = get(2) != std::optional<uint32_t>(0x18);
  if (!shape_bad) shape_bad = types[0] != 2;
  if (!shape_bad) shape_bad = types.back() != 5;
  if (!shape_bad) shape_bad = std::find(types.begin(), types.end() - 1, uint8_t(5)) != types.end() - 1;
  if (!shape_bad)
    for (uint8_t t : types)
      if (t != 2 && t != 4 && t != 5 && t != 6 && t != 9) shape_bad = true;
  if (shape_bad) {
    if (get(2) == std::optional<uint32_t>(0x18)) return MapInPlace(c, new_map, ents);
    Fail("cat directory " + DirRepr(ents) + " not of the converter shape");
  }
  const int64_t end = static_cast<int64_t>(ParseMap(c, 0x18).second);
  if (AnyNonZero(r.Slice(end, Al(end, 8)))) Fail("bytes after the type-2 map");
  const auto [B, fields] = ApaFields(c, d.at(5));
  std::vector<int64_t> heads;
  for (int64_t o : {4, 8, 0xC, 0x14})
    if (r.U32(o)) heads.push_back(o);
  bool after = B % 16 != 0;
  for (int64_t o : heads) after = after || r.U32(o) < B;
  for (int64_t f : fields) after = after || f < B;
  if (after) Fail("attached cat parts not after the apa block");
  const int64_t a0 = static_cast<int64_t>(Al(end, 8));
  for (uint8_t t : {uint8_t(4), uint8_t(6)}) {
    const auto v = get(t);
    if (v && !(a0 <= static_cast<int64_t>(*v) && static_cast<int64_t>(*v) < B))
      Fail("cat type " + Dec(t) + " at " + Hex(*v) + " outside " + Hex(a0) + ".." + Hex(B));
  }
  Bytes d1;
  {
    const auto s = r.Slice(dir2, dir2 + 8 * (static_cast<int64_t>(ents.size()) - 1));
    d1.assign(s.begin(), s.end());
    d1.insert(d1.end(), 8, uint8_t(0));
  }
  const int64_t n1 = static_cast<int64_t>(d1.size());
  std::vector<int64_t> cand;
  for (int64_t p = a0; p < B - n1 + 1; p += 8) {
    const auto at = r.Slice(p, p + n1);
    if (at.size() == d1.size() && std::equal(at.begin(), at.end(), d1.begin()) &&
        static_cast<int64_t>(Al(p + n1, 16)) == B && !AnyNonZero(r.Slice(p + n1, B)))
      cand.push_back(p);
  }
  if (cand.size() != 1) Fail("first cat directory not found once before " + Hex(B) + " (" + ListRepr(cand) + ")");
  const int64_t x = cand[0] + n1;
  const int64_t e2 = 0x18 + static_cast<int64_t>(new_map.size());
  const int64_t dA = static_cast<int64_t>(Al(e2, 8)) - a0;
  const int64_t nB = static_cast<int64_t>(Al(x + dA, 16));
  const int64_t dB = nB - B;
  const auto head18 = Reader(c).Slice(0, 0x18);  // c[:0x18], clamped
  Bytes out(head18.begin(), head18.end());
  out.insert(out.end(), new_map.begin(), new_map.end());
  out.resize(static_cast<size_t>(Al(e2, 8)), 0);
  {
    const auto s = r.Slice(a0, x);
    out.insert(out.end(), s.begin(), s.end());
  }
  if (static_cast<int64_t>(out.size()) < nB) out.resize(static_cast<size_t>(nB), 0);
  {
    const auto s = r.From(B);
    out.insert(out.end(), s.begin(), s.end());
  }
  if (static_cast<int64_t>(out.size()) != static_cast<int64_t>(c.size()) + dB) Fail("assert len(out) == len(c) + dB");
  const auto move_dir = [&](int64_t p_new, int64_t n) {
    for (int64_t k = 0; k < n; ++k) {
      const int64_t q = p_new + 8 * k;
      const Reader ro(S(out));
      const uint8_t t = ro.U8(q + 2);
      const int64_t v = ro.U32(q + 4);
      if (t == 4 || t == 6) {
        be::PackIntoU32(out, q + 4, be::CheckU32S(v + dA));
      } else if (t == 5) {
        be::PackIntoU32(out, q + 4, be::CheckU32S(v + dB));
      }
    }
  };
  move_dir(cand[0] + dA, static_cast<int64_t>(ents.size()) - 1);
  move_dir(dir2 + dB, static_cast<int64_t>(ents.size()));
  for (int64_t o : heads) be::PackIntoU32(out, o, be::CheckU32S(static_cast<int64_t>(r.U32(o)) + dB));
  for (int64_t f : fields) {
    const int64_t v = r.U32(f);
    if (v) be::PackIntoU32(out, f + dB, be::CheckU32S(v + dB));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- type-2 map
// convert.parse_t2
std::pair<std::vector<MapGroup>, uint64_t> ParseMap(std::span<const uint8_t> c, uint64_t off) {
  const Reader r(c);
  std::vector<MapGroup> groups;
  MapGroup cur;  // its kind is the kind of the group being read ('main' first)
  int64_t p = static_cast<int64_t>(off);
  for (;;) {
    const uint8_t b = r.U8(p);
    if (b & 0x80) {
      if ((b & 0xF0) == 0x90 || (b & 0xF0) == 0xA0) {
        cur.items.emplace_back(r.U16(p), r.U8(p + 2));
        p += 3;
      } else if ((b & 0xF0) == 0xD0) {
        const auto s = r.Slice(p, p + 3);  // int.from_bytes(c[p:p + 3], 'big'): a clamped slice
        uint32_t code = 0;
        for (uint8_t v : s) code = (code << 8) | v;
        cur.items.emplace_back(code, r.U8(p + 3));
        p += 4;
      } else {
        char buf[96];
        std::snprintf(buf, sizeof buf, "type-2 map: unknown entry code 0x%02x at %s", b, Hex(p).c_str());
        Fail(buf);
      }
    } else {
      groups.push_back(std::move(cur));
      cur = MapGroup{};
      if (b == 0) return {std::move(groups), static_cast<uint64_t>(p + 1)};
      if (b == 0x60) {
        const uint8_t nn = r.U8(p + 1);
        // seen: 0x10-0x12, 0x20, 0x21; a 00 or top-bit byte here would be an end byte or an entry swallowed
        if (nn == 0 || (nn & 0x80)) {
          char buf[96];
          std::snprintf(buf, sizeof buf, "type-2 map: state switch 60 %02x at %s", nn, Hex(p).c_str());
          Fail(buf);
        }
        cur.kind = MapGroup::Kind::kState;
        cur.value = nn;
        p += 2;
      } else {
        cur.kind = MapGroup::Kind::kMarker;
        cur.value = b;
        p += 1;
      }
    }
  }
}

// convert.t2_bytes; the assert texts are str() of the assert's tuple, as AssertionError prints it
Bytes T2Bytes(const MapGroup& g) {
  if (g.kind == MapGroup::Kind::kState && !(g.value > 0 && g.value < 0x80))
    Fail("('type-2 group kind out of range', ('state', " + Dec(g.value) + "))");
  if (g.kind == MapGroup::Kind::kMarker && !(g.value > 0 && g.value < 0x80 && g.value != 0x60))
    Fail("('type-2 group kind out of range', " + Dec(g.value) + ")");
  for (const auto& [code, id] : g.items)
    if (!((0x9000 <= code && code <= 0xAFFF) || (0xD00000 <= code && code <= 0xDFFFFF)))
      Fail("('type-2 entry out of range', '" + Hex(code) + "', " + Dec(id) + ")");
  Writer w;
  if (g.kind == MapGroup::Kind::kState) {
    w.U8(0x60);
    w.U8(g.value);
  } else if (g.kind == MapGroup::Kind::kMarker) {
    w.U8(g.value);
  }
  for (const auto& [code, id] : g.items) {
    if (code > 0xFFFF) {
      w.U8(code >> 16);
      w.U8((code >> 8) & 0xFF);
      w.U8(code & 0xFF);
    } else {
      w.U16(code);
    }
    w.U8(id);
  }
  return w.Take();
}

// slim_addon.map_bytes
Bytes MapBytes(std::span<const MapGroup> groups) {
  Bytes out;
  for (const auto& g : groups) {
    const Bytes b = T2Bytes(g);
    out.insert(out.end(), b.begin(), b.end());
  }
  out.push_back(0);
  return out;
}

// slim_addon.cat_with_map(cat, map_bytes(groups))
Bytes CatWithMap(std::span<const uint8_t> cat, std::span<const MapGroup> map) {
  return CatWithMapBytes(cat, MapBytes(map));
}

// tpr.buf_set
Bytes BufSet(std::span<const uint8_t> desc, int64_t base, int64_t size) {
  const Reader r(desc);
  const int64_t at = static_cast<int64_t>(be::ResolveOffset(static_cast<int64_t>(desc.size()) - 8, desc.size(), 8));
  const uint32_t w0 = r.U32(at), w1 = r.U32(at + 4);
  if (base % 4 != 0 || size % 4 != 0) Fail("assert base % 4 == 0 and size % 4 == 0");
  Bytes out(desc.begin(), desc.begin() + at);
  Writer w;
  w.U32(be::CheckU32S(static_cast<int64_t>(w0 & 3) | base));
  w.U32(be::CheckU32S(static_cast<int64_t>(w1 & 3) | size));
  out.insert(out.end(), w.bytes().begin(), w.bytes().end());
  return out;
}

// ---------------------------------------------------------------- read_suit
Suit ReadSuit(std::string_view name_in, std::span<const uint8_t> tpr_bytes, std::span<const uint8_t> cat_bytes) {
  Suit s;
  s.name = std::string(name_in);
  const std::string& name = s.name;
  s.tpr.assign(tpr_bytes.begin(), tpr_bytes.end());
  s.cat.assign(cat_bytes.begin(), cat_bytes.end());
  tpr::Xpr x = tpr::Xpr::Parse(S(s.tpr));
  const auto u = Users(x);
  CheckUser0(x, name);
  s.geo = tpr::ParseMdlGeo(S(x.res[u[1]].blob));
  s.info = tpr::ParseMdlInfo(S(x.res[u[2]].blob));
  s.nodes = tpr::ParseHie(S(x.res[u[3]].blob));
  {  // ref_groups_of: parse_t2 at dict(cat_dir(cat))[2]
    std::optional<uint32_t> t2;
    for (const auto& [t, v] : tpr::CatDir(S(s.cat)))
      if (t == 2) t2 = v;
    if (!t2) Fail("KeyError: 2");
    s.groups = ParseMap(S(s.cat), *t2).first;
  }
  s.main = s.groups.at(0).items;
  s.stripped = "nothing";
  std::set<int64_t> cid_set;
  for (size_t gi = 1; gi < s.groups.size(); ++gi) {
    const auto& g = s.groups[gi];
    if (g.kind == MapGroup::Kind::kMarker && (g.value == 2 || g.value == 4))
      for (const auto& [code, id] : g.items) cid_set.insert(id);
  }
  const std::vector<int64_t> copy_ids(cid_set.begin(), cid_set.end());
  if (!copy_ids.empty()) {
    const int64_t K = static_cast<int64_t>(s.nodes.size());
    const int64_t n = static_cast<int64_t>(copy_ids.size());
    bool top = static_cast<int64_t>(s.geo.size()) == K && static_cast<int64_t>(s.info.size()) == K;
    for (int64_t k = 0; top && k < n; ++k) top = copy_ids[k] == K - n + k;
    if (!top)
      Fail(name + ": old copies " + Dec(copy_ids.front()) + ".." + Dec(copy_ids.back()) + " are not the top " + Dec(n) +
           " ids of the " + Dec(K) + " nodes/objects");
    for (int64_t k = 0; k < K; ++k) {
      // the pairing premise: object k is node id k (MdlGeo word 1, MdlInfo word 1)
      const uint32_t gn = s.geo[k].NodeIndex();
      if (gn != k) Fail(name + ": object " + Dec(k) + ": MdlGeo node word " + Dec(gn) + ", not its index");
      const uint32_t iw = s.info[k].words[1];
      if (iw != k) Fail(name + ": object " + Dec(k) + ": MdlInfo word 1 is " + Dec(iw) + ", not its index");
    }
    {
      std::vector<int64_t> ids;
      for (const auto& nd : s.nodes) ids.push_back(nd.id);
      std::sort(ids.begin(), ids.end());
      bool ok = true;
      for (int64_t k = 0; k < K; ++k) ok = ok && ids[k] == k;
      if (!ok) Fail(name + ": Hie node ids are not 0.." + Dec(K - 1) + ", each once");
    }
    for (const auto& nd : s.nodes) {
      if (!(-1 <= nd.parent && nd.parent < K))
        Fail(name + ": Hie node " + Dec(nd.id) + " has its parent at position " + Dec(nd.parent) + ", outside 0.." +
             Dec(K - 1));
      for (int64_t c : nd.children)
        if (!(0 <= c && c < K)) {
          Fail(name + ": Hie node " + Dec(nd.id) + " has a child at position " + Dec(c) + ", outside 0.." + Dec(K - 1));
        }
    }
    for (const auto& nd : s.nodes) {
      const std::optional<int64_t> par =
          nd.parent >= 0 ? std::optional<int64_t>(s.nodes[nd.parent].id) : std::nullopt;
      if (cid_set.count(nd.id)) {
        if (par && !cid_set.count(*par)) Fail(name + ": old copy node " + Dec(nd.id) + " has a kept parent " + Dec(*par));
        for (int64_t c : nd.children)
          if (!cid_set.count(s.nodes[c].id))
            Fail(name + ": old copy node " + Dec(nd.id) + " has a kept child " + Dec(s.nodes[c].id));
      } else {
        if (par && cid_set.count(*par)) Fail(name + ": kept node " + Dec(nd.id) + " has an old-copy parent " + Dec(*par));
        for (int64_t c : nd.children)
          if (cid_set.count(s.nodes[c].id)) Fail(name + ": kept node " + Dec(nd.id) + " has a child among the old copies");
      }
    }
    std::vector<int64_t> kept_pos;
    for (int64_t p = 0; p < K; ++p)
      if (!cid_set.count(s.nodes[p].id)) kept_pos.push_back(p);
    std::vector<tpr::Node> keep;
    bool leading = static_cast<int64_t>(kept_pos.size()) == K - n;
    for (size_t k = 0; leading && k < kept_pos.size(); ++k) leading = kept_pos[k] == static_cast<int64_t>(k);
    if (leading) {
      keep.assign(s.nodes.begin(), s.nodes.begin() + (K - n));  // the copies are the last n positions
    } else {
      std::map<int64_t, int64_t> new_pos;
      for (size_t k = 0; k < kept_pos.size(); ++k) new_pos[kept_pos[k]] = static_cast<int64_t>(k);
      for (int64_t p : kept_pos) {
        tpr::Node nd = s.nodes[p];
        nd.parent = nd.parent >= 0 ? new_pos.at(nd.parent) : -1;
        for (auto& c : nd.children) c = new_pos.at(c);
        keep.push_back(std::move(nd));
      }
    }
    for (const auto& [code, id] : s.main)
      if (code && cid_set.count(id)) Fail(name + ": the main group names an old copy node");
    const auto vbr = x.OfType(kVbuf), ibr = x.OfType(kIbuf);
    std::set<uint32_t> keep_vb, keep_ib, drop_vb, drop_ib;
    for (int64_t k = 0; k < K; ++k)
      for (const auto& dc : s.geo[k].decls) {
        if (dc.tails.empty()) Fail("IndexError: list index out of range");
        (k < K - n ? keep_vb : drop_vb).insert(dc.Vb());
        (k < K - n ? keep_ib : drop_ib).insert(dc.head[2]);
      }
    bool shared = false;
    for (uint32_t v : keep_vb) shared = shared || drop_vb.count(v);
    for (uint32_t i : keep_ib) shared = shared || drop_ib.count(i);
    if (shared) Fail(name + ": a buffer is shared by a kept object and an old copy");
    std::set<size_t> drop;
    for (uint32_t v : drop_vb) {
      if (v >= vbr.size()) Fail("IndexError: list index out of range");
      drop.insert(vbr[v]);
    }
    for (uint32_t i : drop_ib) {
      if (i >= ibr.size()) Fail("IndexError: list index out of range");
      drop.insert(ibr[i]);
    }
    std::map<uint32_t, uint32_t> vmap, imap;
    for (uint32_t v = 0, k = 0; v < vbr.size(); ++v)
      if (!drop_vb.count(v)) vmap[v] = k++;
    for (uint32_t i = 0, k = 0; i < ibr.size(); ++i)
      if (!drop_ib.count(i)) imap[i] = k++;
    RemoveResources(x, drop, name);
    for (int64_t k = 0; k < K - n; ++k)
      for (auto& dc : s.geo[k].decls) {
        const auto vi = vmap.find(dc.Vb());
        const auto ii = imap.find(dc.head[2]);
        if (vi == vmap.end() || ii == imap.end()) Fail("KeyError: buffer index");
        Writer w;
        w.U32(vi->second);
        Bytes t = w.Take();
        t.insert(t.end(), dc.tails[0].begin() + 4, dc.tails[0].end());
        dc.tails[0] = std::move(t);
        dc.head[2] = ii->second;
      }
    s.geo.resize(static_cast<size_t>(K - n));
    s.info.resize(static_cast<size_t>(K - n));
    s.nodes = std::move(keep);
    SetUser0(x);
    s.stripped = "old copies ids " + Dec(copy_ids.front()) + ".." + Dec(copy_ids.back()) + ", " +
                 Dec(static_cast<int64_t>(drop.size())) + " buffers";
  }
  s.x = std::move(x);
  return s;
}

// slim_addon.rebuild (with _container)
Bytes Rebuild(const Suit& suit) {
  tpr::Xpr x;
  x.res = suit.x.res;
  x.data = suit.x.data;
  if (x.res.size() < 4) Fail("IndexError: list assignment index out of range");
  x.res[1].blob = tpr::BuildMdlGeo(suit.geo);
  x.res[2].blob = tpr::BuildMdlInfo(suit.info);
  x.res[3].blob = tpr::BuildHie(suit.nodes);
  return x.Build(1);
}

}  // namespace doa4::suitconv::slim
