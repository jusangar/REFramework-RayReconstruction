#include <algorithm>
#include <utility>

#include "FramePolicy.hpp"

namespace rr::core {
namespace {
// The reasons the game omits frames for, as against RR, the bridge, the Mod or its options: only a run of
// these keeps RR's history.
bool game_reason(SkipReason reason) {
    return reason == SkipReason::IdentificationMismatch || reason == SkipReason::PsNotAdmitted;
}

// The first condition in HistoryBreak's order wins.
void join(std::optional<HistoryBreak>& broken, HistoryBreak cause) {
    broken = broken ? std::min(*broken, cause) : cause;
}

// A condition that breaks a run, as the outcome of a frame without velocity: omitted, but for a cut.
NoVelocityOutcome no_velocity_outcome_of(HistoryBreak condition) {
    switch (condition) {
    case HistoryBreak::RrOff:
        return NoVelocityOutcome::RrOff;
    case HistoryBreak::FeatureOrSize:
        return NoVelocityOutcome::FeatureOrSize;
    case HistoryBreak::OtherReason:
        return NoVelocityOutcome::OtherReason;
    case HistoryBreak::MainLayer:
        return NoVelocityOutcome::MainLayer;
    case HistoryBreak::CameraCut:
        return NoVelocityOutcome::EvaluatedWithReset;
    case HistoryBreak::TooLong:
        return NoVelocityOutcome::TooLong;
    }

    return NoVelocityOutcome::TooLong;
}
} // namespace

FramePolicy::FramePolicy(const Profile& profile, float jitter_amplitude)
    : m_profile{profile},
      m_jitter_amplitude{jitter_amplitude},
      m_cut_detector{profile.cuts} {
}

void FramePolicy::on_toggle(bool enabled) {
    if (!enabled && m_enabled) {
        break_run(HistoryBreak::RrOff);
    }

    m_enabled = enabled;
}

void FramePolicy::on_jitter_injection(bool enabled) {
    m_jitter_injection = enabled;
}

FeatureAction FramePolicy::on_bridge_state(BridgeState state) {
    if (state == BridgeState::Loaded && m_bridge != BridgeState::Loaded) {
        m_ngx_init_requested = false;
    }

    m_bridge = state;

    // A creation that failed, or another NGX consumer: the frames are omitted for the bridge from now on,
    // and nothing asks for the feature again.
    if (!m_feature_alive || state == BridgeState::Loaded || state == BridgeState::Ready) {
        return FeatureAction::None;
    }

    m_feature_alive = false;
    return FeatureAction::Release;
}

FeatureAction FramePolicy::on_device_reset() {
    // The next identified frame has to create the feature again, so it will be omitted.
    m_taa_replaced = false;

    if (!m_feature_alive) {
        return FeatureAction::None;
    }

    m_feature_alive = false;
    return FeatureAction::Release;
}

CameraDecision FramePolicy::on_camera_update(const CameraUpdate& update) {
    // The cut detector starts again: a run of omitted frames after it does not keep RR's history.
    if (!update.main_layer_updated) {
        m_cut_detector.restart();
        break_run(HistoryBreak::MainLayer);
        return {m_camera_updates};
    }

    if (update.layer_changed) {
        m_cut_detector.restart();
        break_run(HistoryBreak::MainLayer);
    }

    CameraDecision decision{++m_camera_updates};

    // Also with RR off, so that its prediction is ready when RR is turned on.
    decision.cut_check = m_cut_detector.on_update(update.jitter_disable.view, update.jitter_disable.projection);

    // A view that cannot be inverted: the detector started again, as for the main layer above.
    if (m_cut_detector.restarted()) {
        break_run(HistoryBreak::MainLayer);
    }

    if (decision.cut_check && decision.cut_check->cut) {
        m_cut_pending = true;
    }

    auto scene = update.scene;

    // The jitter rule: inject in N only if the game's TAA was replaced in N-1, by RR, the debug view or the
    // accumulated reference.
    // It is the bet that N will be replaced too; when it is not, the game's TAA gets RR's jitter for one
    // frame.
    if (m_jitter_injection && !global_skip() && m_taa_replaced) {
        decision.jitter_term = jitter_term(own_jitter(decision.update_number, m_jitter_amplitude), m_evaluated_size);
        scene = inject_jitter(scene, update.jitter_disable.projection, *decision.jitter_term);
    }

    m_camera = Camera{decision.update_number, scene.projection, update.jitter_disable.projection, scene.inverse_view_projection,
        scene.old_view_projection};
    return decision;
}

Decision FramePolicy::on_evaluation_point(const EvaluationPoint& point) {
    m_feature_alive = point.feature.status != FeatureState::Status::Absent;

    Decision decision{};
    decision.render_size = point.t.value_or(Extent{});

    if (m_camera && point.t) {
        const auto j = jitter_of(m_camera->projection, m_camera->jitter_disable_projection, *point.t);
        decision.camera = CameraConstants{m_camera->update_number, j, j, m_camera->inverse_view_projection, m_camera->old_view_projection};
    }

    if (!point.has_velocity) {
        decision.no_velocity = no_velocity_outcome(point, decision);
    }

    const auto last_pass = std::exchange(m_last_point_pass, point.gbuffer_pass);
    m_last_point_update = m_camera ? m_camera->update_number : 0;

    if (decision.no_velocity) {
        return decide_without_velocity(decision);
    }

    decision.identification = point_skip(point);
    decision.skip = global_skip();

    if (!decision.skip) {
        decision.skip = decision.identification;
    }

    if (!decision.skip) {
        decision.skip = feature_skip(point, decision.action);
    }

    if (!decision.skip && !decision.camera) {
        decision.skip = SkipReason::CameraUnknown;
    }

    std::optional<HistoryBreak> history_break{};

    if (!decision.skip) {
        history_break = take_evaluated(decision);
    }

    m_taa_replaced = !decision.skip;
    m_frame = FrameSummary{decision.skip, decision.reset, decision.identification, history_break};

    // An evaluated frame without a G-buffer pass of its own, by the passes' numbers and not by the window
    // between two Presents. A frame without velocity is only evaluated with a newer pass, so it never
    // reuses one.
    if (!decision.skip && last_pass && point.gbuffer_pass <= *last_pass) {
        m_frame->reused_gbuffer_pass = point.gbuffer_pass;
    }

    return decision;
}

// The reset is a new feature's, that of a run that broke, or a cut's. The run ends here.
std::optional<HistoryBreak> FramePolicy::take_evaluated(Decision& decision) {
    std::optional<HistoryBreak> history_break{};

    if (!m_last_frame_evaluated) {
        history_break = run_break(decision);
    }

    if (m_new_feature) {
        decision.reset = ResetCause::FeatureRecreated;
        m_new_feature = false;
    } else if (history_break) {
        decision.reset = ResetCause::AfterOmittedFrame;
    } else if (m_cut_pending) {
        decision.reset = ResetCause::CameraCut;
    }

    // Only an evaluated point takes the cut, so that a cut in a run reaches its first frame evaluated.
    m_evaluated_size = decision.render_size;
    m_last_evaluated_cut = m_cut_pending;
    m_cut_pending = false;
    m_run = {};
    return history_break;
}

// Whatever omits any frame, the state of the reads but the velocity, a G-buffer pass of its own, then the
// conditions of a run that keeps RR's history, joined as in the first frame evaluated after a run: the
// frame is evaluated where RR's history goes on, or where a camera cut is the first thing that ends it, which
// resets it. Nothing here asks anything of NGX or of the feature.
NoVelocityOutcome FramePolicy::no_velocity_outcome(const EvaluationPoint& point, const Decision& decision) const {
    if (!m_enabled || m_run.history_break == HistoryBreak::RrOff) {
        return NoVelocityOutcome::RrOff;
    }

    if (m_bridge != BridgeState::Ready) {
        return NoVelocityOutcome::BridgeNotReady;
    }

    // With its state taken as known, point_skip gives what the identification alone says.
    auto identification_only = point;
    identification_only.states_known = true;

    if (point_skip(identification_only)) {
        return NoVelocityOutcome::NotIdentified;
    }

    if (!point.states_known) {
        return NoVelocityOutcome::StateUnknown;
    }

    // Before the first point, any pass the frame has is its own, and pass 0 is none. The diagnostic of the
    // reused pass differs there: it never marks the first point.
    if (point.gbuffer_pass <= m_last_point_pass.value_or(0)) {
        return NoVelocityOutcome::GBufferPassNotNew;
    }

    auto broken = run_break(decision);

    // A feature that would have to be created or waited for.
    if (point.feature.status != FeatureState::Status::Ready || point.feature.size != decision.render_size) {
        join(broken, HistoryBreak::FeatureOrSize);
    }

    // Without an update of its own, the camera is the last point's: the Present of a frame without the
    // update says so only after the point.
    if (!decision.camera || decision.camera->update_number <= m_last_point_update) {
        join(broken, HistoryBreak::MainLayer);
    }

    return broken ? no_velocity_outcome_of(*broken) : NoVelocityOutcome::Evaluated;
}

// Evaluated as any frame: with Evaluated, whose run, if any, kept RR's history and with no cut pending, without
// a reset; with EvaluatedWithReset, with the reset of its cut. Otherwise the frame closes as before, when its
// TAA draw had no evaluation point: for identifications that do not agree, a reason of the game, under the
// reasons that omit every frame.
Decision FramePolicy::decide_without_velocity(Decision decision) {
    std::optional<HistoryBreak> history_break{};

    if (is_evaluated(*decision.no_velocity)) {
        history_break = take_evaluated(decision);

        // No run and no cut pending: the frame evaluated right before took the cut, maybe this frame's, and a
        // frame with its velocity would go on from it. Without velocity, the same reset as in a run after it.
        if (decision.no_velocity == NoVelocityOutcome::EvaluatedWithReset && decision.reset == ResetCause::None) {
            decision.reset = ResetCause::CameraCut;
        }
    } else {
        decision.identification = SkipReason::IdentificationMismatch;
        decision.skip = global_skip().value_or(SkipReason::IdentificationMismatch);
    }

    m_taa_replaced = !decision.skip;
    m_frame = FrameSummary{decision.skip, decision.reset, decision.identification, history_break, decision.no_velocity};
    return decision;
}

void FramePolicy::on_not_evaluated(SkipReason reason) {
    if (!m_frame || m_frame->skip) {
        return;
    }

    // A new feature still has no history.
    if (m_frame->reset == ResetCause::FeatureRecreated) {
        m_new_feature = true;
    }

    // The frame is omitted, so the run goes on, and breaks for this reason. After the first frame evaluated
    // of a run, the run before it was already taken: the reset is right, but the condition the next run
    // gives may be this one instead of an earlier one in HistoryBreak's order.
    m_frame->skip = reason;
    m_frame->reset = ResetCause::None;
    m_frame->history_break.reset();
    m_frame->reused_gbuffer_pass.reset();

    if (m_frame->no_velocity) {
        m_frame->no_velocity = NoVelocityOutcome::NotEvaluated;
    }

    // The debug view and the accumulated reference write O in place of the game's TAA, as RR does, so the
    // own jitter goes on.
    m_taa_replaced = reason == SkipReason::DebugView || reason == SkipReason::AccumulatedReference;
}

FrameSummary FramePolicy::on_end_frame() {
    if (!m_frame) {
        m_taa_replaced = false;
    }

    auto summary = m_frame.value_or(
        FrameSummary{global_skip().value_or(SkipReason::IdentificationMismatch), ResetCause::None, SkipReason::IdentificationMismatch});
    m_frame.reset();
    m_last_frame_evaluated = !summary.skip;

    // From the closed frame, so that an evaluation the Mod did not record counts as omitted.
    if (summary.skip) {
        ++m_run.frames;

        if (*summary.skip == SkipReason::RrOff) {
            break_run(HistoryBreak::RrOff);
        } else if (!game_reason(*summary.skip)) {
            break_run(HistoryBreak::OtherReason);
        }
    }

    return summary;
}

// The reasons that do not depend on the frame.
std::optional<SkipReason> FramePolicy::global_skip() const {
    if (!m_enabled) {
        return SkipReason::RrOff;
    }

    switch (m_bridge) {
    case BridgeState::Unavailable:
        return SkipReason::BridgeUnavailable;
    case BridgeState::OtherNgxConsumer:
        return SkipReason::OtherNgxConsumer;
    case BridgeState::Loaded:
    case BridgeState::Ready:
        break;
    }

    return std::nullopt;
}

// The reasons the evaluation point itself gives, before the feature.
std::optional<SkipReason> FramePolicy::point_skip(const EvaluationPoint& point) const {
    if (!point.profile_vs_created) {
        return SkipReason::DigestMismatch;
    }

    if (point.t && !point.t_profile_format) {
        return SkipReason::FormatMismatch;
    }

    if (!point.barrier_rule || point.vs != m_profile.taa_vs) {
        return SkipReason::IdentificationMismatch;
    }

    if (!admits_ps(m_profile, point.ps)) {
        return SkipReason::PsNotAdmitted;
    }

    if (!point.states_known) {
        return SkipReason::StateUnknown;
    }

    if (!point.t || !point.o || point.o->size != *point.t) {
        return SkipReason::OutputMismatch;
    }

    if (!point.o->uav) {
        return SkipReason::OutputNoUav;
    }

    return std::nullopt;
}

// Once the frame is identified: what the feature needs before RR can be evaluated.
std::optional<SkipReason> FramePolicy::feature_skip(const EvaluationPoint& point, FeatureAction& action) {
    if (m_bridge == BridgeState::Loaded) {
        if (!m_ngx_init_requested) {
            action = FeatureAction::InitNgx;
            m_ngx_init_requested = true;
        }

        return SkipReason::FeatureNotReady;
    }

    if (point.feature.status == FeatureState::Status::Absent) {
        action = FeatureAction::Create;
        m_new_feature = true;
        m_feature_alive = true;
        return SkipReason::FeatureNotReady;
    }

    // Also while it is being created: a change of image quality goes through several sizes in a few
    // frames, and each one recreates the feature.
    if (point.feature.size != *point.t) {
        action = FeatureAction::Recreate;
        m_new_feature = true;
        return SkipReason::FeatureNotReady;
    }

    if (point.feature.status == FeatureState::Status::Pending) {
        return SkipReason::FeatureNotReady;
    }

    return std::nullopt;
}

void FramePolicy::break_run(HistoryBreak cause) {
    join(m_run.history_break, cause);
}

// At the first frame evaluated after a run of omitted frames: the conditions that are known here join those
// its frames and camera updates gave. A cut in the run, or in this frame, breaks it.
std::optional<HistoryBreak> FramePolicy::run_break(const Decision& decision) const {
    auto broken = m_run.history_break;

    if (m_new_feature || decision.render_size != m_evaluated_size) {
        join(broken, HistoryBreak::FeatureOrSize);
    }

    if (m_cut_pending || m_last_evaluated_cut) {
        join(broken, HistoryBreak::CameraCut);
    }

    if (m_run.frames > m_profile.max_kept_run) {
        join(broken, HistoryBreak::TooLong);
    }

    return broken;
}

void FrameCounters::add(const FrameSummary& frame) {
    ++frames;
    ++resets[static_cast<std::size_t>(frame.reset)];

    if (frame.skip) {
        ++omitted[static_cast<std::size_t>(*frame.skip)];
    } else {
        ++evaluated;
    }

    if (frame.identification) {
        ++not_identified[static_cast<std::size_t>(*frame.identification)];
    } else {
        ++identified;
    }

    if (frame.no_velocity) {
        ++no_velocity[static_cast<std::size_t>(*frame.no_velocity)];
    }

    reused_gbuffer_pass += frame.reused_gbuffer_pass ? 1 : 0;
}

OmittedRuns::Change OmittedRuns::add(std::uint64_t frame, const FrameSummary& summary) {
    Change change{};

    if (!summary.skip) {
        if (m_current) {
            m_current->history_break = summary.history_break;
            ++(summary.history_break ? m_broken_by[static_cast<std::size_t>(*summary.history_break)] : m_kept);
        }

        change.ended = std::exchange(m_current, std::nullopt);
        return change;
    }

    if (!m_current) {
        m_current = OmittedRun{frame};
        change.started = true;
        ++m_count;
    }

    ++m_current->frames;
    ++m_current->omitted[static_cast<std::size_t>(*summary.skip)];
    return change;
}

std::optional<ReusedPassRun> ReusedPassRuns::add(std::uint64_t frame, const FrameSummary& summary) {
    const auto& pass = summary.reused_gbuffer_pass;

    if (m_current && pass == m_current->pass) {
        m_current->last = frame;
        return std::nullopt;
    }

    auto ended = std::exchange(m_current, std::nullopt);

    if (pass) {
        m_current = ReusedPassRun{frame, frame, *pass};
    }

    return ended;
}

std::string_view to_string(SkipReason reason) {
    switch (reason) {
    case SkipReason::RrOff:
        return "RR off";
    case SkipReason::BridgeUnavailable:
        return "bridge not available";
    case SkipReason::OtherNgxConsumer:
        return "another NGX consumer";
    case SkipReason::DigestMismatch:
        return "digest differs from the profile";
    case SkipReason::FormatMismatch:
        return "format differs from the profile";
    case SkipReason::IdentificationMismatch:
        return "identifications do not agree";
    case SkipReason::PsNotAdmitted:
        return "pixel shader not admitted";
    case SkipReason::StateUnknown:
        return "resource state unknown";
    case SkipReason::OutputMismatch:
        return "O does not match T";
    case SkipReason::OutputNoUav:
        return "O without the UAV flag";
    case SkipReason::FeatureNotReady:
        return "feature not ready";
    case SkipReason::CameraUnknown:
        return "camera not updated yet";
    case SkipReason::DebugView:
        return "debug view in place of RR";
    case SkipReason::EvaluationFailed:
        return "evaluation not recorded";
    case SkipReason::AccumulatedReference:
        return "accumulated reference in place of RR";
    }

    return "?";
}

std::string_view to_string(ResetCause cause) {
    switch (cause) {
    case ResetCause::None:
        return "none";
    case ResetCause::AfterOmittedFrame:
        return "first frame after an omitted frame";
    case ResetCause::FeatureRecreated:
        return "feature recreated";
    case ResetCause::CameraCut:
        return "camera cut";
    }

    return "?";
}

std::string_view to_string(HistoryBreak condition) {
    switch (condition) {
    case HistoryBreak::RrOff:
        return "RR turned off";
    case HistoryBreak::FeatureOrSize:
        return "new feature or another size of T";
    case HistoryBreak::OtherReason:
        return "a reason that is not the game's";
    case HistoryBreak::MainLayer:
        return "main layer changed or not updated, or view not invertible";
    case HistoryBreak::CameraCut:
        return "camera cut";
    case HistoryBreak::TooLong:
        return "longer than the profile's limit";
    }

    return "?";
}

std::string_view to_string(NoVelocityOutcome outcome) {
    switch (outcome) {
    case NoVelocityOutcome::Evaluated:
        return "evaluated with the camera's reprojection";
    case NoVelocityOutcome::EvaluatedWithReset:
        return "evaluated with a reset, after a camera cut";
    case NoVelocityOutcome::NotEvaluated:
        return "not evaluated by the Mod";
    case NoVelocityOutcome::RrOff:
        return "RR off or turned off";
    case NoVelocityOutcome::BridgeNotReady:
        return "bridge or NGX not ready";
    case NoVelocityOutcome::NotIdentified:
        return "not identified but for the velocity";
    case NoVelocityOutcome::StateUnknown:
        return "resource state unknown";
    case NoVelocityOutcome::GBufferPassNotNew:
        return "no G-buffer pass of its own";
    case NoVelocityOutcome::FeatureOrSize:
        return "feature not ready, new feature or another size of T";
    case NoVelocityOutcome::OtherReason:
        return "a frame omitted for a reason that is not the game's";
    case NoVelocityOutcome::MainLayer:
        return "main layer changed or not updated, or view not invertible";
    case NoVelocityOutcome::TooLong:
        return "after a run longer than the profile's limit";
    }

    return "?";
}

std::string_view to_string(FeatureAction action) {
    switch (action) {
    case FeatureAction::None:
        return "none";
    case FeatureAction::InitNgx:
        return "initialize NGX";
    case FeatureAction::Create:
        return "create";
    case FeatureAction::Recreate:
        return "recreate";
    case FeatureAction::Release:
        return "release";
    }

    return "?";
}
} // namespace rr::core
