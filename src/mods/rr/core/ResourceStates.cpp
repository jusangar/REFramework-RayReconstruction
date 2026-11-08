#include <algorithm>

#include "ResourceStates.hpp"

namespace rr::core {
using namespace d3d12_value;

namespace {
// Each read and the subresource it is: the depth has one per plane, with one mip and one array slice,
// as identify_gbuffer requires.
std::uint32_t subresource_of(Read read) {
    return read == Read::DepthPlane1 ? 1 : 0;
}

ResourceId resource_of(Read read, const ReadResources& resources) {
    switch (read) {
    case Read::T:
        return resources.t;
    case Read::Velocity:
        return resources.gbuffer.velocity;
    case Read::Albedo:
        return resources.gbuffer.albedo;
    case Read::Normals:
        return resources.gbuffer.normals;
    case Read::DepthPlane0:
    case Read::DepthPlane1:
        return resources.gbuffer.depth;
    }

    return nullptr;
}

constexpr bool covers(std::uint32_t transition_subresource, std::uint32_t subresource) {
    return transition_subresource == subresource || transition_subresource == ALL_SUBRESOURCES;
}

// Whether RR reads it at the TAA draw: every read but the velocity in a frame without velocity.
constexpr bool is_read(Read read, bool reads_velocity) {
    return read != Read::Velocity || reads_velocity;
}
} // namespace

std::uint32_t expected_state(Read read) {
    return read == Read::DepthPlane0 || read == Read::DepthPlane1 ? DEPTH_READ | SHADER_RESOURCE : SHADER_RESOURCE;
}

/////////////////////////////////////////////////////////////////////////////
// Recording
/////////////////////////////////////////////////////////////////////////////

void Recording::reset() {
    m_events.clear();
    m_taa.reset();
}

void Recording::transition(const Transition& transition) {
    m_events.push_back({Event::Kind::Transition, transition});
}

void Recording::gbuffer_pass(std::uint64_t number) {
    m_events.push_back({Event::Kind::GBufferPass, {}, number});
}

void Recording::taa_draw(const TaaMarker& marker) {
    m_events.push_back({Event::Kind::TaaDraw});
    m_taa = marker;
}

std::optional<std::uint32_t> Recording::state(ResourceId resource, std::uint32_t subresource) const {
    for (auto it = m_events.rbegin(); it != m_events.rend(); ++it) {
        if (const auto& t = it->transition;
            it->kind == Event::Kind::Transition && t.resource == resource && covers(t.subresource, subresource)) {
            return t.after;
        }
    }

    return std::nullopt;
}

bool Recording::names(ResourceId resource) const {
    return std::any_of(m_events.begin(), m_events.end(),
        [resource](const Event& e) { return e.kind == Event::Kind::Transition && e.transition.resource == resource; });
}

/////////////////////////////////////////////////////////////////////////////
// The registry
/////////////////////////////////////////////////////////////////////////////

std::optional<std::uint32_t> ResourceStates::Tracked::get(std::uint32_t subresource) const {
    for (const auto& [s, state] : apart) {
        if (s == subresource) {
            return state;
        }
    }

    return all;
}

void ResourceStates::Tracked::set(std::uint32_t subresource, std::optional<std::uint32_t> state) {
    if (subresource == ALL_SUBRESOURCES || count == 1) {
        all = state;
        apart.clear();
        return;
    }

    for (auto& [s, current] : apart) {
        if (s == subresource) {
            current = state;
            return;
        }
    }

    apart.emplace_back(subresource, state);
}

std::optional<std::uint32_t> ResourceStates::Tracked::contradicted(std::uint32_t subresource, std::uint32_t before, bool& known) const {
    const auto check = [&](const std::optional<std::uint32_t>& state) -> std::optional<std::uint32_t> {
        known = known || state.has_value();
        return state && *state != before ? state : std::nullopt;
    };

    known = false;

    if (subresource != ALL_SUBRESOURCES) {
        return check(get(subresource));
    }

    // Every subresource has to agree: those apart, and through `all` the others, if any is left.
    std::optional<std::uint32_t> contradiction{};

    for (const auto& [s, state] : apart) {
        if (const auto c = check(state); c && !contradiction) {
            contradiction = c;
        }
    }

    if (const auto c = apart.size() < count ? check(all) : std::nullopt; c && !contradiction) {
        contradiction = c;
    }

    return contradiction;
}

void ResourceStates::on_created(ResourceId resource, bool followed, std::uint32_t subresources) {
    ++m_counters.created;
    m_resources[resource] = Tracked{followed, std::max(subresources, 1u)};
}

ResourceStates::Pass ResourceStates::on_gbuffer_pass(const GBuffer& gbuffer) {
    ++m_counters.gbuffer_passes;

    const auto changed = m_gbuffer != gbuffer;

    if (changed) {
        ++m_counters.gbuffer_changes;
    }

    m_gbuffer = gbuffer;
    m_recorded_passes.emplace_back(++m_pass, gbuffer);

    // A pass whose list is never submitted must not keep them growing.
    if (m_recorded_passes.size() > 16) {
        m_recorded_passes.pop_front();
    }

    return {m_pass, changed};
}

std::optional<std::uint32_t> ResourceStates::state(ResourceId resource, std::uint32_t subresource) const {
    const auto it = m_resources.find(resource);
    return it != m_resources.end() && it->second.followed ? it->second.get(subresource) : std::nullopt;
}

std::optional<std::uint32_t> ResourceStates::state_of(Read read, const ReadResources& resources) const {
    return state(resource_of(read, resources), subresource_of(read));
}

std::optional<Read> ResourceStates::read_of(ResourceId resource, std::uint32_t subresource) const {
    if (resource == m_t) {
        return Read::T;
    }

    if (!m_gbuffer) {
        return std::nullopt;
    }

    if (resource == m_gbuffer->velocity) {
        return Read::Velocity;
    }

    if (resource == m_gbuffer->albedo) {
        return Read::Albedo;
    }

    if (resource == m_gbuffer->normals) {
        return Read::Normals;
    }

    if (resource == m_gbuffer->depth) {
        return subresource == 1 ? Read::DepthPlane1 : Read::DepthPlane0;
    }

    return std::nullopt;
}

StateAssessment ResourceStates::on_taa_draw(ResourceId t, ResourceId velocity, Extent t_size, Recording& list) {
    ++m_counters.draws;
    m_t = t;

    StateAssessment assessment{};

    if (!m_gbuffer) {
        assessment.problem = StateProblem::GBufferUnknown;
        ++m_counters.problems[static_cast<std::size_t>(assessment.problem)];
        return assessment;
    }

    const ReadResources resources{t, *m_gbuffer};
    assessment.gbuffer = *m_gbuffer;
    assessment.gbuffer_pass = m_pass;
    assessment.gbuffer_submitted = m_pass <= m_submitted_pass;

    if (assessment.gbuffer_submitted) {
        ++m_counters.draws_pass_submitted;
    }

    // What the TAA's list itself transitioned, or else where the lists submitted so far left it. Without
    // the velocity, nothing is predicted of the G-buffer's.
    const auto reads_velocity = velocity != nullptr;

    for (std::size_t i = 0; i < READ_COUNT; ++i) {
        const auto read = static_cast<Read>(i);

        if (!is_read(read, reads_velocity)) {
            continue;
        }

        const auto resource = resource_of(read, resources);
        const auto in_list = list.state(resource, subresource_of(read));
        assessment.predicted[i] = in_list ? in_list : state(resource, subresource_of(read));
    }

    const auto changed = m_draw_gbuffer != *m_gbuffer;
    m_draw_gbuffer = *m_gbuffer;

    const auto& p = assessment.predicted;
    auto unknown = false;
    auto unexpected = false;

    for (std::size_t i = 0; i < READ_COUNT; ++i) {
        if (!is_read(static_cast<Read>(i), reads_velocity)) {
            continue;
        }

        unknown = unknown || !p[i];
        unexpected = unexpected || (p[i] && *p[i] != expected_state(static_cast<Read>(i)));
    }

    if (reads_velocity && velocity != m_gbuffer->velocity) {
        assessment.problem = StateProblem::VelocityNotGBuffer;
    } else if (m_gbuffer->size != t_size) {
        assessment.problem = StateProblem::SizeNotT;
    } else if (changed) {
        assessment.problem = StateProblem::IdentityChanged;
    } else if (m_unconfirmed) {
        assessment.problem = StateProblem::Mismatch;
    } else if (unknown) {
        assessment.problem = StateProblem::Unknown;
    } else if (unexpected) {
        assessment.problem = StateProblem::Unexpected;
    }

    ++m_counters.problems[static_cast<std::size_t>(assessment.problem)];
    list.taa_draw({resources, assessment.gbuffer_pass, assessment.predicted, assessment.problem == StateProblem::None, reads_velocity});
    return assessment;
}

Submission ResourceStates::on_submitted(const Recording& list, bool copy_list) {
    ++m_counters.lists;
    Submission submission{};

    for (const auto& event : list.events()) {
        switch (event.kind) {
        case Recording::Event::Kind::Transition:
            replay(event.transition, copy_list, submission);
            break;
        case Recording::Event::Kind::GBufferPass: {
            m_submitted_pass = std::max(m_submitted_pass, event.gbuffer_pass);
            const auto& passes = m_recorded_passes;
            const auto pass = std::find_if(passes.begin(), passes.end(), [&](const auto& p) { return p.first == event.gbuffer_pass; });
            m_submitted_gbuffer = pass != passes.end() ? std::optional{pass->second} : std::nullopt;
            break;
        }
        case Recording::Event::Kind::TaaDraw:
            if (list.taa()) {
                submission.taa = confirm(*list.taa());
            }

            break;
        }
    }

    return submission;
}

void ResourceStates::replay(const Transition& transition, bool copy_list, Submission& submission) {
    const auto it = m_resources.find(transition.resource);

    if (it == m_resources.end()) {
        ++m_counters.unseen_creation;
        return;
    }

    auto& tracked = it->second;

    if (!tracked.followed) {
        ++m_counters.not_followed;
        return;
    }

    if (copy_list) {
        ++m_counters.copy_list;
        tracked.set(transition.subresource, std::nullopt);
        return;
    }

    ++m_counters.transitions;
    bool known{};
    const auto held = tracked.contradicted(transition.subresource, transition.before, known);

    if (!known) {
        ++m_counters.first_sights;
    }

    if (!held) {
        tracked.set(transition.subresource, transition.after);
        return;
    }

    // Something the registry does not see moved the resource: an implicit promotion or decay, or work it
    // does not follow. Its state stays unknown until its next transition.
    ++m_counters.mismatches;
    tracked.set(transition.subresource, std::nullopt);

    if (const auto read = read_of(transition.resource, transition.subresource)) {
        ++m_counters.read_mismatches;
        m_unconfirmed = true;
        m_mismatch_since_taa = true;
        submission.read_mismatches.push_back({*read, transition.resource, transition.subresource, transition.before, *held});
    }
}

TaaConfirmation ResourceStates::confirm(const TaaMarker& marker) {
    ++m_counters.confirmations;

    TaaConfirmation confirmation{marker};
    const auto& pass = marker.gbuffer_pass;
    confirmation.order = pass == m_submitted_pass  ? GBufferOrder::SamePass
                         : pass < m_submitted_pass ? GBufferOrder::LaterPass
                                                   : GBufferOrder::NotSubmitted;
    ++m_counters.order[static_cast<std::size_t>(confirmation.order)];

    // The states of resources the frame no longer uses can still look right: nothing follows their
    // destruction.
    confirmation.gbuffer_differs = m_submitted_gbuffer != marker.resources.gbuffer;
    confirmation.disagree = confirmation.gbuffer_differs;
    confirmation.expected = true;

    for (std::size_t i = 0; i < READ_COUNT; ++i) {
        const auto read = static_cast<Read>(i);
        const auto& predicted = marker.predicted[i];
        auto& actual = confirmation.actual[i];
        actual = state_of(read, marker.resources);

        // A TAA that reads no velocity needs none in its state.
        if (!is_read(read, marker.reads_velocity)) {
            continue;
        }

        if (actual == expected_state(read)) {
            ++m_counters.expected_at_taa[i];
        } else {
            confirmation.expected = false;
        }

        // A read the draw did not know assumed nothing.
        confirmation.disagree = confirmation.disagree || (predicted && predicted != actual);
    }

    if (confirmation.gbuffer_differs) {
        ++m_counters.gbuffer_differs;
    }

    if (confirmation.disagree) {
        ++m_counters.disagree;

        // The frame could have been evaluated with that state.
        if (marker.known) {
            ++m_counters.wrong;
        }
    }

    // Clean: no mismatch on a read since the last confirmation, and the prediction held.
    m_unconfirmed = m_mismatch_since_taa || confirmation.disagree;
    m_mismatch_since_taa = false;
    return confirmation;
}

std::string_view to_string(Read read) {
    switch (read) {
    case Read::T:
        return "T";
    case Read::Velocity:
        return "velocity";
    case Read::Albedo:
        return "albedo";
    case Read::Normals:
        return "normals";
    case Read::DepthPlane0:
        return "depth plane 0";
    case Read::DepthPlane1:
        return "depth plane 1";
    }

    return "?";
}

std::string_view to_string(StateProblem problem) {
    switch (problem) {
    case StateProblem::None:
        return "known";
    case StateProblem::GBufferUnknown:
        return "no G-buffer pass identified yet";
    case StateProblem::VelocityNotGBuffer:
        return "the velocity is not the G-buffer's";
    case StateProblem::SizeNotT:
        return "the G-buffer is not at T's size";
    case StateProblem::IdentityChanged:
        return "the G-buffer changed identity";
    case StateProblem::Mismatch:
        return "mismatch not confirmed clean yet";
    case StateProblem::Unknown:
        return "a read has no known state";
    case StateProblem::Unexpected:
        return "a read is not in its expected state";
    }

    return "?";
}

std::string_view to_string(GBufferOrder order) {
    switch (order) {
    case GBufferOrder::SamePass:
        return "the draw's G-buffer pass, the last one submitted";
    case GBufferOrder::LaterPass:
        return "a later G-buffer pass submitted before the TAA's list";
    case GBufferOrder::NotSubmitted:
        return "the draw's G-buffer pass not submitted before the TAA's list";
    }

    return "?";
}
} // namespace rr::core
