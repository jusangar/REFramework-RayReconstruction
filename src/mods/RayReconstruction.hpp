#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include "Mod.hpp"
#include "mods/vr/d3d12/CommandContext.hpp"
#include "rr/D3D12Hooks.hpp"
#include "rr/GuidePass.hpp"
#include "rr/core/BridgeLoader.hpp"
#include "rr/core/BridgeSession.hpp"
#include "rr/core/FramePolicy.hpp"
#include "rr/core/Measurements.hpp"
#include "rr/core/Reference.hpp"
#include "rr/core/Settings.hpp"

// DLSS Ray Reconstruction in place of the game's TAA. Everything that needs the NGX SDK lives in the
// NGX bridge, which this Mod loads from reframework/rr/; without it, REFramework works the same and RR
// is unavailable.
class RayReconstruction : public Mod {
public:
    static std::shared_ptr<RayReconstruction>& get();

    // Whether the game has an RR profile. The Mod is registered only then.
    static bool is_supported();

    // From the load notification of d3d12.dll in REFramework.cpp, and right after registering it in case
    // d3d12.dll was already mapped. In a game with a profile, hooks D3D12CreateDevice before the game can
    // call it. Long before the Mod exists.
    static void on_d3d12_loaded(HMODULE d3d12);

public:
    std::string_view get_name() const override { return "RayReconstruction"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override;
    void on_present() override;
    void on_device_reset() override;
    void on_draw_ui() override;
    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;

    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;

private:
    // Frames with this many anchored draws or more share a counter.
    static constexpr std::uint32_t MANY_ANCHORS = 2;

    // The camera input. The main scene layer is the first fully rendered one with the main camera;
    // another takes its place only once the main one has not updated for this many frames, as in a probe
    // that measured the signals of a camera cut in RE2.
    static constexpr std::uint64_t MAIN_LAYER_SILENCE = 120;

    struct Camera {
        void* main_layer{};
        std::uint64_t main_layer_frame{}; // the Present count at its last update
        bool updated{};                   // the main layer updated since the last Present
        std::uint64_t presents{};
    };

    // What the interface shows of the camera input.
    struct CameraView {
        std::uint64_t updates{};
        std::uint64_t frames_without_update{};
        std::uint64_t main_layer_changes{};
        std::uint64_t other_layer_updates{}; // fully rendered layers with the main camera that are not the main one
        std::uint64_t last_update{};
    };

    // What the interface shows of RR's own jitter.
    struct JitterView {
        bool last_injected{};     // at the last camera update of the main layer
        std::uint64_t injected{}; // camera updates whose matrices the Mod rewrote

        // By SceneVariant, for the profile's variants: written, missing, and written with their copy of
        // view_projection at float 160.
        std::array<std::uint64_t, rr::core::SCENE_VARIANT_COUNT> written{};
        std::array<std::uint64_t, rr::core::SCENE_VARIANT_COUNT> missing{};
        std::array<std::uint64_t, rr::core::SCENE_VARIANT_COUNT> copies{};

        // InJitterOffset as it went to the bridge at the last evaluation recorded.
        std::optional<rr::core::Float2> offset_sent{};

        // The order the jitter rule depends on: whether the camera update of the main layer comes before
        // or after the frame's evaluation point, within its Present interval.
        std::uint64_t before_point{};
        std::uint64_t after_point{};

        // The variants without jitter against Scene's inverses as the game wrote them, at every camera
        // update of the main layer, and the candidate correction where it applied.
        std::uint64_t compared{};
        std::uint64_t jitter_disable_same{};
        std::uint64_t post_same{};
        std::uint64_t post_jittered{}; // JitterDisablePost's projection[2][0..1] is not JitterDisable's
        std::uint64_t post_missing{};
        std::uint64_t jitter_disable_followed{};
        std::uint64_t post_followed{};
    };

    // What the interface shows of the cut detector, for the validation pass: its statistics, and its last
    // cut, camera push and near miss with the frame where they came.
    struct CutView {
        rr::core::CutStatistics stats{};
        std::optional<std::pair<std::uint64_t, rr::core::CutCheck>> last_cut{};
        std::optional<std::pair<std::uint64_t, rr::core::CutCheck>> last_push{};
        std::optional<std::pair<std::uint64_t, rr::core::CutCheck>> last_near_miss{};
    };

    // What the interface shows of the measurement interface: the GPU timers by segment, and the count of
    // non-finite values in O. And the intervals between Presents by the mode of their frame.
    struct MeasurementView {
        std::array<rr::core::GpuTimer, rr::d3d12::SEGMENT_COUNT> timers{};
        rr::core::NonFiniteCounter nonfinite{};
        rr::core::FrameIntervals intervals{};
        std::uint64_t frequency{}; // of the timestamps, from REFramework's command queue
        std::uint64_t taken{};     // measurements read back since the start

        // The run of frames with non-finite pixels in progress, for the log.
        std::optional<std::uint64_t> run_start{};
        std::uint64_t run_frames{};
    };

    // What the interface shows of the accumulated reference.
    struct ReferenceView {
        std::string status{"off"}; // at the last evaluation point
        std::uint32_t frames{};    // in the mean
        std::optional<rr::core::ReferenceRestart> last_restart{};
        std::uint64_t last_restart_frame{};
        std::array<std::uint64_t, rr::core::REFERENCE_RESTART_COUNT> restarts{};
    };

    // What the interface shows of the closed frames and of the evaluation points.
    struct Frames {
        rr::core::FrameCounters counters{};
        std::array<std::uint64_t, MANY_ANCHORS + 1> anchors_per_frame{}; // by the number of anchored draws
        std::uint64_t o_rt_uav{};                                        // identified frames whose O has the RT and UAV flags
        std::vector<std::pair<rr::core::ShaderDigest, std::uint64_t>> identified_by_ps{};
        std::optional<rr::core::FrameSummary> last{};

        // The draw of the last evaluation point, and what the policy decided there.
        std::optional<rr::d3d12::AnchoredDraw> last_draw{};
        rr::core::Decision decision{};

        // The last digests seen at an evaluation point that the profile does not have.
        std::optional<rr::core::ShaderDigest> unknown_ps{};

        CameraView camera{};
        JitterView jitter{};
        CutView cuts{};

        // The runs of omitted frames, with those that kept RR's history, and the last one that ended, with
        // the frame that ended it.
        rr::core::OmittedRuns runs{};
        std::optional<rr::core::OmittedRun> last_run{};
        std::uint64_t last_run_end{};

        // The runs of frames evaluated without a G-buffer pass of their own, for the log. "Reset counters"
        // leaves the run in progress, so that the log has it whole.
        rr::core::ReusedPassRuns reused_passes{};

        // Where the counters start: at the last "Reset counters", or when the Mod started. The window of the
        // objective gates of the validation pass, which last 20 minutes.
        std::uint64_t counters_frame{};
        std::chrono::steady_clock::time_point counters_time{std::chrono::steady_clock::now()};

        // The device's removed reason, once it is not S_OK: an objective gate wants none.
        std::optional<HRESULT> device_removed{};

        // Whether the debug view wrote O at the last evaluation point, and why not.
        std::string debug_view{"off"};

        // Whether RR was evaluated at the last evaluation point, and why not.
        std::string evaluation{"no evaluation point yet"};

        // The last check of the AmbientBRDF LUT's axis.
        std::string lut_check{"not run yet"};

        // The measurement interface, and the game's settings the profile checks, as on_frame read them.
        MeasurementView measures{};
        ReferenceView reference{};
        rr::core::GameSettings settings{};
        std::vector<rr::core::SettingWarning> setting_warnings{};
    };

    // The debug view: the guide it writes into O, DEBUG_VIEW_* of Guides.hlsli, or none. And whether the
    // guide pass redoes the saturated velocity, and whether it takes the specular albedo from the game's LUT.
    struct DebugView {
        std::optional<int> guide{};
        float scale{1.0f};
        bool round_trip{};
        bool reproject_saturated{true};
        bool use_lut{};
    };

    // The options of the own jitter.
    struct JitterOptions {
        bool inject{true};

        // Which variants without jitter take the injected inverses.
        bool follow_jitter_disable{};
        bool follow_post{};

        rr::core::Float2 offset_sign{1.0f, 1.0f}; // what InJitterOffset is multiplied by on its way to the bridge
    };

    // What the debug view records at an evaluation point.
    struct DebugWork {
        rr::d3d12::GuideInputs inputs{};
        ID3D12Resource* o{};
        int guide{};
        float scale{};
    };

    // The hooks' handlers: on the thread that records the list, at every anchored draw, and right after
    // one whose TAA draw the Mod times.
    static rr::d3d12::AnchorResult on_anchored_draw(const rr::d3d12::AnchoredDraw& draw);
    static void on_timed_draw(void* list);
    rr::d3d12::AnchorResult on_evaluation_point(const rr::d3d12::AnchoredDraw& draw);

    // Under m_policy_mutex: what the frame's evaluation point decides. False for the later anchored
    // draws of a frame.
    bool decide(const rr::d3d12::AnchoredDraw& draw);

    // Under m_policy_mutex: what writes O in place of the game's TAA at the frame's evaluation point, if
    // anything.
    rr::d3d12::AnchorResult replace_taa(const rr::d3d12::AnchoredDraw& draw);

    // Under m_policy_mutex.
    std::optional<DebugWork> debug_work(const rr::d3d12::AnchoredDraw& draw);
    rr::d3d12::AnchorResult show_debug_view(const DebugWork& work);
    rr::d3d12::AnchorResult accumulate_reference(const rr::d3d12::AnchoredDraw& draw);
    rr::d3d12::AnchorResult evaluate(const rr::d3d12::AnchoredDraw& draw);
    std::optional<rr::d3d12::GuideInputs> guide_inputs(const rr::d3d12::AnchoredDraw& draw, std::string& why) const;
    void read_debug_options();
    void read_jitter_options();
    void read_measurement_options();

    // From the interface or the switch key, once m_enabled changed: the policy hears it, the log gets it
    // with the frame, so that the clips and captures of the validation protocol match it, and the config
    // is saved.
    void on_enabled_changed(std::string_view from);

    // Under m_policy_mutex, right after the camera update of the main layer.
    void write_jitter(sdk::renderer::layer::Scene* layer, const rr::core::CameraUpdate& update, const rr::core::CameraDecision& decision);

    // What an anchored draw has that the profile does not.
    struct Differences {
        bool unknown_ps{}; // with the TAA's vertex shader, a pixel shader neither admitted nor excluded
        bool t_format{};
    };

    void log_digests(const rr::d3d12::AnchoredDraw& draw, const Differences& differences, std::uint32_t discovery_session);
    void log_sizes(const rr::d3d12::AnchoredDraw& draw);
    void log_states(const rr::d3d12::AnchoredDraw& draw);

    // From Present, outside m_policy_mutex: judges the copy of the LUT once the GPU gave it back.
    void check_lut_axis(std::uint64_t frame);

    // From Present, outside m_policy_mutex: what the recordings measured, once the GPU gave it back, to
    // the timers and the count.
    void take_measurements(std::uint64_t frame);

    // REFramework's Graphics settings and the game's anti-aliasing, as the profile checks them. A member,
    // so that it reads Graphics' options as its friend.
    static rr::core::GameSettings read_game_settings();

    // What the policy asks of NGX at an evaluation point waits for Present, where REFramework already
    // executes its own command lists on the same queue, out of the game's recording.
    void run_bridge_work();
    void start_ngx();
    void create_feature();
    void discard_creation();
    bool creation_executed() const;
    void poll_creation();
    void release_feature(std::string_view why);
    void wait_for_evaluations();
    void tell_policy_bridge_state();

    // What the interface shows of the bridge and of the feature.
    struct BridgeView {
        rr::core::Availability availability{};
        std::string detail{};
        std::optional<rr::core::LoadedSnippet> snippet{};
        rr::core::FeatureState feature{};
        std::uint64_t created{};
        std::uint64_t released{};
        std::uint64_t evaluations{};
    };

    void draw_bridge_ui();
    void draw_frame_ui();
    void draw_cuts_ui();
    void draw_guides_ui();
    void draw_jitter_ui();
    void draw_states_ui();
    void draw_measurement_ui();
    void draw_hooks_ui();

    const ModToggle::Ptr m_enabled{ModToggle::create(generate_name("Enabled"), false)};

    // Switches RR on and off without REFramework's menu, which would get into the captures and the clips of
    // the A/B against the game's TAA. Unbound by default.
    const ModKey::Ptr m_switch_key{ModKey::create(generate_name("SwitchKey"))};

    // Logs the digests of every PSO and of the draws with the TAA's vertex shader, to update the
    // profile after a patch of the game.
    const ModToggle::Ptr m_digest_discovery{ModToggle::create(generate_name("DigestDiscovery"), false)};

    // The debug view: off, or a guide in the order of DEBUG_VIEW_* of Guides.hlsli. Not saved, like the
    // accumulated reference: it replaces the game's TAA with RR off too, and a validation mode that takes
    // RR's place never outlives the session. The scale stays in the config: without the view it does nothing.
    const ModCombo::Ptr m_debug_view{ModCombo::create(
        generate_name("DebugView"), {"Off", "Normals", "Roughness", "Diffuse albedo", "Specular albedo", "Depth", "Motion vectors"})};
    const ModFloat::Ptr m_debug_scale{ModFloat::create(generate_name("DebugViewScale"), 1.0f)};

    // The reads of the game's resources declare their transition to NON_PIXEL_SHADER_RESOURCE and back,
    // so that the debug layer checks the states the registry deduced.
    const ModToggle::Ptr m_barrier_round_trip{ModToggle::create(generate_name("BarrierRoundTrip"), false)};

    // The guide pass redoes the saturated velocity by reprojection. Off, the MVs keep the game's saturated
    // velocity: the comparison of the validation protocol.
    const ModToggle::Ptr m_reproject_saturated{ModToggle::create(generate_name("ReprojectSaturatedVelocity"), true)};

    // Where the specular albedo takes the AmbientBRDF LUT from: the profile's choice, or either one, to
    // compare them. In the order of SPECULAR_SOURCES.
    const ModCombo::Ptr m_specular_source{ModCombo::create(
        generate_name("SpecularAlbedo"), {"As the profile says", "The game's AmbientBRDF LUT", "The GGX fit"})};

    // RR's own jitter. Off, the game keeps its jitter while RR is evaluated: the A/B of the own jitter
    // against the game's.
    const ModToggle::Ptr m_own_jitter{ModToggle::create(generate_name("OwnJitter"), true)};

    // The candidate correction of the inverses: off until the interface's comparison of the variants
    // without jitter against Scene's inverses decides it, or for JitterDisable alone or with
    // JitterDisablePost, so that the A/B tells them apart.
    const ModCombo::Ptr m_follow_inverses{
        ModCombo::create(generate_name("FollowSceneInverses"), {"Off", "JitterDisable", "JitterDisable and JitterDisablePost"})};

    // The sign of InJitterOffset on its way to the bridge, for the check against the MVs with the overlay
    // of NGX's development snippet. In the order of JITTER_OFFSET_SIGNS. Not saved, so that a position
    // under test never outlives the session: the sign found goes into the policy.
    const ModCombo::Ptr m_jitter_offset_sign{
        ModCombo::create(generate_name("JitterOffsetSign"), {"As derived", "X inverted", "Y inverted", "Both inverted"})};

    // The accumulated reference: with the camera still, the mean of T goes into O in place of the TAA and
    // of RR. The debug view wins over it. Not saved, like the sign of InJitterOffset: a validation mode
    // that takes RR's place never outlives the session.
    const ModToggle::Ptr m_accumulated_reference{ModToggle::create(generate_name("AccumulatedReference"), false)};

    // The debug test of the count of non-finite values in O: pixel (0, 0) counts as a NaN or as +Inf. The
    // value goes to the count alone, never into O, where the auto-exposure could keep it. Not saved. In the
    // order of FORCED_NONFINITE.
    const ModCombo::Ptr m_force_nonfinite{ModCombo::create(generate_name("ForceNonFinite"), {"Off", "NaN", "+Inf"})};

    ValueList m_options{
        *m_enabled,
        *m_switch_key,
        *m_digest_discovery,
        *m_debug_scale,
        *m_barrier_round_trip,
        *m_reproject_saturated,
        *m_specular_source,
        *m_own_jitter,
        *m_follow_inverses,
    };

    const rr::core::Profile* m_profile{};

    // Every call to the per-frame policy, from the engine's threads at the camera update, from the thread
    // that records the TAA's list and from Present, goes under m_policy_mutex, and so does what the
    // interface shows of it. So does every call to the bridge, and the debug view's options. RR's
    // evaluation is recorded under it as a whole: the bridge never runs together with a creation or a
    // release at Present, and what it reports goes to the frame that decided it. So is the debug view, so
    // that the policy hears how it went before the next camera update asks for the jitter, and so is the
    // accumulated reference, whose ReferenceAccumulator follows the camera updates too.
    std::mutex m_policy_mutex{};
    std::unique_ptr<rr::core::FramePolicy> m_policy{};
    std::unique_ptr<rr::core::BridgeSession> m_session{};

    // The policy asked to start NGX, at the next Present.
    bool m_start_ngx{};

    // The Mod's own command list, which creates the feature on the queue REFramework captures, with its
    // fence. Set up at the first creation.
    d3d12::CommandContext m_creation{};
    bool m_creation_set_up{};

    // The bridge recorded an evaluation since the last wait for the GPU, which a release of the feature
    // needs first. The fence and its event are created at the first wait, for the life of the process.
    bool m_evaluated_since_wait{};
    Microsoft::WRL::ComPtr<ID3D12Fence> m_evaluation_fence{};
    UINT64 m_evaluation_fence_value{};
    HANDLE m_evaluation_event{};

    // Why the feature has to be released at the next Present, after the game's list being recorded.
    std::optional<std::string> m_release_at_present{};

    // The anchored draws of the frame in progress. The first one is its evaluation point: the policy
    // takes at most one per frame.
    std::uint32_t m_frame_anchors{};

    // The number of the frame in progress, as the hooks give it at Present, for the log of the camera
    // updates. An update may overlap the Present of the frame before. Under m_policy_mutex.
    std::uint64_t m_hooks_frame{};

    // When the last Present came, for the intervals between them. Under m_policy_mutex.
    std::optional<std::chrono::steady_clock::time_point> m_last_present{};

    Camera m_camera{};
    DebugView m_debug{};
    JitterOptions m_jitter{};
    Frames m_frames{};

    // The accumulated reference: whether it is on, and how many frames its mean holds. And the bits the
    // count takes for pixel (0, 0), or 0.
    bool m_reference_on{};
    rr::core::ReferenceAccumulator m_reference{};
    std::uint32_t m_forced_nonfinite{};

    // On the thread that records the TAA's list, with its own lock.
    rr::d3d12::GuidePass m_guide_pass{};

    // What is already in the log: digests that differ from the profile, and the evaluation points of
    // the current digest discovery.
    std::vector<rr::core::ShaderDigest> m_logged_digests{};
    bool m_logged_t_format{};
    std::uint32_t m_discovery_session{};
    std::vector<std::pair<rr::core::ShaderDigest, rr::core::ShaderDigest>> m_discovered{};

    // The sizes of T and O the log gave last, so that it shows every step of a change of image quality.
    rr::core::Extent m_logged_t_size{};
    rr::core::Extent m_logged_o_size{};

    // The state problem the log gave last, so that it shows the frame where the identity changes and the
    // first one known again.
    std::optional<rr::core::StateProblem> m_logged_state_problem{};

    // The PT settings and the warnings of the game's settings the log gave last, on on_frame's thread, so
    // that it shows every change of the settings.
    std::optional<std::vector<std::string>> m_logged_settings{};
};
