#include <algorithm>
#include <atomic>
#include <bit>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <wrl/client.h>

#include <spdlog/spdlog.h>

#include <utility/PointerHook.hpp>

#include "ListHooks.hpp"
#include "ListState.hpp"
#include "PsoSubstitution.hpp"

using Microsoft::WRL::ComPtr;

namespace rr::d3d12 {
namespace {
using List = ID3D12GraphicsCommandList;

// Slots of the command list vtable, checked against the C vtables of d3d12.h in Windows SDK 10.0.26100.
namespace slot {
constexpr std::uint32_t RESET = 10;
constexpr std::uint32_t CLEAR_STATE = 11;
constexpr std::uint32_t DRAW_INSTANCED = 12;
constexpr std::uint32_t DRAW_INDEXED_INSTANCED = 13;
constexpr std::uint32_t DISPATCH = 14;
constexpr std::uint32_t RS_SET_VIEWPORTS = 21;
constexpr std::uint32_t RS_SET_SCISSOR_RECTS = 22;
constexpr std::uint32_t SET_PIPELINE_STATE = 25;
constexpr std::uint32_t RESOURCE_BARRIER = 26;
constexpr std::uint32_t EXECUTE_BUNDLE = 27;
constexpr std::uint32_t SET_DESCRIPTOR_HEAPS = 28;
constexpr std::uint32_t SET_COMPUTE_ROOT_SIGNATURE = 29;
constexpr std::uint32_t SET_GRAPHICS_ROOT_SIGNATURE = 30;
constexpr std::uint32_t SET_COMPUTE_ROOT_DESCRIPTOR_TABLE = 31;
constexpr std::uint32_t SET_GRAPHICS_ROOT_DESCRIPTOR_TABLE = 32;
constexpr std::uint32_t SET_COMPUTE_ROOT_32BIT_CONSTANT = 33;
constexpr std::uint32_t SET_GRAPHICS_ROOT_32BIT_CONSTANT = 34;
constexpr std::uint32_t SET_COMPUTE_ROOT_32BIT_CONSTANTS = 35;
constexpr std::uint32_t SET_GRAPHICS_ROOT_32BIT_CONSTANTS = 36;
constexpr std::uint32_t SET_COMPUTE_ROOT_CBV = 37;
constexpr std::uint32_t SET_GRAPHICS_ROOT_CBV = 38;
constexpr std::uint32_t SET_COMPUTE_ROOT_SRV = 39;
constexpr std::uint32_t SET_GRAPHICS_ROOT_SRV = 40;
constexpr std::uint32_t SET_COMPUTE_ROOT_UAV = 41;
constexpr std::uint32_t SET_GRAPHICS_ROOT_UAV = 42;
constexpr std::uint32_t OM_SET_RENDER_TARGETS = 46;
constexpr std::uint32_t EXECUTE_INDIRECT = 59;
constexpr std::uint32_t BEGIN_RENDER_PASS = 68;   // ID3D12GraphicsCommandList4
constexpr std::uint32_t SET_PIPELINE_STATE1 = 75; // ID3D12GraphicsCommandList4
constexpr std::uint32_t BARRIER = 80;             // ID3D12GraphicsCommandList7
constexpr std::uint32_t COUNT = 81;
} // namespace slot

// The core's copies of the D3D12 values the identification reads.
static_assert(core::d3d12_value::RENDER_TARGET == D3D12_RESOURCE_STATE_RENDER_TARGET);
static_assert(core::d3d12_value::UNORDERED_ACCESS == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
static_assert(core::d3d12_value::DEPTH_WRITE == D3D12_RESOURCE_STATE_DEPTH_WRITE);
static_assert(core::d3d12_value::DEPTH_READ == D3D12_RESOURCE_STATE_DEPTH_READ);
static_assert(core::d3d12_value::SHADER_RESOURCE == (static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
                                                        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)));
static_assert(core::d3d12_value::ALL_SUBRESOURCES == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
static_assert(core::d3d12_value::ALLOW_RENDER_TARGET == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
static_assert(core::d3d12_value::ALLOW_UNORDERED_ACCESS == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
static_assert(core::d3d12_value::DSV_READ_ONLY_DEPTH == D3D12_DSV_FLAG_READ_ONLY_DEPTH);

std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Counter::Count)> g_counters{};

void tally(Counter counter, std::uint64_t n = 1) {
    g_counters[static_cast<std::size_t>(counter)] += n;
}

Counters load_counters() {
    Counters counters{};

    for (std::size_t i = 0; i < counters.size(); ++i) {
        counters[i] = g_counters[i];
    }

    return counters;
}

/////////////////////////////////////////////////////////////////////////////
// Vtables patched in place
/////////////////////////////////////////////////////////////////////////////

// The D3D12 runtime gives every list of a class one vtable in D3D12Core.dll (in D3D12SDKLayers.dll
// with the debug layer), and per-instance hooks do not survive the list's Reset. So the slots of every
// list vtable seen are patched in place, and each detour finds its original through the vtable it
// was called through.
struct PatchedVtable {
    void** vtable{};
    std::array<void*, slot::COUNT> originals{};
    std::vector<std::unique_ptr<PointerHook>> hooks{};
};

constexpr std::uint32_t MAX_VTABLES = 16;
std::mutex g_vtables_mutex{};
std::array<std::atomic<PatchedVtable*>, MAX_VTABLES> g_vtables{};
std::atomic<std::uint32_t> g_vtable_count{};
std::string g_vtables_report{"none yet"}; // under g_vtables_mutex

void** vtable_of(void* object) {
    return *static_cast<void***>(object);
}

PatchedVtable* find_vtable(void** vtable) {
    const auto count = std::min<std::uint32_t>(g_vtable_count.load(), MAX_VTABLES);

    for (std::uint32_t i = 0; i < count; ++i) {
        if (const auto patched = g_vtables[i].load(); patched != nullptr && patched->vtable == vtable) {
            return patched;
        }
    }

    return nullptr;
}

void* original(void* self, std::uint32_t index) {
    auto patched = find_vtable(vtable_of(self));

    // Only reachable if an object left the vtable it was called through before the lookup.
    if (patched == nullptr) {
        tally(Counter::OriginalFallbacks);
        patched = g_vtables[0].load();
    }

    return patched->originals[index];
}

// Calls the runtime with the reentry guard up.
template <typename R, typename... Args> R call_original(std::uint32_t index, List* self, Args... args) {
    const auto fn = reinterpret_cast<R(STDMETHODCALLTYPE*)(List*, Args...)>(original(self, index));
    const ReentryGuard guard{};
    return fn(self, args...);
}

/////////////////////////////////////////////////////////////////////////////
// Shadow state
/////////////////////////////////////////////////////////////////////////////

struct ListEntry {
    ListState state{};

    // Debug layer only: the draw after a null descriptor table that was dropped.
    bool skip_next_draw{};

    void reset(ID3D12PipelineState* initial, std::optional<PsoShaders> initial_shaders) {
        state.reset(initial, initial_shaders);
        skip_next_draw = false;
    }
};

// Never erased: a list is recorded by one thread at a time, and its entry outlives it.
std::shared_mutex g_lists_mutex{};
std::unordered_map<void*, std::unique_ptr<ListEntry>> g_lists{};
thread_local void* t_last_list{};
thread_local ListEntry* t_last_entry{};

ListEntry& entry_of(List* list) {
    if (list == t_last_list) {
        return *t_last_entry;
    }

    ListEntry* entry{};

    {
        std::shared_lock _{g_lists_mutex};

        if (const auto it = g_lists.find(list); it != g_lists.end()) {
            entry = it->second.get();
        }
    }

    if (entry == nullptr) {
        std::unique_lock _{g_lists_mutex};
        auto& stored = g_lists[list];

        if (stored == nullptr) {
            stored = std::make_unique<ListEntry>();
            stored->state.type = list->GetType();
        }

        entry = stored.get();
    }

    t_last_list = list;
    t_last_entry = entry;
    return *entry;
}

// A list the hooks saw recorded, without starting an entry for one they did not.
ListEntry* find_entry(void* list) {
    std::shared_lock _{g_lists_mutex};
    const auto it = g_lists.find(list);
    return it != g_lists.end() ? it->second.get() : nullptr;
}

std::optional<PsoShaders> find_shaders(ID3D12PipelineState* pso) {
    const auto registry = pso_registry();
    return pso != nullptr && registry != nullptr ? registry->find(pso) : std::nullopt;
}

/////////////////////////////////////////////////////////////////////////////
// Draws and dispatches
/////////////////////////////////////////////////////////////////////////////

std::atomic<std::uint64_t> g_frame{};

std::mutex g_frame_mutex{};
Counters g_frame_start{}; // under g_frame_mutex
Counters g_last_frame{};  // under g_frame_mutex

std::mutex g_taa_mutex{};
std::optional<TaaDraw> g_taa_draw{};                     // under g_taa_mutex
std::unordered_set<ID3D12PipelineState*> g_discovered{}; // under g_taa_mutex
std::uint32_t g_discovered_session{};                    // under g_taa_mutex

void on_taa_vs_draw(List* self, const ListState& s, bool indexed, UINT count, UINT instances, const PsoRegistry& registry) {
    tally(Counter::TaaVsDraws);

    const auto role = ps_role(registry.profile(), s.shaders->ps);
    const auto admitted = role == PsRole::Admitted;

    if (admitted) {
        tally(Counter::TaaVsAdmittedDraws);
    }

    const TaaDraw draw{
        .frame = g_frame.load(),
        .thread = GetCurrentThreadId(),
        .list = self,
        .list_type = s.type,
        .indexed = indexed,
        .count = count,
        .instances = instances,
        .pso = s.pso,
        .shaders = *s.shaders,
        .ps_role = role,
        .heaps = s.heaps,
        .heap_count = s.heap_count,
        .root_signature = s.graphics.root_signature(),
        .root_arguments = s.graphics.set_mask(),
        .rtv_count = s.rtv_count,
        .rt0 = s.rtv_count > 0 ? s.render_targets[0] : nullptr,
    };

    const auto session = registry.discovery_session();
    std::scoped_lock _{g_taa_mutex};

    if (admitted || !g_taa_draw || g_taa_draw->ps_role != PsRole::Admitted) {
        g_taa_draw = draw;
    }

    if (session == 0) {
        return;
    }

    if (session != g_discovered_session) {
        g_discovered.clear();
        g_discovered_session = session;
    }

    // Once per PSO and discovery, so that the log names every variant without growing with every frame.
    if (g_discovered.insert(s.pso).second) {
        spdlog::info(
            "[RayReconstruction] Draw with the TAA vertex shader: PSO {:x}, PS {} ({}) | {}{} x {} | list {:x} type {} thread {} | "
            "heaps {:x} {:x} | root signature {:x}, root arguments {:x} | {} RTs, RT0 {:x}",
            reinterpret_cast<std::uintptr_t>(draw.pso), to_string(draw.shaders.ps), to_string(role),
            indexed ? "DrawIndexedInstanced " : "DrawInstanced ", count, instances, reinterpret_cast<std::uintptr_t>(self),
            static_cast<int>(draw.list_type), draw.thread, reinterpret_cast<std::uintptr_t>(draw.heaps[0]),
            reinterpret_cast<std::uintptr_t>(draw.heaps[1]), reinterpret_cast<std::uintptr_t>(draw.root_signature), draw.root_arguments,
            draw.rtv_count, reinterpret_cast<std::uintptr_t>(draw.rt0));
    }
}

/////////////////////////////////////////////////////////////////////////////
// Resource states
/////////////////////////////////////////////////////////////////////////////

// Only for resources a barrier of the list being recorded names, so that they are alive.
core::ResourceDesc describe(core::ResourceId resource) {
    const auto desc = static_cast<ID3D12Resource*>(const_cast<void*>(resource))->GetDesc();
    return {{static_cast<std::uint32_t>(desc.Width), desc.Height}, static_cast<core::Format>(desc.Format),
        static_cast<std::uint32_t>(desc.Flags), desc.MipLevels, desc.DepthOrArraySize};
}

const core::Describe DESCRIBE{&describe};

// Every call to the registry goes under g_states_mutex: resources are created, lists submitted and the
// G-buffer pass and the TAA draw recorded on different threads. It is never held while calling the
// runtime or the anchor handler, apart from the AddRefs of refs_of.
std::mutex g_states_mutex{};
core::ResourceStates g_states{};                            // under g_states_mutex
std::optional<core::TaaConfirmation> g_last_confirmation{}; // under g_states_mutex

// The owning references of one G-buffer: albedo, normals, velocity and depth.
using GBufferRefs = std::array<ComPtr<ID3D12Resource>, 4>;

// Under g_states_mutex. Never destroyed: at the exit of the process, the static destructors of this DLL
// may run when the runtime is gone, so its references must not be released there.
auto& g_gbuffer_hold = *new core::GBufferHold<GBufferRefs>{};

// Only for resources a barrier of the list being recorded names, so that they are alive.
GBufferRefs refs_of(const core::GBuffer& g) {
    const auto ref = [](core::ResourceId resource) {
        return ComPtr<ID3D12Resource>{static_cast<ID3D12Resource*>(const_cast<void*>(resource))};
    };

    return {ref(g.albedo), ref(g.normals), ref(g.velocity), ref(g.depth)};
}

// How many lines of each kind of anomaly reach the log, so that a broken assumption shows without
// flooding it.
constexpr std::uint32_t LOG_LIMIT = 30;
std::atomic<std::uint32_t> g_logged_read_mismatches{};
std::atomic<std::uint32_t> g_logged_disagreements{};
std::atomic<std::uint32_t> g_logged_unexpected{};
std::array<std::atomic<std::uint32_t>, core::GBUFFER_ORDER_COUNT> g_logged_orders{};
std::array<std::atomic<bool>, core::GBUFFER_PROBLEM_COUNT> g_logged_gbuffer_problems{};
std::atomic<bool> g_logged_bundle{};

bool log_once_more(std::atomic<std::uint32_t>& logged) {
    return logged.fetch_add(1) < LOG_LIMIT;
}

// The G-buffer pass, at an OMSetRenderTargets.
void observe_gbuffer(List* self, ListState& s, const core::Profile& profile) {
    std::array<core::ResourceId, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    std::copy_n(s.render_targets.begin(), s.rtv_count, targets.begin());
    const core::RenderTargetBinding binding{{targets.data(), s.rtv_count}, s.depth.resource, s.depth.flags};

    if (!core::has_gbuffer_shape(binding, profile)) {
        return;
    }

    // The table may keep the resource of a view the game no longer uses, so the resources are described
    // only if a barrier of the list names them. In RE2 the list takes all four to their pass's states
    // just before it.
    const auto& layout = profile.gbuffer;

    for (const auto resource : {targets[layout.albedo], targets[layout.normals], targets[layout.velocity], binding.depth}) {
        if (!s.recording.names(resource)) {
            tally(Counter::GBufferUnnamed);
            return;
        }
    }

    const auto check = core::identify_gbuffer(binding, profile, DESCRIBE);

    if (!check.gbuffer) {
        tally(Counter::GBufferRejected);

        if (!g_logged_gbuffer_problems[static_cast<std::size_t>(check.problem)].exchange(true)) {
            const auto depth = describe(binding.depth);
            spdlog::info("[RayReconstruction] Pass with the G-buffer's shape rejected: {} | depth {:x} format {} {}x{}, {} mips, {} slices",
                core::to_string(check.problem), reinterpret_cast<std::uintptr_t>(binding.depth), static_cast<std::uint32_t>(depth.format),
                depth.size.width, depth.size.height, depth.mip_levels, depth.array_size);
        }

        return;
    }

    core::ResourceStates::Pass pass{};
    std::vector<GBufferRefs> expired{}; // destroyed after the lock: it may hold the last references

    {
        std::scoped_lock _{g_states_mutex};
        pass = g_states.on_gbuffer_pass(*check.gbuffer);

        if (pass.changed) {
            g_gbuffer_hold.replace(refs_of(*check.gbuffer));
        }

        expired = g_gbuffer_hold.take_expired(g_frame.load());
    }

    s.recording.gbuffer_pass(pass.number);

    // Every change, so that the log shows a change of resolution and whether the inventory's second
    // depth ever takes the place of the scene's.
    if (pass.changed) {
        spdlog::info("[RayReconstruction] G-buffer at frame {}, pass {}: {} | list {:x} thread {}", g_frame.load(), pass.number,
            to_string(*check.gbuffer), reinterpret_cast<std::uintptr_t>(self), GetCurrentThreadId());
    }
}

void log_submission(const core::Submission& submission) {
    for (const auto& m : submission.read_mismatches) {
        if (log_once_more(g_logged_read_mismatches)) {
            spdlog::info(
                "[RayReconstruction] Registry mismatch on the {} ({:x}, subresource {:x}): the game's barrier says before {:x}, the "
                "registry had {:x}; the state stays unknown until a clean TAA",
                core::to_string(m.read), reinterpret_cast<std::uintptr_t>(m.resource), m.subresource, m.before, m.held);
        }
    }

    if (!submission.taa) {
        return;
    }

    const auto& c = *submission.taa;

    if (c.order != core::GBufferOrder::SamePass && log_once_more(g_logged_orders[static_cast<std::size_t>(c.order)])) {
        spdlog::info("[RayReconstruction] TAA submitted at frame {}: {} (the draw used G-buffer pass {})", g_frame.load(),
            core::to_string(c.order), c.marker.gbuffer_pass);
    }

    if (c.disagree && log_once_more(g_logged_disagreements)) {
        spdlog::info("[RayReconstruction] TAA submitted at frame {}: {}the states predicted at the draw{} were {} | at the TAA: {}",
            g_frame.load(), c.gbuffer_differs ? "the last G-buffer submitted is not the one the draw used; " : "",
            c.marker.known ? ", which took them as known," : "", to_string(c.marker.predicted), to_string(c.actual));
    }

    if (!c.expected && log_once_more(g_logged_unexpected)) {
        spdlog::info("[RayReconstruction] TAA submitted at frame {}, not in the expected states: {}", g_frame.load(), to_string(c.actual));
    }
}

/////////////////////////////////////////////////////////////////////////////
// The evaluation point
/////////////////////////////////////////////////////////////////////////////

std::atomic<AnchorHandler> g_anchor_handler{};
std::atomic<TimedDrawHandler> g_timed_draw_handler{};

// What becomes of a draw the hooks saw.
enum class DrawAction : std::uint8_t {
    Forward,
    Skip,  // it does not reach the runtime: RR's work replaced it, or the debug layer's table was dropped
    Timed, // it reaches the runtime, and the TimedDrawHandler follows it
};

// A split barrier changes the state at its END half.
bool changes_state(const D3D12_RESOURCE_BARRIER& b) {
    return b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) == 0;
}

// The setters of the root arguments of one pipeline.
struct RootSetters {
    void (STDMETHODCALLTYPE List::*signature)(ID3D12RootSignature*);
    void (STDMETHODCALLTYPE List::*table)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    void (STDMETHODCALLTYPE List::*constant)(UINT, UINT, UINT);
    void (STDMETHODCALLTYPE List::*cbv)(UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    void (STDMETHODCALLTYPE List::*srv)(UINT, D3D12_GPU_VIRTUAL_ADDRESS);
    void (STDMETHODCALLTYPE List::*uav)(UINT, D3D12_GPU_VIRTUAL_ADDRESS);
};

constexpr RootSetters GRAPHICS_SETTERS{&List::SetGraphicsRootSignature, &List::SetGraphicsRootDescriptorTable,
    &List::SetGraphicsRoot32BitConstant, &List::SetGraphicsRootConstantBufferView, &List::SetGraphicsRootShaderResourceView,
    &List::SetGraphicsRootUnorderedAccessView};

constexpr RootSetters COMPUTE_SETTERS{&List::SetComputeRootSignature, &List::SetComputeRootDescriptorTable,
    &List::SetComputeRoot32BitConstant, &List::SetComputeRootConstantBufferView, &List::SetComputeRootShaderResourceView,
    &List::SetComputeRootUnorderedAccessView};

// A root signature the game never set cannot be set back: the game sets its own before it uses that
// pipeline again.
void bind_again(List* list, const RootBindings& bindings, const RootSetters& set) {
    if (bindings.root_signature() == nullptr) {
        return;
    }

    (list->*set.signature)(bindings.root_signature());

    for (auto mask = bindings.set_mask(); mask != 0; mask &= mask - 1) {
        const auto index = static_cast<UINT>(std::countr_zero(mask));
        const auto& argument = bindings.argument(index);

        switch (argument.kind) {
        case RootArgument::Kind::Table:
            (list->*set.table)(index, D3D12_GPU_DESCRIPTOR_HANDLE{argument.value});
            break;
        case RootArgument::Kind::Constants:
            for (auto constants = argument.constant_mask; constants != 0; constants &= constants - 1) {
                const auto offset = static_cast<UINT>(std::countr_zero(constants));
                (list->*set.constant)(index, argument.constants[offset], offset);
            }
            break;
        case RootArgument::Kind::Cbv:
            (list->*set.cbv)(index, argument.value);
            break;
        case RootArgument::Kind::Srv:
            (list->*set.srv)(index, argument.value);
            break;
        case RootArgument::Kind::Uav:
            (list->*set.uav)(index, argument.value);
            break;
        }
    }
}

// After RR's own work on the game's list, everything the game had bound, as the shadow state has it. The
// descriptor tables go after the heaps, which make them stale.
void restore(List* list, const ListState& s) {
    const ReentryGuard guard{};

    if (s.heap_count > 0) {
        list->SetDescriptorHeaps(s.heap_count, s.heaps.data());
    }

    bind_again(list, s.compute, COMPUTE_SETTERS);
    bind_again(list, s.graphics, GRAPHICS_SETTERS);

    if (s.pso != nullptr) {
        list->SetPipelineState(s.pso);
    }

    if (s.rtv_count > 0 || s.dsv) {
        list->OMSetRenderTargets(s.rtv_count, s.rtv_count > 0 ? s.rtvs.data() : nullptr, FALSE, s.dsv ? &*s.dsv : nullptr);
    }

    if (s.viewport_count > 0) {
        list->RSSetViewports(s.viewport_count, s.viewports.data());
    }

    if (s.scissor_count > 0) {
        list->RSSetScissorRects(s.scissor_count, s.scissors.data());
    }
}

DrawAction on_anchor(List* self, ListState& s, const core::Anchor& anchor, const PsoRegistry& registry) {
    // The draw stays the game's rather than being judged, and the list restored, from stale bindings: the
    // Mod does not see it, and the frame is omitted like any other without an evaluation point.
    if (s.bundle_ran) {
        tally(Counter::AnchorsAfterBundle);
        return DrawAction::Forward;
    }

    const auto& profile = registry.profile();

    // Before the handler, whose copy of the LUT reads it as a Texture2D: dxc tells from the dump what the
    // variant declares there.
    note_anchored_pt_cs(anchor.pt_cs);

    AnchoredDraw draw{
        .frame = g_frame.load(),
        .thread = GetCurrentThreadId(),
        .list = self,
        .shaders = s.shaders.value_or(PsoShaders{}),
        .anchor = anchor,
        .heaps_bound = s.heap_count > 0,
        .ambient_brdf = s.ambient_brdf,
    };

    draw.ps_role = ps_role(profile, draw.shaders.ps);

    // A null RT0 is also a view the table does not know. The table may keep the resource of a view the
    // game no longer uses, so RT0 is described only if a barrier of the list names it: identify_output
    // looks at its state before its format.
    if (s.rtv_count > 0 && s.render_targets[0] != nullptr) {
        auto& rt0 = draw.rt0;
        rt0.resource = s.render_targets[0];
        rt0.state = s.recording.state(rt0.resource, 0);

        if (rt0.state) {
            rt0.desc = describe(rt0.resource);
        }
    }

    const auto output = core::identify_output(draw.rt0, anchor.t, profile);
    draw.o_problem = output.problem;

    auto& p = draw.point;
    p.barrier_rule = true;
    p.has_velocity = anchor.outcome == core::AnchorOutcome::Anchored;
    p.vs = draw.shaders.vs.value_or(core::ShaderDigest{});
    p.profile_vs_created = registry.counts().taa_vs > 0;
    p.ps = draw.shaders.ps.value_or(core::ShaderDigest{});
    p.t = anchor.t_desc.size;
    p.t_profile_format = anchor.t_desc.format == profile.formats.t;
    p.o = output.target;

    {
        std::scoped_lock _{g_states_mutex};
        draw.states = g_states.on_taa_draw(anchor.t, anchor.velocity, anchor.t_desc.size, s.recording);

        // Without a known state, the frame is not identified and the handler does not read the G-buffer.
        if (draw.states.problem == core::StateProblem::None) {
            g_gbuffer_hold.on_read(draw.frame);
        }
    }

    p.states_known = draw.states.problem == core::StateProblem::None;
    p.gbuffer_pass = draw.states.gbuffer_pass;

    const auto handler = g_anchor_handler.load();

    if (handler == nullptr) {
        return DrawAction::Forward;
    }

    const auto result = handler(draw);

    if (result.recorded) {
        tally(Counter::OwnWork);
        restore(self, s);
    }

    if (result.replaces_draw) {
        tally(Counter::ReplacedDraws);
        return DrawAction::Skip;
    }

    if (result.times_draw) {
        tally(Counter::TimedDraws);
        return DrawAction::Timed;
    }

    return DrawAction::Forward;
}

DrawAction on_draw(List* self, bool indexed, UINT count, UINT instances) {
    auto& entry = entry_of(self);

    if (entry.skip_next_draw) {
        entry.skip_next_draw = false;
        return DrawAction::Skip;
    }

    tally(Counter::Draws);
    auto& s = entry.state;
    const auto registry = pso_registry();
    const auto full_screen_triangle = !indexed && count == 3 && instances == 1;

    // The barrier rule does not need the PSO: an anchored draw whose vertex shader is unknown is an
    // evaluation point whose identifications do not agree.
    const auto anchor = registry != nullptr ? s.rule.on_draw(full_screen_triangle, registry->profile(), DESCRIBE) : core::Anchor{};
    auto action = DrawAction::Forward;

    switch (anchor.outcome) {
    case core::AnchorOutcome::Anchored:
        tally(Counter::Anchors);
        action = on_anchor(self, s, anchor, *registry);
        break;
    case core::AnchorOutcome::VelocityMissing:
        tally(Counter::AnchorsWithoutVelocity);

        // The TAA draw of a frame without velocity is still its evaluation point, which the policy judges.
        if (s.shaders && core::is_no_velocity_taa(anchor, s.shaders->vs, s.shaders->ps, registry->profile())) {
            action = on_anchor(self, s, anchor, *registry);
        }

        break;
    case core::AnchorOutcome::VelocityAmbiguous:
        tally(Counter::AnchorsWithTwoVelocities);
        break;
    case core::AnchorOutcome::None:
        break;
    }

    // A draw whose vertex shader cannot be told cannot be ruled out as the TAA's.
    if (!s.shaders || !s.shaders->vs) {
        tally(s.shaders ? Counter::NoVsDigestDraws : Counter::UnknownPsoDraws);

        if (full_screen_triangle) {
            tally(Counter::UnknownVsFullscreenDraws);
        }

        return action;
    }

    if (registry != nullptr && s.shaders->vs == registry->profile().taa_vs) {
        on_taa_vs_draw(self, s, indexed, count, instances, *registry);

        if (anchor.outcome != core::AnchorOutcome::Anchored) {
            tally(Counter::TaaVsUnanchoredDraws);
        }
    }

    return action;
}

// Right after a draw that reached the runtime.
void after_draw(List* self, DrawAction action) {
    if (action != DrawAction::Timed) {
        return;
    }

    if (const auto handler = g_timed_draw_handler.load(); handler != nullptr) {
        handler(self);
    }
}

/////////////////////////////////////////////////////////////////////////////
// Detours
/////////////////////////////////////////////////////////////////////////////

HRESULT STDMETHODCALLTYPE reset(List* self, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial);

void observe(List* list, const char* where);

void STDMETHODCALLTYPE clear_state(List* self, ID3D12PipelineState* pso) {
    const auto track = !ReentryGuard::active();
    call_original<void>(slot::CLEAR_STATE, self, pso);

    if (track) {
        entry_of(self).state.clear_state(pso, find_shaders(pso));
    }
}

void STDMETHODCALLTYPE draw_instanced(List* self, UINT vertices, UINT instances, UINT start_vertex, UINT start_instance) {
    const auto action = ReentryGuard::active() ? DrawAction::Forward : on_draw(self, false, vertices, instances);

    if (action == DrawAction::Skip) {
        return;
    }

    call_original<void>(slot::DRAW_INSTANCED, self, vertices, instances, start_vertex, start_instance);
    after_draw(self, action);
}

void STDMETHODCALLTYPE draw_indexed_instanced(
    List* self, UINT indices, UINT instances, UINT start_index, INT base_vertex, UINT start_instance) {
    const auto action = ReentryGuard::active() ? DrawAction::Forward : on_draw(self, true, indices, instances);

    if (action == DrawAction::Skip) {
        return;
    }

    call_original<void>(slot::DRAW_INDEXED_INSTANCED, self, indices, instances, start_index, base_vertex, start_instance);
    after_draw(self, action);
}

// At a Dispatch: the descriptor at the AmbientBRDF register, from the table the list set at the parameter
// that holds it.
AmbientBrdfDescriptor locate_ambient_brdf(const ListState& s) {
    const auto root_signature = find_root_signature(s.compute.root_signature());

    if (!root_signature) {
        return {.lut = {.location = core::LutLocation::RootSignatureUnknown}};
    }

    const auto& binding = root_signature->ambient_brdf;
    std::optional<std::uint64_t> table{};

    if (binding && binding->kind == core::RootParameter::Kind::Table && binding->parameter < MAX_ROOT_PARAMETERS &&
        (s.compute.set_mask() & (std::uint64_t{1} << binding->parameter)) != 0) {
        if (const auto& argument = s.compute.argument(binding->parameter); argument.kind == RootArgument::Kind::Table) {
            table = argument.value;
        }
    }

    AmbientBrdfDescriptor out{};
    std::optional<core::HeapRange> heap{};

    for (std::uint32_t i = 0; i < s.heap_count; ++i) {
        if (s.heaps[i] == nullptr) {
            continue;
        }

        if (const auto desc = s.heaps[i]->GetDesc(); desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
            out.heap = s.heaps[i];
            heap = core::HeapRange{s.heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr, desc.NumDescriptors};
            break;
        }
    }

    // A PSO the registry does not know declares nothing known.
    const auto declaration = s.shaders ? s.shaders->cs_lut : core::SrvDeclaration::Unknown;
    out.lut = core::locate_lut(declaration, binding, table, heap, srv_increment());
    return out;
}

void STDMETHODCALLTYPE dispatch(List* self, UINT x, UINT y, UINT z) {
    if (!ReentryGuard::active()) {
        tally(Counter::Dispatches);
        auto& s = entry_of(self).state;

        if (!s.shaders) {
            tally(Counter::UnknownPsoDispatches);
        }

        s.rule.on_dispatch(s.shaders ? s.shaders->cs : std::nullopt);

        // Any Dispatch may be the PT's, whatever its compute shader: each setting of the PT is another one.
        s.last_dispatch_lut = locate_ambient_brdf(s);
    }

    call_original<void>(slot::DISPATCH, self, x, y, z);
}

void STDMETHODCALLTYPE rs_set_viewports(List* self, UINT count, const D3D12_VIEWPORT* viewports) {
    const auto track = !ReentryGuard::active();
    call_original<void>(slot::RS_SET_VIEWPORTS, self, count, viewports);

    if (track) {
        auto& s = entry_of(self).state;
        s.viewport_count = viewports != nullptr ? std::min<UINT>(count, static_cast<UINT>(s.viewports.size())) : 0;
        std::copy_n(viewports, s.viewport_count, s.viewports.begin());
    }
}

void STDMETHODCALLTYPE rs_set_scissor_rects(List* self, UINT count, const D3D12_RECT* rects) {
    const auto track = !ReentryGuard::active();
    call_original<void>(slot::RS_SET_SCISSOR_RECTS, self, count, rects);

    if (track) {
        auto& s = entry_of(self).state;
        s.scissor_count = rects != nullptr ? std::min<UINT>(count, static_cast<UINT>(s.scissors.size())) : 0;
        std::copy_n(rects, s.scissor_count, s.scissors.begin());
    }
}

void STDMETHODCALLTYPE set_pipeline_state(List* self, ID3D12PipelineState* pso) {
    const auto track = !ReentryGuard::active();

    // The runtime gets the substitute, and the shadow state the game's PSO. restore never has to bind a
    // substitute: it runs at a draw, and the PSO bound there is a graphics one.
    if (track && substitution_enabled()) {
        const auto shaders = find_shaders(pso);
        call_original<void>(slot::SET_PIPELINE_STATE, self, pso_to_bind(pso, shaders));
        entry_of(self).state.set_pso(pso, shaders);
        return;
    }

    call_original<void>(slot::SET_PIPELINE_STATE, self, pso);

    if (track) {
        entry_of(self).state.set_pso(pso, find_shaders(pso));
    }
}

void STDMETHODCALLTYPE resource_barrier(List* self, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    const auto track = !ReentryGuard::active();
    call_original<void>(slot::RESOURCE_BARRIER, self, count, barriers);

    if (track && barriers != nullptr) {
        tally(Counter::Barriers, count);
        auto& s = entry_of(self).state;

        for (UINT i = 0; i < count; ++i) {
            if (const auto& b = barriers[i]; changes_state(b)) {
                const auto& t = b.Transition;
                const core::Transition transition{
                    t.pResource, t.Subresource, static_cast<std::uint32_t>(t.StateBefore), static_cast<std::uint32_t>(t.StateAfter)};

                // The PT's Dispatch is the last one recorded before the transition that closes its sequence.
                if (s.rule.on_transition(transition, DESCRIBE)) {
                    s.ambient_brdf = s.last_dispatch_lut;
                }

                s.recording.transition(transition);
            }
        }
    }
}

void STDMETHODCALLTYPE execute_bundle(List* self, ID3D12GraphicsCommandList* bundle) {
    const auto track = !ReentryGuard::active();
    call_original<void>(slot::EXECUTE_BUNDLE, self, bundle);

    if (track) {
        tally(Counter::Bundles);
        entry_of(self).state.bundle_ran = true;

        // The prototypes' logs of RE2 do not show any bundle: this line tells a port if its game has them.
        if (!g_logged_bundle.exchange(true)) {
            spdlog::info("[RayReconstruction] First ExecuteBundle, on list {:x} thread {}: the shadow state does not follow bundles, so "
                         "draws anchored after one on the same list stay the game's",
                reinterpret_cast<std::uintptr_t>(self), GetCurrentThreadId());
        }
    }
}

void STDMETHODCALLTYPE set_descriptor_heaps(List* self, UINT count, ID3D12DescriptorHeap* const* heaps) {
    const auto track = !ReentryGuard::active();
    call_original<void>(slot::SET_DESCRIPTOR_HEAPS, self, count, heaps);

    if (track) {
        auto& s = entry_of(self).state;
        s.heap_count = heaps != nullptr ? std::min<UINT>(count, static_cast<UINT>(s.heaps.size())) : 0;
        s.heaps = {};
        std::copy_n(heaps, s.heap_count, s.heaps.begin());
    }
}

template <std::uint32_t SLOT, RootBindings ListState::* BINDINGS>
void STDMETHODCALLTYPE set_root_signature(List* self, ID3D12RootSignature* root_signature) {
    const auto track = !ReentryGuard::active();
    call_original<void>(SLOT, self, root_signature);

    if (track) {
        (entry_of(self).state.*BINDINGS).set_root_signature(root_signature);
    }
}

template <std::uint32_t SLOT, RootBindings ListState::* BINDINGS>
void STDMETHODCALLTYPE set_root_descriptor_table(List* self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    const auto track = !ReentryGuard::active();
    call_original<void>(SLOT, self, index, table);

    if (track && index < MAX_ROOT_PARAMETERS) {
        (entry_of(self).state.*BINDINGS).write(index, RootArgument::Kind::Table).value = table.ptr;
    }
}

// With the debug layer, a null table raises an unhandled 0x87D (message 646) that closes the game even
// with every break off. REFramework's own ImGui pass binds TexID 0 on its first frames, so the table
// and the draw that would use it are dropped. The game's lists never bind a null table, and without
// the debug layer nothing changes. The defect is dodged here, not fixed upstream.
void STDMETHODCALLTYPE set_graphics_root_descriptor_table(List* self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    if (!ReentryGuard::active() && table.ptr == 0 && debug_layer_active()) {
        tally(Counter::NullTablesDropped);
        entry_of(self).skip_next_draw = true;
        return;
    }

    set_root_descriptor_table<slot::SET_GRAPHICS_ROOT_DESCRIPTOR_TABLE, &ListState::graphics>(self, index, table);

    if (!ReentryGuard::active()) {
        entry_of(self).skip_next_draw = false;
    }
}

template <std::uint32_t SLOT, RootBindings ListState::* BINDINGS>
void STDMETHODCALLTYPE set_root_32bit_constant(List* self, UINT index, UINT value, UINT offset) {
    const auto track = !ReentryGuard::active();
    call_original<void>(SLOT, self, index, value, offset);

    if (track && index < MAX_ROOT_PARAMETERS && offset < MAX_ROOT_PARAMETERS) {
        auto& argument = (entry_of(self).state.*BINDINGS).write(index, RootArgument::Kind::Constants);
        argument.constants[offset] = value;
        argument.constant_mask |= std::uint64_t{1} << offset;
    }
}

template <std::uint32_t SLOT, RootBindings ListState::* BINDINGS>
void STDMETHODCALLTYPE set_root_32bit_constants(List* self, UINT index, UINT count, const void* values, UINT offset) {
    const auto track = !ReentryGuard::active();
    call_original<void>(SLOT, self, index, count, values, offset);

    if (track && index < MAX_ROOT_PARAMETERS && values != nullptr) {
        auto& argument = (entry_of(self).state.*BINDINGS).write(index, RootArgument::Kind::Constants);

        for (UINT i = 0; i < count && offset + i < MAX_ROOT_PARAMETERS; ++i) {
            argument.constants[offset + i] = static_cast<const std::uint32_t*>(values)[i];
            argument.constant_mask |= std::uint64_t{1} << (offset + i);
        }
    }
}

template <std::uint32_t SLOT, RootBindings ListState::* BINDINGS, RootArgument::Kind KIND>
void STDMETHODCALLTYPE set_root_view(List* self, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    const auto track = !ReentryGuard::active();
    call_original<void>(SLOT, self, index, address);

    if (track && index < MAX_ROOT_PARAMETERS) {
        (entry_of(self).state.*BINDINGS).write(index, KIND).value = address;
    }
}

void STDMETHODCALLTYPE om_set_render_targets(
    List* self, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    const auto track = !ReentryGuard::active();
    call_original<void>(slot::OM_SET_RENDER_TARGETS, self, count, rtvs, single_range, dsv);

    if (!track) {
        return;
    }

    auto& s = entry_of(self).state;
    s.rtv_count = rtvs != nullptr ? std::min<UINT>(count, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) : 0;

    for (UINT i = 0; i < s.rtv_count; ++i) {
        // One range: consecutive handles from the first.
        const auto handle = single_range ? D3D12_CPU_DESCRIPTOR_HANDLE{rtvs[0].ptr + static_cast<SIZE_T>(i) * rtv_increment()} : rtvs[i];
        const auto view = find_view(handle);
        s.rtvs[i] = handle;
        s.render_targets[i] = view ? static_cast<ID3D12Resource*>(view->resource) : nullptr;
    }

    if (dsv != nullptr) {
        s.dsv = *dsv;
        s.depth = find_view(*dsv).value_or(core::View{});
    } else {
        s.dsv.reset();
        s.depth = {};
    }

    if (const auto registry = pso_registry(); registry != nullptr) {
        observe_gbuffer(self, s, registry->profile());
    }
}

void STDMETHODCALLTYPE execute_indirect(List* self, ID3D12CommandSignature* signature, UINT max_count, ID3D12Resource* args,
    UINT64 args_offset, ID3D12Resource* count_buffer, UINT64 count_offset) {
    if (!ReentryGuard::active()) {
        tally(Counter::Indirect);
    }

    call_original<void>(slot::EXECUTE_INDIRECT, self, signature, max_count, args, args_offset, count_buffer, count_offset);
}

// The shadow state does not follow the next three; they are only counted, to show whether a game uses
// them. The prototype's logs of RE2 do not show any enhanced barrier.
void STDMETHODCALLTYPE begin_render_pass(List* self, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* render_targets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth_stencil, D3D12_RENDER_PASS_FLAGS flags) {
    if (!ReentryGuard::active()) {
        tally(Counter::RenderPasses);
    }

    call_original<void>(slot::BEGIN_RENDER_PASS, self, count, render_targets, depth_stencil, flags);
}

void STDMETHODCALLTYPE set_pipeline_state1(List* self, ID3D12StateObject* state_object) {
    if (!ReentryGuard::active()) {
        tally(Counter::StateObjects);
    }

    call_original<void>(slot::SET_PIPELINE_STATE1, self, state_object);
}

void STDMETHODCALLTYPE barrier(List* self, UINT32 count, const D3D12_BARRIER_GROUP* groups) {
    if (!ReentryGuard::active()) {
        tally(Counter::EnhancedBarriers);
    }

    call_original<void>(slot::BARRIER, self, count, groups);
}

struct Slot {
    std::uint32_t index;
    void* detour;
    const char* name;
};

using Bindings = RootBindings ListState::*;
constexpr Bindings GRAPHICS = &ListState::graphics;
constexpr Bindings COMPUTE = &ListState::compute;

const std::array SLOTS{
    Slot{slot::RESET, reinterpret_cast<void*>(&reset), "Reset"},
    Slot{slot::CLEAR_STATE, reinterpret_cast<void*>(&clear_state), "ClearState"},
    Slot{slot::DRAW_INSTANCED, reinterpret_cast<void*>(&draw_instanced), "DrawInstanced"},
    Slot{slot::DRAW_INDEXED_INSTANCED, reinterpret_cast<void*>(&draw_indexed_instanced), "DrawIndexedInstanced"},
    Slot{slot::DISPATCH, reinterpret_cast<void*>(&dispatch), "Dispatch"},
    Slot{slot::RS_SET_VIEWPORTS, reinterpret_cast<void*>(&rs_set_viewports), "RSSetViewports"},
    Slot{slot::RS_SET_SCISSOR_RECTS, reinterpret_cast<void*>(&rs_set_scissor_rects), "RSSetScissorRects"},
    Slot{slot::SET_PIPELINE_STATE, reinterpret_cast<void*>(&set_pipeline_state), "SetPipelineState"},
    Slot{slot::RESOURCE_BARRIER, reinterpret_cast<void*>(&resource_barrier), "ResourceBarrier"},
    Slot{slot::EXECUTE_BUNDLE, reinterpret_cast<void*>(&execute_bundle), "ExecuteBundle"},
    Slot{slot::SET_DESCRIPTOR_HEAPS, reinterpret_cast<void*>(&set_descriptor_heaps), "SetDescriptorHeaps"},
    Slot{slot::SET_COMPUTE_ROOT_SIGNATURE, reinterpret_cast<void*>(&set_root_signature<slot::SET_COMPUTE_ROOT_SIGNATURE, COMPUTE>),
        "SetComputeRootSignature"},
    Slot{slot::SET_GRAPHICS_ROOT_SIGNATURE, reinterpret_cast<void*>(&set_root_signature<slot::SET_GRAPHICS_ROOT_SIGNATURE, GRAPHICS>),
        "SetGraphicsRootSignature"},
    Slot{slot::SET_COMPUTE_ROOT_DESCRIPTOR_TABLE,
        reinterpret_cast<void*>(&set_root_descriptor_table<slot::SET_COMPUTE_ROOT_DESCRIPTOR_TABLE, COMPUTE>),
        "SetComputeRootDescriptorTable"},
    Slot{slot::SET_GRAPHICS_ROOT_DESCRIPTOR_TABLE, reinterpret_cast<void*>(&set_graphics_root_descriptor_table),
        "SetGraphicsRootDescriptorTable"},
    Slot{slot::SET_COMPUTE_ROOT_32BIT_CONSTANT,
        reinterpret_cast<void*>(&set_root_32bit_constant<slot::SET_COMPUTE_ROOT_32BIT_CONSTANT, COMPUTE>), "SetComputeRoot32BitConstant"},
    Slot{slot::SET_GRAPHICS_ROOT_32BIT_CONSTANT,
        reinterpret_cast<void*>(&set_root_32bit_constant<slot::SET_GRAPHICS_ROOT_32BIT_CONSTANT, GRAPHICS>),
        "SetGraphicsRoot32BitConstant"},
    Slot{slot::SET_COMPUTE_ROOT_32BIT_CONSTANTS,
        reinterpret_cast<void*>(&set_root_32bit_constants<slot::SET_COMPUTE_ROOT_32BIT_CONSTANTS, COMPUTE>),
        "SetComputeRoot32BitConstants"},
    Slot{slot::SET_GRAPHICS_ROOT_32BIT_CONSTANTS,
        reinterpret_cast<void*>(&set_root_32bit_constants<slot::SET_GRAPHICS_ROOT_32BIT_CONSTANTS, GRAPHICS>),
        "SetGraphicsRoot32BitConstants"},
    Slot{slot::SET_COMPUTE_ROOT_CBV, reinterpret_cast<void*>(&set_root_view<slot::SET_COMPUTE_ROOT_CBV, COMPUTE, RootArgument::Kind::Cbv>),
        "SetComputeRootConstantBufferView"},
    Slot{slot::SET_GRAPHICS_ROOT_CBV,
        reinterpret_cast<void*>(&set_root_view<slot::SET_GRAPHICS_ROOT_CBV, GRAPHICS, RootArgument::Kind::Cbv>),
        "SetGraphicsRootConstantBufferView"},
    Slot{slot::SET_COMPUTE_ROOT_SRV, reinterpret_cast<void*>(&set_root_view<slot::SET_COMPUTE_ROOT_SRV, COMPUTE, RootArgument::Kind::Srv>),
        "SetComputeRootShaderResourceView"},
    Slot{slot::SET_GRAPHICS_ROOT_SRV,
        reinterpret_cast<void*>(&set_root_view<slot::SET_GRAPHICS_ROOT_SRV, GRAPHICS, RootArgument::Kind::Srv>),
        "SetGraphicsRootShaderResourceView"},
    Slot{slot::SET_COMPUTE_ROOT_UAV, reinterpret_cast<void*>(&set_root_view<slot::SET_COMPUTE_ROOT_UAV, COMPUTE, RootArgument::Kind::Uav>),
        "SetComputeRootUnorderedAccessView"},
    Slot{slot::SET_GRAPHICS_ROOT_UAV,
        reinterpret_cast<void*>(&set_root_view<slot::SET_GRAPHICS_ROOT_UAV, GRAPHICS, RootArgument::Kind::Uav>),
        "SetGraphicsRootUnorderedAccessView"},
    Slot{slot::OM_SET_RENDER_TARGETS, reinterpret_cast<void*>(&om_set_render_targets), "OMSetRenderTargets"},
    Slot{slot::EXECUTE_INDIRECT, reinterpret_cast<void*>(&execute_indirect), "ExecuteIndirect"},
    Slot{slot::BEGIN_RENDER_PASS, reinterpret_cast<void*>(&begin_render_pass), "BeginRenderPass"},
    Slot{slot::SET_PIPELINE_STATE1, reinterpret_cast<void*>(&set_pipeline_state1), "SetPipelineState1"},
    Slot{slot::BARRIER, reinterpret_cast<void*>(&barrier), "Barrier"},
};

// Whether a slot of SLOTS is in the vtable of this list: the ones past ID3D12GraphicsCommandList are
// there only if the list implements their interface through the same vtable.
template <typename Interface> bool has_interface(List* list, void** vtable) {
    ComPtr<Interface> as{};
    return SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&as))) && vtable_of(as.Get()) == vtable;
}

bool is_detour(void* p) {
    return std::any_of(SLOTS.begin(), SLOTS.end(), [p](const Slot& s) { return s.detour == p; });
}

HRESULT STDMETHODCALLTYPE reset(List* self, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial) {
    const auto track = !ReentryGuard::active();
    const auto before = vtable_of(self);
    const auto hr = call_original<HRESULT>(slot::RESET, self, allocator, initial);

    if (!track) {
        return hr;
    }

    tally(Counter::Resets);

    // The vtable of a recording list, if the runtime has one apart, shows up here first.
    if (vtable_of(self) != before) {
        observe(self, "after Reset");
    }

    if (SUCCEEDED(hr)) {
        entry_of(self).reset(initial, find_shaders(initial));
    }

    return hr;
}

std::string module_of(const void* address) {
    HMODULE module{};
    wchar_t path[MAX_PATH]{L"?"};

    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module)) {
        GetModuleFileNameW(module, path, MAX_PATH);
    }

    return std::filesystem::path{path}.filename().string();
}

void observe(List* list, const char* where) try {
    const auto vtable = vtable_of(list);

    if (vtable == nullptr || find_vtable(vtable) != nullptr) {
        return;
    }

    std::scoped_lock _{g_vtables_mutex};
    const auto index = g_vtable_count.load();

    if (find_vtable(vtable) != nullptr) {
        return;
    }

    if (index >= MAX_VTABLES) {
        spdlog::error("[RayReconstruction] Command list vtable {:x} not patched: already {} patched",
            reinterpret_cast<std::uintptr_t>(vtable), index);
        return;
    }

    const auto has_list4 = has_interface<ID3D12GraphicsCommandList4>(list, vtable);
    const auto has_list7 = has_interface<ID3D12GraphicsCommandList7>(list, vtable);

    auto patched = std::make_unique<PatchedVtable>();
    patched->vtable = vtable;

    for (const auto& s : SLOTS) {
        const auto present = s.index == slot::BARRIER ? has_list7 : s.index >= slot::BEGIN_RENDER_PASS ? has_list4 : true;

        if (!present) {
            continue;
        }

        // A copy of a vtable already patched, made by someone else, would make a detour its own original.
        if (is_detour(vtable[s.index])) {
            spdlog::error("[RayReconstruction] Command list vtable {:x} not patched: {} already leads to a detour",
                reinterpret_cast<std::uintptr_t>(vtable), s.name);
            return;
        }

        patched->originals[s.index] = vtable[s.index];
    }

    // Publish the originals before patching: a detour can run as soon as its slot is written.
    g_vtables[index] = patched.get();
    ++g_vtable_count;
    tally(Counter::ListVtables);

    std::string failures{};

    for (const auto& s : SLOTS) {
        if (patched->originals[s.index] == nullptr) {
            continue;
        }

        try {
            patched->hooks.push_back(std::make_unique<PointerHook>(&vtable[s.index], s.detour));
        } catch (const std::exception& e) {
            failures += std::format(" {}[{}] failed ({});", s.name, s.index, e.what());
        }
    }

    const auto text = std::format("#{} {:x} in {}, first seen {}: {}{}{}", index, reinterpret_cast<std::uintptr_t>(vtable),
        module_of(vtable), where, failures.empty() ? std::string{"all slots patched"} : failures,
        has_list4 ? "" : " (no ID3D12GraphicsCommandList4 slots)", has_list7 ? "" : " (no Barrier slot)");

    g_vtables_report = index == 0 ? text : g_vtables_report + " | " + text;
    spdlog::info("[RayReconstruction] Patched command list vtable {}", text);

    (void)patched.release(); // never freed: its slots stay patched
} catch (const std::exception& e) {
    spdlog::error("[RayReconstruction] Patching a command list vtable: {}", e.what());
}
} // namespace

void on_list_created(IUnknown* unknown, ID3D12PipelineState* initial, const char* where) {
    ComPtr<ID3D12GraphicsCommandList> list{};

    if (unknown == nullptr || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&list)))) {
        return;
    }

    tally(Counter::Lists);
    observe(list.Get(), where);

    // A new list can take the address of a destroyed one, of another type.
    auto& entry = entry_of(list.Get());
    entry.reset(initial, find_shaders(initial));
    entry.state.type = list->GetType();
}

void on_resource_created(void* resource, bool followed, std::uint32_t subresources) {
    std::scoped_lock _{g_states_mutex};
    g_states.on_created(resource, followed, subresources);
}

void on_lists_submitted(UINT count, ID3D12CommandList* const* lists) {
    tally(Counter::Submissions);

    for (UINT i = 0; i < count; ++i) {
        if (lists[i] == nullptr) {
            continue;
        }

        tally(Counter::SubmittedLists);
        const auto entry = find_entry(lists[i]);

        if (entry == nullptr) {
            tally(Counter::UnseenLists);
            continue;
        }

        // The list is closed: its recording does not change until the game resets it, after this.
        core::Submission submission{};

        {
            std::scoped_lock _{g_states_mutex};
            submission = g_states.on_submitted(entry->state.recording, entry->state.type == D3D12_COMMAND_LIST_TYPE_COPY);

            if (submission.taa) {
                g_last_confirmation = submission.taa;
            }
        }

        log_submission(submission);
    }
}

void reset_state_counters() {
    std::scoped_lock _{g_states_mutex};
    g_states.reset_counters();
}

std::string to_string(const core::ReadStates& states) {
    std::string out{};

    for (std::size_t i = 0; i < states.size(); ++i) {
        out += std::format("{}{} {}", i == 0 ? "" : ", ", core::to_string(static_cast<core::Read>(i)),
            states[i] ? std::format("{:x}", *states[i]) : std::string{"unknown"});
    }

    return out;
}

std::string to_string(const core::GBuffer& g) {
    return std::format("albedo {:x}, normals {:x}, velocity {:x}, depth {:x}, {}x{}", reinterpret_cast<std::uintptr_t>(g.albedo),
        reinterpret_cast<std::uintptr_t>(g.normals), reinterpret_cast<std::uintptr_t>(g.velocity),
        reinterpret_cast<std::uintptr_t>(g.depth), g.size.width, g.size.height);
}

void set_anchor_handler(AnchorHandler handler) {
    g_anchor_handler = handler;
}

void set_timed_draw_handler(TimedDrawHandler handler) {
    g_timed_draw_handler = handler;
}

const char* to_string(Counter counter) {
    switch (counter) {
    case Counter::Draws:
        return "Draws";
    case Counter::Dispatches:
        return "Dispatches";
    case Counter::Barriers:
        return "Barriers";
    case Counter::Resets:
        return "Resets";
    case Counter::TaaVsDraws:
        return "Draws with the TAA vertex shader";
    case Counter::TaaVsAdmittedDraws:
        return "  of them with an admitted pixel shader";
    case Counter::TaaVsUnanchoredDraws:
        return "  of them not anchored by the barrier rule";
    case Counter::Anchors:
        return "Draws anchored by the barrier rule";
    case Counter::AnchorsWithoutVelocity:
        return "  attempts without the velocity";
    case Counter::AnchorsWithTwoVelocities:
        return "  attempts with two or more velocities";
    case Counter::AnchorsAfterBundle:
        return "  left to the game: a bundle ran on the list";
    case Counter::OwnWork:
        return "Anchored draws with RR's own work, list state restored";
    case Counter::ReplacedDraws:
        return "Anchored draws replaced by RR's work";
    case Counter::TimedDraws:
        return "Anchored draws timed by the Mod";
    case Counter::UnknownPsoDraws:
        return "Draws with an unknown PSO";
    case Counter::NoVsDigestDraws:
        return "Draws whose vertex shader has no digest";
    case Counter::UnknownVsFullscreenDraws:
        return "  either of them in a DrawInstanced(3, 1)";
    case Counter::UnknownPsoDispatches:
        return "Dispatches with an unknown PSO";
    case Counter::Bundles:
        return "Bundles";
    case Counter::Indirect:
        return "ExecuteIndirect";
    case Counter::EnhancedBarriers:
        return "Enhanced barriers (not followed)";
    case Counter::RenderPasses:
        return "Render passes (not followed)";
    case Counter::StateObjects:
        return "SetPipelineState1 (not followed)";
    case Counter::NullTablesDropped:
        return "Null tables dropped (debug layer)";
    case Counter::OriginalFallbacks:
        return "Original lookup fallbacks";
    case Counter::Lists:
        return "Command lists created";
    case Counter::ListVtables:
        return "Command list vtables patched";
    case Counter::GBufferUnnamed:
        return "G-buffer passes with a resource no barrier of the list names";
    case Counter::GBufferRejected:
        return "G-buffer passes rejected by the profile";
    case Counter::Submissions:
        return "ExecuteCommandLists";
    case Counter::SubmittedLists:
        return "  lists submitted";
    case Counter::UnseenLists:
        return "  of them never seen recorded";
    default:
        return "?";
    }
}

void fill_list_status(Status& status) {
    {
        std::scoped_lock _{g_vtables_mutex};
        status.list_vtables = g_vtables_report;
    }

    {
        std::scoped_lock _{g_frame_mutex};
        status.last_frame = g_last_frame;
    }

    {
        std::scoped_lock _{g_taa_mutex};
        status.taa_draw = g_taa_draw;
    }

    {
        std::scoped_lock _{g_states_mutex};
        status.states = g_states.counters();
        status.gbuffer = g_states.gbuffer();
        status.last_confirmation = g_last_confirmation;
    }

    status.frames = g_frame.load();
    status.total = load_counters();
}

std::uint64_t end_frame() {
    const auto now = load_counters();

    std::scoped_lock _{g_frame_mutex};

    for (std::size_t i = 0; i < now.size(); ++i) {
        g_last_frame[i] = now[i] - g_frame_start[i];
    }

    g_frame_start = now;
    return ++g_frame;
}

Counters last_frame_counters() {
    std::scoped_lock _{g_frame_mutex};
    return g_last_frame;
}
} // namespace rr::d3d12
