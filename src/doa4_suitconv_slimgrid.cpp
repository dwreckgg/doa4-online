// Suit slim copies, Task 4: the grid (vertex-cluster) merge and the cell-size bisection, a port of slim.py
// (doax2_work/physics_spike/scripts/slim.py: origin, _vertex, merge, count, choose_h; see doa4_suitconv_slim.h).
//
// Every output order is decided by traversal order, as in slim.py: keys in first-traversal order, cells numbered in
// first-appearance order inside a key, kept faces in traversal order, output vertices in first-reference order over the
// kept faces. The hash tables below are for lookup only; nothing iterates them, so no hash order reaches the output.
// The arithmetic is slim.py's: the cell is floor((p - o) / h) of a double per axis, the cell position is a plain
// sequential sum from 0.0 over the cell's members divided by their number (not a compensated sum).
//
// Speed (fix round 1): the tables are flat open-addressing arrays sized from the triangle count (no node allocation),
// and ChooseH's bisection probes use a count-only pass (Counter) that builds no vertex, payload or face: slim.count of
// merge(tris, h, org) is, per key, the number of distinct cells that a non-degenerate face references (a dropped
// duplicate face references the cells of the kept face it repeats, and membership by vid only decides positions). Only
// the merge ChooseH returns is built in full.

#include "doa4_suitconv_fp.h"

#include "doa4_suitconv_slim.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace doa4::suitconv::slim {

namespace {

[[noreturn]] void Fail(const std::string& what) { throw ConvertError(what); }

constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

uint64_t Mix(uint64_t x) {  // splitmix64's finaliser
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

// A power-of-two capacity that keeps the load at or under 2/3 for n entries.
size_t Capacity(size_t n) {
  size_t c = 16;
  while (c < n + n / 2 + 1) c <<= 1;
  return c;
}

// Open-addressing table Key -> uint32_t (linear probing); n bounds the number of entries ever inserted. Lookup only.
template <class Key>
class Flat {
 public:
  explicit Flat(size_t n) : mask_(Capacity(n) - 1), keys_(mask_ + 1), vals_(mask_ + 1, kNone) {}
  // The value stored for k; when k is absent, stores v and returns it with *added = true.
  uint32_t Get(const Key& k, uint64_t hash, uint32_t v, bool* added) {
    size_t i = static_cast<size_t>(hash) & mask_;
    while (true) {
      if (vals_[i] == kNone) {
        keys_[i] = k;
        vals_[i] = v;
        *added = true;
        return v;
      }
      if (keys_[i] == k) {
        *added = false;
        return vals_[i];
      }
      i = (i + 1) & mask_;
    }
  }

 private:
  size_t mask_;
  std::vector<Key> keys_;
  std::vector<uint32_t> vals_;
};

template <size_t N, class T>
uint64_t HashOf(const std::array<T, N>& k) {
  uint64_t h = 0x9e3779b97f4a7c15ull;
  for (size_t i = 0; i < N; ++i) h = Mix(h ^ static_cast<uint64_t>(k[i]));
  return h;
}

uint64_t DBits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, sizeof u);
  return u;
}

// One cell's members, accumulated in member order (slim.py _Bucket.members and _vertex).
struct CellAcc {
  std::array<double, 3> sum{0.0, 0.0, 0.0};  // s = 0.0; s += m.pos[a] per member, in first-appearance order
  uint32_t n = 0;
  std::array<double, 3> first_pos{};  // identity mode: members[0]'s position, no averaging
  uint32_t first_payload = 0;         // the payload is the first member's
};

using Triple = std::array<uint32_t, 3>;

struct Bucket {
  uint32_t key = 0;
  std::vector<CellAcc> cells;  // per cell index, first-appearance order
  std::vector<Triple> faces;   // kept faces, traversal order, winding kept
};

// math.floor((p - o) / h) as Python evaluates it: ZeroDivisionError for h == 0, ValueError for NaN, OverflowError for
// an infinity. The cell is the floored double's bits: a finite floor is an exact integer, so two are equal as doubles
// iff Python's ints are (no range limit); + 0.0 turns floor(-0.0) = -0.0 into 0.0, Python's int 0 like floor(0.0).
uint64_t CellCoord(double p, double o, double h) {
  // the text of py -3 = CPython 3.14.7 (older ones said "float division by zero")
  if (h == 0.0) Fail("ZeroDivisionError: division by zero");
  const double q = (p - o) / h;
  if (std::isnan(q)) Fail("ValueError: cannot convert float NaN to integer");
  if (std::isinf(q)) Fail("OverflowError: cannot convert float infinity to integer");
  return DBits(std::floor(q) + 0.0);
}

// slim.count(slim.merge(tris, h, org)) without building the merge, for ChooseH's probes. Set up once per ChooseH:
// the distinct positions (by their bits) with p - org per axis (the same subtraction merge does), and the triangles'
// position indices grouped by key. Count(h) gives the count, or false when some cell is not finite (NaN or an
// infinity); the caller then runs Merge, which raises Python's error at Python's corner.
class Counter {
 public:
  Counter(std::span<const Tri> tris, const std::array<double, 3>& org) {
    const size_t nt = tris.size();
    Flat<std::array<uint64_t, 3>> pos_of(3 * nt);
    Flat<uint32_t> group_of(nt);
    std::vector<uint32_t> group(nt);
    std::vector<uint32_t> group_size;
    std::vector<uint32_t> tp(3 * nt);
    for (size_t t = 0; t < nt; ++t) {
      for (int c = 0; c < 3; ++c) {
        const auto& p = tris[t].pos[c];
        const std::array<uint64_t, 3> k{DBits(p[0]), DBits(p[1]), DBits(p[2])};
        bool added;
        const uint32_t i = pos_of.Get(k, HashOf(k), static_cast<uint32_t>(d_[0].size()), &added);
        if (added) {
          for (int a = 0; a < 3; ++a) d_[a].push_back(p[a] - org[a]);
        }
        tp[3 * t + c] = i;
      }
      bool added;
      const uint32_t g = group_of.Get(tris[t].key, Mix(tris[t].key), static_cast<uint32_t>(group_size.size()), &added);
      if (added) group_size.push_back(0);
      ++group_size[g];
      group[t] = g;
    }
    // triangles grouped by key (a counting sort; the count does not depend on the order inside a group)
    group_end_.resize(group_size.size());
    std::vector<uint32_t> at(group_size.size());
    uint32_t run = 0;
    for (size_t g = 0; g < group_size.size(); ++g) {
      at[g] = run;
      run += group_size[g];
      group_end_[g] = run;
    }
    tri_.resize(3 * nt);
    for (size_t t = 0; t < nt; ++t) {
      const uint32_t s = at[group[t]]++;
      for (int c = 0; c < 3; ++c) tri_[3 * s + c] = tp[3 * t + c];
    }
    const size_t np = d_[0].size();
    for (int a = 0; a < 3; ++a) f_[a].resize(np);
    cid_.resize(np);
    stamp_.assign(np, 0);
    mask_ = Capacity(np) - 1;
    slot_key_.resize(mask_ + 1);
    slot_gen_.assign(mask_ + 1, 0);
    slot_id_.resize(mask_ + 1);
  }

  bool Count(double h, size_t* out) {
    const size_t np = d_[0].size();
    bool ok = true;
    for (int a = 0; a < 3; ++a) {
      const double* d = d_[a].data();
      double* f = f_[a].data();
      for (size_t i = 0; i < np; ++i) {
        const double v = std::floor(d[i] / h) + 0.0;  // + 0.0: floor(-0.0) is the cell of 0 (Python's int 0)
        ok &= (v - v == 0.0);  // false for NaN and infinities
        f[i] = v;
      }
    }
    if (!ok) return false;
    // cell ids: equal floors (all three axes) get one id; a generation stamp empties the table without clearing it
    ++gen_;
    uint32_t next = 0;
    for (size_t i = 0; i < np; ++i) {
      const std::array<uint64_t, 3> k{DBits(f_[0][i]), DBits(f_[1][i]), DBits(f_[2][i])};
      size_t s = static_cast<size_t>(Mix(k[0] + k[1] * 0x9e3779b97f4a7c15ull + k[2] * 0xc2b2ae3d27d4eb4full)) & mask_;
      while (true) {
        if (slot_gen_[s] != gen_) {
          slot_gen_[s] = gen_;
          slot_key_[s] = k;
          slot_id_[s] = next;
          cid_[i] = next++;
          break;
        }
        if (slot_key_[s] == k) {
          cid_[i] = slot_id_[s];
          break;
        }
        s = (s + 1) & mask_;
      }
    }
    // per key, the distinct cells a non-degenerate face references
    size_t n = 0;
    uint32_t begin = 0;
    for (const uint32_t end : group_end_) {
      const uint32_t mark = ++mark_;
      for (uint32_t t = begin; t < end; ++t) {
        const uint32_t a = cid_[tri_[3 * t]], b = cid_[tri_[3 * t + 1]], c = cid_[tri_[3 * t + 2]];
        if (a == b || b == c || a == c) continue;
        for (const uint32_t x : {a, b, c}) {
          if (stamp_[x] != mark) {
            stamp_[x] = mark;
            ++n;
          }
        }
      }
      begin = end;
    }
    *out = n;
    return true;
  }

 private:
  std::vector<double> d_[3];          // per distinct position: p - org
  std::vector<uint32_t> tri_;         // 3 position indices per triangle, grouped by key
  std::vector<uint32_t> group_end_;   // end of each key's run in tri_ (in triangles)
  std::vector<double> f_[3];          // per position: the cell's floor per axis, this probe
  std::vector<uint32_t> cid_;         // per position: the cell id, this probe
  std::vector<uint32_t> stamp_;       // per cell id: the last key run that counted it
  uint32_t mark_ = 0;
  size_t mask_ = 0;
  std::vector<std::array<uint64_t, 3>> slot_key_;
  std::vector<uint32_t> slot_gen_;
  std::vector<uint32_t> slot_id_;
  uint32_t gen_ = 0;
};

}  // namespace

// slim.origin
std::array<double, 3> Origin(std::span<const Tri> tris) {
  bool have = false;
  std::array<double, 3> lo{};
  for (const Tri& t : tris) {
    for (const auto& pos : t.pos) {
      if (!have) {
        lo = pos;
        have = true;
      } else {
        for (int a = 0; a < 3; ++a) {
          if (pos[a] < lo[a]) lo[a] = pos[a];
        }
      }
    }
  }
  if (!have) Fail("origin: no triangles");
  return lo;
}

// slim.merge
Merged Merge(std::span<const Tri> tris, std::optional<double> h, const std::array<double, 3>& org) {
  const size_t nt = tris.size();
  std::vector<Bucket> buckets;                       // first-traversal order
  Flat<uint32_t> bucket_of(nt);                      // key -> index into buckets
  Flat<std::array<uint64_t, 4>> cell_of(3 * nt);     // (bucket, cell) -> cell index in that bucket
  Flat<std::array<uint32_t, 3>> member(3 * nt);      // (bucket, cell index, vid) already a member
  Flat<std::array<uint32_t, 4>> kept(nt);            // (bucket, sorted triple) of a kept face
  uint32_t last_key = 0, last_b = kNone;
  for (const Tri& t : tris) {
    uint32_t bi = last_b;
    if (bi == kNone || t.key != last_key) {
      bool added;
      bi = bucket_of.Get(t.key, Mix(t.key), static_cast<uint32_t>(buckets.size()), &added);
      if (added) {
        buckets.emplace_back();
        buckets.back().key = t.key;
      }
      last_key = t.key;
      last_b = bi;
    }
    Bucket& b = buckets[bi];
    Triple idx{};
    for (int c = 0; c < 3; ++c) {
      const auto& pos = t.pos[c];
      std::array<uint64_t, 4> cell;
      if (!h) {
        cell = {bi, t.vid[c], 0, 0};
      } else {
        cell = {bi, CellCoord(pos[0], org[0], *h), CellCoord(pos[1], org[1], *h), CellCoord(pos[2], org[2], *h)};
      }
      bool added;
      const uint32_t ci = cell_of.Get(cell, HashOf(cell), static_cast<uint32_t>(b.cells.size()), &added);
      if (added) b.cells.emplace_back();
      const std::array<uint32_t, 3> mk{bi, ci, t.vid[c]};
      member.Get(mk, HashOf(mk), 0, &added);
      if (added) {
        CellAcc& acc = b.cells[ci];
        if (acc.n == 0) {
          acc.first_pos = pos;
          acc.first_payload = t.payload[c];
        }
        for (int a = 0; a < 3; ++a) acc.sum[a] += pos[a];
        ++acc.n;
      }
      idx[c] = ci;
    }
    if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) continue;
    Triple st = idx;
    if (st[0] > st[1]) std::swap(st[0], st[1]);
    if (st[1] > st[2]) std::swap(st[1], st[2]);
    if (st[0] > st[1]) std::swap(st[0], st[1]);
    const std::array<uint32_t, 4> fk{bi, st[0], st[1], st[2]};
    bool added;
    kept.Get(fk, HashOf(fk), 0, &added);
    if (!added) continue;
    b.faces.push_back(idx);
  }

  Merged out;
  for (const Bucket& b : buckets) {
    if (b.faces.empty()) continue;
    MergedKey mk;
    mk.key = b.key;
    std::vector<uint32_t> remap(b.cells.size(), kNone);
    mk.faces.reserve(b.faces.size());
    for (const Triple& f : b.faces) {
      Triple nf{};
      for (int c = 0; c < 3; ++c) {
        uint32_t& ni = remap[f[c]];
        if (ni == kNone) {
          ni = static_cast<uint32_t>(mk.pos.size());
          const CellAcc& acc = b.cells[f[c]];
          if (!h) {
            mk.pos.push_back(acc.first_pos);
          } else {
            const double n = static_cast<double>(acc.n);
            mk.pos.push_back({acc.sum[0] / n, acc.sum[1] / n, acc.sum[2] / n});
          }
          mk.payload.push_back(acc.first_payload);
        }
        nf[c] = ni;
      }
      mk.faces.push_back(nf);
    }
    out.push_back(std::move(mk));
  }
  return out;
}

// slim.count
size_t Count(const Merged& merged) {
  size_t n = 0;
  for (const MergedKey& k : merged) n += k.pos.size();
  return n;
}

// slim.choose_h. The probes count without building (Counter); a probe Counter cannot place (a non-finite or
// NaN or infinite cell) runs the full Merge instead, which raises as Python's merge does at that h.
Chosen ChooseH(std::span<const Tri> tris, size_t target) {
  static_assert(kHHi == 1.0, "the refusal text spells H_HI's repr as 1.0");
  const std::array<double, 3> org = Origin(tris);
  Counter counter(tris, org);
  const auto count_at = [&](double h) {
    size_t n = 0;
    if (counter.Count(h, &n)) return n;
    return Count(Merge(tris, h, org));
  };
  double lo = kHLo, hi = kHHi;
  const size_t n_hi = count_at(hi);
  if (n_hi > target) {
    Fail("count " + std::to_string(n_hi) + " at H_HI = 1.0 exceeds target " + std::to_string(target));
  }
  if (count_at(lo) <= target) return {lo, Merge(tris, lo, org), lo};
  for (int i = 0; i < kHalvings; ++i) {
    const double mid = (lo + hi) / 2;
    if (count_at(mid) <= target) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  return {hi, Merge(tris, hi, org), lo};
}

}  // namespace doa4::suitconv::slim
