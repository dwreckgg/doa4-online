#include "doa4_suitconv_fp.h"

#include "doa4_colourgen.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "doa4_suitconv_num.h"
#include "doa4_suitconv_tpr.h"

// The Spartan-458 armour repaint, one IEEE double operation per statement in the order the reference evaluates them
// (no reordering, no float, no C runtime maths call), and the file-level MakeSpartan and Make. The sRGB-to-linear table
// and the byte thresholds come from doa4_colourgen_tables.inc (exact doubles), so no power function is evaluated here.

namespace doa4::colourgen {

#include "doa4_colourgen_tables.inc"

namespace {

// numpy's maximum / minimum on one pair: the first operand when it compares >= (<=) or is NaN, else the second
inline double MaxD(double a, double b) { return (a >= b || a != a) ? a : b; }
inline double MinD(double a, double b) { return (a <= b || a != a) ? a : b; }

// clip to 0..1 as the reference does it: maximum with 0, then minimum with 1
inline double Clip01(double x) {
  const double y = MaxD(x, 0.0);
  return MinD(y, 1.0);
}

// The sRGB byte of a linear value: the number of kByteThreshold entries <= c (numpy searchsorted side='right'; written
// as !(c < entry) so that a NaN counts every entry, as numpy sorts NaN last).
inline uint8_t ToByte(double c) {
  int lo = 0, hi = 255;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (!(c < kByteThreshold[mid])) lo = mid + 1;
    else hi = mid;
  }
  return static_cast<uint8_t>(lo);
}

constexpr double kInf = std::numeric_limits<double>::infinity();

// Repaint's domain (each test written so that NaN fails it); null when the recipe is inside it.
// hue_shift: h is in [0, 1] (h + 1.0 of a tiny negative hue can round to 1.0), so the hue sum below is in [0, 2] for
// -1 <= hue_shift <= 0; on [0, 2) the wrap equals the reference's mod 1.0, and at 2.0 the wrap gives 1.0 where mod
// gives 0.0, which hsv_to_rgb's first step (1.0 -> 0.0) maps to the same value.
const char* OutsideDomain(const SpartanRecipe& r) {
  if (!(r.hue_shift >= -1.0)) return "recipe: hue_shift below -1 or NaN";
  if (!(r.hue_shift <= 0.0)) return "recipe: hue_shift above 0";
  if (!(r.vref > 0.0)) return "recipe: vref not above 0, or NaN";
  if (!(r.vref < 1.0)) return "recipe: vref not below 1";
  if (!(r.saturation >= 0.0)) return "recipe: saturation negative or NaN";
  if (!(r.saturation < kInf)) return "recipe: saturation infinite";
  if (!(r.brightness >= 0.0)) return "recipe: brightness negative or NaN";
  if (!(r.brightness < kInf)) return "recipe: brightness infinite";
  if (!(r.amount > -kInf && r.amount < kInf)) return "recipe: amount not finite";
  return nullptr;
}

void RepaintPixel(const SpartanRecipe& rc, const double col[3], double a, double oma, const uint8_t* in, uint8_t* out) {
  const double lin[3] = {kSrgbToLinear[in[0]], kSrgbToLinear[in[1]], kSrgbToLinear[in[2]]};
  const double vref = rc.vref;
  const double one_m_vref = 1.0 - vref;
  double x = MaxD(lin[0], lin[1]);
  const double value = MaxD(x, lin[2]);
  x = value / vref;
  const double dark = Clip01(x);
  x = value - vref;
  x = x / one_m_vref;
  const double hi = Clip01(x);
  x = hi * hi;  // the reference's hi ** 2.0, then * 0.8 (the scratch curve times the scratch strength)
  x = x * 0.8;
  const double hi3 = Clip01(x);
  const double omh = 1.0 - hi3;
  double m3[3];
  for (int c = 0; c < 3; ++c) {
    const double m1 = col[c] * dark;
    x = m1 * omh;
    const double m2 = x + hi3;
    x = lin[c] * oma;  // lin * (1 - a) + m2 * a
    const double y = m2 * a;
    m3[c] = x + y;
  }
  const double r = m3[0], g = m3[1], b = m3[2];
  // Blender's rgb_to_hsv
  x = MaxD(r, g);
  const double cmax = MaxD(x, b);
  x = MinD(r, g);
  const double cmin = MinD(x, b);
  const double delta = cmax - cmin;
  double v = cmax;
  const bool nz = cmax != 0.0;
  const double den = nz ? cmax : 1.0;
  x = delta / den;
  double s = nz ? x : 0.0;
  const double safe = delta != 0.0 ? delta : 1.0;
  x = cmax - r;
  const double cr = x / safe;
  x = cmax - g;
  const double cg = x / safe;
  x = cmax - b;
  const double cb = x / safe;
  const double h_r = cb - cg;
  x = 2.0 + cr;  // (2.0 + cr) - cb, left to right
  const double h_g = x - cb;
  x = 4.0 + cg;  // (4.0 + cg) - cr
  const double h_b = x - cr;
  double h = r == cmax ? h_r : (g == cmax ? h_g : h_b);
  h = h / 6.0;
  x = h + 1.0;
  h = h < 0.0 ? x : h;
  h = s == 0.0 ? 0.0 : h;
  // the Hue/Saturation/Value node: hue sum (h + (hue_shift + 0.5)) + 0.5, wrapped by one subtraction. The hue sum is
  // below 2.0 on every input of the nine pinned recipes (asserted by the model); see OutsideDomain for the domain.
  const double hs = rc.hue_shift + 0.5;
  x = h + hs;
  x = x + 0.5;
  h = x >= 1.0 ? x - 1.0 : x;
  x = s * rc.saturation;
  s = Clip01(x);
  v = v * rc.brightness;
  // hsv_to_rgb
  h = h == 1.0 ? 0.0 : h;
  const double h6 = h * 6.0;
  int i = static_cast<int>(h6);  // truncation (h6 >= 0)
  const double f = h6 - static_cast<double>(i);
  x = 1.0 - s;
  const double p = v * x;
  x = s * f;
  x = 1.0 - x;
  const double q = v * x;
  x = 1.0 - f;
  x = s * x;
  x = 1.0 - x;
  const double t = v * x;
  i = i >= 6 ? i - 6 : i;  // i % 6 for 0 <= i <= 6
  double o[3];
  switch (i) {
    case 0: o[0] = v, o[1] = t, o[2] = p; break;
    case 1: o[0] = q, o[1] = v, o[2] = p; break;
    case 2: o[0] = p, o[1] = v, o[2] = t; break;
    case 3: o[0] = p, o[1] = q, o[2] = v; break;
    case 4: o[0] = t, o[1] = p, o[2] = v; break;
    default: o[0] = v, o[1] = p, o[2] = q; break;
  }
  if (s == 0.0) o[0] = v, o[1] = v, o[2] = v;
  for (int c = 0; c < 3; ++c) out[c] = ToByte(MaxD(o[c], 0.0));
}

class Refusal : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void Refuse(const std::string& m) { throw Refusal(m); }

// Writing the reason must not throw out of MakeSpartan or Make either (an allocation failure leaves *why as it was).
void SetWhy(std::string* why, const char* m) noexcept {
  if (!why) return;
  try {
    *why = m;
  } catch (...) {
  }
}

struct Job {
  std::span<const uint8_t> src;
  const SpartanRecipe* recipe;
  Bytes out;
};

constexpr size_t kMinTextures = 70;  // the reference indexes texture 69; more textures are accepted

// file bytes (tiled, each 16-bit word byte-swapped) -> row-major little-endian blocks. The caller checked that bw and bh
// are multiples of 32, where TiledBlockIndex is a permutation of 0..bw*bh-1 (the guard below is never taken then).
void Untile(const uint8_t* file, uint32_t bw, uint32_t bh, uint8_t* blocks) {
  const uint64_t n = uint64_t(bw) * bh;
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint32_t t = TiledBlockIndex(bx, by, bw);
      if (t >= n) Refuse("tiled block index outside the texture");
      const uint8_t* s = file + size_t(t) * 8;
      uint8_t* d = blocks + (size_t(by) * bw + bx) * 8;
      for (int i = 0; i < 4; ++i) {
        d[2 * i] = s[2 * i + 1];
        d[2 * i + 1] = s[2 * i];
      }
    }
}

// the inverse: row-major little-endian blocks -> the file's bytes
void Tile(const uint8_t* blocks, uint32_t bw, uint32_t bh, uint8_t* file) {
  const uint64_t n = uint64_t(bw) * bh;
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint32_t t = TiledBlockIndex(bx, by, bw);
      if (t >= n) Refuse("tiled block index outside the texture");
      const uint8_t* s = blocks + (size_t(by) * bw + bx) * 8;
      uint8_t* d = file + size_t(t) * 8;
      for (int i = 0; i < 4; ++i) {
        d[2 * i] = s[2 * i + 1];
        d[2 * i + 1] = s[2 * i];
      }
    }
}

void RunJob(void* ctx) {
  Job& job = *static_cast<Job*>(ctx);
  // first: the domain is tested under the conversion environment (a caller's denormals-are-zero must not refuse a
  // subnormal vref that is inside it)
  if (const char* m = OutsideDomain(*job.recipe)) Refuse(m);
  const std::span<const uint8_t> src = job.src;
  const suitconv::tpr::Xpr x = suitconv::tpr::Xpr::Parse(src);
  if (!x.hs) Refuse("no header size");
  const uint64_t data0 = 12ull + *x.hs;
  const std::vector<size_t> tex = x.OfType(suitconv::tpr::MakeFourcc("TX2D"));
  if (tex.size() < kMinTextures)
    Refuse("the file has " + std::to_string(tex.size()) + " textures, fewer than " + std::to_string(kMinTextures));
  job.out.assign(src.begin(), src.end());
  for (const int k : kPaintTextures) {
    const std::array<uint32_t, 6> fc = suitconv::tpr::TexFc(x.res[tex[size_t(k)]].blob);
    const uint32_t fmt = fc[1] & 0x3F, endian = (fc[1] >> 6) & 3, tiled = (fc[0] >> 31) & 1;
    const uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    const uint32_t base = fc[1] & 0xFFFFF000u, mip_addr = fc[5] & 0xFFFFF000u, mip_max = (fc[4] >> 6) & 15;
    const std::string at = "texture " + std::to_string(k) + ": ";
    if (fmt != 0x12) Refuse(at + "format " + std::to_string(fmt) + ", not DXT1 (18)");
    if (tiled != 1) Refuse(at + "not tiled");
    if (endian != 1) Refuse(at + "endian " + std::to_string(endian) + ", not 1 (16-bit swap)");
    if (mip_addr != 0) Refuse(at + "has a mip address");
    if (mip_max != 0) Refuse(at + "mip max " + std::to_string(mip_max) + ", not 0");
    if (w % 4 != 0) Refuse(at + "width " + std::to_string(w) + " is not a multiple of 4");
    if (h % 4 != 0) Refuse(at + "height " + std::to_string(h) + " is not a multiple of 4");
    const uint32_t bw = w / 4, bh = h / 4;  // at most 2048 each
    if (bw % 32 != 0) Refuse(at + "width " + std::to_string(w) + " is not a multiple of 32 blocks");
    if (bh % 32 != 0) Refuse(at + "height " + std::to_string(h) + " is not a multiple of 32 blocks");
    const uint64_t nblocks = uint64_t(bw) * bh;  // at most 2**22
    const uint64_t start = data0 + base;        // at most (2**32 + 11) + (2**32 - 4096)
    const uint64_t end = start + nblocks * 8;   // below 2**34: no wrap in 64 bits
    if (end > src.size()) Refuse(at + "its blocks run past the end of the file");
    const size_t px = size_t(nblocks) * 16;
    // decoded from the SOURCE (never from the output being built: two textures may share a range)
    Bytes blocks(size_t(nblocks) * 8), rgba(px * 4), rgb(px * 3);
    Untile(src.data() + start, bw, bh, blocks.data());
    DecodeDxt1(blocks.data(), bw, bh, rgba.data());
    for (size_t i = 0; i < px; ++i) {
      if (rgba[4 * i + 3] != 255) Refuse(at + "has transparent texels");
      for (int c = 0; c < 3; ++c) rgb[3 * i + c] = rgba[4 * i + c];
    }
    Repaint(*job.recipe, rgb.data(), rgb.data(), px);
    EncodeDxt1(rgb.data(), bw * 4, bh * 4, blocks.data());
    Tile(blocks.data(), bw, bh, job.out.data() + start);
  }
}

}  // namespace

void Repaint(const SpartanRecipe& r, const uint8_t* srgb_in, uint8_t* srgb_out, size_t pixels) {
  const double col[3] = {kSrgbToLinear[r.colour[0]], kSrgbToLinear[r.colour[1]], kSrgbToLinear[r.colour[2]]};
  double a = r.amount;
  if (!(a > 0.0)) a = 0.0;  // max(0.0, amount)
  if (!(a < 1.0)) a = 1.0;  // min(1.0, .)
  const double oma = 1.0 - a;
  for (size_t i = 0; i < pixels; ++i) RepaintPixel(r, col, a, oma, srgb_in + 3 * i, srgb_out + 3 * i);
}

std::optional<Bytes> MakeSpartan(std::span<const uint8_t> source, const SpartanRecipe& r, std::string* why) {
  try {
    Job job{source, &r, {}};
    suitconv::num::RunUnderConvertEnv(&RunJob, &job);
    return std::move(job.out);
  } catch (const std::exception& e) {
    SetWhy(why, e.what());
  } catch (...) {
    SetWhy(why, "unknown exception");
  }
  return std::nullopt;
}

std::optional<Bytes> Make(std::string_view slot, std::span<const uint8_t> source, std::string* why) {
  if (slot == "alpha_red") return MakeAlpha(source, kAlphaRed, why);
  if (slot == "alpha_blue") return MakeAlpha(source, kAlphaBlue, why);
  if (slot == "alpha_lilac") return MakeAlpha(source, kAlphaLilac, why);
  if (slot == "spartan_h3_pink") return MakeSpartan(source, kH3Pink, why);
  if (slot == "spartan_h3_orchid") return MakeSpartan(source, kH3Orchid, why);
  if (slot == "spartan_h2_orange") return MakeSpartan(source, kH2Orange, why);
  if (slot == "spartan_h2_cyan") return MakeSpartan(source, kH2Cyan, why);
  SetWhy(why, "unknown colour slot name");
  return std::nullopt;
}

}  // namespace doa4::colourgen
