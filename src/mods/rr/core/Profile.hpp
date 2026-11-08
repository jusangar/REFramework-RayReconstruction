#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "Geometry.hpp"
#include "ShaderDigest.hpp"

namespace rr::core {
// Texture formats, with the values of DXGI_FORMAT so that the core does not need dxgiformat.h.
enum class Format : std::uint32_t {
    R16G16B16A16_SNORM = 13,
    R32G8X24_TYPELESS = 19,
    R10G10B10A2_UNORM = 24,
    R11G11B10_FLOAT = 26,
    R8G8B8A8_UNORM_SRGB = 29,
};

// The SceneInfo variants of the scene layer, by the name of their getter in sdk::renderer::layer::Scene.
enum class SceneVariant : std::uint8_t {
    Scene,
    DepthDistortion,
    Filter,
    ZPrepass,
    JitterDisable,
    JitterDisablePost,
};

// From the last variant, which a new one has to replace here.
inline constexpr std::size_t SCENE_VARIANT_COUNT = static_cast<std::size_t>(SceneVariant::JitterDisablePost) + 1;

// Where a SceneInfo keeps a copy of its view_projection: floats 160 to 175.
constexpr std::size_t VIEW_PROJECTION_COPY_FLOAT = 160;

// A variant that carries the game's jitter, where RR's own replaces it.
struct JitterVariant {
    SceneVariant variant{};
    bool view_projection_copy{}; // the copy at VIEW_PROJECTION_COPY_FLOAT is rewritten too
};

// How the game writes the velocity its TAA reads. It selects the code that turns it into NGX's MV.
enum class VelocityConvention : std::uint8_t {
    // v.xy = ndc_prev - ndc_cur, in NDC with Y up. ndc_cur is the pixel centre, where the jittered sample
    // falls, and ndc_prev comes from old_view_projection, without jitter: v carries minus the current
    // jitter, not its change between frames.
    NdcToUnjitteredPrevious,
};

// Where the specular albedo of the guide pass takes the AmbientBRDF LUT from.
enum class AmbientBrdfSource : std::uint8_t {
    GameLut, // the texture the PT samples, through the descriptor its Dispatch binds
    GgxFit,  // env_brdf_fit, the fallback of Guides.hlsli
};

// The game profile: what RR needs to know of one game to recognize its evaluation point and prepare
// its inputs. A compiled table, whose fields arrive with the code that uses them.
struct Profile {
    std::string_view game{}; // as sdk::GameIdentity::game_name() gives it

    // The TAA's vertex shader: the second identification of the evaluation point.
    ShaderDigest taa_vs{};

    // The pixel shaders of the TAA draw. Any pixel shader that is not admitted omits the frame; the
    // excluded ones are known and rejected on purpose.
    std::span<const ShaderDigest> admitted_ps{};
    std::span<const ShaderDigest> excluded_ps{};

    // The compute shader of the path tracing that runs in the game, as a diagnostic: the variant the
    // AmbientBRDF LUT was validated on, and the one to start from if the PT's shader ever had to be replaced
    // for its shared noise pattern. Each PT setting is another variant, and nothing depends on this one.
    ShaderDigest pt_cs{};

    // Expected formats, as a check and never as an identification.
    struct Formats {
        Format t{};        // the scene target where the PT adds its light and that the TAA reads
        Format o{};        // the TAA's output target
        Format albedo{};   // G-buffer
        Format normals{};  // G-buffer
        Format velocity{}; // G-buffer
        Format depth{};
    } formats{};

    // The G-buffer pass: an OMSetRenderTargets with this many render targets and the depth writable.
    // Where the resources RR reads are among them.
    struct GBufferLayout {
        std::uint32_t render_targets{};
        std::uint32_t albedo{};
        std::uint32_t normals{};
        std::uint32_t velocity{};
    } gbuffer{};

    // The variants, chosen by name, where RR's own jitter replaces the game's after the update of the
    // scene layer.
    std::span<const JitterVariant> jitter_variants{};

    VelocityConvention velocity{};

    // The AmbientBRDF LUT: with GameLut, the SRV the PT's compute shader declares at t<srv_register> in
    // that space, as the root signature of its Dispatch binds it. The Dispatch is the one of the PT
    // sequence the barrier rule chooses, whatever its compute shader. A texture of another size is not the
    // LUT, and the frame falls back.
    struct AmbientBrdf {
        AmbientBrdfSource source{};
        std::uint32_t srv_register{};
        std::uint32_t space{};
        Extent size{};
    } ambient_brdf{};

    // The cut detector: an update of the camera is a cut if one of its residuals against a
    // constant-velocity prediction goes over its threshold, save a camera push, as when the camera is
    // pushed against a wall: an update the pivot alone would make a cut is none if its pivot residual lies
    // along the view's axis and the camera neither turned nor zoomed against the prediction. The push's
    // margins are under the thresholds of their residuals, so a push only lifts the pivot's.
    struct Cuts {
        float pivot_distance{};    // m in front of the camera: the point whose jump is measured
        float pivot_residual{};    // m
        float rotation_residual{}; // degrees
        float focal_residual{};    // relative change of projection[1][1] in one update

        float push_axis_fraction{};     // of the pivot residual along axis 2 of the inverse view, at least
        float push_rotation_residual{}; // degrees, at most
        float push_focal_residual{};    // relative, at most
    } cuts{};

    // The most frames a run of omitted frames can have and keep RR's history. The cut detector sees jumps
    // against a constant-velocity prediction, not how far the camera went in all: over a long run, a slow
    // turn could take the history away without any cut.
    std::uint32_t max_kept_run{};

    // The exposure T arrives with at the evaluation point.
    float pre_exposure{1.0f};

    // The game's settings the Mod checks, which RR and the profile were measured with. The ray tracing ones
    // are REFramework's Graphics settings, by the names its interface gives them, since Pure PT and the Clone
    // Types only exist there. The PT settings, spp and bounces, are the player's to choose: RR works with any.
    struct Settings {
        std::string_view ray_trace_type{}; // Ray Trace Type
        std::string_view clone_type{};     // each of the three Ray Trace Clone Types
        bool game_taa{};                   // the game's anti-aliasing includes its TAA
    } settings{};
};

// The profile of a game, or nullptr if RR does not support it.
const Profile* find_profile(std::string_view game);

// Whether the profile admits a pixel shader at the TAA draw.
bool admits_ps(const Profile& profile, const ShaderDigest& ps);

std::string_view to_string(SceneVariant variant);
} // namespace rr::core
