// Colour generator (DOA4 Online 0.3.8): makes colour costume files on the player's PC from the player's own game
// files. A made file is used only when its SHA-1 equals the value compiled into the slot table, so every function here
// must give the same bytes on every PC: the sources are built under the converter's floating-point rules
// (doa4_suitconv_fp.h, CMakeLists.txt DOA4_COLOURGEN_SOURCES) and call no C runtime maths function.
//
// Alpha-152 effect recolour: the two RGB565 colour endpoints of every DXT5 block of textures 0..120 of the effect
// texture file get a new hue; alpha blocks, index bits and everything else in the file keep their bytes.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace doa4::colourgen {

using Bytes = std::vector<uint8_t>;

// hue: degrees; spread: how much of each colour's own hue distance from 0.5 is kept; sat: saturation factor (the
// result is clamped at 1.0 from above only).
struct AlphaRecipe {
  double hue, spread, sat;
};
inline constexpr AlphaRecipe kAlphaRed{352.0, 0.3, 1.2}, kAlphaBlue{238.0, 0.3, 1.2}, kAlphaLilac{256.0, 0.3, 1.1};

// One RGB565 word recoloured: to HSV (doubles), h' = (hue / 360 + (h - 0.5) * spread) mod 1, s' = min(1, s * sat),
// back to RGB, each channel rounded half to even. Defined for recipes with 0 <= hue <= 360, -1 <= spread <= 1 and
// 0 <= sat < infinity (MakeAlpha refuses others). The caller runs it under the converter's floating-point control word
// (num::RunUnderConvertEnv; MakeAlpha does).
uint16_t AlphaRecolour565(uint16_t c, const AlphaRecipe& r);

// Textures 0..120 of the 144 (all DXT5, tiled, 16-bit swapped, no mip address, sizes multiples of 128): the two endpoint
// words at +8 and +10 of every 16-byte block. Anything else in the file is copied. nullopt + *why on any other layout.
// why may be null. Nothing is thrown: a malformed file, an exception of the reader or a recipe outside the domain
// above gives nullopt and the reason.
std::optional<Bytes> MakeAlpha(std::span<const uint8_t> source, const AlphaRecipe& r, std::string* why);

}  // namespace doa4::colourgen
