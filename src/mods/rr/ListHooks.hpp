#pragma once

// Internal to the D3D12 hooks: what the device hooks (D3D12Hooks.cpp) and the command list hooks
// (ListHooks.cpp) need from each other.

#include <cstdint>
#include <optional>

#include <d3d12.h>

#include "D3D12Hooks.hpp"
#include "core/RootLayout.hpp"
#include "core/ViewTable.hpp"

namespace rr::d3d12 {
// Provided by ListHooks.cpp.

// A list the device created: patches its class vtable in place if it is a new one, and starts its
// shadow state.
void on_list_created(IUnknown* list, ID3D12PipelineState* initial, const char* where);

// A resource the device created, with its subresources. followed: a texture without simultaneous
// access, whose state the registry follows between submissions.
void on_resource_created(void* resource, bool followed, std::uint32_t subresources);

// ExecuteCommandLists, on any queue, before the runtime gets the lists: the registry replays them in
// this order.
void on_lists_submitted(UINT count, ID3D12CommandList* const* lists);

void fill_list_status(Status& status);

// Provided by D3D12Hooks.cpp.

std::optional<core::View> find_view(D3D12_CPU_DESCRIPTOR_HANDLE handle);
UINT rtv_increment();
UINT srv_increment(); // CBV_SRV_UAV
bool debug_layer_active();

// What the hooks learned of a root signature as the device created it.
struct RootSignatureInfo {
    std::optional<core::RegisterBinding> ambient_brdf{}; // where it binds the profile's AmbientBRDF register
};

// None if the root signature was not created through the hooks, or could not be read.
std::optional<RootSignatureInfo> find_root_signature(ID3D12RootSignature* root_signature);

// The compute shader of the PT's Dispatch at a draw the barrier rule anchored: the first time, its bytecode
// goes to reframework/rr/ for dxc.
void note_anchored_pt_cs(const std::optional<core::ShaderDigest>& cs);
} // namespace rr::d3d12
