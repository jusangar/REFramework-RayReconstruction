#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "CutDetector.hpp"
#include "Jitter.hpp"
#include "Profile.hpp"
#include "ShaderDigest.hpp"

namespace rr::core {
// Why RR is not evaluated in a frame: a frame omitted, where the game's TAA keeps its place. The
// interface shows it as is. When several apply, the first one in this order wins.
enum class SkipReason : std::uint8_t {
    RrOff,
    BridgeUnavailable,
    OtherNgxConsumer,
    // No PSO of the game carries the profile's TAA vertex shader, as after a patch.
    DigestMismatch,
    // T, found by its role, has another format than the profile's. The format is a check and not part of
    // the identification, so that a game whose T differs says so instead of never being identified.
    FormatMismatch,
    // The two identifications of the evaluation point do not pick the same draw: the barrier rule
    // anchors a draw without the profile's TAA vertex shader (the menus), or a frame ends without
    // any evaluation point.
    IdentificationMismatch,
    // The pixel shader of the TAA draw is excluded by the profile or unknown to it.
    PsNotAdmitted,
    StateUnknown,
    // O is not identified, or its size is not T's.
    OutputMismatch,
    OutputNoUav,
    FeatureNotReady,
    // No camera update has completed yet, so there are no constants for the evaluation, and a frame where
    // something is not known is omitted.
    CameraUnknown,
    // The last three come after the decision, from FramePolicy::on_not_evaluated. The debug view wrote O
    // in place of RR: with the accumulated reference, the only omitted frames where the game's TAA does not
    // keep its place either.
    DebugView,
    // The Mod could not record the evaluation: the guide pass, or the bridge, failed, or the feature
    // changed in between.
    EvaluationFailed,
    // The accumulated reference wrote its mean into O in place of RR.
    AccumulatedReference,
};

// From the last reason, which a new one has to replace here.
inline constexpr std::size_t SKIP_REASON_COUNT = static_cast<std::size_t>(SkipReason::AccumulatedReference) + 1;

// Why RR is told that its history has no connection with the current frame.
enum class ResetCause : std::uint8_t {
    None,
    // The first frame evaluated after a run of omitted frames that does not keep RR's history: a short
    // run that the game omits, without a cut, keeps it (HistoryBreak).
    AfterOmittedFrame,
    // The first frame evaluated with a new feature, created or recreated: it has no history. It wins
    // over AfterOmittedFrame, since the frames spent creating the feature are omitted too.
    FeatureRecreated,
    // The cut detector saw a camera cut since the last evaluation point. Only for the cuts that leave
    // no omitted frame: the two causes above win over it. In a frame without velocity, also a cut that the
    // frame evaluated right before it took (NoVelocityOutcome::EvaluatedWithReset).
    CameraCut,
};

// From the last cause, which a new one has to replace here.
inline constexpr std::size_t RESET_CAUSE_COUNT = static_cast<std::size_t>(ResetCause::CameraCut) + 1;

// Why a run of omitted frames does not keep RR's history, so that the first frame evaluated after it
// resets with AfterOmittedFrame, or FeatureRecreated for a new feature. When several apply, the first one
// in this order wins.
enum class HistoryBreak : std::uint8_t {
    // RR was turned off, with or without a frame closed while it was off: turning RR on always resets.
    RrOff,
    // A new feature, or T at another size than at the last frame evaluated: also the first frame ever
    // evaluated, which has no history.
    FeatureOrSize,
    // A frame omitted for a reason that is not the game's: the game omits frames for identifications
    // that do not agree and for a pixel shader not admitted.
    OtherReason,
    // The main scene layer changed, did not update in a frame, or gave a view that cannot be inverted, since
    // the last frame evaluated: the cut detector started its prediction again, and does not see the camera
    // through the run.
    MainLayer,
    // The cut detector saw a cut during the run or at its first frame evaluated, or the last frame evaluated
    // before the run took one.
    CameraCut,
    // More frames than Profile::max_kept_run.
    TooLong,
};

// From the last condition, which a new one has to replace here.
inline constexpr std::size_t HISTORY_BREAK_COUNT = static_cast<std::size_t>(HistoryBreak::TooLong) + 1;

// What became of a frame without velocity: one whose TAA draw the barrier rule tried with T ready and no
// velocity, with the profile's TAA vertex shader and an admitted pixel shader. RR is evaluated there with
// the camera's reprojection in every pixel, only if nothing would omit the frame, and RR's history goes on or
// a camera cut is the first condition that breaks it: otherwise the frame closes as before, when that draw
// had no evaluation point. After the two evaluated outcomes and NotEvaluated, the conditions that keep it
// omitted; when several apply, the first one in this order wins.
enum class NoVelocityOutcome : std::uint8_t {
    Evaluated,
    // Evaluated too, with InReset: a camera cut is the first condition that breaks RR's history, in the frame,
    // since the last frame evaluated, or taken by that frame. The reset voids the history, so the MVs have
    // nothing to reproject: at a cut in the frame itself, they go to the camera of the shot before.
    EvaluatedWithReset,
    // The Mod did not evaluate RR where the decision did: the debug view, the accumulated reference or an
    // evaluation not recorded, as the frame's reason says.
    NotEvaluated,
    // RR is off, or was turned off since the last frame evaluated.
    RrOff,
    // The bridge cannot evaluate RR, or NGX is not initialized: a frame without velocity asks nothing of
    // NGX or of the feature.
    BridgeNotReady,
    // The evaluation point would not be identified even with its velocity: a digest, T's format, the pixel
    // shader or O.
    NotIdentified,
    // The state of T, the albedo, the normals or the depth is not known and expected.
    StateUnknown,
    // The draw's G-buffer pass is not later than the last evaluation point's: the frame has none of its
    // own, and its guides would be an earlier frame's.
    GBufferPassNotNew,
    // From here, the conditions of a run that keeps RR's history, on the frame itself and on any run
    // before it, in HistoryBreak's order. A cut, which goes after MainLayer, gives EvaluatedWithReset.

    // No feature ready at T's size, a new feature, or T at another size than at the last frame evaluated.
    FeatureOrSize,
    // A frame omitted since the last frame evaluated for a reason that is not the game's.
    OtherReason,
    // The main layer changed, did not update or gave a view that cannot be inverted since the last frame
    // evaluated, or has no update since the last evaluation point: the reprojection would give an earlier
    // frame's motion.
    MainLayer,
    // The run since the last frame evaluated is longer than Profile::max_kept_run.
    TooLong,
};

// From the last outcome, which a new one has to replace here.
inline constexpr std::size_t NO_VELOCITY_OUTCOME_COUNT = static_cast<std::size_t>(NoVelocityOutcome::TooLong) + 1;

// Whether RR is evaluated in a frame with this outcome, as far as the decision goes.
constexpr bool is_evaluated(NoVelocityOutcome outcome) {
    return outcome == NoVelocityOutcome::Evaluated || outcome == NoVelocityOutcome::EvaluatedWithReset;
}

enum class FeatureAction : std::uint8_t {
    None,
    InitNgx,
    Create,
    Recreate,
    Release,
};

// Reported by the Mod whenever it changes.
enum class BridgeState : std::uint8_t {
    // Missing, another ABI version, built without the NGX SDK, or NGX said RR is not available.
    Unavailable,
    // NGX or an NGX snippet was already loaded in the process.
    OtherNgxConsumer,
    // Loaded, with NGX not initialized yet: that happens lazily, at the first identified frame.
    Loaded,
    // NGX initialized and RR available.
    Ready,
};

// The feature as the Mod has it at the evaluation point. The Mod creates it on its own command list,
// so it stays pending for some frames after the policy asks for it.
struct FeatureState {
    enum class Status : std::uint8_t {
        Absent,
        Pending,
        Ready,
    };

    Status status{Status::Absent};
    Extent size{};
};

// What the Mod reads right after the update of the main scene layer.
struct CameraUpdate {
    SceneMatrices scene{};          // SceneInfo, as the game wrote it, before any injection
    SceneMatrices jitter_disable{}; // JitterDisableSceneInfo: the same camera without jitter

    // Whether the main scene layer updated in this frame. When it did not, the matrices are those of an
    // earlier update and the policy keeps that one, but the cut detector starts its prediction again.
    bool main_layer_updated{true};

    // Whether the main scene layer is another one than at the previous update. The cut detector starts
    // its prediction again with this update.
    bool layer_changed{};
};

struct CameraDecision {
    // The number of the update the policy keeps: this one, or the last one if the main layer did not
    // update. It grows by one with each update.
    std::uint64_t update_number{};

    // When set, the Mod replaces the game's jitter with RR's own in the profile's variants, through
    // inject_jitter: the term of projection[2][0..1] over JitterDisable's. Set only if the game's TAA was
    // replaced at the last evaluation point completed, by RR, by the debug view or by the accumulated
    // reference: never with RR off. The debug view counts since the game's TAA does not draw either, so
    // that its view of the MVs shows the own jitter, and so does the reference, so that RR and the
    // reference are compared with the same jitter.
    std::optional<Float2> jitter_term{};

    // The cut detector's check of this update, if it evaluated it: also with RR off, and also for a cut
    // that no reset follows because an omitted frame comes first.
    std::optional<CutCheck> cut_check{};
};

// The constants of the guides pass and of the evaluation, all from a single camera update.
struct CameraConstants {
    std::uint64_t update_number{};
    Float2 jitter{};                 // j, in render pixels at T's size
    Float2 jitter_offset{};            // InJitterOffset = j; its sign before NGX is checked in RE2
    Matrix4 inverse_view_projection{}; // Scene's, with the jitter of its update
    Matrix4 old_view_projection{};     // Scene's, the previous view_projection without jitter
};

// O, the TAA's output target: RT0 of the TAA draw, distinct from T, in RENDER_TARGET by a barrier of
// the same list, with the profile's format.
struct OutputTarget {
    Extent size{};
    bool uav{}; // D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
};

// What the Mod sees at a candidate draw for the TAA.
struct EvaluationPoint {
    // First identification: the PT sequence precedes this draw in its list, and T and the velocity went
    // to NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE before it.
    bool barrier_rule{};

    // False at the TAA draw of a frame without velocity, where the barrier rule found T and no velocity
    // (NoVelocityOutcome).
    bool has_velocity{true};

    // Second identification: the draw's vertex shader against the profile's.
    ShaderDigest vs{};

    // Whether the game created some PSO with the profile's TAA vertex shader.
    bool profile_vs_created{};

    ShaderDigest ps{};

    // T, identified by its role: the resource that goes to UAV before the PT's Dispatch and back to
    // RENDER_TARGET after it.
    std::optional<Extent> t{};

    // Whether T has the profile's format: a check, never an identification.
    bool t_profile_format{};

    std::optional<OutputTarget> o{};

    // Whether the state of the game resources RR reads is known.
    bool states_known{};

    // The number of the G-buffer pass the draw used, as the registry of the states counts them; 0 without
    // one. A frame without velocity needs a pass of its own.
    std::uint64_t gbuffer_pass{};

    FeatureState feature{};
};

struct Decision {
    // Evaluate RR when empty; otherwise the frame is omitted for this reason.
    std::optional<SkipReason> skip{};

    // What the evaluation point itself says, whatever RR, the bridge or the feature say: empty when it
    // is identified, otherwise the first of its reasons. Shown also when RR cannot be evaluated.
    std::optional<SkipReason> identification{};

    ResetCause reset{ResetCause::None};
    FeatureAction action{FeatureAction::None};

    // T's size: the render size, of the evaluation and of the feature to create.
    Extent render_size{};

    // From the last camera update completed, whenever there is one and T is identified, also in omitted
    // frames.
    std::optional<CameraConstants> camera{};

    // In a frame without velocity only: whether it is evaluated, with the camera's reprojection in every
    // pixel, or the condition that keeps it omitted.
    std::optional<NoVelocityOutcome> no_velocity{};
};

// A frame once it is closed.
struct FrameSummary {
    std::optional<SkipReason> skip{};
    ResetCause reset{ResetCause::None};

    // As in Decision. A frame without an evaluation point has two identifications that do not agree.
    std::optional<SkipReason> identification{};

    // In the first frame evaluated after a run of omitted frames: why the run did not keep RR's history,
    // or nothing if it kept it. Nothing in any other frame.
    std::optional<HistoryBreak> history_break{};

    // As in Decision, once the Mod evaluated RR or not.
    std::optional<NoVelocityOutcome> no_velocity{};

    // In an evaluated frame whose G-buffer pass is not later than that of the last evaluation point, whether
    // that point was evaluated or not: the pass, so its guides are an earlier frame's. Never at the first
    // point of the session. A diagnostic, which changes no decision.
    std::optional<std::uint64_t> reused_gbuffer_pass{};
};

// The interface's counters of closed frames.
struct FrameCounters {
    std::uint64_t frames{};
    std::uint64_t evaluated{};
    std::array<std::uint64_t, SKIP_REASON_COUNT> omitted{}; // by reason
    std::array<std::uint64_t, RESET_CAUSE_COUNT> resets{};  // by cause; None counts the frames without one
    std::uint64_t identified{};
    std::array<std::uint64_t, SKIP_REASON_COUNT> not_identified{}; // by the reason the identification gave
    std::array<std::uint64_t, NO_VELOCITY_OUTCOME_COUNT> no_velocity{}; // the frames without velocity, by outcome
    std::uint64_t reused_gbuffer_pass{};                                // the frames evaluated reusing a G-buffer pass

    void add(const FrameSummary& frame);
};

// A run of omitted frames, with its frames by reason, so that the log of the validation pass tells the
// menus and the transitions from normal play, where no frame may be omitted.
struct OmittedRun {
    std::uint64_t first{}; // the frame it started at
    std::uint64_t frames{};
    std::array<std::uint64_t, SKIP_REASON_COUNT> omitted{}; // by reason

    // Once it ended: why it did not keep RR's history, or nothing if it kept it.
    std::optional<HistoryBreak> history_break{};
};

// Follows the runs of omitted frames through the closed frames, in their order. A run ends at its first
// frame evaluated, which says whether it kept RR's history.
class OmittedRuns {
public:
    struct Change {
        bool started{};                    // the frame starts a run
        std::optional<OmittedRun> ended{}; // the run that the frame, evaluated, ends
    };

    // A closed frame, with the number the Mod gives it.
    Change add(std::uint64_t frame, const FrameSummary& summary);

    // The run in progress, if any.
    const std::optional<OmittedRun>& current() const { return m_current; }

    // The runs started.
    std::uint64_t count() const { return m_count; }

    // The runs ended that kept RR's history, and the others by the condition that broke them.
    std::uint64_t kept() const { return m_kept; }
    const std::array<std::uint64_t, HISTORY_BREAK_COUNT>& broken_by() const { return m_broken_by; }

private:
    std::optional<OmittedRun> m_current{};
    std::uint64_t m_count{};
    std::uint64_t m_kept{};
    std::array<std::uint64_t, HISTORY_BREAK_COUNT> m_broken_by{};
};

// A run of frames evaluated reusing the same G-buffer pass (FrameSummary::reused_gbuffer_pass), for the log:
// whether the noise of a transition falls in the frames without a pass of their own.
struct ReusedPassRun {
    std::uint64_t first{}; // its first frame
    std::uint64_t last{};  // its last frame
    std::uint64_t pass{};  // the pass they reused
};

// Follows those runs through the closed frames, in their order. A run ends at the first frame that does not
// reuse its pass.
class ReusedPassRuns {
public:
    // A closed frame, with the number the Mod gives it. Returns the run it ends, if any.
    std::optional<ReusedPassRun> add(std::uint64_t frame, const FrameSummary& summary);

    // The run in progress, if any.
    const std::optional<ReusedPassRun>& current() const { return m_current; }

private:
    std::optional<ReusedPassRun> m_current{};
};

// The per-frame policy: the state machine that decides, in every frame, whether RR is evaluated or the
// frame is omitted, when the history is reset and what to do with the feature. The Mod observes, calls it
// and does what it decides.
//
// Single-threaded: the Mod serializes every call with one mutex. The entries of a frame come in this
// order: the camera update, at most one evaluation point, then the end of the frame at Present. The
// engine may overlap the camera update of a frame with the evaluation point of the one before.
class FramePolicy {
public:
    explicit FramePolicy(const Profile& profile, float jitter_amplitude = DEFAULT_JITTER_AMPLITUDE);

    void on_toggle(bool enabled);

    // Whether the Mod writes RR's own jitter into the game. While it does not, the camera updates never
    // ask for it and the constants keep the game's jitter. On by default; the Mod turns it off for an A/B
    // against the game's jitter.
    void on_jitter_injection(bool enabled);

    // Returns Release if the bridge is no longer usable and there is a feature to free.
    FeatureAction on_bridge_state(BridgeState state);

    // Returns Release if there is a feature to free right away, before the device goes.
    FeatureAction on_device_reset();

    CameraDecision on_camera_update(const CameraUpdate& update);

    Decision on_evaluation_point(const EvaluationPoint& point);

    // The Mod did not evaluate RR where the frame's evaluation point decided it, for reason: the frame is
    // omitted, and its run does not keep RR's history, since the reason is not the game's. Nothing if the
    // frame has no evaluation point or was already omitted. With DebugView or AccumulatedReference, the
    // Mod wrote O in place of the game's TAA, so the own jitter goes on.
    void on_not_evaluated(SkipReason reason);

    // Closes the frame. A frame without an evaluation point is omitted.
    FrameSummary on_end_frame();

private:
    std::optional<SkipReason> global_skip() const;
    std::optional<SkipReason> point_skip(const EvaluationPoint& point) const;
    std::optional<SkipReason> feature_skip(const EvaluationPoint& point, FeatureAction& action);
    void break_run(HistoryBreak cause);
    std::optional<HistoryBreak> run_break(const Decision& decision) const;

    // What every evaluated point does to the run before it and to the reset. Returns the condition that broke
    // the run, if any.
    std::optional<HistoryBreak> take_evaluated(Decision& decision);

    // A frame without velocity, against the last evaluation point, before this one replaces it.
    NoVelocityOutcome no_velocity_outcome(const EvaluationPoint& point, const Decision& decision) const;
    Decision decide_without_velocity(Decision decision);

    // The last camera update completed, with the matrices the game has after it.
    struct Camera {
        std::uint64_t update_number{};
        Matrix4 projection{}; // Scene's
        Matrix4 jitter_disable_projection{};
        Matrix4 inverse_view_projection{};
        Matrix4 old_view_projection{};
    };

    const Profile& m_profile;
    float m_jitter_amplitude{};
    bool m_jitter_injection{true};
    bool m_enabled{};
    BridgeState m_bridge{BridgeState::Unavailable};

    // InitNgx was asked for since the bridge last reported Loaded.
    bool m_ngx_init_requested{};

    // A feature was asked for and has not been evaluated yet.
    bool m_new_feature{};

    // The Mod has a feature, or is creating one: a device reset has to release it.
    bool m_feature_alive{};

    // The frame in progress, once it has had its evaluation point.
    std::optional<FrameSummary> m_frame{};
    bool m_last_frame_evaluated{};

    std::uint64_t m_camera_updates{};
    std::optional<Camera> m_camera{};

    CutDetector m_cut_detector;

    // A cut was detected and no evaluation point has come since. Kept apart from m_camera, which a
    // later update may replace before that point.
    bool m_cut_pending{};

    // Whether the game's TAA was replaced, by RR, the debug view or the reference, at the last evaluation
    // point completed; false if the last frame closed without one: the jitter rule. Unlike
    // m_last_frame_evaluated, it does not wait for Present.
    bool m_taa_replaced{};

    // T's size at the last evaluated point: the render size the own jitter is converted with.
    Extent m_evaluated_size{};

    // The run of omitted frames since the last evaluated point: whether the first frame evaluated after it
    // keeps RR's history. It starts again at each evaluated point, not when its frame closes, so
    // that it takes the camera updates that come after that point.
    //
    // The update of a frame comes before its evaluation point in 20,495 of 20,496 updates measured in RE2. If
    // update N comes before the point of N-1, a cut or a change of layer in it goes to that point. For the
    // cut, m_last_evaluated_cut; a change of layer is left to the cut detector, which evaluates the update
    // of the run's first frame evaluated when the overlap goes on.
    struct Run {
        std::uint64_t frames{}; // omitted, closed
        std::optional<HistoryBreak> history_break{};
    };

    Run m_run{};

    // The last evaluated point took a cut: the run right after it breaks too. With update N before the
    // point of N-1, that point takes the cut of N, a frame of the run, and resets with the image of the old
    // shot. Without the overlap, it costs a second reset a few frames after the first.
    bool m_last_evaluated_cut{};

    // At the last evaluation point, evaluated or not: its G-buffer pass, and the number of the camera
    // update it had. A frame without velocity needs both newer. The pass is empty before the first point,
    // which reuses none.
    std::optional<std::uint64_t> m_last_point_pass{};
    std::uint64_t m_last_point_update{};
};

std::string_view to_string(SkipReason reason);
std::string_view to_string(ResetCause cause);
std::string_view to_string(HistoryBreak condition);
std::string_view to_string(NoVelocityOutcome outcome);
std::string_view to_string(FeatureAction action);
} // namespace rr::core
