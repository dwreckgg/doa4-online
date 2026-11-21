// Host test for src/doa4_colourgen_alpha.cpp (the Alpha-152 effect recolour of the colour generator). Built and run by
// run_colourgen_test.bat under the converter's floating-point flags.
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
// No byte of any game file is stored. Last line: "PASSED: N checks, 0 failures" or "FAILED: N checks, M failures".

#include "../../src/doa4_colourgen.h"
#include "../../src/doa4_gamefiles.h"
#include "../../src/doa4_suitconv_num.h"
#include "../../src/doa4_suitconv_tpr.h"
#include "colourgen_pins.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
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
static constexpr int kPrintCap = 200;  // above the file's check count: every FAIL line prints (the probe census)

static void Check(bool ok, const char* file, int line, const char* expr, const char* fmt = nullptr, ...) {
  ++g_checks;
  if (ok) return;
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

// A group that throws loses its remaining checks, never the other groups'.
static void RunGroup(void (*fn)(), const char* name) {
  try {
    fn();
  } catch (const std::exception& e) {
    ++g_checks;
    ++g_failures;
    std::printf("FAIL %s threw: %s\n", name, e.what());
  }
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not swallow what was printed before it
  doa4::suitconv::num::FpEnvGuard fp;        // the converter's floating-point environment (MXCSR 0x1F80)
  RunGroup(TestAlphaExhaustive, "alpha_exhaustive");
  RunGroup(TestAlphaSamples, "alpha_samples");
  RunGroup(TestAlphaEdgeRecipes, "alpha_edge_recipes");
  RunGroup(TestMakeAlphaSynthetic, "make_alpha_synthetic");
  RunGroup(TestMakeAlphaRefusals, "make_alpha_refusals");
  if (g_failures) {
    std::printf("FAILED: %d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }
  std::printf("PASSED: %d checks, 0 failures\n", g_checks);
  return 0;
}
