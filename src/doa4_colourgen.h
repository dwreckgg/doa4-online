// Colour generator (DOA4 Online 0.3.8): makes colour costume files on the player's PC from the player's own game
// files. A made file is used only when its SHA-1 equals the value compiled into the slot table, so every function here
// must give the same bytes on every PC: the sources are built under the converter's floating-point rules
// (doa4_suitconv_fp.h, CMakeLists.txt DOA4_COLOURGEN_SOURCES) and call no C runtime maths function.
//
// Alpha-152 effect recolour: the two RGB565 colour endpoints of every DXT5 block of textures 0..120 of the effect
// texture file get a new hue; alpha blocks, index bits and everything else in the file keep their bytes.
// Spartan-458 armour repaint (doa4_colourgen_spartan.cpp, with the DXT1 codec and tiling of doa4_colourgen_dxt.cpp):
// see the second half of this header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// ---------------------------------------------------------------- Spartan-458 armour repaint
// Twelve paint textures of a Spartan costume file (DXT1, tiled, 16-bit swapped) are decoded, every texel repainted
// (the armour colour node group: shadow part, scratch part, mix with the paint, then hue / saturation / value), and
// encoded again. colour: the armour colour as sRGB bytes; vref: the costume's median linear paint value.
struct SpartanRecipe {
  uint8_t colour[3];
  double amount, hue_shift, saturation, brightness, vref;
};
inline constexpr SpartanRecipe kH3Pink{{0xFF, 0x8F, 0xEC}, 1.0, 0.0, 1.0, 1.4266, 0.41254261348390375};
inline constexpr SpartanRecipe kH3Orchid{{0x76, 0x60, 0xAA}, 1.0, 0.0, 1.0, 1.0, 0.6104955708078648};
inline constexpr SpartanRecipe kH2Orange{{0xAB, 0x6D, 0x2D}, 1.0, 0.0, 1.0, 1.0, 0.2704977910130658};
inline constexpr SpartanRecipe kH2Cyan{{0x24, 0x69, 0x69}, 1.0, 0.0, 1.0, 1.0, 0.05780543019106723};
// The texture numbers (k-th TX2D resource) of the paint textures, in the order MakeSpartan writes them.
inline constexpr int kPaintTextures[12] = {11, 22, 27, 31, 36, 42, 46, 52, 61, 63, 67, 69};

// Repaints `pixels` sRGB pixels (3 bytes each, r g b) from srgb_in to srgb_out (the two may be the same buffer). Defined
// for recipes with no NaN, -1 <= hue_shift <= 0, 0 < vref < 1, 0 <= saturation < inf, 0 <= brightness < inf and a
// finite amount (clamped to 0..1); it cannot refuse, MakeSpartan enforces that domain. The caller runs it under the
// converter's floating-point control word (num::RunUnderConvertEnv; MakeSpartan does).
void Repaint(const SpartanRecipe& r, const uint8_t* srgb_in, uint8_t* srgb_out, size_t pixels);

// One DXT1 endpoint (r, g, b as 0..255 floats) to its RGB565 word: each channel (e * steps) / 255 in float, rounded half
// to even, clipped to the field. The caller runs it under the converter's control word.
uint16_t Quant565(const float e[3]);

// bw x bh little-endian DXT1 blocks (8 bytes: c0, c1, index word; row-major) to (4 * bw) x (4 * bh) RGBA pixels,
// row-major, 4 bytes each, with the decoder's integer palette (index 3 of a block with c0 <= c1: black, alpha 0).
void DecodeDxt1(const uint8_t* blocks_le, uint32_t bw, uint32_t bh, uint8_t* rgba);

// w x h RGB pixels (row-major, 3 bytes each; w and h multiples of 4) to (w / 4) x (h / 4) opaque DXT1 blocks,
// little-endian, row-major: every block has c0 > c1, or c0 == c1 with every index 0. The caller runs it under the
// converter's control word.
void EncodeDxt1(const uint8_t* rgb, uint32_t w, uint32_t h, uint8_t* blocks_le);

// The Xbox 360 tiled position of block (bx, by) in a grid bw blocks wide, for 8-byte blocks.
uint32_t TiledBlockIndex(uint32_t bx, uint32_t by, uint32_t bw);

// A copy of the source in which only the data of the twelve paint textures differs: each is decoded from the source,
// repainted, encoded and written back at its own range, in kPaintTextures order. nullopt + *why (why may be null) when
// the file has fewer than 70 textures, a paint texture is not one-level tiled 16-bit-swapped DXT1 with a block grid a
// multiple of 32 inside the file, or holds a transparent texel, or the recipe is outside Repaint's domain. Nothing is
// thrown.
std::optional<Bytes> MakeSpartan(std::span<const uint8_t> source, const SpartanRecipe& r, std::string* why);

// The recipe table by slot name ("alpha_red", "alpha_blue", "alpha_lilac", "spartan_h3_pink", "spartan_h3_orchid",
// "spartan_h2_orange", "spartan_h2_cyan"): MakeAlpha or MakeSpartan with that recipe. nullopt + *why for an unknown
// name (names are matched exactly) or when the maker refuses the source.
std::optional<Bytes> Make(std::string_view slot, std::span<const uint8_t> source, std::string* why);

}  // namespace doa4::colourgen
