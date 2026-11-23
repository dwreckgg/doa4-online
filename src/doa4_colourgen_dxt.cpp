#include "doa4_suitconv_fp.h"

#include "doa4_colourgen.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

// DXT1 for the Spartan-458 repaint: the decoder's integer palette, an opaque encoder in float (one IEEE float operation
// per statement, in the order the reference evaluates them: sums left to right unless stated, no fused or reordered
// form, no C runtime maths call; the one square root is std::sqrt on a float, a correctly rounded instruction), and
// the Xbox 360 tiling index for 8-byte blocks.
//
// Encoder, per 4x4 block: mean and covariance of the 16 pixels, the principal axis by 8 rounds of power iteration
// from the bounding-box diagonal, endpoints at the extreme projections (start 1) and at the box corners (start 2);
// for each start, endpoints rounded to 565, every pixel given its first-nearest palette entry, then 4 rounds of
// least-squares endpoint refits for the current indices; the lowest-error candidate of the ten is kept (ties keep the
// earlier). Blocks come out with c0 > c1, or c0 == c1 with every index 0.

namespace doa4::colourgen {
namespace {

// numpy's maximum / minimum on one pair: the first operand when it compares >= (<=) or is NaN, else the second
inline float MaxF(float a, float b) { return (a >= b || a != a) ? a : b; }
inline float MinF(float a, float b) { return (a <= b || a != a) ? a : b; }

// clip to 0..255 as the reference does it: maximum with 0, then minimum with 255
inline float Clip255(float x) {
  const float y = MaxF(x, 0.0f);
  return MinF(y, 255.0f);
}

// The reference's float32 constants: each is the double literal rounded to float (not a float literal of the decimal).
constexpr float kAxisBias = static_cast<float>(1e-3);
constexpr float kNormFloor = static_cast<float>(1e-12);
constexpr float kDetMin = static_cast<float>(1e-4);
// weight of endpoint 0 for palette index 0..3
constexpr float kW0[4] = {1.0f, 0.0f, static_cast<float>(2.0 / 3.0), static_cast<float>(1.0 / 3.0)};
constexpr int kRefine = 4;
constexpr int kPowerRounds = 8;

// For 0 <= x < 2**22: the nearest integer, ties to even. The truncation and x - t are exact there.
inline int RoundHalfEvenF(float x) {
  int t = static_cast<int>(x);
  const float d = x - static_cast<float>(t);
  if (d > 0.5f || (d == 0.5f && (t & 1) == 1)) ++t;
  return t;
}

// The decoder's expansion of a 565 word: x * 255 // max per field.
inline void Expand565(uint32_t c, int out[3]) {
  out[0] = static_cast<int>((c >> 11) & 31) * 255 / 31;
  out[1] = static_cast<int>((c >> 5) & 63) * 255 / 63;
  out[2] = static_cast<int>(c & 31) * 255 / 31;
}

struct Palette {
  int c[4][3];
  bool four;  // c0 > c1: four colours; otherwise three and a transparent black at index 3
};

inline Palette MakePalette(uint16_t c0, uint16_t c1) {
  Palette p;
  Expand565(c0, p.c[0]);
  Expand565(c1, p.c[1]);
  p.four = c0 > c1;
  for (int ch = 0; ch < 3; ++ch) {
    const int p0 = p.c[0][ch], p1 = p.c[1][ch];
    if (p.four) {
      p.c[2][ch] = (2 * p0 + p1) / 3;
      p.c[3][ch] = (p0 + 2 * p1) / 3;
    } else {
      p.c[2][ch] = (p0 + p1) / 2;
      p.c[3][ch] = 0;
    }
  }
  return p;
}

// a0*b0 + a1*b1 + a2*b2 as ((a0*b0 + a1*b1) + a2*b2), each product rounded
inline float Dot3(const float a[3], const float b[3]) {
  float x = a[0] * b[0];
  float y = a[1] * b[1];
  x = x + y;
  y = a[2] * b[2];
  return x + y;
}

// squared distance of a pixel to a palette entry: ((d0 * d0 + d1 * d1) + d2 * d2)
inline float SqDist(const float p[3], const float e[3]) {
  float x = p[0] - e[0];
  float d = x * x;
  x = p[1] - e[1];
  x = x * x;
  d = d + x;
  x = p[2] - e[2];
  x = x * x;
  return d + x;
}

// a sum of 16 terms with eight accumulators: r[j] = x[j] + x[8 + j]; ((r0 + r1) + (r2 + r3)) + ((r4 + r5) + (r6 + r7))
inline float Sum16(const float x[16]) {
  float r[8];
  for (int j = 0; j < 8; ++j) r[j] = x[j] + x[8 + j];
  float a = r[0] + r[1];
  float b = r[2] + r[3];
  a = a + b;
  b = r[4] + r[5];
  const float c = r[6] + r[7];
  b = b + c;
  return a + b;
}

struct Candidate {
  uint16_t c0, c1;
  uint8_t idx[16];
  float err;
};

// Endpoints ordered so that c0 >= c1; each pixel's FIRST nearest palette entry (entry 0 only when c0 == c1); err = the
// left-to-right sum of the chosen squared distances.
void Assign(const float P[16][3], uint16_t q0, uint16_t q1, Candidate& out) {
  uint16_t c0 = q0, c1 = q1;
  if (q0 < q1) {
    c0 = q1;
    c1 = q0;
  }
  const Palette pal = MakePalette(c0, c1);
  float pf[4][3];
  for (int e = 0; e < 4; ++e)
    for (int ch = 0; ch < 3; ++ch) pf[e][ch] = static_cast<float>(pal.c[e][ch]);
  float err = 0.0f;
  for (int k = 0; k < 16; ++k) {
    float best = SqDist(P[k], pf[0]);
    int ik = 0;
    for (int e = 1; e < 4; ++e) {
      const float d = SqDist(P[k], pf[e]);
      if (d < best && pal.four) {
        best = d;
        ik = e;
      }
    }
    out.idx[k] = static_cast<uint8_t>(ik);
    err = k == 0 ? best : err + best;
  }
  out.c0 = c0;
  out.c1 = c1;
  out.err = err;
}

// Least-squares endpoints for fixed indices; the given endpoints e0, e1 where det <= kDetMin.
void Refit(const float P[16][3], const uint8_t idx[16], const float e0[3], const float e1[3], float o0[3], float o1[3]) {
  float a[16], b[16], t[16];
  for (int k = 0; k < 16; ++k) {
    a[k] = kW0[idx[k]];
    b[k] = 1.0f - a[k];
  }
  for (int k = 0; k < 16; ++k) t[k] = a[k] * a[k];
  const float aa = Sum16(t);
  for (int k = 0; k < 16; ++k) t[k] = b[k] * b[k];
  const float bb = Sum16(t);
  for (int k = 0; k < 16; ++k) t[k] = a[k] * b[k];
  const float ab = Sum16(t);
  float x = aa * bb;
  float y = ab * ab;
  const float det = x - y;
  const bool ok = det > kDetMin;
  const float safe = ok ? det : 1.0f;
  for (int ch = 0; ch < 3; ++ch) {
    float ax = a[0] * P[0][ch];
    for (int k = 1; k < 16; ++k) {
      const float m = a[k] * P[k][ch];
      ax = ax + m;
    }
    float bx = b[0] * P[0][ch];
    for (int k = 1; k < 16; ++k) {
      const float m = b[k] * P[k][ch];
      bx = bx + m;
    }
    x = ax * bb;
    y = bx * ab;
    x = x - y;
    const float n0 = x / safe;
    x = bx * aa;
    y = ax * ab;
    x = x - y;
    const float n1 = x / safe;
    o0[ch] = ok ? Clip255(n0) : e0[ch];
    o1[ch] = ok ? Clip255(n1) : e1[ch];
  }
}

void EncodeBlock(const float P[16][3], uint8_t* out) {
  float mean[3], lo[3], hi[3], axis[3];
  for (int ch = 0; ch < 3; ++ch) {
    float s = P[0][ch];
    for (int k = 1; k < 16; ++k) s = s + P[k][ch];
    mean[ch] = s / 16.0f;
  }
  float cen[16][3];
  for (int k = 0; k < 16; ++k)
    for (int ch = 0; ch < 3; ++ch) cen[k][ch] = P[k][ch] - mean[ch];
  float cov[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      float s = cen[0][i] * cen[0][j];
      for (int k = 1; k < 16; ++k) {
        const float m = cen[k][i] * cen[k][j];
        s = s + m;
      }
      cov[i][j] = s;
    }
  for (int ch = 0; ch < 3; ++ch) {
    float mn = P[0][ch], mx = P[0][ch];
    for (int k = 1; k < 16; ++k) {
      mn = MinF(mn, P[k][ch]);
      mx = MaxF(mx, P[k][ch]);
    }
    lo[ch] = mn;
    hi[ch] = mx;
    const float x = mx - mn;
    axis[ch] = x + kAxisBias;
  }
  for (int pass = 0; pass < kPowerRounds; ++pass) {
    float nv[3];
    for (int i = 0; i < 3; ++i) nv[i] = Dot3(cov[i], axis);
    float ss = nv[0] * nv[0];
    float y = nv[1] * nv[1];
    ss = ss + y;
    y = nv[2] * nv[2];
    ss = ss + y;
    const float nrm = std::sqrt(ss);
    const float den = MaxF(nrm, kNormFloor);
    for (int ch = 0; ch < 3; ++ch) axis[ch] = nv[ch] / den;
  }
  float tmax = 0.0f, tmin = 0.0f;
  for (int k = 0; k < 16; ++k) {
    const float t = Dot3(cen[k], axis);
    tmax = k == 0 ? t : MaxF(tmax, t);
    tmin = k == 0 ? t : MinF(tmin, t);
  }
  // A negative zero in axis or t (a zero product or a sum of zeros) can change only the sign of other zeros on the way
  // here (products, sums of zeros, the max / min of tied zeros, +-0 / den with den >= kNormFloor); its first use that
  // could see the sign is mean + axis * t below, where mean >= 0 (a mean of bytes): mean + (+-0) is mean for mean > 0,
  // and +0 + -0 is +0 under round to nearest. So no byte depends on the sign of a zero.
  float starts[2][2][3];
  for (int ch = 0; ch < 3; ++ch) {
    float x = axis[ch] * tmax;
    starts[0][0][ch] = mean[ch] + x;
    x = axis[ch] * tmin;
    starts[0][1][ch] = mean[ch] + x;
    starts[1][0][ch] = hi[ch];
    starts[1][1][ch] = lo[ch];
  }
  Candidate best{};
  bool have = false;
  for (int s = 0; s < 2; ++s) {
    float e0[3], e1[3];
    for (int ch = 0; ch < 3; ++ch) {
      e0[ch] = Clip255(starts[s][0][ch]);
      e1[ch] = Clip255(starts[s][1][ch]);
    }
    for (int it = 0; it <= kRefine; ++it) {
      Candidate cand;
      Assign(P, Quant565(e0), Quant565(e1), cand);
      if (!have || cand.err < best.err) {
        best = cand;
        have = true;
      }
      if (it < kRefine) {
        int x0[3], x1[3];
        Expand565(cand.c0, x0);
        Expand565(cand.c1, x1);
        float p0[3], p1[3];
        for (int ch = 0; ch < 3; ++ch) {
          p0[ch] = static_cast<float>(x0[ch]);
          p1[ch] = static_cast<float>(x1[ch]);
        }
        Refit(P, cand.idx, p0, p1, e0, e1);
      }
    }
  }
  uint32_t bits = 0;
  for (int k = 0; k < 16; ++k) bits |= static_cast<uint32_t>(best.idx[k]) << (2 * k);
  out[0] = static_cast<uint8_t>(best.c0 & 255);
  out[1] = static_cast<uint8_t>(best.c0 >> 8);
  out[2] = static_cast<uint8_t>(best.c1 & 255);
  out[3] = static_cast<uint8_t>(best.c1 >> 8);
  for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<uint8_t>(bits >> (8 * i));
}

}  // namespace

uint16_t Quant565(const float e[3]) {
  static constexpr int kSteps[3] = {31, 63, 31};
  int q[3];
  for (int ch = 0; ch < 3; ++ch) {
    float x = e[ch] * static_cast<float>(kSteps[ch]);
    x = x / 255.0f;
    int v = RoundHalfEvenF(x);
    if (v < 0) v = 0;  // the reference clips at 0 too (never reached: e >= 0)
    q[ch] = v < kSteps[ch] ? v : kSteps[ch];
  }
  return static_cast<uint16_t>((q[0] << 11) | (q[1] << 5) | q[2]);
}

void DecodeDxt1(const uint8_t* blocks_le, uint32_t bw, uint32_t bh, uint8_t* rgba) {
  const size_t width = size_t(bw) * 4;
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* p = blocks_le + (size_t(by) * bw + bx) * 8;
      const uint16_t c0 = static_cast<uint16_t>(p[0] | (p[1] << 8));
      const uint16_t c1 = static_cast<uint16_t>(p[2] | (p[3] << 8));
      const uint32_t bits = uint32_t(p[4]) | (uint32_t(p[5]) << 8) | (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
      const Palette pal = MakePalette(c0, c1);
      for (int k = 0; k < 16; ++k) {
        const uint32_t i = (bits >> (2 * k)) & 3;
        uint8_t* o = rgba + ((size_t(by) * 4 + (k >> 2)) * width + size_t(bx) * 4 + (k & 3)) * 4;
        for (int ch = 0; ch < 3; ++ch) o[ch] = static_cast<uint8_t>(pal.c[i][ch]);
        o[3] = (!pal.four && i == 3) ? 0 : 255;
      }
    }
}

void EncodeDxt1(const uint8_t* rgb, uint32_t w, uint32_t h, uint8_t* blocks_le) {
  const uint32_t bw = w / 4, bh = h / 4;
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      float P[16][3];
      for (int k = 0; k < 16; ++k) {
        const uint8_t* s = rgb + ((size_t(by) * 4 + (k >> 2)) * w + size_t(bx) * 4 + (k & 3)) * 3;
        for (int ch = 0; ch < 3; ++ch) P[k][ch] = static_cast<float>(s[ch]);
      }
      EncodeBlock(P, blocks_le + (size_t(by) * bw + bx) * 8);
    }
}

uint32_t TiledBlockIndex(uint32_t bx, uint32_t by, uint32_t bw) {
  // XGAddress2DTiledOffset for log2(bytes per block) = 3, in 64-bit unsigned arithmetic (every term is >= 0)
  constexpr uint64_t lb = 3;
  const uint64_t x = bx, y = by;
  const uint64_t aw = (uint64_t(bw) + 31) & ~uint64_t(31);
  const uint64_t macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7);
  const uint64_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint64_t off = macro + ((micro & ~uint64_t(15)) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  const uint64_t r = ((off & ~uint64_t(511)) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
                     (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
  return static_cast<uint32_t>(r >> lb);
}

}  // namespace doa4::colourgen
