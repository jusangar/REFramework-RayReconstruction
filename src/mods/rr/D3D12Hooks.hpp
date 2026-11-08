#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <d3d12.h>

#include "PsoRegistry.hpp"
#include "core/AmbientBrdf.hpp"
#include "core/FramePolicy.hpp"
#include "core/Identification.hpp"
#include "core/Profile.hpp"
#include "core/ResourceStates.hpp"

// The hooks of D3D12 the RayReconstruction Mod sees the game's renderer through: what shaders each PSO
// carries, and what each command list records, with its shadow state. They are installed once per
// process and never removed.
namespace rr::d3d12 {
// From the load notification of d3d12.dll, and right after the notification is registered in case it
// was already mapped: hooks D3D12CreateDevice before the game can call it, so that the PSO creation
// hooks go in before the first PSO. Only the first call does anything.
void install(HMODULE d3d12, const core::Profile& profile);

// The registry of the PSOs, once install has run.
PsoRegistry* pso_registry();

// While one is alive on a thread, the command list hooks pass every call from that thread straight to
// the runtime: RR's own work recorded on the game's lists never goes through their logic. The hooks
// also raise it around every call to the runtime, so that the debug layer's calls on the lists it
// wraps are not seen twice, with the wrapped objects.
class ReentryGuard {
public:
    ReentryGuard()
        : m_previous{s_active} {
        s_active = true;
    }
    ~ReentryGuard() { s_active = m_previous; }

    ReentryGuard(const ReentryGuard&) = delete;
    ReentryGuard& operator=(const ReentryGuard&) = delete;

    static bool active() { return s_active; }

private:
    static inline thread_local bool s_active{};
    bool m_previous{};
};

// Closes a frame for the per-frame counters, and gives the number of the one that starts. From Present.
std::uint64_t end_frame();

// The descriptor a Dispatch read the AmbientBRDF LUT through, and the heap it is in. Both are the game's,
// valid while it records the list.
struct AmbientBrdfDescriptor {
    core::LutDescriptor lut{.location = core::LutLocation::NoPtDispatch};
    ID3D12DescriptorHeap* heap{}; // the CBV_SRV_UAV heap bound at the Dispatch
};

// A draw the barrier rule anchored: the candidate for the evaluation point of its frame. Also the TAA
// draw of a frame without velocity, where the rule found no velocity and point.has_velocity is false, and
// RR takes every MV from the camera's reprojection. The resources in it are only identities: the game may
// free them after the draw, and their descriptions are copied as values.
struct AnchoredDraw {
    // Everything but the feature, which is the Mod's.
    core::EvaluationPoint point{};

    std::uint64_t frame{};
    DWORD thread{};
    void* list{};
    PsoShaders shaders{};
    PsRole ps_role{};

    core::Anchor anchor{};

    // RT0 of the draw, whether or not it is O. Described only if a barrier of the list names it.
    core::OutputCandidate rt0{};
    core::OutputProblem o_problem{};

    // Whether the state of what RR reads is known, and why not. point.states_known says the same.
    core::StateAssessment states{};

    // The game has descriptor heaps bound on the list. Without them, the hooks could not bind the game's
    // again after RR's own work, and RR's would stay bound for the rest of the list.
    bool heaps_bound{};

    // From the Dispatch of the PT sequence the barrier rule chose, whatever its compute shader: each
    // setting of the PT is another one.
    AmbientBrdfDescriptor ambient_brdf{};
};

// What the handler did at the anchored draw.
struct AnchorResult {
    // It recorded its own work on the list, under a ReentryGuard. The hooks then bind again everything
    // the game had bound: heaps, root signatures, every root argument, PSO, render targets, viewports
    // and scissors.
    bool recorded{};

    // RR's work wrote O in place of the TAA, so the draw does not reach the runtime. Not a frame omitted,
    // which leaves the TAA in its place.
    bool replaces_draw{};

    // The handler began timing the draw, which reaches the runtime, for RR's net cost: the hooks call the
    // TimedDrawHandler right after it.
    bool times_draw{};
};

// Called at every anchored draw, on the thread that records the list and with no lock of the hooks
// held.
using AnchorHandler = AnchorResult (*)(const AnchoredDraw& draw);
void set_anchor_handler(AnchorHandler handler);

// Called right after an anchored draw whose handler began timing it, once the draw reached the runtime: on
// the same thread, with the same list and with no lock of the hooks held.
using TimedDrawHandler = void (*)(void* list);
void set_timed_draw_handler(TimedDrawHandler handler);

// What the last draw with the profile's TAA vertex shader had bound. It prefers a draw whose pixel
// shader the profile admits. Only a diagnostic: the evaluation point is the draw the barrier rule
// anchors, and in the menus the draws with that vertex shader include some that are not the TAA.
struct TaaDraw {
    std::uint64_t frame{};
    DWORD thread{};
    void* list{};
    D3D12_COMMAND_LIST_TYPE list_type{};
    bool indexed{};
    std::uint32_t count{};
    std::uint32_t instances{};
    ID3D12PipelineState* pso{};
    PsoShaders shaders{};
    PsRole ps_role{};
    std::array<ID3D12DescriptorHeap*, 2> heaps{};
    std::uint32_t heap_count{};
    ID3D12RootSignature* root_signature{};
    std::uint64_t root_arguments{}; // the mask of the graphics root arguments set
    std::uint32_t rtv_count{};
    ID3D12Resource* rt0{};
};

// What the command list hooks count, in the order the interface shows it.
enum class Counter : std::uint8_t {
    Draws,
    Dispatches,
    Barriers,
    Resets,
    TaaVsDraws,
    TaaVsAdmittedDraws,
    TaaVsUnanchoredDraws,     // with the TAA's vertex shader, not anchored by the barrier rule
    Anchors,                  // draws the barrier rule anchored, in any list
    AnchorsWithoutVelocity,   // attempts with T ready and no velocity in SHADER_RESOURCE
    AnchorsWithTwoVelocities, // attempts with two or more
    AnchorsAfterBundle,       // draws the Mod would have got, left to the game: a bundle ran on their list
    OwnWork,                  // anchored draws where RR recorded its own work, and the list state was restored
    ReplacedDraws,            // anchored draws that RR's work replaced, which did not reach the runtime
    TimedDraws,               // anchored draws that reached the runtime between two timestamps of the Mod
    UnknownPsoDraws,          // the PSO bound is not in the registry
    NoVsDigestDraws,          // the PSO is, but its vertex shader has no HASH part, as FXC's
    UnknownVsFullscreenDraws, // either of the two in a DrawInstanced(3, 1), the shape of the TAA's draw
    UnknownPsoDispatches,
    Bundles,
    Indirect,
    EnhancedBarriers,  // they bypass the barriers of the shadow state
    RenderPasses,      // BeginRenderPass binds targets the shadow state does not see
    StateObjects,      // SetPipelineState1, which the shadow state does not see either
    NullTablesDropped, // debug layer only: REFramework's ImGui, see ListHooks.cpp
    OriginalFallbacks, // detours that did not find the vtable they were called through
    Lists,
    ListVtables,
    GBufferUnnamed,  // passes with the G-buffer's shape and a resource no barrier of the list names
    GBufferRejected, // passes with the G-buffer's shape and another format or size
    Submissions,     // ExecuteCommandLists, on any queue
    SubmittedLists,
    UnseenLists, // submitted lists whose recording the hooks did not see
    Count,
};

const char* to_string(Counter counter);

using Counters = std::array<std::uint64_t, static_cast<std::size_t>(Counter::Count)>;

// What became of the bytecode of a PT compute shader at the first draw the barrier rule anchored after its sequence.
enum class PtCsDump : std::uint8_t {
    Written,      // into reframework/rr/pt_cs_<digest>.dxil
    AlreadyThere, // the file was there, from an earlier session
    NotRetained,  // no copy was kept as its PSO was created
    WriteFailed,
};

const char* to_string(PtCsDump dump);

struct PtCsVariant {
    core::ShaderDigest digest{};
    PtCsDump dump{};
};

// The copies of the compute shaders the hooks keep until the barrier rule says which one is the PT's,
// and the variants it anchored.
struct PtCsStatus {
    std::uint64_t retained{};
    std::uint64_t retained_bytes{};
    std::uint64_t over_limit{};             // copies not kept, past the hooks' limit
    std::uint64_t without_root_signature{}; // compute PSOs whose root signature the hooks did not read
    std::uint64_t anchors_without_digest{}; // anchored draws whose PT's PSO had no digest
    std::vector<PtCsVariant> variants{};    // in the order of their first anchor
    std::optional<core::ShaderDigest> last{};
};

struct Status {
    std::string create_device{}; // the hook on D3D12CreateDevice
    std::string device{};        // the hooks on the device and its pipeline libraries
    std::string list_vtables{};  // the command list vtables patched, with their module
    std::string debug_layer{};

    std::uint64_t frames{};
    Counters total{};
    Counters last_frame{};
    PsoRegistry::Counts psos{};
    std::uint64_t views{}; // render target and depth-stencil views known

    std::uint64_t root_signatures{}; // created and read
    std::uint64_t root_signature_failures{};

    PtCsStatus pt_cs{};

    std::uint64_t debug_messages{};
    std::uint64_t debug_errors{};              // CORRUPTION and ERROR
    std::uint64_t debug_messages_naming_mod{}; // naming an object of the Mod

    std::optional<TaaDraw> taa_draw{};

    // The registry of the resource states, and the last G-buffer pass identified.
    std::string queues{}; // the command queues whose ExecuteCommandLists is hooked
    core::StateCounters states{};
    std::optional<core::GBuffer> gbuffer{};
    std::optional<core::TaaConfirmation> last_confirmation{};
};

Status status();

// The compute shader of the PT's Dispatch at the last anchored draw since the last call, watched or not: the barrier
// rule tells it by what it does, so this names the variants nobody asked to watch. None without an anchored draw.
std::optional<core::ShaderDigest> take_anchored_pt_cs();

// The counters of the frame that the last end_frame closed, for the log of the omitted frames.
Counters last_frame_counters();

// Starts the counters of the resource states again; what the registry learned stays.
void reset_state_counters();

// For the interface and the log: each read with its state in hexadecimal, and the G-buffer's resources.
std::string to_string(const core::ReadStates& states);
std::string to_string(const core::GBuffer& gbuffer);
} // namespace rr::d3d12
