#include <cstring>

#include "Jitter.hpp"

namespace rr::core {
namespace {
float halton(std::uint32_t index, std::uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;

    for (; index > 0; index /= base) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
    }

    return r;
}
} // namespace

Float2 own_jitter(std::uint64_t index, float amplitude) {
    const auto phase = static_cast<std::uint32_t>(index % JITTER_PHASES) + 1;
    return {(2.0f * halton(phase, 2) - 1.0f) * amplitude, (2.0f * halton(phase, 3) - 1.0f) * amplitude};
}

Float2 jitter_of(const Matrix4& projection, const Matrix4& jitter_disable_projection, Extent render_size) {
    const auto dx = projection[2][0] - jitter_disable_projection[2][0];
    const auto dy = projection[2][1] - jitter_disable_projection[2][1];
    return {-dx * static_cast<float>(render_size.width) / 2.0f, dy * static_cast<float>(render_size.height) / 2.0f};
}

Float2 jitter_term(Float2 jitter, Extent render_size) {
    return {-2.0f * jitter.x / static_cast<float>(render_size.width), 2.0f * jitter.y / static_cast<float>(render_size.height)};
}

SceneMatrices inject_jitter(const SceneMatrices& variant, const Matrix4& jitter_disable_projection, Float2 term) {
    auto out = variant;
    out.projection[2][0] = jitter_disable_projection[2][0] + term.x;
    out.projection[2][1] = jitter_disable_projection[2][1] + term.y;
    out.inverse_projection = inverse(out.projection);
    out.view_projection = out.projection * out.view;
    out.inverse_view_projection = inverse(out.view_projection);
    return out;
}

bool write_injection_with_copy(std::span<float, SCENE_INFO_FLOATS> scene_info, const SceneMatrices& injected) {
    // Before the matrices are written: floats 0 to 15 are the game's view_projection.
    const auto copy = scene_info.subspan<VIEW_PROJECTION_COPY_FLOAT, 16>();
    const auto copy_matches = std::memcmp(copy.data(), scene_info.data(), sizeof(Matrix4)) == 0;

    std::memcpy(scene_info.data(), &injected, sizeof(injected));

    if (copy_matches) {
        std::memcpy(copy.data(), &injected.view_projection, sizeof(Matrix4));
    }

    return copy_matches;
}

bool same_inverses(const SceneMatrices& a, const SceneMatrices& b) {
    return std::memcmp(&a.inverse_projection, &b.inverse_projection, sizeof(Matrix4)) == 0 &&
           std::memcmp(&a.inverse_view_projection, &b.inverse_view_projection, sizeof(Matrix4)) == 0;
}

bool follow_scene_inverses(SceneMatrices& variant, const SceneMatrices& game_scene, const SceneMatrices& injected_scene) {
    if (!same_inverses(variant, game_scene)) {
        return false;
    }

    variant.inverse_projection = injected_scene.inverse_projection;
    variant.inverse_view_projection = injected_scene.inverse_view_projection;
    return true;
}
} // namespace rr::core
