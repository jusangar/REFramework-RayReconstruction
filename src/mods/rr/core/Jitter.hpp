#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "Geometry.hpp"
#include "Profile.hpp"

namespace rr::core {
// RR's own jitter: a Halton(2, 3) sequence of 32 phases.
constexpr std::uint32_t JITTER_PHASES = 32;
constexpr float DEFAULT_JITTER_AMPLITUDE = 0.5f; // render pixels: the sequence covers +-0.5 px

// The sample displacement of phase index % 32, in render pixels, X to the right and Y down, within
// +-amplitude. It skips the first point of Halton, which is 0 in both bases.
Float2 own_jitter(std::uint64_t index, float amplitude);

// The jitter a projection carries, in the same render pixels: j = (-d.x W/2, +d.y H/2), with
// d = projection[2][0..1] - jitter_disable_projection[2][0..1]. Read from the final matrices of the frame,
// never from the offset injected.
Float2 jitter_of(const Matrix4& projection, const Matrix4& jitter_disable_projection, Extent render_size);

// The term d that gives the jitter j: the inverse of jitter_of.
Float2 jitter_term(Float2 jitter, Extent render_size);

// The matrices of a SceneInfo, in the order of sdk::renderer::SceneInfo, so that the Mod copies them as
// they are.
struct SceneMatrices {
    Matrix4 view_projection{};
    Matrix4 view{};
    Matrix4 inverse_view{};
    Matrix4 projection{};
    Matrix4 inverse_projection{};
    Matrix4 inverse_view_projection{};
    Matrix4 old_view_projection{}; // the previous view_projection, without jitter in RE2
};

static_assert(sizeof(SceneMatrices) == 7 * sizeof(Matrix4));

// A SceneInfo variant with its jitter replaced: projection[2][0..1] becomes JitterDisable's plus the
// term, and view_projection and both inverses are recomputed. old_view_projection is left alone. The
// Mod writes these matrices into the game, and the policy keeps the same ones for the evaluation.
SceneMatrices inject_jitter(const SceneMatrices& variant, const Matrix4& jitter_disable_projection, Float2 term);

// The floats of a SceneInfo up to the end of its copy of view_projection, past the seven matrices. The
// game writes the copy with its jitter too.
constexpr std::size_t SCENE_INFO_FLOATS = VIEW_PROJECTION_COPY_FLOAT + 16;

// Writes the injected matrices over a variant whose profile entry has view_projection_copy, given by its
// first SCENE_INFO_FLOATS floats, and over the copy only while it still holds the game's view_projection
// bit for bit, as the jitter probe in the game did. The floats in between stay. Returns whether the copy
// was rewritten.
bool write_injection_with_copy(std::span<float, SCENE_INFO_FLOATS> scene_info, const SceneMatrices& injected);

// Whether two variants have the same inverse_projection and inverse_view_projection, bit for bit. In RE2,
// JitterDisable's are Scene's, with the game's jitter.
bool same_inverses(const SceneMatrices& a, const SceneMatrices& b);

// A candidate correction, not yet tried in the game: a variant without jitter whose inverses are Scene's
// as the game wrote them takes those of the injected Scene, so that they still match the depth, which
// carries the own jitter. Returns whether it applied.
bool follow_scene_inverses(SceneMatrices& variant, const SceneMatrices& game_scene, const SceneMatrices& injected_scene);
} // namespace rr::core
