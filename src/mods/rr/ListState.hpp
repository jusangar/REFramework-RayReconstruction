#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <d3d12.h>

#include "D3D12Hooks.hpp"
#include "PsoRegistry.hpp"
#include "core/Identification.hpp"
#include "core/ResourceStates.hpp"
#include "core/ViewTable.hpp"

namespace rr::d3d12 {
// D3D12 caps a root signature at 64 DWORDs, so it never has more parameters than this.
inline constexpr std::uint32_t MAX_ROOT_PARAMETERS = 64;

// One root argument, as the game set it.
struct RootArgument {
    enum class Kind : std::uint8_t { Table, Constants, Cbv, Srv, Uav };

    Kind kind{};
    std::uint64_t value{};         // the GPU descriptor handle of a table, or the GPU virtual address of a root view
    std::uint64_t constant_mask{}; // root constants: which of the 32-bit values were set
    std::array<std::uint32_t, MAX_ROOT_PARAMETERS> constants{};
};

// The root signature of one of the two pipelines, graphics or compute, and its arguments.
class RootBindings {
public:
    ID3D12RootSignature* root_signature() const { return m_root_signature; }

    // A different root signature makes every argument stale; setting the same one again keeps them.
    void set_root_signature(ID3D12RootSignature* root_signature) {
        if (root_signature != m_root_signature) {
            m_set = 0;
        }

        m_root_signature = root_signature;
    }

    // The argument at a root parameter, to be written: a fresh one if it was not set.
    RootArgument& write(std::uint32_t index, RootArgument::Kind kind) {
        auto& argument = m_arguments[index];

        if ((m_set & bit(index)) == 0) {
            argument = RootArgument{};
            m_set |= bit(index);
        }

        argument.kind = kind;
        return argument;
    }

    // Which root parameters the game set since the root signature changed, one bit each.
    std::uint64_t set_mask() const { return m_set; }

    // The argument at a root parameter of set_mask.
    const RootArgument& argument(std::uint32_t index) const { return m_arguments[index]; }

    void clear() {
        m_root_signature = nullptr;
        m_set = 0;
    }

private:
    static std::uint64_t bit(std::uint32_t index) { return std::uint64_t{1} << index; }

    ID3D12RootSignature* m_root_signature{};
    std::array<RootArgument, MAX_ROOT_PARAMETERS> m_arguments{};
    std::uint64_t m_set{};
};

// What the game has recorded and bound on a command list since its last Reset: the shadow state. It
// says what the game bound at the evaluation point, and what to bind again after RR's own work.
struct ListState {
    D3D12_COMMAND_LIST_TYPE type{};

    ID3D12PipelineState* pso{};
    std::optional<PsoShaders> shaders{}; // the digests of pso; none if the registry does not know it

    std::array<ID3D12DescriptorHeap*, 2> heaps{};
    std::uint32_t heap_count{};

    RootBindings graphics{};
    RootBindings compute{};

    // As OMSetRenderTargets bound them, one handle per target, with the resources the views name;
    // a resource is null if the view is unknown or a null descriptor.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    std::array<ID3D12Resource*, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> render_targets{};
    std::uint32_t rtv_count{};
    std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> dsv{};
    core::View depth{}; // the view at dsv, with its flags; no resource if it is unknown

    // As RSSetViewports and RSSetScissorRects bound them.
    std::array<D3D12_VIEWPORT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    std::uint32_t viewport_count{};
    std::array<D3D12_RECT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
    std::uint32_t scissor_count{};

    // What the state registry replays when the list is submitted: the recording's transitions, its
    // G-buffer pass and its TAA draw, in order.
    core::Recording recording{};

    // The first identification of the evaluation point, fed with the recording's transitions,
    // dispatches and draws.
    core::BarrierRule rule{};

    // Where the last Dispatch read the AmbientBRDF LUT, and where the Dispatch of the PT sequence the
    // barrier rule chose read it, whatever its compute shader: each setting of the PT is another one.
    // The rule only tells at the transition that closes the sequence, when the bindings of its Dispatch
    // may be gone.
    AmbientBrdfDescriptor last_dispatch_lut{};
    AmbientBrdfDescriptor ambient_brdf{}; // the PT's

    // ExecuteBundle ran on the list. The PSO and the root bindings a bundle sets stay on the list, and
    // the shadow state does not follow them: until ClearState or Reset, it no longer says what the
    // game bound.
    bool bundle_ran{};

    void set_pso(ID3D12PipelineState* new_pso, std::optional<PsoShaders> new_shaders) {
        pso = new_pso;
        shaders = new_shaders;
    }

    // ClearState: back to the state of a new list. What was recorded stays recorded.
    void clear_state(ID3D12PipelineState* initial, std::optional<PsoShaders> initial_shaders) {
        set_pso(initial, initial_shaders);
        heaps = {};
        heap_count = 0;
        graphics.clear();
        compute.clear();
        rtv_count = 0;
        dsv.reset();
        depth = {};
        viewport_count = 0;
        scissor_count = 0;
        bundle_ran = false;
    }

    // Reset: a new recording.
    void reset(ID3D12PipelineState* initial, std::optional<PsoShaders> initial_shaders) {
        clear_state(initial, initial_shaders);
        recording.reset();
        rule.reset();
        last_dispatch_lut = {};
        ambient_brdf = {};
    }
};
} // namespace rr::d3d12
