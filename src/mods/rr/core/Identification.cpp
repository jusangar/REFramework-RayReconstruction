#include <algorithm>

#include "Identification.hpp"

namespace rr::core {
using namespace d3d12_value;

namespace {
// A buffer has no format and a height of 1.
bool is_texture(const ResourceDesc& desc) {
    return desc.format != Format{} && desc.size.height > 1;
}
} // namespace

// Keeps the vectors' storage: the game resets its lists every frame.
void BarrierRule::reset() {
    m_uav.clear();
    m_t = nullptr;
    m_t_state = 0;
    m_t_from_render_target = false;
    m_pt_cs.reset();
    m_shader_resources.clear();
    m_attempted = false;
}

bool BarrierRule::on_transition(const Transition& transition, const Describe& describe) {
    const auto resource = transition.resource;

    if (m_attempted || !covers_render_target(transition.subresource)) {
        return false;
    }

    const auto uav = std::find_if(m_uav.begin(), m_uav.end(), [&](const UavCandidate& c) { return c.resource == resource; });
    auto closes_sequence = false;

    if (uav != m_uav.end()) {
        const auto candidate = *uav;
        m_uav.erase(uav);

        // The PT sequence: from UAV, after its Dispatch, to RENDER_TARGET, or in the inspection to
        // SHADER_RESOURCE, for a texture and not in place of a T that ended in RENDER_TARGET.
        if (candidate.dispatched && transition.before == UNORDERED_ACCESS) {
            const auto render_target = (transition.after & RENDER_TARGET) != 0;
            const auto shader_resource = transition.after == SHADER_RESOURCE && !m_t_from_render_target && is_texture(describe(resource));

            if (render_target || shader_resource) {
                m_t = resource;
                m_t_from_render_target = render_target;
                m_pt_cs = candidate.cs;
                m_shader_resources.clear();
                closes_sequence = true;
            }
        }
    }

    if (transition.after == UNORDERED_ACCESS) {
        m_uav.push_back({resource});
    }

    if (resource == m_t) {
        m_t_state = transition.after;
        return closes_sequence;
    }

    if (m_t == nullptr) {
        return closes_sequence;
    }

    std::erase(m_shader_resources, resource);

    if (transition.after == SHADER_RESOURCE) {
        m_shader_resources.push_back(resource);
    }

    return closes_sequence;
}

// Every candidate in UAV takes the compute shader of the last Dispatch, so the PT's is the last one
// recorded before the transition that closes its sequence.
void BarrierRule::on_dispatch(const std::optional<ShaderDigest>& cs) {
    for (auto& candidate : m_uav) {
        candidate.dispatched = true;
        candidate.cs = cs;
    }
}

Anchor BarrierRule::on_draw(bool full_screen_triangle, const Profile& profile, const Describe& describe) {
    if (!full_screen_triangle || m_attempted || m_t == nullptr || m_t_state != SHADER_RESOURCE) {
        // The PT's Dispatch comes before any draw once its target is a UAV.
        std::erase_if(m_uav, [](const UavCandidate& c) { return !c.dispatched; });
        return {};
    }

    Anchor anchor{AnchorOutcome::Anchored, m_t, describe(m_t), m_pt_cs};
    std::size_t velocities{};

    for (const auto resource : m_shader_resources) {
        if (const auto desc = describe(resource); desc.format == profile.formats.velocity && desc.size == anchor.t_desc.size) {
            anchor.velocity = resource;
            ++velocities;
        }
    }

    if (velocities != 1) {
        anchor.outcome = velocities == 0 ? AnchorOutcome::VelocityMissing : AnchorOutcome::VelocityAmbiguous;
        anchor.velocity = nullptr;
    }

    m_attempted = true;
    m_uav.clear();
    m_shader_resources.clear();
    return anchor;
}

bool is_no_velocity_taa(
    const Anchor& anchor, const std::optional<ShaderDigest>& vs, const std::optional<ShaderDigest>& ps, const Profile& profile) {
    return anchor.outcome == AnchorOutcome::VelocityMissing && vs == profile.taa_vs && ps && admits_ps(profile, *ps);
}

OutputCheck identify_output(const OutputCandidate& rt0, ResourceId t, const Profile& profile) {
    if (rt0.resource == nullptr) {
        return {std::nullopt, OutputProblem::NoRenderTarget};
    }

    if (rt0.resource == t) {
        return {std::nullopt, OutputProblem::IsT};
    }

    if (rt0.state != RENDER_TARGET) {
        return {std::nullopt, OutputProblem::NotRenderTarget};
    }

    if (rt0.desc.format != profile.formats.o) {
        return {std::nullopt, OutputProblem::Format};
    }

    return {OutputTarget{rt0.desc.size, (rt0.desc.flags & ALLOW_UNORDERED_ACCESS) != 0}, OutputProblem::None};
}

std::string_view to_string(OutputProblem problem) {
    switch (problem) {
    case OutputProblem::None:
        return "identified";
    case OutputProblem::NoRenderTarget:
        return "the draw has no RT0, or its view is unknown";
    case OutputProblem::IsT:
        return "RT0 is T";
    case OutputProblem::NotRenderTarget:
        return "no barrier of the list left RT0 in RENDER_TARGET";
    case OutputProblem::Format:
        return "RT0 has another format than the profile's";
    }

    return "?";
}

namespace {
ResourceId target_at(const RenderTargetBinding& binding, std::uint32_t index) {
    return index < binding.render_targets.size() ? binding.render_targets[index] : nullptr;
}
} // namespace

bool has_gbuffer_shape(const RenderTargetBinding& binding, const Profile& profile) {
    const auto& layout = profile.gbuffer;

    return layout.render_targets > 0 && binding.render_targets.size() == layout.render_targets && binding.depth != nullptr &&
           (binding.depth_view_flags & DSV_READ_ONLY_DEPTH) == 0 && target_at(binding, layout.albedo) != nullptr &&
           target_at(binding, layout.normals) != nullptr && target_at(binding, layout.velocity) != nullptr;
}

GBufferCheck identify_gbuffer(const RenderTargetBinding& binding, const Profile& profile, const Describe& describe) {
    if (!has_gbuffer_shape(binding, profile)) {
        return {std::nullopt, GBufferProblem::NotGBufferPass};
    }

    const auto& layout = profile.gbuffer;
    const GBuffer gbuffer{
        target_at(binding, layout.albedo), target_at(binding, layout.normals), target_at(binding, layout.velocity), binding.depth};

    const auto albedo = describe(gbuffer.albedo);
    const auto normals = describe(gbuffer.normals);
    const auto velocity = describe(gbuffer.velocity);
    const auto depth = describe(gbuffer.depth);
    const auto& formats = profile.formats;

    if (albedo.format != formats.albedo || normals.format != formats.normals || velocity.format != formats.velocity ||
        depth.format != formats.depth) {
        return {std::nullopt, GBufferProblem::Format};
    }

    if (normals.size != albedo.size || velocity.size != albedo.size || depth.size != albedo.size) {
        return {std::nullopt, GBufferProblem::Size};
    }

    if (depth.mip_levels != 1 || depth.array_size != 1) {
        return {std::nullopt, GBufferProblem::DepthLayout};
    }

    auto identified = gbuffer;
    identified.size = albedo.size;
    return {identified, GBufferProblem::None};
}

std::string_view to_string(GBufferProblem problem) {
    switch (problem) {
    case GBufferProblem::None:
        return "identified";
    case GBufferProblem::NotGBufferPass:
        return "not the G-buffer pass";
    case GBufferProblem::Format:
        return "a resource RR reads, or the depth, has another format than the profile's";
    case GBufferProblem::Size:
        return "the resources RR reads and the depth are not all of one size";
    case GBufferProblem::DepthLayout:
        return "the depth has mips or array slices";
    }

    return "?";
}
} // namespace rr::core
