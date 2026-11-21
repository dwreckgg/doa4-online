#include "doa4_suitconv_fp.h"

#include "doa4_colourgen.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "doa4_suitconv_num.h"
#include "doa4_suitconv_tpr.h"

// The Alpha-152 recolour, one IEEE double operation per statement in the order the reference evaluates them (no
// reordering, no float, no C runtime maths call). The HSV conversions are written out from the Python standard
// library's colorsys (rgb_to_hsv, hsv_to_rgb), as the reference uses them.

namespace doa4::colourgen {
namespace {

// Python's `x % 1.0` for x in (-1, 2): the remainder is x itself in [0, 1), x - 1.0 (exact) in [1, 2), and
// x + 1.0 (rounded) in (-1, 0).
double Mod1(double x) {
  if (x >= 1.0) return x - 1.0;
  if (x < 0.0) return x + 1.0;
  return x;
}

// Python's int(round(x)) for 0 <= x < 2**31: nearest integer, ties to even. The truncation and x - t are exact here.
int RoundHalfEven(double x) {
  int t = static_cast<int>(x);
  const double d = x - static_cast<double>(t);
  if (d > 0.5 || (d == 0.5 && (t & 1) == 1)) ++t;
  return t;
}

bool InDomain(const AlphaRecipe& r) {
  // written so that NaN fails every test
  return r.hue >= 0.0 && r.hue <= 360.0 && r.spread >= -1.0 && r.spread <= 1.0 && r.sat >= 0.0 &&
         r.sat < std::numeric_limits<double>::infinity();
}

class Refusal : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void Refuse(const std::string& m) { throw Refusal(m); }

// Writing the reason must not throw out of MakeAlpha either (an allocation failure leaves *why as it was).
void SetWhy(std::string* why, const char* m) noexcept {
  if (!why) return;
  try {
    *why = m;
  } catch (...) {
  }
}

struct Job {
  std::span<const uint8_t> src;
  const AlphaRecipe* recipe;
  Bytes out;
};

constexpr size_t kTextures = 144;
constexpr size_t kEffectEnd = 121;  // textures 0..120 are recoloured

void RunJob(void* ctx) {
  Job& job = *static_cast<Job*>(ctx);
  const std::span<const uint8_t> src = job.src;
  const suitconv::tpr::Xpr x = suitconv::tpr::Xpr::Parse(src);
  if (!x.hs) Refuse("no header size");
  const uint64_t data0 = 12ull + *x.hs;
  const std::vector<size_t> tex = x.OfType(suitconv::tpr::MakeFourcc("TX2D"));
  if (tex.size() != kTextures)
    Refuse("the file has " + std::to_string(tex.size()) + " textures, not " + std::to_string(kTextures));
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  spans.reserve(kEffectEnd);
  for (size_t k = 0; k < kEffectEnd; ++k) {
    const std::array<uint32_t, 6> fc = suitconv::tpr::TexFc(x.res[tex[k]].blob);
    const uint32_t fmt = fc[1] & 0x3F, endian = (fc[1] >> 6) & 3, tiled = (fc[0] >> 31) & 1;
    const uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    const uint32_t base = fc[1] & 0xFFFFF000u, mip_addr = fc[5] & 0xFFFFF000u;
    const std::string at = "texture " + std::to_string(k) + ": ";
    if (fmt != 0x14) Refuse(at + "format " + std::to_string(fmt) + ", not DXT5 (20)");
    if (endian != 1) Refuse(at + "endian " + std::to_string(endian) + ", not 1 (16-bit swap)");
    if (tiled != 1) Refuse(at + "not tiled");
    if (mip_addr != 0) Refuse(at + "has a mip address");
    if (w % 128 != 0) Refuse(at + "width " + std::to_string(w) + " is not a multiple of 128");
    if (h % 128 != 0) Refuse(at + "height " + std::to_string(h) + " is not a multiple of 128");
    const uint64_t n = uint64_t(w / 4) * uint64_t(h / 4);
    const uint64_t start = data0 + base;
    const uint64_t end = start + n * 16;  // at most 2**32 + 2**30: no wrap in 64 bits
    if (end > src.size()) Refuse(at + "its blocks run past the end of the file");
    spans.emplace_back(start, end);
  }
  std::sort(spans.begin(), spans.end());
  for (size_t i = 0; i + 1 < spans.size(); ++i)
    if (spans[i].second > spans[i + 1].first) Refuse("texture data ranges overlap");
  job.out.assign(src.begin(), src.end());
  uint8_t* out = job.out.data();
  for (const auto& [start, end] : spans)
    for (uint64_t b = start; b < end; b += 16)
      for (const uint64_t o : {b + 8, b + 10}) {  // the two colour endpoints, big-endian words
        const uint16_t c = static_cast<uint16_t>((src[o] << 8) | src[o + 1]);
        const uint16_t v = AlphaRecolour565(c, *job.recipe);
        out[o] = static_cast<uint8_t>(v >> 8);
        out[o + 1] = static_cast<uint8_t>(v & 0xFF);
      }
}

}  // namespace

uint16_t AlphaRecolour565(uint16_t c, const AlphaRecipe& rc) {
  const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  const double rf = static_cast<double>(r) / 31.0;
  const double gf = static_cast<double>(g) / 63.0;
  const double bf = static_cast<double>(b) / 31.0;
  // colorsys.rgb_to_hsv
  double maxc = rf;
  if (gf > maxc) maxc = gf;
  if (bf > maxc) maxc = bf;
  double minc = rf;
  if (gf < minc) minc = gf;
  if (bf < minc) minc = bf;
  const double v = maxc;
  double h, s, x;
  if (minc == maxc) {
    h = 0.0;
    s = 0.0;
  } else {
    const double rangec = maxc - minc;
    s = rangec / maxc;
    x = maxc - rf;
    const double rc_ = x / rangec;
    x = maxc - gf;
    const double gc = x / rangec;
    x = maxc - bf;
    const double bc = x / rangec;
    if (rf == maxc) {
      h = bc - gc;
    } else if (gf == maxc) {
      x = 2.0 + rc_;  // (2.0 + rc) - bc, left to right
      h = x - bc;
    } else {
      x = 4.0 + gc;  // (4.0 + gc) - rc
      h = x - rc_;
    }
    x = h / 6.0;
    h = Mod1(x);
  }
  // the recolour
  x = rc.hue / 360.0;
  double y = h - 0.5;
  y = y * rc.spread;
  x = x + y;
  h = Mod1(x);
  s = s * rc.sat;
  if (!(s < 1.0)) s = 1.0;  // min(1.0, s): a clamp at the top only
  // colorsys.hsv_to_rgb
  double ro, go, bo;
  if (s == 0.0) {
    ro = v;
    go = v;
    bo = v;
  } else {
    const double h6 = h * 6.0;
    int i = static_cast<int>(h6);  // truncation; h6 >= 0
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
    switch (i) {
      case 0: ro = v, go = t, bo = p; break;
      case 1: ro = q, go = v, bo = p; break;
      case 2: ro = p, go = v, bo = t; break;
      case 3: ro = p, go = q, bo = v; break;
      case 4: ro = t, go = p, bo = v; break;
      default: ro = v, go = p, bo = q; break;
    }
  }
  x = ro * 31.0;
  const int rq = RoundHalfEven(x);
  x = go * 63.0;
  const int gq = RoundHalfEven(x);
  x = bo * 31.0;
  const int bq = RoundHalfEven(x);
  return static_cast<uint16_t>((rq << 11) | (gq << 5) | bq);
}

std::optional<Bytes> MakeAlpha(std::span<const uint8_t> source, const AlphaRecipe& r, std::string* why) {
  try {
    if (!InDomain(r)) Refuse("recipe outside 0 <= hue <= 360, -1 <= spread <= 1, 0 <= sat < inf");
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

}  // namespace doa4::colourgen
