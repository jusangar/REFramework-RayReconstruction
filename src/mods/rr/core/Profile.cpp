#include <algorithm>
#include <array>

#include "Profile.hpp"

namespace rr::core {
namespace {
// RE2, DX12, current Steam build. The digests come from the PSO log of pass A, a session in the game of
// the probe that measured the camera cuts.
constexpr std::array RE2_ADMITTED_PS{
    shader_digest("cfb9e6c25453f521c56908a6b750254a"), // normal play
    shader_digest("17c6254cc3ebc09f724342713707f11b"), // some cinematic shots and the title menu
    // The 3 frames that open the screen that picks an object: a reduced variant of cfb9e6c2's
    // PreTonemap2_PS that does not dilate the velocity with the depth, has no response mask and reads the
    // history point by point, as dxc -dumpbin shows it on the shaders pass A dumped. A probe of the
    // inspection screens saw it with the same T, history and O as normal play. It was excluded at first;
    // now RE2 excludes none.
    shader_digest("f0011414e06f9033bca4a1109be7fee5"),
};

// The variants with the game's jitter in every update of the two passes of a jitter probe in the game.
// Filter, JitterDisable and JitterDisablePost never carry it, and the floats 160 to 175 of
// DepthDistortion and ZPrepass do not match their view_projection.
constexpr std::array RE2_JITTER_VARIANTS{
    JitterVariant{SceneVariant::Scene, true},
    JitterVariant{SceneVariant::DepthDistortion, false},
    JitterVariant{SceneVariant::ZPrepass, false},
};

constexpr Profile RE2{
    .game = "re2",
    .taa_vs = shader_digest("9e3395e579652c2915e92caa6c93ae34"),
    .admitted_ps = RE2_ADMITTED_PS,
    // Pure PT, 1 spp, as the PT logs of pass A show it. PIX's b788c6e4 is another of its variants.
    .pt_cs = shader_digest("27413cdde227d650e4e3a16000c886bd"),
    .formats =
        {
            .t = Format::R11G11B10_FLOAT,
            .o = Format::R11G11B10_FLOAT,
            .albedo = Format::R8G8B8A8_UNORM_SRGB,
            .normals = Format::R10G10B10A2_UNORM,
            .velocity = Format::R16G16B16A16_SNORM,
            .depth = Format::R32G8X24_TYPELESS,
        },
    // As a probe of the resource states saw it in the game, at the evaluation point. RT0 is another
    // R10G10B10A2_UNORM that RR does not read.
    .gbuffer =
        {
            .render_targets = 4,
            .albedo = 1,
            .normals = 2,
            .velocity = 3,
        },
    .jitter_variants = RE2_JITTER_VARIANTS,
    .velocity = VelocityConvention::NdcToUnjitteredPrevious,
    // AmbientBRDF is a Texture2D at t14, a 256x256 BC6H_UF16, sampled at (saturate(N.V), max(0.01,
    // roughness)) while its GGX takes alpha = roughness^2: in the DXIL of PIX's variant b788c6e4, and in
    // the running 27413cdd, dumped by a probe of the LUT in the game. That probe read the LUT back: the
    // perceptual roughness on V, scale in .x and bias in .y, and at three texels of its diagonal 7 to 15 %
    // below GGX with height-correlated Smith, the GGX fit's model. An offset that large is beyond what the
    // judge is built to absorb, and it found the LUT inconclusive, so the axis was judged by hand.
    //
    // The register is not the same in every variant: with 1, 2, 3 and 7 bounces it is t14, but the
    // variant with 0 bounces declares three resources fewer, has AmbientBRDF at t11 and a raytracing
    // acceleration structure at t14. The LUT is only read where the PT's compute shader declares a
    // Texture2D at t14; elsewhere the guide pass falls back on the GGX fit.
    .ambient_brdf =
        {
            .source = AmbientBrdfSource::GameLut,
            .srv_register = 14,
            .space = 0,
            .size = {256, 256},
        },
    // From pass A. In the fast turns, at up to 84 degrees and 2 m per update, the residuals stayed under
    // 0.64 m and 31 degrees; the gentle zooms reached 4.94 %.
    //
    // The camera push, from pass A and the validation sessions in the game. The camera against a wall
    // jumps along the view's axis (51301, the camera after a door, at 99.7 %), with no zoom and at most
    // 1.26 degrees in a validation session. 95 % leaves out the cinematic cuts 15233, 41377 and 41364 (61
    // to 81 %); 2.5 degrees, 40288 (98 %, 5.29 degrees, no zoom); 1 %, 7504 (98 %, 1.31 degrees, a zoom
    // of 8.5 %). Over the whole of pass A, only 51301 stops being a cut.
    .cuts =
        {
            .pivot_distance = 1.5f,
            .pivot_residual = 1.0f,
            .rotation_residual = 45.0f,
            .focal_residual = 0.10f,
            .push_axis_fraction = 0.95f,
            .push_rotation_residual = 2.5f,
            .push_focal_residual = 0.01f,
        },
    // In a validation session, the game omits a loose frame at the start and the end of a cinematic and when
    // the inventory opens, and two loose frames, 9 frames apart, when a screen of the interface closes.
    // The menus and the screen that picks an object omit 128 frames or more.
    .max_kept_run = 4,
    .pre_exposure = 1.0f,
    // What RR was validated with: Pure PT, with the game's TAA as the draw RR replaces. A Clone Type would
    // add another ray tracing pass to T. Any spp and bounce count: two of the validation sessions ran RR at
    // 4 spp most of the time.
    .settings =
        {
            .ray_trace_type = "Pure Path Tracing",
            .clone_type = "Disabled",
            .game_taa = true,
        },
};

constexpr std::array PROFILES{&RE2};
} // namespace

const Profile* find_profile(std::string_view game) {
    for (const auto* profile : PROFILES) {
        if (profile->game == game) {
            return profile;
        }
    }

    return nullptr;
}

bool admits_ps(const Profile& profile, const ShaderDigest& ps) {
    return std::find(profile.admitted_ps.begin(), profile.admitted_ps.end(), ps) != profile.admitted_ps.end();
}

std::string_view to_string(SceneVariant variant) {
    switch (variant) {
    case SceneVariant::Scene:
        return "Scene";
    case SceneVariant::DepthDistortion:
        return "DepthDistortion";
    case SceneVariant::Filter:
        return "Filter";
    case SceneVariant::ZPrepass:
        return "ZPrepass";
    case SceneVariant::JitterDisable:
        return "JitterDisable";
    case SceneVariant::JitterDisablePost:
        return "JitterDisablePost";
    }

    return "?";
}
} // namespace rr::core
