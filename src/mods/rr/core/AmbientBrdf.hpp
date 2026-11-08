#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "Geometry.hpp"
#include "RootLayout.hpp"
#include "ShaderDigest.hpp"

// The game's AmbientBRDF LUT, which the specular albedo of the guide pass samples as the PT does.
namespace rr::core {
// Whether the PT's Dispatch says where the LUT is, and why not.
enum class LutLocation : std::uint8_t {
    Located,
    NoPtDispatch,         // the barrier rule found no PT sequence before the draw, whatever its compute shader
    DeclarationUnknown,   // what the PT's compute shader declares at the register is unknown, or so is its PSO
    NotTexture2D,         // the PT's compute shader declares no Texture2D at the register, as with 0 bounces
    RootSignatureUnknown, // the root signature of that Dispatch was not seen at its creation, or could not be read
    RegisterUnbound,      // its root signature binds nothing at the profile's register
    RootView,             // it binds the register as a root view, which only takes buffers
    TableUnset,           // the list did not set the table that holds the register
    HeapUnbound,          // no CBV_SRV_UAV heap was bound at the Dispatch
    OutsideHeap,          // the descriptor falls outside the heap bound
};

const char* to_string(LutLocation location);

// A shader-visible descriptor heap, from its GPU descriptor handle.
struct HeapRange {
    std::uint64_t start{};
    std::uint32_t descriptors{};
};

// The descriptor the PT reads the LUT through, on the heap bound at its Dispatch.
struct LutDescriptor {
    LutLocation location{};
    std::uint64_t handle{}; // its GPU descriptor handle, if located

    // Where the root signature binds it, if it does.
    std::uint32_t parameter{};
    std::uint32_t offset{};
};

// declaration: what the compute shader of the PT's Dispatch declares at the profile's register. The copy
// reads the descriptor as a Texture2D, and reading one of another kind is undefined behaviour; the root
// signature binds the register whatever the shader declares there. binding: where that root signature
// binds the register. table: the GPU handle the list set at that parameter, if any. heap: the CBV_SRV_UAV
// heap bound, if any, and increment its descriptor size.
LutDescriptor locate_lut(SrvDeclaration declaration, std::optional<RegisterBinding> binding, std::optional<std::uint64_t> table,
    std::optional<HeapRange> heap, std::uint32_t increment);

// What the V axis of a LUT holds. The PT samples it with the perceptual roughness, whose square is the
// alpha of its GGX; if the LUT holds alpha, the guide pass has to convert the roughness before sampling it.
enum class LutAxis : std::uint8_t {
    PerceptualRoughness,
    Alpha,
};

// One way to read a LUT: what its V axis holds, and whether .x holds the bias and .y the scale. The PT
// only uses x + y, which cannot tell the order.
struct LutReading {
    LutAxis axis{};
    bool swapped{};

    friend constexpr bool operator==(const LutReading&, const LutReading&) = default;
};

std::string to_string(const LutReading& reading);

// The center of texel i of n along an axis, from 0 to 1.
float texel_center(std::uint32_t i, std::uint32_t n);

// What a LUT holds at (u, v) under a reading, as the GGX fit gives it: its .x, then its .y.
std::array<float, 2> expected_lut_texel(LutReading reading, float u, float v);

// The rows of a LUT the check judges: at V = 0 and 1, the roughness and its square coincide.
inline constexpr float JUDGED_V_MIN = 0.2f;
inline constexpr float JUDGED_V_MAX = 0.8f;

// How far a LUT is from the GGX fit under one reading: the mean absolute difference of a channel.
struct LutFit {
    LutReading reading{};
    double error{};
};

// The check of the LUT's axis, which the validation of RR blocks on: the four readings, best first, and
// the best one if it stands out. Without one, the check is inconclusive.
struct LutJudgement {
    std::array<LutFit, 4> fits{};
    std::optional<LutReading> reading{};
};

// texels: mip 0 of a LUT of that size, as the guide pass copies it: row by row from V = 0, two floats per
// texel, .x then .y. Only the rows from JUDGED_V_MIN to JUDGED_V_MAX count.
LutJudgement judge_ambient_brdf_lut(std::span<const float> texels, Extent size);
} // namespace rr::core
