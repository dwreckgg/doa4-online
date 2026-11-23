// Suit slim copies, Task 4: the grid (vertex-cluster) merge and the cell-size bisection, a port of slim.py
// (doax2_work/physics_spike/scripts/slim.py: origin, _vertex, merge, count, choose_h; see doa4_suitconv_slim.h).
//
// Every output order is decided by traversal order, as in slim.py: keys in first-traversal order, cells numbered in
// first-appearance order inside a key, kept faces in traversal order, output vertices in first-reference order over the
// kept faces. The hash containers below are for lookup only; nothing iterates them, so no hash order reaches the output.
// The arithmetic is slim.py's: the cell is floor((p - o) / h) of a double per axis, the cell position is a plain
// sequential sum from 0.0 over the cell's members divided by their number (not a compensated sum).

#include "doa4_suitconv_fp.h"

#include "doa4_suitconv_slim.h"

#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace doa4::suitconv::slim {

namespace {

[[noreturn]] void Fail(const std::string& what) { throw ConvertError(what); }

uint64_t Mix(uint64_t x) {  // splitmix64's finaliser
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

// A cell: (floor x, floor y, floor z) in grid mode, (vid, 0, 0) in identity mode (one mode per Merge call).
using Cell = std::array<int64_t, 3>;
struct CellHash {
  size_t operator()(const Cell& c) const {
    uint64_t h = Mix(static_cast<uint64_t>(c[0]));
    h = Mix(h ^ static_cast<uint64_t>(c[1]));
    h = Mix(h ^ static_cast<uint64_t>(c[2]));
    return static_cast<size_t>(h);
  }
};
using Triple = std::array<uint32_t, 3>;
struct TripleHash {
  size_t operator()(const Triple& t) const {
    return static_cast<size_t>(Mix((static_cast<uint64_t>(t[0]) << 32 | t[1]) ^ Mix(t[2])));
  }
};

// One cell's members, accumulated in member order (slim.py _Bucket.members and _vertex).
struct CellAcc {
  std::array<double, 3> sum{0.0, 0.0, 0.0};  // s = 0.0; s += m.pos[a] per member, in first-appearance order
  uint32_t n = 0;
  std::array<double, 3> first_pos{};  // identity mode: members[0]'s position, no averaging
  uint32_t first_payload = 0;         // the payload is the first member's
};

struct Bucket {
  uint32_t key = 0;
  std::unordered_map<Cell, uint32_t, CellHash> cell_index;  // cell -> index (lookup only)
  std::vector<CellAcc> cells;                               // per cell index, first-appearance order
  std::unordered_set<uint64_t> seen;                        // (cell index, vid) already a member (lookup only)
  std::vector<Triple> faces;                                // kept faces, traversal order, winding kept
  std::unordered_set<Triple, TripleHash> kept_sorted;       // sorted triples of kept faces (lookup only)
};

// math.floor((p - o) / h) as Python evaluates it: ZeroDivisionError for h == 0, ValueError for NaN, OverflowError for
// an infinity. Python's int has no bound; a finite floor outside int64_t (only degenerate inputs: |p - o| / h at or
// past 2^63) is refused here instead.
int64_t CellCoord(double p, double o, double h) {
  if (h == 0.0) Fail("ZeroDivisionError: division by zero");  // the text of py -3 = CPython 3.14.7 (older ones said "float division by zero")
  const double q = (p - o) / h;
  if (std::isnan(q)) Fail("ValueError: cannot convert float NaN to integer");
  if (std::isinf(q)) Fail("OverflowError: cannot convert float infinity to integer");
  const double f = std::floor(q);
  if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0)) {
    Fail("grid cell out of range: floor((p - o) / h) does not fit 64 bits (Python would carry a big int)");
  }
  return static_cast<int64_t>(f);
}

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
  std::vector<Bucket> buckets;                       // first-traversal order
  std::unordered_map<uint32_t, size_t> bucket_of;   // key -> index into buckets (lookup only)
  for (const Tri& t : tris) {
    auto it = bucket_of.find(t.key);
    if (it == bucket_of.end()) {
      it = bucket_of.emplace(t.key, buckets.size()).first;
      buckets.emplace_back();
      buckets.back().key = t.key;
    }
    Bucket& b = buckets[it->second];
    Triple idx{};
    for (int c = 0; c < 3; ++c) {
      const auto& pos = t.pos[c];
      Cell cell;
      if (!h) {
        cell = {static_cast<int64_t>(t.vid[c]), 0, 0};
      } else {
        cell = {CellCoord(pos[0], org[0], *h), CellCoord(pos[1], org[1], *h), CellCoord(pos[2], org[2], *h)};
      }
      auto ci_it = b.cell_index.find(cell);
      if (ci_it == b.cell_index.end()) {
        ci_it = b.cell_index.emplace(cell, static_cast<uint32_t>(b.cells.size())).first;
        b.cells.emplace_back();
      }
      const uint32_t ci = ci_it->second;
      if (b.seen.insert(static_cast<uint64_t>(ci) << 32 | t.vid[c]).second) {
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
    if (!b.kept_sorted.insert(st).second) continue;
    b.faces.push_back(idx);
  }

  Merged out;
  for (const Bucket& b : buckets) {
    if (b.faces.empty()) continue;
    MergedKey mk;
    mk.key = b.key;
    std::vector<uint32_t> remap(b.cells.size(), std::numeric_limits<uint32_t>::max());
    mk.faces.reserve(b.faces.size());
    for (const Triple& f : b.faces) {
      Triple nf{};
      for (int c = 0; c < 3; ++c) {
        uint32_t& ni = remap[f[c]];
        if (ni == std::numeric_limits<uint32_t>::max()) {
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

// slim.choose_h
Chosen ChooseH(std::span<const Tri> tris, size_t target) {
  static_assert(kHHi == 1.0, "the refusal text spells H_HI's repr as 1.0");
  const std::array<double, 3> org = Origin(tris);
  double lo = kHLo, hi = kHHi;
  const size_t n_hi = Count(Merge(tris, hi, org));
  if (n_hi > target) {
    Fail("count " + std::to_string(n_hi) + " at H_HI = 1.0 exceeds target " + std::to_string(target));
  }
  Merged m_lo = Merge(tris, lo, org);
  if (Count(m_lo) <= target) return {lo, std::move(m_lo), lo};
  for (int i = 0; i < kHalvings; ++i) {
    const double mid = (lo + hi) / 2;
    if (Count(Merge(tris, mid, org)) <= target) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  return {hi, Merge(tris, hi, org), lo};
}

}  // namespace doa4::suitconv::slim
