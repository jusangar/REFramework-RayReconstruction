#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "FramePolicy.hpp"
#include "Geometry.hpp"
#include "Profile.hpp"
#include "ShaderDigest.hpp"

// How the evaluation point is recognized on a command list of the game, without D3D12: the hooks
// describe what the list records, and these say what it means.
namespace rr::core {
// The D3D12 values the identification and its interface read, so that the core does not need d3d12.h.
// The hooks check them against the SDK's.
namespace d3d12_value {
// D3D12_RESOURCE_STATES
constexpr std::uint32_t RENDER_TARGET = 0x4;
constexpr std::uint32_t UNORDERED_ACCESS = 0x8;
constexpr std::uint32_t DEPTH_WRITE = 0x10;
constexpr std::uint32_t DEPTH_READ = 0x20;
constexpr std::uint32_t SHADER_RESOURCE = 0xc0; // NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE

// D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
constexpr std::uint32_t ALL_SUBRESOURCES = 0xffffffff;

// D3D12_RESOURCE_FLAGS
constexpr std::uint32_t ALLOW_RENDER_TARGET = 0x1;
constexpr std::uint32_t ALLOW_UNORDERED_ACCESS = 0x4;

// D3D12_DSV_FLAGS
constexpr std::uint32_t DSV_READ_ONLY_DEPTH = 0x1;
} // namespace d3d12_value

// A resource of the game, by identity: its ID3D12Resource, opaque here.
using ResourceId = const void*;

// What GetDesc says of a resource, as far as the identification needs it.
struct ResourceDesc {
    Extent size{};
    Format format{};       // the DXGI_FORMAT value
    std::uint32_t flags{}; // D3D12_RESOURCE_FLAGS
    std::uint32_t mip_levels{1};
    std::uint32_t array_size{1}; // DepthOrArraySize of a 2D texture

    friend constexpr bool operator==(const ResourceDesc&, const ResourceDesc&) = default;
};

// GetDesc, or a double in the tests.
using Describe = std::function<ResourceDesc(ResourceId)>;

// A transition the game recorded with ResourceBarrier. A split barrier counts at its END half only.
struct Transition {
    ResourceId resource{};
    std::uint32_t subresource{};
    std::uint32_t before{};
    std::uint32_t after{};
};

// Whether a transition of this subresource moves a render target the identification follows: they all
// have a single subresource.
constexpr bool covers_render_target(std::uint32_t subresource) {
    return subresource == 0 || subresource == d3d12_value::ALL_SUBRESOURCES;
}

enum class AnchorOutcome : std::uint8_t {
    // Not a full-screen triangle, T not in SHADER_RESOURCE, or the recording already had its attempt.
    None,
    Anchored,
    // T was ready, but no resource with the profile's velocity format at T's size went to
    // SHADER_RESOURCE after the PT sequence.
    VelocityMissing,
    // More than one did, and the rule does not pick one blindly.
    VelocityAmbiguous,
};

// What a draw meant to the barrier rule.
struct Anchor {
    AnchorOutcome outcome{};

    // When anchored, or when the velocity failed it.
    ResourceId t{};
    ResourceDesc t_desc{};
    std::optional<ShaderDigest> pt_cs{}; // the compute shader of the PT's Dispatch

    // When anchored.
    ResourceId velocity{};
};

// The first identification of the evaluation point, on one command list: the PT sequence precedes the
// draw in the list, and T and the velocity go to SHADER_RESOURCE before it.
//
// T is told by its role, never by the profile's format: the resource that goes to UAV before the PT's
// Dispatch and from UAV to RENDER_TARGET after it, where the game draws the transparencies on T. In the
// inspection that pass does not exist, and T goes from UAV straight to SHADER_RESOURCE. That end only
// counts for a texture, with a format and a height above 1, since buffers take it after their Dispatch in
// every frame, and never in place of a T whose sequence ended in RENDER_TARGET in the same recording, so
// that gameplay keeps its T. Otherwise a later sequence before the anchor takes its place. The
// velocity is the resource with the profile's velocity format at T's size that went to SHADER_RESOURCE
// after the sequence. It stays apart from the G-buffer, which another list names: the state assessment
// checks that it is the G-buffer's, so that a G-buffer not known yet, or recreated, gives an unknown
// state and not an identification that fails.
//
// The rule makes one attempt per recording, at the first DrawInstanced(3, 1) with T in
// SHADER_RESOURCE, where a prototype in the game anchored the TAA, and anchors that draw if the velocity
// is there too. After it, the rule ignores the list, where AMD CAS repeats the PT's pattern on T.
class BarrierRule {
public:
    // Reset of the list, or a new list: a new recording.
    void reset();

    // True at the transition that closes a PT sequence, whatever the compute shader of its Dispatch. That
    // Dispatch is the last one recorded before it: where the hooks take the AmbientBRDF LUT. describe is
    // only called on a transition that may close one in SHADER_RESOURCE.
    bool on_transition(const Transition& transition, const Describe& describe);
    void on_dispatch(const std::optional<ShaderDigest>& cs);

    // Every draw. describe is only called on the attempt.
    Anchor on_draw(bool full_screen_triangle, const Profile& profile, const Describe& describe);

private:
    // A resource that went to UAV and has not left it since.
    struct UavCandidate {
        ResourceId resource{};
        bool dispatched{}; // a Dispatch came after it went to UAV, with no draw in between
        std::optional<ShaderDigest> cs{};
    };

    std::vector<UavCandidate> m_uav{};

    ResourceId m_t{};
    std::uint32_t m_t_state{};
    bool m_t_from_render_target{}; // T's sequence ended in RENDER_TARGET
    std::optional<ShaderDigest> m_pt_cs{};

    // The resources other than T that went to SHADER_RESOURCE after the PT sequence and are still there.
    std::vector<ResourceId> m_shader_resources{};

    bool m_attempted{};
};

// Whether an attempt of the barrier rule is the TAA draw of a frame without velocity, where RR is
// evaluated with the camera's reprojection: only the velocity is missing, at a draw with the profile's
// TAA vertex shader and a pixel shader it admits. Any other attempt without the velocity, as at the output
// pass of the menus (PS cbe7934e), leaves its frame without an evaluation point.
bool is_no_velocity_taa(
    const Anchor& anchor, const std::optional<ShaderDigest>& vs, const std::optional<ShaderDigest>& ps, const Profile& profile);

// Why the RT0 of the anchored draw is not O.
enum class OutputProblem : std::uint8_t {
    None,
    NoRenderTarget,
    IsT,
    // No barrier of the list left it in RENDER_TARGET.
    NotRenderTarget,
    Format,
};

struct OutputCandidate {
    ResourceId resource{}; // RT0 of the draw; none without a render target
    ResourceDesc desc{};
    std::optional<std::uint32_t> state{}; // where the last transition of RT0 in the list left it
};

struct OutputCheck {
    std::optional<OutputTarget> target{};
    OutputProblem problem{};
};

// O is the RT0 of the anchored draw, distinct from T, in RENDER_TARGET by a barrier of the same list and
// with the profile's format. Its size and its UAV flag are the policy's to judge.
OutputCheck identify_output(const OutputCandidate& rt0, ResourceId t, const Profile& profile);

std::string_view to_string(OutputProblem problem);

// The resources of the G-buffer that RR reads, as one G-buffer pass names them. Equal G-buffers have
// the same identity.
struct GBuffer {
    ResourceId albedo{};
    ResourceId normals{};
    ResourceId velocity{};
    ResourceId depth{};
    Extent size{};

    friend constexpr bool operator==(const GBuffer&, const GBuffer&) = default;
};

// What an OMSetRenderTargets binds, as far as the G-buffer pass needs it.
struct RenderTargetBinding {
    std::span<const ResourceId> render_targets{}; // null where the view is unknown or a null descriptor
    ResourceId depth{};                           // none without a depth-stencil view, or if it is unknown
    std::uint32_t depth_view_flags{};             // D3D12_DSV_FLAGS
};

// Whether a binding has the shape of the G-buffer pass: the profile's number of render targets, a known
// view at each position RR reads, and the depth writable. Most passes do not, and telling them apart needs
// no description.
bool has_gbuffer_shape(const RenderTargetBinding& binding, const Profile& profile);

enum class GBufferProblem : std::uint8_t {
    None,
    NotGBufferPass,
    // A resource RR reads, or the depth, has another format than the profile's.
    Format,
    // The resources RR reads and the depth are not all of one size.
    Size,
    // The depth has mips or array slices, so its plane 1 would not be subresource 1.
    DepthLayout,
};

// From the last problem, which a new one has to replace here.
inline constexpr std::size_t GBUFFER_PROBLEM_COUNT = static_cast<std::size_t>(GBufferProblem::DepthLayout) + 1;

struct GBufferCheck {
    std::optional<GBuffer> gbuffer{};
    GBufferProblem problem{};
};

// The G-buffer a binding names if it has the shape of the G-buffer pass and the profile's formats, as
// a check. describe is only called on the albedo, the normals, the velocity and the depth.
GBufferCheck identify_gbuffer(const RenderTargetBinding& binding, const Profile& profile, const Describe& describe);

std::string_view to_string(GBufferProblem problem);
} // namespace rr::core
