// Host test for the colour generator: src/doa4_colourgen_alpha.cpp (the Alpha-152 effect recolour),
// src/doa4_colourgen_spartan.cpp (the Spartan-458 armour repaint, MakeSpartan, Make) and src/doa4_colourgen_dxt.cpp
// (DXT1 and tiling). Built and run by run_colourgen_test.bat under the converter's floating-point flags.
//
// Groups:
//   alpha_exhaustive        SHA-1 of AlphaRecolour565 over all 65,536 inputs, per recipe, equals kAlphaPin
//                           (colourgen_pins.h: hashes over synthetic input made by the reference model)
//   alpha_samples           eight literal input -> output pairs per recipe (outputs of the reference tool), so a pin
//                           mismatch has a readable neighbour
//   alpha_edge_recipes      the same over two recipes at the domain's edges, where each branch of the recipe's
//                           `% 1.0` decides outputs (values from the reference tool)
//   make_alpha_synthetic    a synthetic XPR2 laid out by hand here: only the endpoint words of textures 0..120 change,
//                           each to AlphaRecolour565's value (expected bytes built from the layout this file chose)
//   make_alpha_refusals     every layout refusal, one input each, each differing from the good synthetic file in one
//                           property; the good file itself is the accepted control
//   repaint_exhaustive      SHA-1 of Repaint over all 16,777,216 colours for the four shipping recipes (kRepaintPin) and
//                           the five test recipes (kRepaintTestPin)
//   quant_ties              Quant565 at every kQuantTies row, the whole 565 word compared
//   tiled                   kTiledPin over five grids; each grid's index set (and 32x64's) is a permutation
//   family_inputs           the eleven family pictures built here hash to kFamilyInputPin (the fixture is the pin maker's)
//   encode_families         SHA-1 of EncodeDxt1 of each family equals kEncodePin
//   decode_round_trip       the encoded families' block shapes and opacity; four hand-written blocks decode to the
//                           reference tool's output (literals)
//   make_spartan_synthetic  a synthetic 70-texture XPR2: only the twelve paint ranges change, each to this file's own
//                           tile(encode(repaint(decode(blocks it chose)))); a shared range; the caller's control word
//   make_spartan_refusals   every layout and recipe refusal, one input each; properties of other textures never refuse
//   make_by_name            the seven slot names give their recipe's output; other names refuse
// Every group prints its run time. No byte of any game file is stored. The last three lines: GREEN-SITES and
// RED-SITES (check lines that ran without and with a failure: the mutation probe census), then
// "PASSED: N checks, 0 failures" or "FAILED: N checks, M failures".

#include "../../src/doa4_colourgen.h"
#include "../../src/doa4_gamefiles.h"
#include "../../src/doa4_suitconv_num.h"
#include "../../src/doa4_suitconv_tpr.h"
#include "colourgen_pins.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace cg = doa4::colourgen;
namespace tp = doa4::suitconv::tpr;
using cg::Bytes;

static int g_checks = 0;
static int g_failures = 0;
static int g_printed = 0;
static constexpr int kPrintCap = 200;  // per group; the SITES lines at the end name every failing check site anyway
static std::map<int, std::pair<int, int>> g_sites;  // check line -> (runs, failures), printed last (the probe census)

static void Check(bool ok, const char* file, int line, const char* expr, const char* fmt = nullptr, ...) {
  ++g_checks;
  std::pair<int, int>& site = g_sites[line];
  ++site.first;
  if (ok) return;
  ++site.second;
  ++g_failures;
  if (g_printed++ >= kPrintCap) return;
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
#define CK(cond) Check((cond), __FILE__, __LINE__, #cond)
#define CKF(cond, ...) Check((cond), __FILE__, __LINE__, #cond, __VA_ARGS__)

static void Group(const char* name) {
  g_printed = 0;
  std::printf("-- %s\n", name);
}

static std::string Sha1(const Bytes& b) {
  static const uint8_t kEmpty = 0;
  return doa4::gamefiles::Sha1Hex(b.empty() ? &kEmpty : b.data(), b.size());
}

static std::span<const uint8_t> Sp(const Bytes& b) { return std::span<const uint8_t>(b.data(), b.size()); }

static const cg::AlphaRecipe kRecipes[3] = {cg::kAlphaRed, cg::kAlphaBlue, cg::kAlphaLilac};
static const char* const kRecipeNames[3] = {"red", "blue", "lilac"};

// ---------------------------------------------------------------------------------------------------------------
// alpha_exhaustive
// ---------------------------------------------------------------------------------------------------------------

static void TestAlphaExhaustive() {
  Group("alpha_exhaustive");
  // the recipes are the plan's values (design 5.1's numbers)
  CK(cg::kAlphaRed.hue == 352.0 && cg::kAlphaRed.spread == 0.3 && cg::kAlphaRed.sat == 1.2);
  CK(cg::kAlphaBlue.hue == 238.0 && cg::kAlphaBlue.spread == 0.3 && cg::kAlphaBlue.sat == 1.2);
  CK(cg::kAlphaLilac.hue == 256.0 && cg::kAlphaLilac.spread == 0.3 && cg::kAlphaLilac.sat == 1.1);
  for (int i = 0; i < 3; ++i) {
    Bytes w(2 * 65536);
    int changed = 0;
    for (uint32_t c = 0; c < 65536; ++c) {
      const uint16_t o = cg::AlphaRecolour565(static_cast<uint16_t>(c), kRecipes[i]);
      changed += o != c;
      w[2 * c] = static_cast<uint8_t>(o >> 8);  // big-endian words, as the pin is defined
      w[2 * c + 1] = static_cast<uint8_t>(o & 0xFF);
    }
    const std::string got = Sha1(w);
    CKF(got == colourgen_pins::kAlphaPin[i], "%s: sha1 %s, pin %s (%d of 65536 inputs changed)", kRecipeNames[i],
        got.c_str(), colourgen_pins::kAlphaPin[i], changed);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// alpha_samples: the reference tool's outputs on eight synthetic inputs per recipe
// ---------------------------------------------------------------------------------------------------------------

static void TestAlphaSamples() {
  Group("alpha_samples");
  static const uint16_t kIn[8] = {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x1234, 0x8410, 0xA5C3};
  static const uint16_t kOut[3][8] = {
      {0x0000, 0xFFFF, 0xF01F, 0xF80D, 0xF940, 0xA040, 0x8410, 0xB810},  // red
      {0x0000, 0xFFFF, 0x077F, 0x02BF, 0x401F, 0x1814, 0x8410, 0x0377},  // blue
      {0x0000, 0xFFFF, 0x051F, 0x005F, 0x901F, 0x4814, 0x8410, 0x09F7},  // lilac
  };
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 8; ++j) {
      const uint16_t got = cg::AlphaRecolour565(kIn[j], kRecipes[i]);
      CKF(got == kOut[i][j], "%s: 0x%04X -> 0x%04X, the tool's 0x%04X", kRecipeNames[i], kIn[j], got, kOut[i][j]);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// alpha_edge_recipes: two recipes at the domain's edges (for the three slot recipes x = hue / 360 + (h - 0.5) * 0.3
// never falls below 0.51, so the recipe's negative `% 1.0` branch is never taken there). SHA-1 over all 65,536
// results and samples, both from the reference tool.
//   A = (0, 1, 1):    x = h - 0.5 in [-0.5, 0.5): without the negative branch 31,247 results differ
//   B = (360, -1, 1): x = 1.5 - h in (0.5, 1.5]: the >= 1.0 branch is taken, but hsv_to_rgb's i % 6 absorbs its
//                     absence here (0 results differ); the kAlphaRed pin is what sees that branch (2 inputs)
// ---------------------------------------------------------------------------------------------------------------

static void TestAlphaEdgeRecipes() {
  Group("alpha_edge_recipes");
  static const cg::AlphaRecipe kEdge[2] = {{0.0, 1.0, 1.0}, {360.0, -1.0, 1.0}};
  static const char* const kPin[2] = {"01cd67ddb896e2be781845f388df732791415ee2",
                                      "54208e45b253faf334194839706d275c854d3e3d"};
  static const uint16_t kIn[10] = {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x1234, 0x8410, 0xA5C3, 0x781F, 0x7BEF};
  static const uint16_t kOut[2][10] = {
      {0x0000, 0xFFFF, 0x07FF, 0xF81F, 0xFFE0, 0xA382, 0x8430, 0x30D7, 0x87E0, 0x7BCF},
      {0x0000, 0xFFFF, 0x07FF, 0xFFE0, 0xF81F, 0xA08E, 0x8410, 0x35C3, 0x801F, 0x7BEF},
  };
  for (int i = 0; i < 2; ++i) {
    Bytes w(2 * 65536);
    for (uint32_t c = 0; c < 65536; ++c) {
      const uint16_t o = cg::AlphaRecolour565(static_cast<uint16_t>(c), kEdge[i]);
      w[2 * c] = static_cast<uint8_t>(o >> 8);
      w[2 * c + 1] = static_cast<uint8_t>(o & 0xFF);
    }
    const std::string got = Sha1(w);
    CKF(got == kPin[i], "%c: sha1 %s, the tool's %s", 'A' + i, got.c_str(), kPin[i]);
    for (int j = 0; j < 10; ++j) {
      const uint16_t o = cg::AlphaRecolour565(kIn[j], kEdge[i]);
      CKF(o == kOut[i][j], "%c: 0x%04X -> 0x%04X, the tool's 0x%04X", 'A' + i, kIn[j], o, kOut[i][j]);
    }
  }
  // C = (the largest double below 180, 1, 1): for an input with h == 0 and s > 0, x = -2**-54, and x + 1.0 ties to
  // even at 1.0, so hsv_to_rgb gets h == 1.0: i = 6, which i % 6 turns into 0 (31 of the 65,536 inputs). Each of these
  // keeps its colour in the tool; without the i % 6 it would turn magenta (0xF800 -> 0xF81F).
  const cg::AlphaRecipe kC{0x1.67fffffffffffp+7, 1.0, 1.0};
  CK(kC.hue < 180.0 && kC.hue > 179.9999);
  for (const uint16_t c : {uint16_t(0xF800), uint16_t(0x8000), uint16_t(0x0800), uint16_t(0x1000)}) {
    const uint16_t o = cg::AlphaRecolour565(c, kC);
    CKF(o == c, "C: 0x%04X -> 0x%04X, the tool's 0x%04X", c, o, c);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// the synthetic XPR2
// ---------------------------------------------------------------------------------------------------------------
//
// Resources, in table order: USER (8 bytes), textures 0..49, IBUF (24 bytes), textures 50..143, VBUF (32 bytes):
// 147 resources, so the k-th TX2D is not resource k (a reader indexing the table by k reads the wrong descriptor).
// The table is 16 + 16 * 147 = 2368 bytes; the blobs follow it; the header is padded to 0x2000 bytes, so the data
// section starts at byte 0x2000 (hs = 0x2000 - 12).
// A texture descriptor is 8 filler bytes and the six fetch-constant words (big-endian):
//   fc[0] = tiled << 31 | 0x01000002
//   fc[1] = base | 0x300 | endian << 6 | fmt          (0x300: other bits of the low 12)
//   fc[2] = (w - 1) | (h - 1) << 13 | 5 << 26         (5 << 26: bits above both size fields)
//   fc[3] = 0x00D10000, fc[4] = 0x12345678, fc[5] = mip | 0xA55
// Textures: 0 = 256x256, 1 = 128x256, 2 = 256x128, 3..121 = 128x128, all DXT5 (0x14), endian 1, tiled, no mip; 121 is
// outside the effect range although it is a valid DXT5 texture. 122..143 = 64x64 DXT1 (0x12), 2048 bytes in a
// 4096-byte slot, with odd properties (122 not tiled, 123 a mip address, 124 endian 0, 125 64x128): none of those
// may refuse.
// Data section: texture data in an address order unlike the index order (effect textures by (j * 67) % 120 for
// j = 0..119, an outside texture after every sixth, a 4096-byte gap of 0xEE after every fifth placed texture,
// texture 120 last, ending exactly at the end of the file).

struct TexSpec {
  uint32_t w = 128, h = 128, fmt = 0x14, endian = 1, tiled = 1, mip = 0, base = 0;
  uint32_t desc_len = 32;
  uint32_t data_bytes = 0;  // bytes this file fills at base
  bool dxt5 = true;
};

struct Layout {
  std::vector<TexSpec> tex;  // 144
  Bytes data;
  const char* magic = "XPR2";
  const char* last_tex_type = "TX2D";  // texture 143's resource type
  const char* tail_type = "VBUF";      // the last resource's type
  size_t truncate = 0;                 // bytes cut from the end of the data section
};

static constexpr uint32_t kData0 = 0x2000;

struct Lcg {
  uint32_t s;
  uint32_t Next() {
    s = s * 1103515245u + 12345u;
    return s >> 16;
  }
};

static void Put32(Bytes& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}

static Layout GoodLayout() {
  Layout L;
  L.tex.resize(144);
  L.tex[0].w = 256, L.tex[0].h = 256;
  L.tex[1].w = 128, L.tex[1].h = 256;
  L.tex[2].w = 256, L.tex[2].h = 128;
  for (int k = 0; k <= 121; ++k) L.tex[k].data_bytes = (L.tex[k].w / 4) * (L.tex[k].h / 4) * 16;
  for (int k = 122; k < 144; ++k) {
    TexSpec& t = L.tex[k];
    t.w = 64, t.h = 64, t.fmt = 0x12, t.dxt5 = false, t.data_bytes = 2048;
  }
  L.tex[122].tiled = 0;
  L.tex[123].mip = 0x7000;
  L.tex[124].endian = 0;
  L.tex[125].h = 128;
  // address order
  std::vector<int> order;
  int outside = 121;
  for (int j = 0; j < 120; ++j) {
    order.push_back((j * 67) % 120);
    if (j % 6 == 5 && outside < 144) order.push_back(outside++);
  }
  while (outside < 144) order.push_back(outside++);
  order.push_back(120);
  Lcg rng{0x5EED};
  uint32_t pos = 0;
  int placed = 0;
  for (int k : order) {
    TexSpec& t = L.tex[k];
    t.base = pos;
    const uint32_t slot = (t.data_bytes + 0xFFF) & ~0xFFFu;
    L.data.resize(pos + slot, 0);
    if (t.dxt5) {
      for (uint32_t i = 0; i < t.data_bytes / 16; ++i) {
        uint8_t* b = L.data.data() + pos + 16 * i;
        for (int a = 0; a < 8; ++a) b[a] = uint8_t(0x40 + a + 3 * k + i);  // alpha block
        for (int e = 8; e < 12; ++e) b[e] = uint8_t(rng.Next());           // two endpoint words
        for (int x = 12; x < 16; ++x) b[x] = uint8_t(0x90 ^ (x * 7 + k + 5 * i));  // index bits
      }
    } else {
      for (uint32_t i = 0; i < t.data_bytes; ++i) L.data[pos + i] = uint8_t(rng.Next());
    }
    for (uint32_t i = t.data_bytes; i < slot; ++i) L.data[pos + i] = 0xEE;
    pos += slot;
    if (++placed % 5 == 0 && k != 120) {
      L.data.resize(pos + 0x1000, 0xEE);
      pos += 0x1000;
    }
  }
  return L;
}

static Bytes Descriptor(const TexSpec& t) {
  Bytes d;
  for (int i = 0; i < 8; ++i) d.push_back(uint8_t(0xD0 + i));
  Put32(d, (t.tiled << 31) | 0x01000002u);
  Put32(d, t.base | 0x300u | (t.endian << 6) | t.fmt);
  Put32(d, (t.w - 1) | ((t.h - 1) << 13) | (5u << 26));
  Put32(d, 0x00D10000u);
  Put32(d, 0x12345678u);
  Put32(d, t.mip | 0xA55u);
  d.resize(t.desc_len);
  return d;
}

static Bytes Pack(const Layout& L) {
  struct R {
    const char* type;
    Bytes blob;
  };
  std::vector<R> rs;
  rs.push_back({"USER", Bytes(8, 0x55)});
  for (int k = 0; k < 144; ++k) {
    if (k == 50) rs.push_back({"IBUF", Bytes(24, 0x66)});
    rs.push_back({k == 143 ? L.last_tex_type : "TX2D", Descriptor(L.tex[k])});
  }
  rs.push_back({L.tail_type, Bytes(32, 0x77)});
  const uint32_t n = uint32_t(rs.size());
  const uint32_t hs = kData0 - 12;
  Bytes data(L.data.begin(), L.data.end() - L.truncate);
  Bytes f;
  for (int i = 0; i < 4; ++i) f.push_back(uint8_t(L.magic[i]));
  Put32(f, hs);
  Put32(f, uint32_t(data.size()));
  Put32(f, n);
  uint32_t at = 16 + 16 * n;  // file offset of the next blob
  for (const R& r : rs) {
    for (int i = 0; i < 4; ++i) f.push_back(uint8_t(r.type[i]));
    Put32(f, at - 12);
    Put32(f, uint32_t(r.blob.size()));
    Put32(f, 0);
    at += uint32_t(r.blob.size());
  }
  for (const R& r : rs) f.insert(f.end(), r.blob.begin(), r.blob.end());
  f.resize(kData0, 0);
  f.insert(f.end(), data.begin(), data.end());
  return f;
}

// The expected output, from this file's own knowledge of where each block is.
static Bytes Expected(const Layout& L, const Bytes& src, const cg::AlphaRecipe& r) {
  Bytes e = src;
  for (int k = 0; k <= 120; ++k)
    for (uint32_t i = 0; i < L.tex[k].data_bytes / 16; ++i)
      for (uint32_t j : {8u, 10u}) {
        const size_t o = kData0 + L.tex[k].base + 16 * i + j;
        const uint16_t c = uint16_t((src[o] << 8) | src[o + 1]);
        const uint16_t v = cg::AlphaRecolour565(c, r);
        e[o] = uint8_t(v >> 8);
        e[o + 1] = uint8_t(v & 0xFF);
      }
  return e;
}

// ---------------------------------------------------------------------------------------------------------------
// make_alpha_synthetic
// ---------------------------------------------------------------------------------------------------------------

static void TestMakeAlphaSynthetic() {
  Group("make_alpha_synthetic");
  const Layout L = GoodLayout();
  const Bytes src = Pack(L);
  // fixture preconditions: the layout is the one described above
  {
    const tp::Xpr x = tp::Xpr::Parse(Sp(src));
    CK(x.res.size() == 147 && x.OfType(tp::MakeFourcc("TX2D")).size() == 144);
    CK(x.hs && 12 + *x.hs == kData0);
    CK(src.size() == size_t(kData0) + L.tex[120].base + L.tex[120].data_bytes);  // 120 ends at the end of the file
  }
  int descending = 0, adjacent = 0, gaps = 0;
  {
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    for (int k = 0; k <= 120; ++k) {
      if (k < 120 && L.tex[k + 1].base < L.tex[k].base) ++descending;
      spans.push_back({L.tex[k].base, L.tex[k].base + L.tex[k].data_bytes});
    }
    std::sort(spans.begin(), spans.end());
    for (size_t i = 0; i + 1 < spans.size(); ++i) {
      adjacent += spans[i].second == spans[i + 1].first;
      gaps += spans[i].second < spans[i + 1].first;
    }
  }
  CKF(descending > 10 && adjacent > 10 && gaps > 10, "descending %d adjacent %d gaps %d", descending, adjacent, gaps);
  // endpoint words whose recolour differs from them, and whose recoloured two bytes differ (a byte swap shows)
  int words = 0, changed = 0, swap_shows = 0, changed121 = 0;
  for (int k = 0; k <= 121; ++k)
    for (uint32_t i = 0; i < L.tex[k].data_bytes / 16; ++i)
      for (uint32_t j : {8u, 10u}) {
        const size_t o = kData0 + L.tex[k].base + 16 * i + j;
        const uint16_t c = uint16_t((src[o] << 8) | src[o + 1]);
        const uint16_t v = cg::AlphaRecolour565(c, cg::kAlphaRed);
        if (k == 121) {
          changed121 += v != c;
          continue;
        }
        ++words;
        changed += v != c;
        swap_shows += (v >> 8) != (v & 0xFF);
      }
  CKF(words == 2 * (4096 + 2048 + 2048 + 118 * 1024) && changed > words / 2 && swap_shows > words / 2,
      "words %d changed %d swap_shows %d", words, changed, swap_shows);
  CKF(changed121 > 1000, "texture 121 words a wrong range would change: %d", changed121);

  for (int ri = 0; ri < 2; ++ri) {
    const cg::AlphaRecipe& r = kRecipes[ri];
    std::string why;
    const std::optional<Bytes> out = cg::MakeAlpha(Sp(src), r, &why);
    const Bytes want = Expected(L, src, r);
    CKF(out.has_value(), "%s: refused: %s", kRecipeNames[ri], why.c_str());
    CKF(want != src, "%s: the expected output differs from the source", kRecipeNames[ri]);
    if (!out) continue;
    CKF(out->size() == src.size(), "%s: size %zu, source %zu", kRecipeNames[ri], out->size(), src.size());
    if (out->size() != src.size()) continue;
    CKF(*out == want, "%s: output differs from the expected bytes", kRecipeNames[ri]);
    // the same property read in parts, so a failure says which part moved
    CK(std::equal(out->begin(), out->begin() + kData0, src.begin()));  // header and descriptors
    int moved_outside = 0, moved_alpha_index = 0, endpoint_diff = 0;
    for (int k = 0; k < 144; ++k) {
      const size_t a = kData0 + L.tex[k].base;
      const TexSpec& t = L.tex[k];
      if (k > 120) {
        moved_outside += !std::equal(out->begin() + a, out->begin() + a + t.data_bytes, src.begin() + a);
        continue;
      }
      for (uint32_t i = 0; i < t.data_bytes / 16; ++i) {
        const size_t b = a + 16 * i;
        moved_alpha_index += !std::equal(out->begin() + b, out->begin() + b + 8, src.begin() + b) ||
                             !std::equal(out->begin() + b + 12, out->begin() + b + 16, src.begin() + b + 12);
        endpoint_diff += !std::equal(out->begin() + b + 8, out->begin() + b + 12, want.begin() + b + 8);
      }
    }
    CKF(moved_outside == 0, "%s: %d of textures 121..143 changed", kRecipeNames[ri], moved_outside);
    CKF(moved_alpha_index == 0, "%s: %d blocks with alpha or index bytes changed", kRecipeNames[ri], moved_alpha_index);
    CKF(endpoint_diff == 0, "%s: %d blocks with endpoint words unlike AlphaRecolour565's", kRecipeNames[ri],
        endpoint_diff);
    CK(tp::Xpr::Parse(Sp(*out)).res.size() == 147);
  }
#if defined(_M_X64) || defined(__x86_64__)
  // The same bytes whatever control word the caller runs under: MakeAlpha sets the converter's own (round to
  // nearest) and restores the caller's. Precondition: under round toward zero AlphaRecolour565 itself differs.
  {
    const Bytes want = Expected(L, src, cg::kAlphaRed);
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(saved | 0x6000u);  // rounding control 11: toward zero
    const Bytes want_rz = Expected(L, src, cg::kAlphaRed);
    const std::optional<Bytes> rz = cg::MakeAlpha(Sp(src), cg::kAlphaRed, nullptr);
    const unsigned during = _mm_getcsr();
    _mm_setcsr(saved);
    CK(want_rz != want);
    CK(rz.has_value() && *rz == want);
    CK((during & 0x6000u) == 0x6000u);  // the caller's control word is back after the call
  }
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// make_alpha_refusals
// ---------------------------------------------------------------------------------------------------------------

static void Refused(const char* name, const Bytes& f, const cg::AlphaRecipe& r = cg::kAlphaRed,
                    const char* want_why = nullptr) {
  std::string why;
  const std::optional<Bytes> out = cg::MakeAlpha(Sp(f), r, &why);
  CKF(!out.has_value() && !why.empty(), "%s: accepted=%d why '%s'", name, int(out.has_value()), why.c_str());
  if (want_why) CKF(why == want_why, "%s: why '%s', want '%s'", name, why.c_str(), want_why);
}

static void Accepted(const char* name, const Bytes& f, const cg::AlphaRecipe& r = cg::kAlphaRed) {
  std::string why;
  const std::optional<Bytes> out = cg::MakeAlpha(Sp(f), r, &why);
  CKF(out.has_value() && out->size() == f.size(), "%s: refused: %s", name, why.c_str());
}

static void TestMakeAlphaRefusals() {
  Group("make_alpha_refusals");
  const Layout good = GoodLayout();
  // the control: the good file (textures 122..143 not tiled, a mip address, endian 0, 64x64, DXT1) is accepted
  Accepted("good file", Pack(good));
  {
    std::string* none = nullptr;
    CK(cg::MakeAlpha(Sp(Pack(good)), cg::kAlphaRed, none).has_value());
  }
  auto with = [&](auto&& edit) {
    Layout L = good;
    edit(L);
    return Pack(L);
  };
  Refused("143 textures", with([](Layout& L) { L.last_tex_type = "VBUF"; }));
  Refused("145 textures", with([](Layout& L) { L.tail_type = "TX2D"; }));
  Refused("texture 60 DXT1", with([](Layout& L) { L.tex[60].fmt = 0x12; }));
  Refused("texture 60 endian 2", with([](Layout& L) { L.tex[60].endian = 2; }));
  Refused("texture 60 not tiled", with([](Layout& L) { L.tex[60].tiled = 0; }));
  Refused("texture 60 mip address", with([](Layout& L) { L.tex[60].mip = 0x5000; }));
  Refused("texture 60 width 64", with([](Layout& L) { L.tex[60].w = 64; }));
  Refused("texture 60 height 64", with([](Layout& L) { L.tex[60].h = 64; }));
  Refused("texture 0 width 192", with([](Layout& L) { L.tex[0].w = 192; }));
  Refused("data truncated by 1 byte inside texture 120", with([](Layout& L) { L.truncate = 1; }));
  Refused("texture 70 base 0xFFFFF000", with([](Layout& L) { L.tex[70].base = 0xFFFFF000u; }));
  Refused("textures 30 and 31 overlap", with([](Layout& L) { L.tex[30].base = L.tex[31].base; }));
  CK(good.tex[31].base >= 0x3000);  // precondition of the next row (no wrap below 0)
  Refused("texture 30 overlaps 31 by 4096 bytes", with([](Layout& L) { L.tex[30].base = L.tex[31].base - 0x3000; }));
  Refused("texture 10 descriptor of 20 bytes", with([](Layout& L) { L.tex[10].desc_len = 20; }));
  // the reader's ConvertError text, passed on unchanged (doa4_suitconv_tpr.cpp: Assert prefixes "assert ")
  Refused("bad magic", with([](Layout& L) { L.magic = "XPR3"; }), cg::kAlphaRed, "assert d[:4] == b'XPR2'");
  {
    const Bytes f = with([](Layout& L) { L.tex[60].fmt = 0x12; });
    CK(!cg::MakeAlpha(Sp(f), cg::kAlphaRed, nullptr).has_value());  // why may be null on a refusal too
  }
  // properties of textures 121..143 never refuse
  Accepted("texture 121 DXT1", with([](Layout& L) { L.tex[121].fmt = 0x12; }));
  Accepted("texture 143 mip address", with([](Layout& L) { L.tex[143].mip = 0x9000; }));
  Accepted("texture 121 overlapping texture 0", with([](Layout& L) { L.tex[121].base = L.tex[0].base; }));
  // recipes outside the domain AlphaRecolour565 is defined on
  const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
  const Bytes f = Pack(good);
  Refused("hue NaN", f, {nan, 0.3, 1.2});
  Refused("hue -0.5", f, {-0.5, 0.3, 1.2});
  Refused("hue 360.5", f, {360.5, 0.3, 1.2});
  Refused("spread 1.5", f, {352.0, 1.5, 1.2});
  Refused("spread -1.5", f, {352.0, -1.5, 1.2});
  Refused("spread NaN", f, {352.0, nan, 1.2});
  Refused("sat -0.1", f, {352.0, 0.3, -0.1});
  Refused("sat inf", f, {352.0, 0.3, inf});
  Refused("sat NaN", f, {352.0, 0.3, nan});
  Accepted("hue 0, spread -1", f, {0.0, -1.0, 1.2});
  Accepted("hue 360, spread 1, sat 0", f, {360.0, 1.0, 0.0});
}

// ===============================================================================================================
// Spartan-458: repaint, DXT1, tiling, MakeSpartan, Make
// ===============================================================================================================

namespace pins = colourgen_pins;

static const cg::SpartanRecipe kShip[4] = {cg::kH3Pink, cg::kH3Orchid, cg::kH2Orange, cg::kH2Cyan};
static const char* const kShipNames[4] = {"pink", "orchid", "orange", "cyan"};
static const char* const kTestNames[pins::kRepaintTestCount] = {"t1", "t2", "a1", "a2", "a3"};

static cg::SpartanRecipe TestRecipe(size_t i) {
  const pins::RepaintTestRecipe& t = pins::kRepaintTestRecipe[i];
  return cg::SpartanRecipe{{t.colour[0], t.colour[1], t.colour[2]}, t.amount, t.hue_shift, t.saturation,
                           t.brightness, t.vref};
}

static uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// ---------------------------------------------------------------------------------------------------------------
// repaint_exhaustive: SHA-1 of Repaint over all 16,777,216 colours (r outermost, b innermost; 3 output bytes each),
// for the four shipping recipes (kRepaintPin) and the five test recipes (kRepaintTestPin).
// ---------------------------------------------------------------------------------------------------------------

static void TestRepaintExhaustive() {
  Group("repaint_exhaustive");
  // the shipping recipes and the paint texture numbers are the plan's values (design 5.1's numbers)
  CK(cg::kH3Pink.colour[0] == 0xFF && cg::kH3Pink.colour[1] == 0x8F && cg::kH3Pink.colour[2] == 0xEC &&
     cg::kH3Pink.amount == 1.0 && cg::kH3Pink.hue_shift == 0.0 && cg::kH3Pink.saturation == 1.0 &&
     cg::kH3Pink.brightness == 1.4266 && cg::kH3Pink.vref == 0.41254261348390375);
  CK(cg::kH3Orchid.colour[0] == 0x76 && cg::kH3Orchid.colour[1] == 0x60 && cg::kH3Orchid.colour[2] == 0xAA &&
     cg::kH3Orchid.amount == 1.0 && cg::kH3Orchid.hue_shift == 0.0 && cg::kH3Orchid.saturation == 1.0 &&
     cg::kH3Orchid.brightness == 1.0 && cg::kH3Orchid.vref == 0.6104955708078648);
  CK(cg::kH2Orange.colour[0] == 0xAB && cg::kH2Orange.colour[1] == 0x6D && cg::kH2Orange.colour[2] == 0x2D &&
     cg::kH2Orange.amount == 1.0 && cg::kH2Orange.hue_shift == 0.0 && cg::kH2Orange.saturation == 1.0 &&
     cg::kH2Orange.brightness == 1.0 && cg::kH2Orange.vref == 0.2704977910130658);
  CK(cg::kH2Cyan.colour[0] == 0x24 && cg::kH2Cyan.colour[1] == 0x69 && cg::kH2Cyan.colour[2] == 0x69 &&
     cg::kH2Cyan.amount == 1.0 && cg::kH2Cyan.hue_shift == 0.0 && cg::kH2Cyan.saturation == 1.0 &&
     cg::kH2Cyan.brightness == 1.0 && cg::kH2Cyan.vref == 0.05780543019106723);
  {
    static const int kWant[12] = {11, 22, 27, 31, 36, 42, 46, 52, 61, 63, 67, 69};
    CK(std::equal(std::begin(kWant), std::end(kWant), std::begin(cg::kPaintTextures)));
  }
  const size_t n = size_t(1) << 24;
  Bytes in(3 * n), out(3 * n);
  for (uint32_t c = 0; c < n; ++c) {
    in[3 * c] = uint8_t(c >> 16);
    in[3 * c + 1] = uint8_t(c >> 8);
    in[3 * c + 2] = uint8_t(c);
  }
  auto one = [&](const cg::SpartanRecipe& r, const char* name, const char* pin) {
    std::fill(out.begin(), out.end(), uint8_t(0));
    cg::Repaint(r, in.data(), out.data(), n);
    size_t changed = 0;
    for (size_t i = 0; i < 3 * n; i += 3) changed += in[i] != out[i] || in[i + 1] != out[i + 1] || in[i + 2] != out[i + 2];
    const std::string got = Sha1(out);
    CKF(got == pin, "%s: sha1 %s, pin %s (%zu of 16777216 colours changed)", name, got.c_str(), pin, changed);
  };
  for (int i = 0; i < 4; ++i) one(kShip[i], kShipNames[i], pins::kRepaintPin[i]);
  for (size_t i = 0; i < pins::kRepaintTestCount; ++i) one(TestRecipe(i), kTestNames[i], pins::kRepaintTestPin[i]);
  // in place (srgb_in == srgb_out) gives the same bytes: the last 65,536 colours under t2
  {
    const cg::SpartanRecipe t2 = TestRecipe(1);
    Bytes a(in.end() - 3 * 65536, in.end()), b(3 * 65536);
    cg::Repaint(t2, a.data(), b.data(), 65536);
    cg::Repaint(t2, a.data(), a.data(), 65536);
    CK(a == b && b != Bytes(in.end() - 3 * 65536, in.end()));
  }
}

// ---------------------------------------------------------------------------------------------------------------
// quant_ties: Quant565 at every kQuantTies row (float32 values at and around each rounding tie), the value in one
// channel and the other two at 255.0 / 0.0 (fields known: all ones, zero), so the whole word is compared.
// ---------------------------------------------------------------------------------------------------------------

static void TestQuantTies() {
  Group("quant_ties");
  int n31 = 0, n63 = 0, other = 0;
  for (size_t i = 0; i < pins::kQuantTieCount; ++i) {
    const pins::QuantTie& t = pins::kQuantTies[i];
    const float v = std::bit_cast<float>(t.bits);
    if (t.steps == 31) {
      ++n31;
      const float er[3] = {v, 255.0f, 0.0f};  // red under test, green 63, blue 0
      const uint16_t qr = cg::Quant565(er);
      const uint16_t wr = uint16_t((t.q << 11) | (63 << 5));
      CKF(qr == wr, "row %zu red 0x%08X: 0x%04X, want 0x%04X", i, t.bits, qr, wr);
      const float eb[3] = {0.0f, 255.0f, v};  // blue under test
      const uint16_t qb = cg::Quant565(eb);
      const uint16_t wb = uint16_t((63 << 5) | t.q);
      CKF(qb == wb, "row %zu blue 0x%08X: 0x%04X, want 0x%04X", i, t.bits, qb, wb);
    } else if (t.steps == 63) {
      ++n63;
      const float eg[3] = {255.0f, v, 0.0f};  // green under test, red 31
      const uint16_t qg = cg::Quant565(eg);
      const uint16_t wg = uint16_t((31 << 11) | (t.q << 5));
      CKF(qg == wg, "row %zu green 0x%08X: 0x%04X, want 0x%04X", i, t.bits, qg, wg);
    } else {
      ++other;
    }
  }
  CKF(n31 > 0 && n63 > 0 && other == 0 && n31 + n63 == 846, "rows: 31 steps %d, 63 steps %d, other %d", n31, n63,
      other);
  // readable neighbours: (128 * 31) / 255 = 15.56 -> 16, (128 * 63) / 255 = 31.62 -> 32
  const float mid[3] = {128.0f, 128.0f, 128.0f}, white[3] = {255.0f, 255.0f, 255.0f}, black[3] = {0.0f, 0.0f, 0.0f};
  CK(cg::Quant565(mid) == 0x8410);
  CK(cg::Quant565(white) == 0xFFFF);
  CK(cg::Quant565(black) == 0x0000);
}

// ---------------------------------------------------------------------------------------------------------------
// tiled: kTiledPin over the grids 32x32, 64x32, 128x128, 256x128, 256x256 (bw x bh; by outer, bx inner; 32-bit LE
// words), and per grid that the index set is a permutation of 0..bw*bh-1 (also on 32x64, higher than wide).
// ---------------------------------------------------------------------------------------------------------------

static bool IsPermutation(uint32_t bw, uint32_t bh) {
  std::vector<uint8_t> seen(size_t(bw) * bh, 0);
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint32_t t = cg::TiledBlockIndex(bx, by, bw);
      if (t >= seen.size() || seen[t]) return false;
      seen[t] = 1;
    }
  return true;
}

static void TestTiled() {
  Group("tiled");
  static const uint32_t kGrids[5][2] = {{32, 32}, {64, 32}, {128, 128}, {256, 128}, {256, 256}};
  Bytes w;
  for (const auto& g : kGrids)
    for (uint32_t by = 0; by < g[1]; ++by)
      for (uint32_t bx = 0; bx < g[0]; ++bx) {
        const uint32_t t = cg::TiledBlockIndex(bx, by, g[0]);
        for (int i = 0; i < 4; ++i) w.push_back(uint8_t(t >> (8 * i)));
      }
  const std::string got = Sha1(w);
  CKF(got == pins::kTiledPin, "sha1 %s, pin %s", got.c_str(), pins::kTiledPin);
  for (const auto& g : kGrids) CKF(IsPermutation(g[0], g[1]), "%ux%u is not a permutation", g[0], g[1]);
  CK(IsPermutation(32, 64));
}

// ---------------------------------------------------------------------------------------------------------------
// family_inputs: the eleven 256x256 pictures of colourgen_pins.h (lines 80-94 there), built here, hash to
// kFamilyInputPin (the fixture is the pin maker's). encode_families and decode_round_trip use them.
// ---------------------------------------------------------------------------------------------------------------

struct XorShift32 {
  uint32_t x;
  uint8_t Draw() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return uint8_t(x >> 24);
  }
};

static uint8_t Clamp255(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

static Bytes Family(size_t f) {
  Bytes img(256 * 256 * 3, 0);
  if (f == 3) {  // F4: no draws
    for (int y = 0; y < 256; ++y)
      for (int x = 0; x < 256; ++x) {
        uint8_t* p = &img[(size_t(y) * 256 + x) * 3];
        p[0] = uint8_t(x), p[1] = uint8_t(y), p[2] = uint8_t((x + y) / 2);
      }
    return img;
  }
  XorShift32 rng{pins::kFamilySeed[f]};
  for (int by = 0; by < 64; ++by)
    for (int bx = 0; bx < 64; ++bx) {
      const int n = by * 64 + bx;
      auto px = [&](int k) { return &img[(size_t(by * 4 + (k >> 2)) * 256 + (bx * 4 + (k & 3))) * 3]; };
      int a[3], b[3];
      switch (f) {
        case 0:  // F1
          for (int k = 0; k < 16; ++k)
            for (int c = 0; c < 3; ++c) px(k)[c] = rng.Draw();
          break;
        case 1:  // F2
          for (int c = 0; c < 3; ++c) a[c] = rng.Draw();
          for (int k = 0; k < 16; ++k)
            for (int c = 0; c < 3; ++c) px(k)[c] = Clamp255(a[c] + rng.Draw() % 7 - 3);
          break;
        case 2:  // F3
          for (int c = 0; c < 3; ++c) a[c] = rng.Draw();
          for (int c = 0; c < 3; ++c) b[c] = rng.Draw();
          for (int k = 0; k < 16; ++k) {
            const int* s = (rng.Draw() & 1) == 0 ? a : b;
            for (int c = 0; c < 3; ++c) px(k)[c] = uint8_t(s[c]);
          }
          break;
        case 4:  // F5
          for (int c = 0; c < 3; ++c) a[c] = rng.Draw();
          for (int k = 0; k < 16; ++k)
            for (int c = 0; c < 3; ++c) px(k)[c] = uint8_t(a[c]);
          if (n % 4 == 3) px(0)[0] = uint8_t(std::min(a[0] + 1, 255));
          break;
        case 5: {  // F6
          const int base = rng.Draw();
          for (int k = 0; k < 16; ++k) {
            const uint8_t v = Clamp255(base + rng.Draw() % 25 - 12);
            px(k)[0] = v, px(k)[1] = v, px(k)[2] = v;
          }
          break;
        }
        case 6: {  // F7
          const int sel = rng.Draw() % 4;
          if (sel == 0) break;  // all black
          if (sel == 3) {
            for (int k = 0; k < 16; ++k)
              for (int c = 0; c < 3; ++c) px(k)[c] = uint8_t(rng.Draw() % 3);
            break;
          }
          for (int c = 0; c < 3; ++c) a[c] = rng.Draw();
          for (int k = 0; k < 16; ++k) {
            const bool black = sel == 2 && (rng.Draw() & 1) == 0;
            for (int c = 0; c < 3; ++c) px(k)[c] = black ? 0 : uint8_t(a[c]);
          }
          break;
        }
        case 7:  // F8
          for (int c = 0; c < 3; ++c) a[c] = rng.Draw();
          for (int c = 0; c < 3; ++c) b[c] = rng.Draw();
          for (int k = 0; k < 16; ++k) {
            const int w = rng.Draw() % 4;
            for (int c = 0; c < 3; ++c) px(k)[c] = Clamp255((a[c] * (3 - w) + b[c] * w) / 3 + rng.Draw() % 3 - 1);
          }
          break;
        case 8: {  // F9
          int d[3];
          for (int c = 0; c < 3; ++c) a[c] = rng.Draw();
          for (int c = 0; c < 3; ++c) d[c] = rng.Draw() % 16 - 8;
          for (int k = 0; k < 16; ++k) {
            const int t = rng.Draw() % 16;
            for (int c = 0; c < 3; ++c) px(k)[c] = Clamp255(a[c] + d[c] * (t - 8));
          }
          break;
        }
        default:  // F10, F11
          for (int c = 0; c < 3; ++c) a[c] = rng.Draw();
          for (int c = 0; c < 3; ++c) b[c] = rng.Draw();
          for (int k = 0; k < 16; ++k) {
            const int w = rng.Draw() % 16;
            for (int c = 0; c < 3; ++c) px(k)[c] = Clamp255((a[c] * (15 - w) + b[c] * w) / 15 + rng.Draw() % 3 - 1);
          }
          break;
      }
    }
  return img;
}

static std::vector<Bytes> g_family;   // the eleven pictures
static std::vector<Bytes> g_encoded;  // EncodeDxt1 of each (4,096 blocks)

static void TestFamilyInputs() {
  Group("family_inputs");
  CK(pins::kFamilyCount == 11);
  g_family.clear();
  for (size_t f = 0; f < pins::kFamilyCount; ++f) {
    g_family.push_back(Family(f));
    const std::string got = Sha1(g_family.back());
    CKF(got == pins::kFamilyInputPin[f], "F%zu: sha1 %s, pin %s", f + 1, got.c_str(), pins::kFamilyInputPin[f]);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// encode_families: SHA-1 of EncodeDxt1 of each family picture (4,096 blocks row-major, 8 LE bytes) == kEncodePin
// ---------------------------------------------------------------------------------------------------------------

static void TestEncodeFamilies() {
  Group("encode_families");
  g_encoded.clear();
  for (size_t f = 0; f < g_family.size(); ++f) {
    Bytes blocks(4096 * 8 + 16, 0xCD);  // 16 sentinel bytes after the last block
    cg::EncodeDxt1(g_family[f].data(), 256, 256, blocks.data());
    CKF(std::all_of(blocks.end() - 16, blocks.end(), [](uint8_t v) { return v == 0xCD; }),
        "F%zu: bytes written past the last block", f + 1);
    blocks.resize(4096 * 8);
    const std::string got = Sha1(blocks);
    CKF(got == pins::kEncodePin[f], "F%zu: sha1 %s, pin %s", f + 1, got.c_str(), pins::kEncodePin[f]);
    g_encoded.push_back(std::move(blocks));
  }
  CK(g_encoded.size() == pins::kFamilyCount);
}

// ---------------------------------------------------------------------------------------------------------------
// decode_round_trip: every encoded family block is c0 > c1, or c0 == c1 with every index 0, and decodes opaque; four
// hand-written blocks decode to the reference tool's output (its decode_blocks run on these blocks: literals).
// ---------------------------------------------------------------------------------------------------------------

static void PutBlock(uint8_t* p, uint16_t c0, uint16_t c1, uint32_t bits) {
  p[0] = uint8_t(c0), p[1] = uint8_t(c0 >> 8), p[2] = uint8_t(c1), p[3] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) p[4 + i] = uint8_t(bits >> (8 * i));
}

static Bytes FromHex(const char* s) {
  Bytes b;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (; s[0] && s[1]; s += 2) b.push_back(uint8_t(nib(s[0]) * 16 + nib(s[1])));
  return b;
}

static void TestDecodeRoundTrip() {
  Group("decode_round_trip");
  CK(g_encoded.size() == pins::kFamilyCount);
  for (size_t f = 0; f < g_encoded.size(); ++f) {
    const Bytes& bl = g_encoded[f];
    int bad_mode = 0, four = 0, equal = 0;
    for (size_t i = 0; i + 8 <= bl.size(); i += 8) {
      const uint16_t c0 = Le16(&bl[i]), c1 = Le16(&bl[i + 2]);
      const uint32_t bits = Le32(&bl[i + 4]);
      if (c0 > c1) ++four;
      else if (c0 == c1 && bits == 0) ++equal;
      else ++bad_mode;
    }
    CKF(bad_mode == 0, "F%zu: %d blocks neither c0 > c1 nor c0 == c1 with indices 0 (%d four-colour, %d equal)", f + 1,
        bad_mode, four, equal);
    Bytes rgba(256 * 256 * 4 + 16, 0xCD);
    cg::DecodeDxt1(bl.data(), 64, 64, rgba.data());
    CKF(std::all_of(rgba.end() - 16, rgba.end(), [](uint8_t v) { return v == 0xCD; }),
        "F%zu: bytes written past the last pixel", f + 1);
    int transparent = 0;
    for (size_t p = 3; p < 256 * 256 * 4; p += 4) transparent += rgba[p] != 255;
    CKF(transparent == 0, "F%zu: %d pixels not opaque", f + 1, transparent);
  }
  // F7 holds all-black blocks (c0 == c1 == 0, indices 0): the encoder's c0 == c1 output shape is on the population
  {
    int equal = 0;
    if (g_encoded.size() > 6)
      for (size_t i = 0; i + 8 <= g_encoded[6].size(); i += 8)
        equal += Le16(&g_encoded[6][i]) == 0 && Le16(&g_encoded[6][i + 2]) == 0 && Le32(&g_encoded[6][i + 4]) == 0;
    CKF(equal > 0, "F7: %d blocks with c0 == c1 == 0 and indices 0", equal);
  }
  // hand-written blocks, 2 x 2: c0 > c1 (four colours), c0 < c1 (three colours and a transparent index 3), c0 == c1
  // (three-colour mode too), and c0 > c1 with the other index order. Expected: the tool's decode_blocks on them.
  Bytes bl(32);
  PutBlock(&bl[0], 0xA5C3, 0x1234, 0xE4E4E4E4u);   // indices 0 1 2 3 per row
  PutBlock(&bl[8], 0x1234, 0xA5C3, 0xE4E4E4E4u);
  PutBlock(&bl[16], 0x7BEF, 0x7BEF, 0xE4E4E4E4u);
  PutBlock(&bl[24], 0xF81F, 0x07E0, 0x1B1B1B1Bu);  // indices 3 2 1 0 per row
  static const char* const kRows[8] = {
      "a4ba18ff1044a4ff729246ff416b75ff1044a4ffa4ba18ff5a7f5eff00000000",
      "a4ba18ff1044a4ff729246ff416b75ff1044a4ffa4ba18ff5a7f5eff00000000",
      "a4ba18ff1044a4ff729246ff416b75ff1044a4ffa4ba18ff5a7f5eff00000000",
      "a4ba18ff1044a4ff729246ff416b75ff1044a4ffa4ba18ff5a7f5eff00000000",
      "7b7d7bff7b7d7bff7b7d7bff0000000055aa55ffaa55aaff00ff00ffff00ffff",
      "7b7d7bff7b7d7bff7b7d7bff0000000055aa55ffaa55aaff00ff00ffff00ffff",
      "7b7d7bff7b7d7bff7b7d7bff0000000055aa55ffaa55aaff00ff00ffff00ffff",
      "7b7d7bff7b7d7bff7b7d7bff0000000055aa55ffaa55aaff00ff00ffff00ffff",
  };
  Bytes want;
  for (const char* r : kRows) {
    const Bytes b = FromHex(r);
    want.insert(want.end(), b.begin(), b.end());
  }
  CK(want.size() == 256 && Sha1(want) == "5aa8a7d049ee8cf4a8d043cd4a2ff6ba4b2f5091");
  Bytes got(256 + 16, 0xCD);
  cg::DecodeDxt1(bl.data(), 2, 2, got.data());
  CK(std::all_of(got.end() - 16, got.end(), [](uint8_t v) { return v == 0xCD; }));
  got.resize(256);
  CK(got == want);
  // the same read in parts (pixel (x, y) at 4 * (8 * y + x)), so a failure says which rule moved
  auto pix = [&](int x, int y) { return std::vector<uint8_t>(got.begin() + 4 * (8 * y + x), got.begin() + 4 * (8 * y + x) + 4); };
  using V = std::vector<uint8_t>;
  CK(pix(2, 0) == V({0x72, 0x92, 0x46, 0xFF}) && pix(3, 1) == V({0x41, 0x6B, 0x75, 0xFF}));  // the two thirds
  CK(pix(6, 2) == V({0x5A, 0x7F, 0x5E, 0xFF}) && pix(7, 3) == V({0, 0, 0, 0}));  // c0 < c1: half, transparent black
  CK(pix(2, 4) == V({0x7B, 0x7D, 0x7B, 0xFF}) && pix(3, 5) == V({0, 0, 0, 0}));  // c0 == c1: index 3 transparent
  CK(pix(4, 6) == V({0x55, 0xAA, 0x55, 0xFF}) && pix(5, 7) == V({0xAA, 0x55, 0xAA, 0xFF}));  // other index order
}

// ---------------------------------------------------------------------------------------------------------------
// The synthetic Spartan file
// ---------------------------------------------------------------------------------------------------------------
//
// Resources, in table order: USER (8 bytes), textures 0..29, IBUF (24 bytes), textures 30..69 (70 TX2D; one more,
// texture 70, when ntex is 71), VBUF (32 bytes): the k-th TX2D is not resource k. Header padded to 0x1000 bytes (data
// section at byte 0x1000). Descriptor: 8 filler bytes and the six fetch-constant words (big-endian):
//   fc[0] = tiled << 31 | 0x01000002;  fc[1] = base | 0x300 | endian << 6 | fmt;  fc[2] = (w - 1) | (h - 1) << 13 | 5 << 26
//   fc[3] = 0x00D10000;  fc[4] = 0x12345438 | mip_max << 6 (bits 6..9 of the filler are 0: 0x12345438 & 0x3C0 == 0);
//   fc[5] = mip | 0xA55
// The twelve paint textures are DXT1, tiled, endian 1, of three sizes and a fourth (see kPaintSize); textures 10, 12,
// 21, 23 and 68 (neighbours of paint ones) are opaque DXT1 128x128 too; the others are DXT5 64x64, DXT1 64x64 or
// odd ones (not tiled, a mip address, endian 0, 20x20). Paint texture blocks: random bytes made opaque (c0 < c1: the
// two words swapped; c0 == c1: index word cleared; every 61st block forced to c0 == c1 first), placed by this file's
// own tiling loop (TiledBlockIndex, pinned by `tiled`) with each 16-bit word byte-swapped. Data: in the order
// (j * 37) % 69 for j = 0..68, texture 69 last and ending exactly at the end of the file; a 4096-byte gap of 0xEE
// after every fourth placed texture.

struct SpTex {
  uint32_t w = 64, h = 64, fmt = 0x14, endian = 1, tiled = 1, mip = 0, mip_max = 0, base = 0;
  uint32_t desc_len = 32;
  bool blocks_kind = false;  // the data is `blocks` (row-major LE DXT1 blocks), placed tiled
  Bytes blocks;
  uint32_t data_bytes = 0;  // bytes this file fills at base
};

struct SpLayout {
  std::vector<SpTex> tex;  // 70
  Bytes data;
  const char* magic = "XPR2";
  size_t ntex = 70;
  size_t truncate = 0;
};

static constexpr uint32_t kSpData0 = 0x1000;

// The paint texture numbers as the plan gives them (the fixture does not take them from the product).
static const int kPaintList[12] = {11, 22, 27, 31, 36, 42, 46, 52, 61, 63, 67, 69};

static bool IsPaint(int k) {
  return std::find(std::begin(kPaintList), std::end(kPaintList), k) != std::end(kPaintList);
}

// this file's own tiling: row-major LE blocks -> the file's bytes at dst (block n at TiledBlockIndex, words swapped)
static void PlaceTiled(uint8_t* dst, const Bytes& blocks, uint32_t bw, uint32_t bh) {
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* s = &blocks[(size_t(by) * bw + bx) * 8];
      uint8_t* d = dst + size_t(cg::TiledBlockIndex(bx, by, bw)) * 8;
      for (int i = 0; i < 4; ++i) {
        d[2 * i] = s[2 * i + 1];
        d[2 * i + 1] = s[2 * i];
      }
    }
}

static void Place(SpLayout& L, int k) {
  const SpTex& t = L.tex[k];
  if (t.blocks_kind) PlaceTiled(L.data.data() + t.base, t.blocks, t.w / 4, t.h / 4);
}

struct FixCounts {
  int blocks = 0, swapped = 0, cleared = 0;
};

static Bytes OpaqueBlocks(uint32_t nblocks, Lcg& rng, FixCounts& fc) {
  Bytes b(size_t(nblocks) * 8);
  for (uint32_t i = 0; i < nblocks; ++i) {
    uint8_t* p = &b[size_t(i) * 8];
    for (int j = 0; j < 8; ++j) p[j] = uint8_t(rng.Next());
    if (i % 61 == 0) p[2] = p[0], p[3] = p[1];
    ++fc.blocks;
    const uint16_t c0 = Le16(p), c1 = Le16(p + 2);
    if (c0 < c1) {
      std::swap(p[0], p[2]);
      std::swap(p[1], p[3]);
      ++fc.swapped;
    } else if (c0 == c1) {
      p[4] = p[5] = p[6] = p[7] = 0;
      ++fc.cleared;
    }
  }
  return b;
}

static const uint32_t kPaintSize[12][2] = {{128, 128}, {256, 128}, {128, 256}, {128, 128}, {256, 256}, {128, 128},
                                           {128, 128}, {256, 128}, {128, 128}, {128, 256}, {128, 128}, {128, 128}};

static FixCounts g_fix;  // what the opaque fix touched in the good layout (reported)

static SpLayout GoodSpLayout() {
  SpLayout L;
  L.tex.resize(71);  // texture 70 exists only when ntex is 71
  Lcg rng{0xC0DE};
  FixCounts fc;
  for (int i = 0; i < 12; ++i) {
    SpTex& t = L.tex[kPaintList[i]];
    t.w = kPaintSize[i][0], t.h = kPaintSize[i][1], t.fmt = 0x12, t.blocks_kind = true;
    t.blocks = OpaqueBlocks((t.w / 4) * (t.h / 4), rng, fc);
    t.data_bytes = uint32_t(t.blocks.size());
  }
  for (int k : {10, 12, 21, 23, 68}) {
    SpTex& t = L.tex[k];
    t.w = 128, t.h = 128, t.fmt = 0x12, t.blocks_kind = true;
    FixCounts unused;
    t.blocks = OpaqueBlocks(32 * 32, rng, unused);
    t.data_bytes = uint32_t(t.blocks.size());
  }
  for (int k = 0; k < 71; ++k) {
    SpTex& t = L.tex[k];
    if (t.blocks_kind) continue;
    if (k % 3 == 0) t.fmt = 0x12;  // DXT1 64x64, 2048 bytes
    t.data_bytes = t.fmt == 0x12 ? 2048 : 4096;
  }
  L.tex[13].tiled = 0;
  L.tex[14].mip = 0x7000, L.tex[14].mip_max = 2;
  L.tex[15].endian = 0;
  L.tex[16].w = 20, L.tex[16].h = 20, L.tex[16].data_bytes = 200;
  g_fix = fc;
  // address order
  std::vector<int> order;
  for (int j = 0; j < 69; ++j) order.push_back((j * 37) % 69);
  order.push_back(70);
  order.push_back(69);
  uint32_t pos = 0;
  int placed = 0;
  for (int k : order) {
    SpTex& t = L.tex[k];
    t.base = pos;
    const uint32_t slot = k == 69 ? t.data_bytes : (t.data_bytes + 0xFFF) & ~0xFFFu;
    L.data.resize(pos + slot, 0xEE);
    if (!t.blocks_kind)
      for (uint32_t i = 0; i < t.data_bytes; ++i) L.data[pos + i] = uint8_t(rng.Next());
    pos += slot;
    if (++placed % 4 == 0 && k != 69) {
      L.data.resize(pos + 0x1000, 0xEE);
      pos += 0x1000;
    }
  }
  for (int k = 0; k < 71; ++k) Place(L, k);
  return L;
}

static Bytes SpDescriptor(const SpTex& t) {
  Bytes d;
  for (int i = 0; i < 8; ++i) d.push_back(uint8_t(0xD0 + i));
  Put32(d, (t.tiled << 31) | 0x01000002u);
  Put32(d, t.base | 0x300u | (t.endian << 6) | t.fmt);
  Put32(d, (t.w - 1) | ((t.h - 1) << 13) | (5u << 26));
  Put32(d, 0x00D10000u);
  Put32(d, 0x12345438u | (t.mip_max << 6));
  Put32(d, t.mip | 0xA55u);
  d.resize(t.desc_len);
  return d;
}

static Bytes SpPack(const SpLayout& L) {
  struct R {
    const char* type;
    Bytes blob;
  };
  std::vector<R> rs;
  rs.push_back({"USER", Bytes(8, 0x55)});
  for (size_t k = 0; k < L.ntex; ++k) {
    if (k == 30) rs.push_back({"IBUF", Bytes(24, 0x66)});
    rs.push_back({"TX2D", SpDescriptor(L.tex[k])});
  }
  rs.push_back({"VBUF", Bytes(32, 0x77)});
  const uint32_t n = uint32_t(rs.size());
  Bytes data(L.data.begin(), L.data.end() - L.truncate);
  Bytes f;
  for (int i = 0; i < 4; ++i) f.push_back(uint8_t(L.magic[i]));
  Put32(f, kSpData0 - 12);
  Put32(f, uint32_t(data.size()));
  Put32(f, n);
  uint32_t at = 16 + 16 * n;
  for (const R& r : rs) {
    for (int i = 0; i < 4; ++i) f.push_back(uint8_t(r.type[i]));
    Put32(f, at - 12);
    Put32(f, uint32_t(r.blob.size()));
    Put32(f, 0);
    at += uint32_t(r.blob.size());
  }
  for (const R& r : rs) f.insert(f.end(), r.blob.begin(), r.blob.end());
  f.resize(kSpData0, 0);
  f.insert(f.end(), data.begin(), data.end());
  return f;
}

// tile(EncodeDxt1(Repaint(RGB of DecodeDxt1(blocks)))) as row-major LE blocks
static Bytes RepaintBlocks(const Bytes& blocks, uint32_t w, uint32_t h, const cg::SpartanRecipe& r) {
  const size_t px = size_t(w) * h;
  Bytes rgba(px * 4), rgb(px * 3), out(px / 2);
  cg::DecodeDxt1(blocks.data(), w / 4, h / 4, rgba.data());
  for (size_t i = 0; i < px; ++i)
    for (int c = 0; c < 3; ++c) rgb[3 * i + c] = rgba[4 * i + c];
  cg::Repaint(r, rgb.data(), rgb.data(), px);
  cg::EncodeDxt1(rgb.data(), w, h, out.data());
  return out;
}

// The expected output, from this file's own knowledge of where each texture's blocks are.
static Bytes SpExpected(const SpLayout& L, const Bytes& src, const cg::SpartanRecipe& r) {
  Bytes e = src;
  for (int k : kPaintList) {
    const SpTex& t = L.tex[k];
    PlaceTiled(e.data() + kSpData0 + t.base, RepaintBlocks(t.blocks, t.w, t.h, r), t.w / 4, t.h / 4);
  }
  return e;
}

static bool RangeEq(const Bytes& a, const Bytes& b, size_t at, size_t n) {
  return at + n <= a.size() && at + n <= b.size() && std::equal(a.begin() + at, a.begin() + at + n, b.begin() + at);
}

// ---------------------------------------------------------------------------------------------------------------
// make_spartan_synthetic
// ---------------------------------------------------------------------------------------------------------------

static void TestMakeSpartanSynthetic() {
  Group("make_spartan_synthetic");
  const SpLayout L = GoodSpLayout();
  const Bytes src = SpPack(L);
  std::printf("   fixture: %d paint blocks, %d had their words swapped, %d (c0 == c1) had their index word cleared\n",
              g_fix.blocks, g_fix.swapped, g_fix.cleared);
  CKF(g_fix.swapped > 1000 && g_fix.cleared > 100, "fix counts: swapped %d cleared %d", g_fix.swapped, g_fix.cleared);
  // fixture preconditions
  {
    const tp::Xpr x = tp::Xpr::Parse(Sp(src));
    CK(x.res.size() == 73 && x.OfType(tp::MakeFourcc("TX2D")).size() == 70);
    CK(x.hs && 12 + *x.hs == kSpData0);
    CK(src.size() == size_t(kSpData0) + L.tex[69].base + L.tex[69].data_bytes);  // 69 ends at the end of the file
  }
  int descending = 0, wide = 0, high = 0, square = 0;
  for (int i = 0; i < 12; ++i) {
    const SpTex& t = L.tex[kPaintList[i]];
    if (i < 11 && L.tex[kPaintList[i + 1]].base < t.base) ++descending;
    wide += t.w == 256 && t.h == 128;
    high += t.w == 128 && t.h == 256;
    square += t.w == 128 && t.h == 128;
  }
  CKF(descending >= 3 && wide >= 1 && high >= 1 && square >= 1, "descending %d wide %d high %d square %d", descending,
      wide, high, square);
  const cg::SpartanRecipe recipes[2] = {cg::kH3Pink, TestRecipe(1)};
  const char* const names[2] = {"pink", "t2"};
  Bytes outs[2];
  for (int ri = 0; ri < 2; ++ri) {
    const cg::SpartanRecipe& r = recipes[ri];
    std::string why;
    const std::optional<Bytes> out = cg::MakeSpartan(Sp(src), r, &why);
    const Bytes want = SpExpected(L, src, r);
    // each of the twelve ranges changes under this recipe (a recipe that leaves one alone would prove nothing there)
    int same_paint = 0;
    for (int k : kPaintList) same_paint += RangeEq(want, src, kSpData0 + L.tex[k].base, L.tex[k].data_bytes);
    CKF(same_paint == 0, "%s: %d paint ranges the recipe does not change", names[ri], same_paint);
    CKF(out.has_value(), "%s: refused: %s", names[ri], why.c_str());
    if (!out) continue;
    CKF(out->size() == src.size(), "%s: size %zu, source %zu", names[ri], out->size(), src.size());
    if (out->size() != src.size()) continue;
    outs[ri] = *out;
    CKF(*out == want, "%s: output differs from the expected bytes", names[ri]);
    CK(RangeEq(*out, src, 0, kSpData0));  // header and descriptors
    int other_moved = 0, paint_wrong = 0;
    for (int k = 0; k < 70; ++k) {
      const SpTex& t = L.tex[k];
      const size_t a = kSpData0 + t.base;
      if (IsPaint(k)) paint_wrong += !RangeEq(*out, want, a, t.data_bytes);
      else other_moved += !RangeEq(*out, src, a, t.data_bytes);
    }
    CKF(other_moved == 0, "%s: %d of the 58 other textures changed", names[ri], other_moved);
    CKF(paint_wrong == 0, "%s: %d paint textures unlike the expected bytes", names[ri], paint_wrong);
    // the gaps (0xEE) between textures
    size_t gap_moved = 0;
    {
      std::vector<uint8_t> covered(src.size(), 0);
      for (int k = 0; k < 70; ++k)
        for (uint32_t i = 0; i < L.tex[k].data_bytes; ++i) covered[kSpData0 + L.tex[k].base + i] = 1;
      for (size_t i = kSpData0; i < src.size(); ++i) gap_moved += !covered[i] && (*out)[i] != src[i];
    }
    CKF(gap_moved == 0, "%s: %zu gap bytes changed", names[ri], gap_moved);
  }
  CK(!outs[0].empty() && !outs[1].empty() && outs[0] != outs[1]);
  // Shared range (the tool accepts it): texture 69 has the base and the blocks of texture 67. Both are decoded from the
  // source, so the range holds the repaint of the source once; decoding 69 from the output being built would repaint
  // it twice.
  {
    SpLayout S = L;
    S.tex[69].base = S.tex[67].base;
    S.tex[69].blocks = S.tex[67].blocks;
    CK(S.tex[69].w == S.tex[67].w && S.tex[69].h == S.tex[67].h);
    const Bytes ssrc = SpPack(S);
    const SpTex& t = S.tex[67];
    const Bytes once = RepaintBlocks(t.blocks, t.w, t.h, cg::kH3Pink);
    const Bytes twice = RepaintBlocks(once, t.w, t.h, cg::kH3Pink);
    CK(once != twice);  // precondition: the double repaint differs
    Bytes want = ssrc;
    for (int k : kPaintList)
      if (k != 69) PlaceTiled(want.data() + kSpData0 + S.tex[k].base, RepaintBlocks(S.tex[k].blocks, S.tex[k].w, S.tex[k].h, cg::kH3Pink), S.tex[k].w / 4, S.tex[k].h / 4);
    std::string why;
    const std::optional<Bytes> out = cg::MakeSpartan(Sp(ssrc), cg::kH3Pink, &why);
    CKF(out.has_value() && *out == want, "shared range: refused=%d why '%s'", int(!out.has_value()), why.c_str());
  }
#if defined(_M_X64) || defined(__x86_64__)
  // The same bytes whatever control word the caller runs under. Precondition: under round toward zero the expected
  // computation itself (Repaint and EncodeDxt1 called directly) differs.
  {
    const Bytes want = SpExpected(L, src, TestRecipe(1));
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(saved | 0x6000u);  // rounding control 11: toward zero
    const Bytes want_rz = SpExpected(L, src, TestRecipe(1));
    const std::optional<Bytes> rz = cg::MakeSpartan(Sp(src), TestRecipe(1), nullptr);
    const unsigned during = _mm_getcsr();
    _mm_setcsr(saved);
    CK(want_rz != want);
    CK(rz.has_value() && *rz == want);
    CK((during & 0x6000u) == 0x6000u);
  }
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// make_spartan_refusals: one row per refusal, each differing from the good synthetic file in one property; the good
// file is the accepted control. Properties of textures outside kPaintTextures never refuse.
// ---------------------------------------------------------------------------------------------------------------

static void SpRefused(const char* name, const Bytes& f, const cg::SpartanRecipe& r = cg::kH3Pink,
                      const char* want_why = nullptr) {
  std::string why;
  const std::optional<Bytes> out = cg::MakeSpartan(Sp(f), r, &why);
  CKF(!out.has_value() && !why.empty(), "%s: accepted=%d why '%s'", name, int(out.has_value()), why.c_str());
  if (want_why) CKF(why.rfind(want_why, 0) == 0, "%s: why '%s', want it to start '%s'", name, why.c_str(), want_why);
}

static std::optional<Bytes> SpAccepted(const char* name, const Bytes& f, const cg::SpartanRecipe& r = cg::kH3Pink) {
  std::string why;
  std::optional<Bytes> out = cg::MakeSpartan(Sp(f), r, &why);
  CKF(out.has_value() && out->size() == f.size(), "%s: refused: %s", name, why.c_str());
  return out;
}

static void TestMakeSpartanRefusals() {
  Group("make_spartan_refusals");
  const SpLayout good = GoodSpLayout();
  const Bytes gf = SpPack(good);
  const std::optional<Bytes> good_out = SpAccepted("good file", gf);
  CK(cg::MakeSpartan(Sp(gf), cg::kH3Pink, nullptr).has_value());
  auto with = [&](auto&& edit) {
    SpLayout L = good;
    edit(L);
    return SpPack(L);
  };
  SpRefused("bad magic", with([](SpLayout& L) { L.magic = "XPR3"; }), cg::kH3Pink, "assert ");
  SpRefused("69 textures", with([](SpLayout& L) { L.ntex = 69; }), cg::kH3Pink, "the file has 69 textures, fewer than 70");
  SpRefused("texture 36 DXT5", with([](SpLayout& L) { L.tex[36].fmt = 0x14; }), cg::kH3Pink, "texture 36: format 20, not DXT1 (18)");
  SpRefused("texture 36 not tiled", with([](SpLayout& L) { L.tex[36].tiled = 0; }), cg::kH3Pink, "texture 36: not tiled");
  SpRefused("texture 36 endian 0", with([](SpLayout& L) { L.tex[36].endian = 0; }), cg::kH3Pink, "texture 36: endian 0, not 1");
  SpRefused("texture 36 endian 2", with([](SpLayout& L) { L.tex[36].endian = 2; }), cg::kH3Pink, "texture 36: endian 2, not 1");
  SpRefused("texture 36 mip address", with([](SpLayout& L) { L.tex[36].mip = 0x5000; }), cg::kH3Pink, "texture 36: has a mip address");
  SpRefused("texture 36 mip max 3, no mip address", with([](SpLayout& L) { L.tex[36].mip_max = 3; }), cg::kH3Pink, "texture 36: mip max 3, not 0");
  SpRefused("texture 22 width 192 (48 blocks)", with([](SpLayout& L) { L.tex[22].w = 192; }), cg::kH3Pink, "texture 22: width 192 is not a multiple of 32 blocks");
  SpRefused("texture 27 height 192 (48 blocks)", with([](SpLayout& L) { L.tex[27].h = 192; }), cg::kH3Pink, "texture 27: height 192 is not a multiple of 32 blocks");
  SpRefused("texture 11 width 130 (32 blocks)", with([](SpLayout& L) { L.tex[11].w = 130; }), cg::kH3Pink, "texture 11: width 130 is not a multiple of 4");
  SpRefused("texture 11 height 131 (32 blocks)", with([](SpLayout& L) { L.tex[11].h = 131; }), cg::kH3Pink, "texture 11: height 131 is not a multiple of 4");
  SpRefused("data truncated by 1 byte inside texture 69", with([](SpLayout& L) { L.truncate = 1; }), cg::kH3Pink, "texture 69: its blocks run past the end of the file");
  SpRefused("texture 36 base 0xFFFFF000", with([](SpLayout& L) { L.tex[36].base = 0xFFFFF000u; }), cg::kH3Pink, "texture 36: its blocks run past the end of the file");
  SpRefused("texture 31 descriptor of 20 bytes", with([](SpLayout& L) { L.tex[31].desc_len = 20; }));
  // a transparent texel: c0 < c1 with an index 3 (pixel 7 of block 5 of texture 42), and c0 == c1 with an index 3
  SpRefused("texture 42 transparent texel (c0 < c1)", with([](SpLayout& L) {
              PutBlock(&L.tex[42].blocks[5 * 8], 0x1234, 0xA5C3, 3u << 14);
              Place(L, 42);
            }), cg::kH3Pink, "texture 42: has transparent texels");
  SpRefused("texture 61 transparent texel (c0 == c1)", with([](SpLayout& L) {
              PutBlock(&L.tex[61].blocks[200 * 8], 0x7BEF, 0x7BEF, 0xC0000000u);
              Place(L, 61);
            }), cg::kH3Pink, "texture 61: has transparent texels");
  {
    const Bytes f = with([](SpLayout& L) { L.tex[36].fmt = 0x14; });
    CK(!cg::MakeSpartan(Sp(f), cg::kH3Pink, nullptr).has_value());  // why may be null on a refusal too
  }
  // accepted: more than 70 textures; a three-colour block with no index 3 (opaque); properties of other textures
  SpAccepted("71 textures", with([](SpLayout& L) { L.ntex = 71; }));
  SpAccepted("texture 42 c0 < c1 without an index 3", with([](SpLayout& L) {
               PutBlock(&L.tex[42].blocks[5 * 8], 0x1234, 0xA5C3, 0x24924924u);  // indices 0, 1, 2 only
               Place(L, 42);
             }));
  SpAccepted("texture 10 DXT5", with([](SpLayout& L) { L.tex[10].fmt = 0x14; }));
  SpAccepted("texture 12 mip address and mip max", with([](SpLayout& L) { L.tex[12].mip = 0x9000, L.tex[12].mip_max = 4; }));
  SpAccepted("texture 23 20x20", with([](SpLayout& L) { L.tex[23].w = 20, L.tex[23].h = 20; }));
  SpAccepted("texture 68 not tiled, endian 0", with([](SpLayout& L) { L.tex[68].tiled = 0, L.tex[68].endian = 0; }));
  SpAccepted("texture 21 base 0xFFFFF000", with([](SpLayout& L) { L.tex[21].base = 0xFFFFF000u; }));
  // recipes outside Repaint's domain
  const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
  auto rc = [](auto&& edit) {
    cg::SpartanRecipe r = cg::kH3Pink;
    edit(r);
    return r;
  };
  SpRefused("hue_shift 0.01", gf, rc([](cg::SpartanRecipe& r) { r.hue_shift = 0.01; }), "recipe: hue_shift above 0");
  SpRefused("hue_shift -1.01", gf, rc([](cg::SpartanRecipe& r) { r.hue_shift = -1.01; }), "recipe: hue_shift below -1 or NaN");
  SpRefused("hue_shift NaN", gf, rc([&](cg::SpartanRecipe& r) { r.hue_shift = nan; }), "recipe: hue_shift below -1 or NaN");
  SpRefused("vref 0", gf, rc([](cg::SpartanRecipe& r) { r.vref = 0.0; }), "recipe: vref not above 0, or NaN");
  SpRefused("vref 1", gf, rc([](cg::SpartanRecipe& r) { r.vref = 1.0; }), "recipe: vref not below 1");
  SpRefused("vref NaN", gf, rc([&](cg::SpartanRecipe& r) { r.vref = nan; }), "recipe: vref not above 0, or NaN");
  SpRefused("saturation -0.1", gf, rc([](cg::SpartanRecipe& r) { r.saturation = -0.1; }), "recipe: saturation negative or NaN");
  SpRefused("saturation inf", gf, rc([&](cg::SpartanRecipe& r) { r.saturation = inf; }), "recipe: saturation infinite");
  SpRefused("saturation NaN", gf, rc([&](cg::SpartanRecipe& r) { r.saturation = nan; }), "recipe: saturation negative or NaN");
  SpRefused("brightness -0.1", gf, rc([](cg::SpartanRecipe& r) { r.brightness = -0.1; }), "recipe: brightness negative or NaN");
  SpRefused("brightness inf", gf, rc([&](cg::SpartanRecipe& r) { r.brightness = inf; }), "recipe: brightness infinite");
  SpRefused("brightness NaN", gf, rc([&](cg::SpartanRecipe& r) { r.brightness = nan; }), "recipe: brightness negative or NaN");
  SpRefused("amount NaN", gf, rc([&](cg::SpartanRecipe& r) { r.amount = nan; }), "recipe: amount not finite");
  SpRefused("amount inf", gf, rc([&](cg::SpartanRecipe& r) { r.amount = inf; }), "recipe: amount not finite");
  SpRefused("amount -inf", gf, rc([&](cg::SpartanRecipe& r) { r.amount = -inf; }), "recipe: amount not finite");
  // the domain's edges are accepted; amount is clamped to 0..1, not refused
  SpAccepted("hue_shift -1", gf, rc([](cg::SpartanRecipe& r) { r.hue_shift = -1.0; }));
  SpAccepted("saturation 0, brightness 0", gf, rc([](cg::SpartanRecipe& r) { r.saturation = 0.0, r.brightness = 0.0; }));
  const std::optional<Bytes> a5 = SpAccepted("amount 5", gf, rc([](cg::SpartanRecipe& r) { r.amount = 5.0; }));
  CK(a5 && good_out && *a5 == *good_out);  // amount 5 is amount 1 (the pink recipe's)
  const std::optional<Bytes> am3 = SpAccepted("amount -3", gf, rc([](cg::SpartanRecipe& r) { r.amount = -3.0; }));
  const std::optional<Bytes> a0 = SpAccepted("amount 0", gf, rc([](cg::SpartanRecipe& r) { r.amount = 0.0; }));
  CK(am3 && a0 && *am3 == *a0 && *a0 != *good_out);
}

// ---------------------------------------------------------------------------------------------------------------
// make_by_name: the seven slot names, each giving MakeAlpha / MakeSpartan with its own recipe (the seven direct outputs
// are first shown pairwise different within each kind, so two swapped names are visible); unknown names refuse.
// ---------------------------------------------------------------------------------------------------------------

static void TestMakeByName() {
  Group("make_by_name");
  const Bytes asrc = Pack(GoodLayout());
  const Bytes ssrc = SpPack(GoodSpLayout());
  std::optional<Bytes> direct[7] = {cg::MakeAlpha(Sp(asrc), cg::kAlphaRed, nullptr),
                                    cg::MakeAlpha(Sp(asrc), cg::kAlphaBlue, nullptr),
                                    cg::MakeAlpha(Sp(asrc), cg::kAlphaLilac, nullptr),
                                    cg::MakeSpartan(Sp(ssrc), cg::kH3Pink, nullptr),
                                    cg::MakeSpartan(Sp(ssrc), cg::kH3Orchid, nullptr),
                                    cg::MakeSpartan(Sp(ssrc), cg::kH2Orange, nullptr),
                                    cg::MakeSpartan(Sp(ssrc), cg::kH2Cyan, nullptr)};
  static const char* const kNames[7] = {"alpha_red",         "alpha_blue",        "alpha_lilac",    "spartan_h3_pink",
                                        "spartan_h3_orchid", "spartan_h2_orange", "spartan_h2_cyan"};
  int missing = 0, same = 0;
  for (int i = 0; i < 7; ++i) missing += !direct[i].has_value();
  CKF(missing == 0, "%d direct outputs missing", missing);
  if (missing) return;
  for (int i = 0; i < 7; ++i)
    for (int j = i + 1; j < 7; ++j) same += (i < 3) == (j < 3) && *direct[i] == *direct[j];
  CKF(same == 0, "%d pairs of direct outputs of one kind are equal", same);
  for (int i = 0; i < 7; ++i) {
    std::string why;
    const std::optional<Bytes> got = cg::Make(kNames[i], Sp(i < 3 ? asrc : ssrc), &why);
    CKF(got.has_value() && *got == *direct[i], "%s: refused=%d why '%s', or unlike its recipe's output", kNames[i],
        int(!got.has_value()), why.c_str());
  }
  for (const char* bad : {"alpha_green", "", "Alpha_Red", "spartan_h3_pink ", "spartan_h2_cyan2"}) {
    std::string why;
    const std::optional<Bytes> got = cg::Make(bad, Sp(ssrc), &why);
    CKF(!got.has_value() && !why.empty(), "'%s': accepted=%d why '%s'", bad, int(got.has_value()), why.c_str());
  }
  CK(!cg::Make("alpha_green", Sp(asrc), nullptr).has_value());
  // a Spartan name on the Alpha source: texture 11 there is DXT5; an Alpha name on the Spartan source: 70 textures
  {
    std::string why;
    const bool sp_no = !cg::Make("spartan_h3_pink", Sp(asrc), &why).has_value(); CKF(sp_no && why == "texture 11: format 20, not DXT1 (18)",
        "why '%s'", why.c_str());
    why.clear();
    const bool al_no = !cg::Make("alpha_red", Sp(ssrc), &why).has_value(); CKF(al_no && why == "the file has 70 textures, not 144", "why '%s'",
        why.c_str());
  }
}

// A group that throws loses its remaining checks, never the other groups'.
static void RunGroup(void (*fn)(), const char* name) {
  const auto t0 = std::chrono::steady_clock::now();
  try {
    fn();
  } catch (const std::exception& e) {
    ++g_checks;
    ++g_failures;
    std::printf("FAIL %s threw: %s\n", name, e.what());
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("   (%s: %.2f s)\n", name, s);
}

// The probe census: every check line that ran, with and without a failure.
static void PrintSites() {
  std::printf("GREEN-SITES:");
  for (const auto& [line, rf] : g_sites)
    if (rf.second == 0) std::printf(" %d", line);
  std::printf("\nRED-SITES:");
  for (const auto& [line, rf] : g_sites)
    if (rf.second != 0) std::printf(" %d", line);
  std::printf("\n");
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not swallow what was printed before it
  doa4::suitconv::num::FpEnvGuard fp;        // the converter's floating-point environment (MXCSR 0x1F80)
  const auto t0 = std::chrono::steady_clock::now();
  RunGroup(TestAlphaExhaustive, "alpha_exhaustive");
  RunGroup(TestAlphaSamples, "alpha_samples");
  RunGroup(TestAlphaEdgeRecipes, "alpha_edge_recipes");
  RunGroup(TestMakeAlphaSynthetic, "make_alpha_synthetic");
  RunGroup(TestMakeAlphaRefusals, "make_alpha_refusals");
  RunGroup(TestRepaintExhaustive, "repaint_exhaustive");
  RunGroup(TestQuantTies, "quant_ties");
  RunGroup(TestTiled, "tiled");
  RunGroup(TestFamilyInputs, "family_inputs");
  RunGroup(TestEncodeFamilies, "encode_families");
  RunGroup(TestDecodeRoundTrip, "decode_round_trip");
  RunGroup(TestMakeSpartanSynthetic, "make_spartan_synthetic");
  RunGroup(TestMakeSpartanRefusals, "make_spartan_refusals");
  RunGroup(TestMakeByName, "make_by_name");
  std::printf("   (all groups: %.2f s)\n",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  PrintSites();
  if (g_failures) {
    std::printf("FAILED: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }
  std::printf("PASSED: %d checks, 0 failures\n", g_checks);
  return 0;
}
