#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <climits>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wrl/client.h>

#include <spdlog/spdlog.h>

#include <utility/FunctionHook.hpp>
#include <utility/VtableHook.hpp>

#include "REFramework.hpp"

#include "ListHooks.hpp"
#include "PsoSubstitution.hpp"

using Microsoft::WRL::ComPtr;

namespace rr::d3d12 {
namespace {
// Slots of the device and pipeline library vtables, checked against the C vtables of d3d12.h in
// Windows SDK 10.0.26100 (without the duplicates under #if !defined(_WIN32)).
namespace slot {
constexpr std::uint32_t DEVICE_CREATE_COMMAND_QUEUE = 8;
constexpr std::uint32_t DEVICE_CREATE_GRAPHICS_PSO = 10;
constexpr std::uint32_t DEVICE_CREATE_COMPUTE_PSO = 11;
constexpr std::uint32_t DEVICE_CREATE_COMMAND_LIST = 12;
constexpr std::uint32_t DEVICE_CREATE_ROOT_SIGNATURE = 16;
constexpr std::uint32_t DEVICE_CREATE_RTV = 20;
constexpr std::uint32_t DEVICE_CREATE_DSV = 21;
constexpr std::uint32_t DEVICE_COPY_DESCRIPTORS = 23;
constexpr std::uint32_t DEVICE_COPY_DESCRIPTORS_SIMPLE = 24;
constexpr std::uint32_t DEVICE_CREATE_COMMITTED_RESOURCE = 27;
constexpr std::uint32_t DEVICE_CREATE_PLACED_RESOURCE = 29;
constexpr std::uint32_t DEVICE_CREATE_RESERVED_RESOURCE = 30;
constexpr std::uint32_t DEVICE1_CREATE_PIPELINE_LIBRARY = 44;
constexpr std::uint32_t DEVICE2_CREATE_PIPELINE_STATE = 47;
constexpr std::uint32_t DEVICE4_CREATE_COMMAND_LIST1 = 51;
constexpr std::uint32_t DEVICE4_CREATE_COMMITTED_RESOURCE1 = 53;
constexpr std::uint32_t DEVICE4_CREATE_RESERVED_RESOURCE1 = 55;
constexpr std::uint32_t DEVICE8_CREATE_COMMITTED_RESOURCE2 = 69;
constexpr std::uint32_t DEVICE8_CREATE_PLACED_RESOURCE1 = 70;
constexpr std::uint32_t DEVICE9_CREATE_COMMAND_QUEUE1 = 75;
constexpr std::uint32_t DEVICE10_CREATE_COMMITTED_RESOURCE3 = 76;
constexpr std::uint32_t DEVICE10_CREATE_PLACED_RESOURCE2 = 77;
constexpr std::uint32_t DEVICE10_CREATE_RESERVED_RESOURCE2 = 78;
constexpr std::uint32_t LIBRARY_LOAD_GRAPHICS_PIPELINE = 9;
constexpr std::uint32_t LIBRARY_LOAD_COMPUTE_PIPELINE = 10;
constexpr std::uint32_t LIBRARY1_LOAD_PIPELINE = 13;
constexpr std::uint32_t QUEUE_EXECUTE_COMMAND_LISTS = 10;
} // namespace slot

using D3D12CreateDeviceFn = HRESULT(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateGraphicsPsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateComputePsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreatePipelineStateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using CreatePipelineLibraryFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device1*, const void*, SIZE_T, REFIID, void**);
using LoadGraphicsPipelineFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using LoadComputePipelineFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12PipelineLibrary*, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using LoadPipelineFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using CreateCommandListFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using CreateCommandList1Fn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
using CreateRootSignatureFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using CreateRtvFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateDsvFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescriptorsFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
    const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescriptorsSimpleFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using CreateCommittedResourceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlacedResourceFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateReservedResourceFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateCommittedResource1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreateReservedResource1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreateCommittedResource2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreatePlacedResource1Fn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateCommittedResource3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32,
    const DXGI_FORMAT*, REFIID, void**);
using CreatePlacedResource2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
    D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**);
using CreateReservedResource2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_RESOURCE_DESC*, D3D12_BARRIER_LAYOUT,
    const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
using CreateCommandQueueFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using CreateCommandQueue1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device9*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, REFIID, void**);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// Written before the slot that calls them is hooked.
struct {
    std::atomic<CreateGraphicsPsoFn> create_graphics_pso{};
    std::atomic<CreateComputePsoFn> create_compute_pso{};
    std::atomic<CreatePipelineStateFn> create_pipeline_state{};
    std::atomic<CreatePipelineLibraryFn> create_pipeline_library{};
    std::atomic<LoadGraphicsPipelineFn> load_graphics_pipeline{};
    std::atomic<LoadComputePipelineFn> load_compute_pipeline{};
    std::atomic<LoadPipelineFn> load_pipeline{};
    std::atomic<CreateCommandListFn> create_command_list{};
    std::atomic<CreateCommandList1Fn> create_command_list1{};
    std::atomic<CreateRootSignatureFn> create_root_signature{};
    std::atomic<CreateRtvFn> create_rtv{};
    std::atomic<CreateDsvFn> create_dsv{};
    std::atomic<CopyDescriptorsFn> copy_descriptors{};
    std::atomic<CopyDescriptorsSimpleFn> copy_descriptors_simple{};
    std::atomic<CreateCommittedResourceFn> create_committed_resource{};
    std::atomic<CreatePlacedResourceFn> create_placed_resource{};
    std::atomic<CreateReservedResourceFn> create_reserved_resource{};
    std::atomic<CreateCommittedResource1Fn> create_committed_resource1{};
    std::atomic<CreateReservedResource1Fn> create_reserved_resource1{};
    std::atomic<CreateCommittedResource2Fn> create_committed_resource2{};
    std::atomic<CreatePlacedResource1Fn> create_placed_resource1{};
    std::atomic<CreateCommittedResource3Fn> create_committed_resource3{};
    std::atomic<CreatePlacedResource2Fn> create_placed_resource2{};
    std::atomic<CreateReservedResource2Fn> create_reserved_resource2{};
    std::atomic<CreateCommandQueueFn> create_command_queue{};
    std::atomic<CreateCommandQueue1Fn> create_command_queue1{};
} g_orig{};

std::mutex g_install_mutex{};
bool g_installed{};                                // under g_install_mutex
std::unique_ptr<PsoRegistry> g_registry_storage{}; // under g_install_mutex
std::atomic<PsoRegistry*> g_registry{};
std::unique_ptr<FunctionHook> g_create_device_hook{};

std::mutex g_status_mutex{};
std::string g_create_device_status{"not installed: the game has no RR profile, or d3d12.dll never loaded"}; // under g_status_mutex
std::string g_device_status{"not hooked: D3D12CreateDevice not called yet"};                                // under g_status_mutex
std::string g_debug_layer_status{"off (reframework/rr/d3d12_debug_layer.txt enables it)"};                  // under g_status_mutex

void set_status(std::string& status, std::string text) {
    std::scoped_lock _{g_status_mutex};
    status = std::move(text);
}

/////////////////////////////////////////////////////////////////////////////
// Views
/////////////////////////////////////////////////////////////////////////////

std::shared_mutex g_views_mutex{};
core::ViewTable g_views{}; // under g_views_mutex
std::atomic<UINT> g_rtv_increment{};
std::atomic<UINT> g_dsv_increment{};

UINT increment_of(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
        return g_rtv_increment;
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        return g_dsv_increment;
    default:
        return 0; // the copies of other heap types do not concern the view table
    }
}

// The ranges of one side of CopyDescriptors; without sizes, every range has one descriptor.
std::vector<core::DescriptorRange> ranges_of(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* starts, const UINT* sizes) {
    std::vector<core::DescriptorRange> ranges(count);

    for (UINT i = 0; i < count; ++i) {
        ranges[i] = {starts[i].ptr, sizes != nullptr ? sizes[i] : 1};
    }

    return ranges;
}

void STDMETHODCALLTYPE create_rtv(
    ID3D12Device* self, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    g_orig.create_rtv.load()(self, resource, desc, handle);

    std::unique_lock _{g_views_mutex};
    g_views.write(handle.ptr, {resource, 0});
}

void STDMETHODCALLTYPE create_dsv(
    ID3D12Device* self, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    g_orig.create_dsv.load()(self, resource, desc, handle);

    std::unique_lock _{g_views_mutex};
    g_views.write(handle.ptr, {resource, desc != nullptr ? static_cast<std::uint32_t>(desc->Flags) : 0u});
}

void STDMETHODCALLTYPE copy_descriptors(ID3D12Device* self, UINT dst_count, const D3D12_CPU_DESCRIPTOR_HANDLE* dst_starts,
    const UINT* dst_sizes, UINT src_count, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts, const UINT* src_sizes,
    D3D12_DESCRIPTOR_HEAP_TYPE type) {
    g_orig.copy_descriptors.load()(self, dst_count, dst_starts, dst_sizes, src_count, src_starts, src_sizes, type);

    if (const auto increment = increment_of(type); increment != 0 && dst_starts != nullptr && src_starts != nullptr) {
        const auto dst = ranges_of(dst_count, dst_starts, dst_sizes);
        const auto src = ranges_of(src_count, src_starts, src_sizes);

        std::unique_lock _{g_views_mutex};
        g_views.copy(dst, src, increment);
    }
}

void STDMETHODCALLTYPE copy_descriptors_simple(
    ID3D12Device* self, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    g_orig.copy_descriptors_simple.load()(self, count, dst, src, type);

    if (const auto increment = increment_of(type); increment != 0) {
        const core::DescriptorRange dst_range{dst.ptr, count};
        const core::DescriptorRange src_range{src.ptr, count};

        std::unique_lock _{g_views_mutex};
        g_views.copy({&dst_range, 1}, {&src_range, 1}, increment);
    }
}

/////////////////////////////////////////////////////////////////////////////
// Root signatures
/////////////////////////////////////////////////////////////////////////////

// What every root signature the device created binds at the profile's AmbientBRDF register, as the PT's
// Dispatch needs it. A new root signature can take the address of a freed one, and then replaces its
// entry. Every Dispatch of every recording thread looks here, so only a new root signature takes the lock
// exclusively.
std::shared_mutex g_root_signatures_mutex{};
std::unordered_map<void*, RootSignatureInfo> g_root_signatures{}; // under g_root_signatures_mutex
std::atomic<std::uint64_t> g_root_signature_failures{};
std::atomic<UINT> g_srv_increment{};

core::DescriptorType descriptor_type(D3D12_DESCRIPTOR_RANGE_TYPE type) {
    return static_cast<core::DescriptorType>(type);
}

static_assert(static_cast<int>(core::DescriptorType::Srv) == D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
static_assert(static_cast<int>(core::DescriptorType::Uav) == D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
static_assert(static_cast<int>(core::DescriptorType::Cbv) == D3D12_DESCRIPTOR_RANGE_TYPE_CBV);
static_assert(static_cast<int>(core::DescriptorType::Sampler) == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
static_assert(core::UNBOUNDED_RANGE == UINT_MAX && core::APPEND_RANGE == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);

// The blob goes to the deserializer whole: a serialized root signature is a container with an RTS0 part,
// and the deserializer refuses the part alone. None if it cannot be read.
std::optional<std::vector<core::RootParameter>> read_root_signature(const void* blob, SIZE_T size) {
    ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer{};
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* versioned{};

    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&deserializer))) ||
        FAILED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &versioned)) || versioned == nullptr) {
        return std::nullopt;
    }

    const auto& desc = versioned->Desc_1_1;
    std::vector<core::RootParameter> parameters(desc.NumParameters);

    for (UINT i = 0; i < desc.NumParameters; ++i) {
        const auto& p = desc.pParameters[i];
        auto& out = parameters[i];

        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            out.kind = core::RootParameter::Kind::Table;

            for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
                const auto& range = p.DescriptorTable.pDescriptorRanges[r];
                out.ranges.push_back({
                    .type = descriptor_type(range.RangeType),
                    .count = range.NumDescriptors,
                    .base_register = range.BaseShaderRegister,
                    .space = range.RegisterSpace,
                    .offset = range.OffsetInDescriptorsFromTableStart,
                });
            }

            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            out = {.kind = core::RootParameter::Kind::Constants, .shader_register = p.Constants.ShaderRegister, .space = p.Constants.RegisterSpace};
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV: {
            const auto kind = p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ? core::RootParameter::Kind::Cbv
                : p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV           ? core::RootParameter::Kind::Srv
                                                                             : core::RootParameter::Kind::Uav;
            out = {.kind = kind, .shader_register = p.Descriptor.ShaderRegister, .space = p.Descriptor.RegisterSpace};
            break;
        }
        default:
            return std::nullopt;
        }
    }

    return parameters;
}

HRESULT STDMETHODCALLTYPE create_root_signature(ID3D12Device* self, UINT node, const void* blob, SIZE_T size, REFIID riid, void** out) {
    const auto hr = g_orig.create_root_signature.load()(self, node, blob, size, riid, out);

    if (FAILED(hr) || blob == nullptr || out == nullptr || *out == nullptr) {
        return hr;
    }

    const auto parameters = read_root_signature(blob, size);
    std::unique_lock _{g_root_signatures_mutex};

    if (!parameters) {
        ++g_root_signature_failures;
        g_root_signatures.erase(*out);
        return hr;
    }

    const auto& lut = g_registry.load()->profile().ambient_brdf;
    g_root_signatures[*out] = {.ambient_brdf = core::find_register(*parameters, core::DescriptorType::Srv, lut.srv_register, lut.space)};
    return hr;
}

/////////////////////////////////////////////////////////////////////////////
// PSOs
/////////////////////////////////////////////////////////////////////////////

// Each variant of the PT the barrier rule anchors goes once into reframework/rr/pt_cs_<digest>.dxil, for
// dxc -dumpbin: which resource it declares at the AmbientBRDF register. The bytecode only exists while its
// PSO is created, and only the anchor says which compute shader is the PT's: until then the hooks keep a
// copy of every compute shader whose root signature binds the register in a table and that declares an
// SRV there. Its PSV0 part says what kind, and the LUT's copy only reads a Texture2D; the dump lets dxc
// confirm it. It is Capcom's code: it stays in the game's folder.

// Insurance on the copies kept. Before the filter on what the shader declares, 1,201 of RE2's compute
// shaders passed it in one session, 31.5 MB.
constexpr std::uint64_t MAX_RETAINED_BYTES = std::uint64_t{64} << 20;

std::mutex g_pt_cs_mutex{};
std::map<decltype(core::ShaderDigest::bytes), std::vector<std::uint8_t>> g_retained_cs{}; // by digest, under g_pt_cs_mutex
PtCsStatus g_pt_cs{};                                                                     // under g_pt_cs_mutex
std::optional<core::ShaderDigest> g_anchored_pt_cs{}; // since the last take_anchored_pt_cs, under g_pt_cs_mutex

std::filesystem::path pt_cs_path(const core::ShaderDigest& digest) {
    return REFramework::get_persistent_dir() / "reframework" / "rr" / std::format("pt_cs_{}.dxil", core::to_string(digest));
}

bool anchored_before(const core::ShaderDigest& digest) {
    return std::any_of(g_pt_cs.variants.begin(), g_pt_cs.variants.end(), [&](const PtCsVariant& v) { return v.digest == digest; });
}

// What a compute shader declares at the profile's AmbientBRDF register.
core::SrvDeclaration lut_declaration(const D3D12_SHADER_BYTECODE& cs) {
    if (cs.pShaderBytecode == nullptr || cs.BytecodeLength == 0) {
        return core::SrvDeclaration::Unknown;
    }

    const auto& lut = g_registry.load()->profile().ambient_brdf;
    return core::read_srv_declaration({static_cast<const std::uint8_t*>(cs.pShaderBytecode), cs.BytecodeLength}, lut.srv_register, lut.space);
}

// At the creation of a compute PSO, with the root signature it was created with (none if the shader
// carries its own) and what the shader declares at the register.
void retain_cs(const D3D12_SHADER_BYTECODE& cs, ID3D12RootSignature* root_signature, core::SrvDeclaration declaration) {
    on_compute_pso_created(cs, root_signature);

    const auto digest = digest_of(cs);

    if (!digest || declaration == core::SrvDeclaration::None) {
        return;
    }

    const auto info = root_signature != nullptr ? find_root_signature(root_signature) : std::nullopt;
    std::scoped_lock _{g_pt_cs_mutex};

    if (!info) {
        ++g_pt_cs.without_root_signature;
        return;
    }

    if (!info->ambient_brdf || info->ambient_brdf->kind != core::RootParameter::Kind::Table || g_retained_cs.contains(digest->bytes) ||
        anchored_before(*digest)) {
        return;
    }

    if (g_pt_cs.retained_bytes + cs.BytecodeLength > MAX_RETAINED_BYTES) {
        ++g_pt_cs.over_limit;
        return;
    }

    const auto bytes = static_cast<const std::uint8_t*>(cs.pShaderBytecode);
    g_retained_cs.emplace(digest->bytes, std::vector<std::uint8_t>(bytes, bytes + cs.BytecodeLength));
    ++g_pt_cs.retained;
    g_pt_cs.retained_bytes += cs.BytecodeLength;
}

PsoShaders graphics_shaders(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    return {
        .vs = digest_of(desc.VS), .ps = digest_of(desc.PS), .ds = digest_of(desc.DS), .hs = digest_of(desc.HS), .gs = digest_of(desc.GS)};
}

bool graphics_lacks_digest(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    return lacks_digest(desc.VS) || lacks_digest(desc.PS) || lacks_digest(desc.DS) || lacks_digest(desc.HS) || lacks_digest(desc.GS);
}

// The layout of the CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT helpers of d3dx12.h.
template <typename Inner> struct alignas(void*) StreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    Inner inner;
};

// The shaders of a pipeline state stream, the bytecode of its compute shader and its root signature. False
// if a subobject is unknown, which stops the walk: its size, and so where the next one starts, is unknown
// too.
bool parse_stream(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, PsoShaders& shaders, bool& without_digest, const D3D12_SHADER_BYTECODE*& cs,
    ID3D12RootSignature*& root_signature) {
    if (desc == nullptr || desc->pPipelineStateSubobjectStream == nullptr) {
        return false;
    }

    const auto base = static_cast<const std::uint8_t*>(desc->pPipelineStateSubobjectStream);

    for (std::size_t offset = 0; offset + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= desc->SizeInBytes;) {
        const auto p = base + offset;
        const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
        const auto& shader = reinterpret_cast<const StreamSubobject<D3D12_SHADER_BYTECODE>*>(p)->inner;
        std::size_t size = 0;

        // A shader subobject: its digest goes to its stage.
        const auto read = [&](std::optional<core::ShaderDigest>& stage) {
            stage = digest_of(shader);
            without_digest = without_digest || lacks_digest(shader);
            return sizeof(StreamSubobject<D3D12_SHADER_BYTECODE>);
        };

        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            size = read(shaders.vs);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            size = read(shaders.ps);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            size = read(shaders.cs);
            cs = &shader;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            size = read(shaders.ds);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            size = read(shaders.hs);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            size = read(shaders.gs);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            size = read(shaders.as);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            size = read(shaders.ms);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            root_signature = reinterpret_cast<const StreamSubobject<ID3D12RootSignature*>*>(p)->inner;
            size = sizeof(StreamSubobject<ID3D12RootSignature*>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            size = sizeof(StreamSubobject<D3D12_STREAM_OUTPUT_DESC>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
            size = sizeof(StreamSubobject<D3D12_BLEND_DESC>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            size = sizeof(StreamSubobject<UINT>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
            size = sizeof(StreamSubobject<D3D12_RASTERIZER_DESC>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            size = sizeof(StreamSubobject<D3D12_DEPTH_STENCIL_DESC>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            size = sizeof(StreamSubobject<D3D12_INPUT_LAYOUT_DESC>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            size = sizeof(StreamSubobject<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            size = sizeof(StreamSubobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
            size = sizeof(StreamSubobject<D3D12_RT_FORMAT_ARRAY>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            size = sizeof(StreamSubobject<DXGI_FORMAT>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            size = sizeof(StreamSubobject<DXGI_SAMPLE_DESC>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            size = sizeof(StreamSubobject<UINT>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            size = sizeof(StreamSubobject<D3D12_CACHED_PIPELINE_STATE>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            size = sizeof(StreamSubobject<D3D12_PIPELINE_STATE_FLAGS>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            size = sizeof(StreamSubobject<D3D12_DEPTH_STENCIL_DESC1>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            size = sizeof(StreamSubobject<D3D12_VIEW_INSTANCING_DESC>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
            size = sizeof(StreamSubobject<D3D12_DEPTH_STENCIL_DESC2>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
            size = sizeof(StreamSubobject<D3D12_RASTERIZER_DESC1>);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
            size = sizeof(StreamSubobject<D3D12_RASTERIZER_DESC2>);
            break;
        default:
            return false;
        }

        offset += size;
    }

    return true;
}

void add_stream_pso(void* pso, PsoSource source, const D3D12_PIPELINE_STATE_STREAM_DESC* desc) {
    PsoShaders shaders{};
    bool without_digest{};
    const D3D12_SHADER_BYTECODE* cs{};
    ID3D12RootSignature* root_signature{};
    const auto registry = g_registry.load();

    if (!parse_stream(desc, shaders, without_digest, cs, root_signature)) {
        registry->note_stream_parse_failure();
    }

    if (cs != nullptr) {
        shaders.cs_lut = lut_declaration(*cs);
        shaders.cs_root_signature = root_signature;
    }

    registry->add(static_cast<ID3D12PipelineState*>(pso), source, shaders, without_digest);

    if (cs != nullptr) {
        retain_cs(*cs, root_signature, shaders.cs_lut);
    }
}

HRESULT STDMETHODCALLTYPE create_graphics_pso(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    const auto hr = g_orig.create_graphics_pso.load()(self, desc, riid, out);

    if (SUCCEEDED(hr) && desc != nullptr && out != nullptr && *out != nullptr) {
        g_registry.load()->add(
            static_cast<ID3D12PipelineState*>(*out), PsoSource::Graphics, graphics_shaders(*desc), graphics_lacks_digest(*desc));
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE create_compute_pso(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    const auto hr = g_orig.create_compute_pso.load()(self, desc, riid, out);

    if (SUCCEEDED(hr) && desc != nullptr && out != nullptr && *out != nullptr) {
        const auto lut = lut_declaration(desc->CS);
        g_registry.load()->add(static_cast<ID3D12PipelineState*>(*out), PsoSource::Compute,
            {.cs = digest_of(desc->CS), .cs_lut = lut, .cs_root_signature = desc->pRootSignature}, lacks_digest(desc->CS));
        retain_cs(desc->CS, desc->pRootSignature, lut);
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE create_pipeline_state(
    ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    const auto hr = g_orig.create_pipeline_state.load()(self, desc, riid, out);

    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        add_stream_pso(*out, PsoSource::Stream, desc);
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE load_graphics_pipeline(
    ID3D12PipelineLibrary* self, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    const auto hr = g_orig.load_graphics_pipeline.load()(self, name, desc, riid, out);

    if (SUCCEEDED(hr) && desc != nullptr && out != nullptr && *out != nullptr) {
        g_registry.load()->add(
            static_cast<ID3D12PipelineState*>(*out), PsoSource::LibraryGraphics, graphics_shaders(*desc), graphics_lacks_digest(*desc));
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE load_compute_pipeline(
    ID3D12PipelineLibrary* self, LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    const auto hr = g_orig.load_compute_pipeline.load()(self, name, desc, riid, out);

    if (SUCCEEDED(hr) && desc != nullptr && out != nullptr && *out != nullptr) {
        const auto lut = lut_declaration(desc->CS);
        g_registry.load()->add(static_cast<ID3D12PipelineState*>(*out), PsoSource::LibraryCompute,
            {.cs = digest_of(desc->CS), .cs_lut = lut, .cs_root_signature = desc->pRootSignature}, lacks_digest(desc->CS));
        retain_cs(desc->CS, desc->pRootSignature, lut);
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE load_pipeline(
    ID3D12PipelineLibrary1* self, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    const auto hr = g_orig.load_pipeline.load()(self, name, desc, riid, out);

    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        add_stream_pso(*out, PsoSource::LibraryStream, desc);
    }

    return hr;
}

/////////////////////////////////////////////////////////////////////////////
// Resources
/////////////////////////////////////////////////////////////////////////////

// Mips x array slices x planes: the subresources a barrier can name one by one. Among the formats a game
// renders to, only the depth-stencil ones with a stencil have a second plane.
template <typename Desc> std::uint32_t subresource_count(const Desc& desc) {
    const auto three_d = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const UINT64 extent = std::max<UINT64>({desc.Width, desc.Height, three_d ? desc.DepthOrArraySize : 1u});

    // MipLevels 0 asks for the full chain.
    const auto mips = desc.MipLevels != 0 ? std::uint32_t{desc.MipLevels} : static_cast<std::uint32_t>(std::bit_width(extent));
    const auto slices = three_d ? 1u : std::uint32_t{desc.DepthOrArraySize};
    auto planes = 1u;

    switch (desc.Format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        planes = 2;
        break;
    default:
        break;
    }

    return mips * slices * planes;
}

// The registry starts every resource again when the device creates it, and not by its address, format
// and size as a prototype in the game did. D3D12_RESOURCE_DESC1 has the same fields as D3D12_RESOURCE_DESC.
template <typename Desc> void note_created(HRESULT hr, const Desc* desc, void** out) {
    if (FAILED(hr) || desc == nullptr || out == nullptr || *out == nullptr) {
        return;
    }

    const auto followed =
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) == 0;
    on_resource_created(*out, followed, followed ? subresource_count(*desc) : 1);
}

HRESULT STDMETHODCALLTYPE create_committed_resource(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const auto hr = g_orig.create_committed_resource.load()(self, heap, flags, desc, state, clear, riid, out);
    note_created(hr, desc, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_placed_resource(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const auto hr = g_orig.create_placed_resource.load()(self, heap, offset, desc, state, clear, riid, out);
    note_created(hr, desc, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_reserved_resource(ID3D12Device* self, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const auto hr = g_orig.create_reserved_resource.load()(self, desc, state, clear, riid, out);
    note_created(hr, desc, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_committed_resource1(ID3D12Device4* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
    REFIID riid, void** out) {
    const auto hr = g_orig.create_committed_resource1.load()(self, heap, flags, desc, state, clear, session, riid, out);
    note_created(hr, desc, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_reserved_resource1(ID3D12Device4* self, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, REFIID riid, void** out) {
    const auto hr = g_orig.create_reserved_resource1.load()(self, desc, state, clear, session, riid, out);
    note_created(hr, desc, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_committed_resource2(ID3D12Device8* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
    REFIID riid, void** out) {
    const auto hr = g_orig.create_committed_resource2.load()(self, heap, flags, desc, state, clear, session, riid, out);
    note_created(hr, desc, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_placed_resource1(ID3D12Device8* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const auto hr = g_orig.create_placed_resource1.load()(self, heap, offset, desc, state, clear, riid, out);
    note_created(hr, desc, out);
    return hr;
}

// The next three take a layout of the enhanced barriers, which the registry does not follow; the
// legacy barriers on these resources it does.
HRESULT STDMETHODCALLTYPE create_committed_resource3(ID3D12Device10* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
    UINT32 castable_count, const DXGI_FORMAT* castable, REFIID riid, void** out) {
    const auto hr =
        g_orig.create_committed_resource3.load()(self, heap, flags, desc, layout, clear, session, castable_count, castable, riid, out);
    note_created(hr, desc, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_placed_resource2(ID3D12Device10* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
    D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear, UINT32 castable_count, const DXGI_FORMAT* castable, REFIID riid,
    void** out) {
    const auto hr = g_orig.create_placed_resource2.load()(self, heap, offset, desc, layout, clear, castable_count, castable, riid, out);
    note_created(hr, desc, out);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_reserved_resource2(ID3D12Device10* self, const D3D12_RESOURCE_DESC* desc, D3D12_BARRIER_LAYOUT layout,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, UINT32 castable_count, const DXGI_FORMAT* castable,
    REFIID riid, void** out) {
    const auto hr = g_orig.create_reserved_resource2.load()(self, desc, layout, clear, session, castable_count, castable, riid, out);
    note_created(hr, desc, out);
    return hr;
}

/////////////////////////////////////////////////////////////////////////////
// Command queues
/////////////////////////////////////////////////////////////////////////////

// Every queue is hooked as the device creates it, each with its own copy of its vtable: the registry
// replays every submission, whatever queue it goes to, as a prototype in the game did. Never unhooked or
// freed.
struct HookedQueue {
    void* queue{};
    std::atomic<ExecuteCommandListsFn> original{};
    std::unique_ptr<VtableHook> hook{};
};

constexpr std::uint32_t MAX_QUEUES = 32;
std::mutex g_queues_mutex{};
std::array<std::atomic<HookedQueue*>, MAX_QUEUES> g_queues{};
std::atomic<std::uint32_t> g_queue_count{};
std::string g_queues_report{"none yet"}; // under g_queues_mutex

HookedQueue* find_queue(void* queue) {
    const auto count = std::min<std::uint32_t>(g_queue_count.load(), MAX_QUEUES);

    for (std::uint32_t i = 0; i < count; ++i) {
        if (const auto hooked = g_queues[i].load(); hooked != nullptr && hooked->queue == queue) {
            return hooked;
        }
    }

    return nullptr;
}

void STDMETHODCALLTYPE execute_command_lists(ID3D12CommandQueue* self, UINT count, ID3D12CommandList* const* lists) {
    auto hooked = find_queue(self);

    // Not expected: every queue is published before its hook goes in, and never removed. It keeps a
    // lookup that misses from calling through null.
    if (hooked == nullptr) {
        hooked = g_queues[0].load();
    }

    if (!ReentryGuard::active() && lists != nullptr) {
        on_lists_submitted(count, lists);
    }

    const ReentryGuard guard{};
    hooked->original.load()(self, count, lists);
}

void hook_queue(IUnknown* unknown) try {
    ComPtr<ID3D12CommandQueue> queue{};

    if (unknown == nullptr || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&queue)))) {
        return;
    }

    std::scoped_lock _{g_queues_mutex};
    auto hook = std::make_unique<VtableHook>(queue.Get());
    const auto original = hook->get_method(slot::QUEUE_EXECUTE_COMMAND_LISTS).ptr();

    // A vtable already hooked would make the detour its own original.
    if (original == nullptr || original == reinterpret_cast<void*>(&execute_command_lists)) {
        spdlog::error("[RayReconstruction] Command queue {:x} not hooked: ExecuteCommandLists is {}",
            reinterpret_cast<std::uintptr_t>(queue.Get()), original == nullptr ? "missing" : "already hooked");
        return;
    }

    // A new queue can take the address of a destroyed one.
    auto hooked = find_queue(queue.Get());

    if (hooked == nullptr) {
        const auto index = g_queue_count.load();

        if (index >= MAX_QUEUES) {
            spdlog::error("[RayReconstruction] Command queue {:x} not hooked: already {} hooked",
                reinterpret_cast<std::uintptr_t>(queue.Get()), index);
            return;
        }

        hooked = new HookedQueue{queue.Get()};
        hooked->original = reinterpret_cast<ExecuteCommandListsFn>(original);
        g_queues[index] = hooked;
        ++g_queue_count;
    } else {
        hooked->original = reinterpret_cast<ExecuteCommandListsFn>(original);

        // The object the old hook patched may be gone, so it is never removed.
        (void)hooked->hook.release();
    }

    // On failure the hook goes, and with it the copy of the vtable: the queue is left as it was, and its
    // entry is never called.
    if (!hook->hook_method(slot::QUEUE_EXECUTE_COMMAND_LISTS, reinterpret_cast<std::uintptr_t>(&execute_command_lists))) {
        spdlog::error(
            "[RayReconstruction] Command queue {:x}: hooking ExecuteCommandLists failed", reinterpret_cast<std::uintptr_t>(queue.Get()));
        return;
    }

    hooked->hook = std::move(hook);

    const auto text = std::format("{:x} type {}", reinterpret_cast<std::uintptr_t>(queue.Get()), static_cast<int>(queue->GetDesc().Type));
    g_queues_report = g_queue_count == 1 ? text : g_queues_report + " | " + text;
    spdlog::info("[RayReconstruction] Hooked command queue {}", text);
} catch (const std::exception& e) {
    spdlog::error("[RayReconstruction] Hooking a command queue: {}", e.what());
}

HRESULT STDMETHODCALLTYPE create_command_queue(ID3D12Device* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out) {
    const auto hr = g_orig.create_command_queue.load()(self, desc, riid, out);

    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        hook_queue(static_cast<IUnknown*>(*out));
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE create_command_queue1(
    ID3D12Device9* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID creator, REFIID riid, void** out) {
    const auto hr = g_orig.create_command_queue1.load()(self, desc, creator, riid, out);

    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        hook_queue(static_cast<IUnknown*>(*out));
    }

    return hr;
}

/////////////////////////////////////////////////////////////////////////////
// Device, pipeline libraries and command lists
/////////////////////////////////////////////////////////////////////////////

// Never unhooked or freed: a device or library may still be in use, and every hook routes through the
// same originals.
std::mutex g_device_mutex{};
std::vector<std::unique_ptr<VtableHook>> g_device_hooks{};  // under g_device_mutex
std::vector<void*> g_device_vtables{};                      // the hooks' copies, under g_device_mutex
std::vector<std::unique_ptr<VtableHook>> g_library_hooks{}; // under g_device_mutex

// D3D12CreateDevice gives the same device again for the same adapter.
bool is_hooked_device(void* device) {
    return std::find(g_device_vtables.begin(), g_device_vtables.end(), *static_cast<void**>(device)) != g_device_vtables.end();
}

std::string slots_report(const std::string& failures) {
    return failures.empty() ? std::string{"all slots"} : failures;
}

// Takes the original from the hook's copy of the vtable before the detour goes in, so that a detour
// never runs without its original.
template <typename Fn>
void hook_slot(VtableHook& hook, std::uint32_t index, void* detour, std::atomic<Fn>& original, std::string& report, const char* name) {
    const auto existing = hook.get_method(index).ptr();

    if (existing == nullptr) {
        report += std::format("{}[{}] missing; ", name, index);
        return;
    }

    // An object whose vtable is already a hooked copy would make the detour its own original.
    if (existing == detour) {
        report += std::format("{}[{}] already hooked; ", name, index);
        return;
    }

    original = reinterpret_cast<Fn>(existing);

    if (!hook.hook_method(index, reinterpret_cast<std::uintptr_t>(detour))) {
        report += std::format("{}[{}] failed; ", name, index);
    }
}

void hook_library(IUnknown* unknown) {
    ComPtr<ID3D12PipelineLibrary> library{};

    if (unknown == nullptr || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&library)))) {
        return;
    }

    std::scoped_lock _{g_device_mutex};
    auto hook = std::make_unique<VtableHook>(library.Get());
    std::string failures{};
    hook_slot(*hook, slot::LIBRARY_LOAD_GRAPHICS_PIPELINE, reinterpret_cast<void*>(&load_graphics_pipeline), g_orig.load_graphics_pipeline,
        failures, "LoadGraphicsPipeline");
    hook_slot(*hook, slot::LIBRARY_LOAD_COMPUTE_PIPELINE, reinterpret_cast<void*>(&load_compute_pipeline), g_orig.load_compute_pipeline,
        failures, "LoadComputePipeline");

    // ID3D12PipelineLibrary1 adds LoadPipeline; an older library has no such slot.
    if (ComPtr<ID3D12PipelineLibrary1> library1{}; SUCCEEDED(library.As(&library1))) {
        hook_slot(
            *hook, slot::LIBRARY1_LOAD_PIPELINE, reinterpret_cast<void*>(&load_pipeline), g_orig.load_pipeline, failures, "LoadPipeline");
    }

    g_library_hooks.push_back(std::move(hook));
    spdlog::info(
        "[RayReconstruction] Hooked pipeline library {:x}: {}", reinterpret_cast<std::uintptr_t>(library.Get()), slots_report(failures));
}

HRESULT STDMETHODCALLTYPE create_pipeline_library(ID3D12Device1* self, const void* blob, SIZE_T length, REFIID riid, void** out) {
    const auto hr = g_orig.create_pipeline_library.load()(self, blob, length, riid, out);
    g_registry.load()->note_library(SUCCEEDED(hr));

    // A blob from another driver fails, and the game then builds its PSOs without the library.
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        hook_library(static_cast<IUnknown*>(*out));
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE create_command_list(ID3D12Device* self, UINT node, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial, REFIID riid, void** out) {
    const auto hr = g_orig.create_command_list.load()(self, node, type, allocator, initial, riid, out);

    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        on_list_created(static_cast<IUnknown*>(*out), initial, "at CreateCommandList");
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE create_command_list1(
    ID3D12Device4* self, UINT node, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** out) {
    const auto hr = g_orig.create_command_list1.load()(self, node, type, flags, riid, out);

    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        on_list_created(static_cast<IUnknown*>(*out), nullptr, "at CreateCommandList1");
    }

    return hr;
}

/////////////////////////////////////////////////////////////////////////////
// Debug layer
/////////////////////////////////////////////////////////////////////////////

std::atomic<bool> g_debug_layer_active{};
std::atomic<bool> g_debug_layer_enabled_by_mod{};
std::atomic<std::uint64_t> g_debug_messages{};
std::atomic<std::uint64_t> g_debug_errors{};
std::atomic<std::uint64_t> g_debug_messages_naming_mod{};
std::mutex g_debug_log_mutex{};
std::unordered_map<int, std::uint32_t> g_debug_logged{}; // per message ID, under g_debug_log_mutex

// The prefix, in lower case, of the names the Mod gives the D3D12 objects it creates: the command list,
// allocator and fence that create the feature, the guide pass's root signatures, PSOs, heap and textures,
// and the fence that waits for the evaluations. NGX names its own.
constexpr std::string_view MOD_OBJECT_PREFIX{"rayreconstruction"};

bool names_mod(std::string_view description) {
    std::string lower{description};
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(MOD_OBJECT_PREFIX) != std::string::npos;
}

// Logs the errors as the layer emits them, so that the message behind a crash reaches the log.
void __stdcall on_debug_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void*) {
    const std::string_view text{description != nullptr ? description : ""};
    const auto error = severity <= D3D12_MESSAGE_SEVERITY_ERROR;
    const auto mod = names_mod(text);

    ++g_debug_messages;
    g_debug_errors += error ? 1 : 0;
    g_debug_messages_naming_mod += mod ? 1 : 0;

    if (!error && !mod) {
        return;
    }

    std::scoped_lock _{g_debug_log_mutex};

    if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || mod || g_debug_logged[static_cast<int>(id)]++ < 5) {
        spdlog::info("[RayReconstruction] D3D12 debug layer, severity {} id {}{}: {}", static_cast<int>(severity), static_cast<int>(id),
            mod ? ", names the Mod" : "", text);
    }
}

// Before the device exists, if <game>/reframework/rr/d3d12_debug_layer.txt is there. It is a file
// and not an option of the Mod because the device comes long before the Mod's config loads.
void enable_debug_layer_if_asked() {
    std::error_code ec{};

    if (!std::filesystem::exists(REFramework::get_persistent_dir() / "reframework" / "rr" / "d3d12_debug_layer.txt", ec)) {
        return;
    }

    // With the Agility SDK, the debug layer has to be the D3D12SDKLayers.dll of the same version as
    // D3D12Core.dll and in its folder; without it, creating the device can fail.
    const auto exe = GetModuleHandleW(nullptr);

    if (const auto sdk_path = reinterpret_cast<const char* const*>(GetProcAddress(exe, "D3D12SDKPath"));
        sdk_path != nullptr && *sdk_path != nullptr) {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(exe, exe_path, MAX_PATH);
        const auto layers = std::filesystem::path{exe_path}.parent_path() / *sdk_path / "D3D12SDKLayers.dll";

        if (!std::filesystem::exists(layers, ec)) {
            set_status(g_debug_layer_status, std::format("asked for, NOT enabled: {} is missing", layers.string()));
            return;
        }
    }

    using GetDebugInterfaceFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto get_debug_interface =
        reinterpret_cast<GetDebugInterfaceFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12GetDebugInterface"));
    ComPtr<ID3D12Debug> debug{};

    if (get_debug_interface == nullptr || FAILED(get_debug_interface(IID_PPV_ARGS(&debug)))) {
        set_status(g_debug_layer_status, "asked for, NOT enabled: D3D12GetDebugInterface failed");
        return;
    }

    debug->EnableDebugLayer();
    g_debug_layer_enabled_by_mod = true;
}

// Whoever enabled it: the Mod, or an external tool.
void set_up_debug_layer(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> info_queue{};

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
        g_debug_layer_active = false;
        return;
    }

    g_debug_layer_active = true;

    // The layer breaks on CORRUPTION by default, and with no debugger attached that break is an
    // unhandled exception 0x87D.
    for (const auto severity : {D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR, D3D12_MESSAGE_SEVERITY_WARNING}) {
        info_queue->SetBreakOnSeverity(severity, FALSE);
    }

    std::string callback{"no ID3D12InfoQueue1: messages not read"};

    if (ComPtr<ID3D12InfoQueue1> info_queue1{}; SUCCEEDED(info_queue.As(&info_queue1))) {
        DWORD cookie{};
        const auto hr = info_queue1->RegisterMessageCallback(&on_debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        callback =
            SUCCEEDED(hr) ? "messages read as they come" : std::format("message callback failed: {:x}", static_cast<std::uint32_t>(hr));
    }

    set_status(g_debug_layer_status,
        std::format("ACTIVE, enabled {}; breaks off; {}",
            g_debug_layer_enabled_by_mod ? "by reframework/rr/d3d12_debug_layer.txt" : "from outside the Mod", callback));
}

void hook_device(IUnknown* unknown) {
    ComPtr<ID3D12Device> device{};

    if (unknown == nullptr || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&device)))) {
        return;
    }

    std::scoped_lock _{g_device_mutex};

    if (is_hooked_device(device.Get())) {
        return;
    }

    auto& hook = *g_device_hooks.emplace_back(std::make_unique<VtableHook>(device.Get()));
    g_device_vtables.push_back(*reinterpret_cast<void**>(device.Get()));

    // The increments first: the view hooks read them.
    g_rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dsv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    g_srv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    std::string failures{};
    hook_slot(hook, slot::DEVICE_CREATE_COMMAND_QUEUE, reinterpret_cast<void*>(&create_command_queue), g_orig.create_command_queue,
        failures, "CreateCommandQueue");
    hook_slot(hook, slot::DEVICE_CREATE_GRAPHICS_PSO, reinterpret_cast<void*>(&create_graphics_pso), g_orig.create_graphics_pso, failures,
        "CreateGraphicsPipelineState");
    hook_slot(hook, slot::DEVICE_CREATE_COMPUTE_PSO, reinterpret_cast<void*>(&create_compute_pso), g_orig.create_compute_pso, failures,
        "CreateComputePipelineState");
    hook_slot(hook, slot::DEVICE_CREATE_COMMAND_LIST, reinterpret_cast<void*>(&create_command_list), g_orig.create_command_list, failures,
        "CreateCommandList");
    hook_slot(hook, slot::DEVICE_CREATE_ROOT_SIGNATURE, reinterpret_cast<void*>(&create_root_signature), g_orig.create_root_signature,
        failures, "CreateRootSignature");
    hook_slot(hook, slot::DEVICE_CREATE_RTV, reinterpret_cast<void*>(&create_rtv), g_orig.create_rtv, failures, "CreateRenderTargetView");
    hook_slot(hook, slot::DEVICE_CREATE_DSV, reinterpret_cast<void*>(&create_dsv), g_orig.create_dsv, failures, "CreateDepthStencilView");
    hook_slot(hook, slot::DEVICE_COPY_DESCRIPTORS, reinterpret_cast<void*>(&copy_descriptors), g_orig.copy_descriptors, failures,
        "CopyDescriptors");
    hook_slot(hook, slot::DEVICE_COPY_DESCRIPTORS_SIMPLE, reinterpret_cast<void*>(&copy_descriptors_simple), g_orig.copy_descriptors_simple,
        failures, "CopyDescriptorsSimple");
    hook_slot(hook, slot::DEVICE_CREATE_COMMITTED_RESOURCE, reinterpret_cast<void*>(&create_committed_resource),
        g_orig.create_committed_resource, failures, "CreateCommittedResource");
    hook_slot(hook, slot::DEVICE_CREATE_PLACED_RESOURCE, reinterpret_cast<void*>(&create_placed_resource), g_orig.create_placed_resource,
        failures, "CreatePlacedResource");
    hook_slot(hook, slot::DEVICE_CREATE_RESERVED_RESOURCE, reinterpret_cast<void*>(&create_reserved_resource),
        g_orig.create_reserved_resource, failures, "CreateReservedResource");

    // The slots of the later interfaces exist only if the device implements them.
    if (ComPtr<ID3D12Device1> device1{}; SUCCEEDED(device.As(&device1))) {
        hook_slot(hook, slot::DEVICE1_CREATE_PIPELINE_LIBRARY, reinterpret_cast<void*>(&create_pipeline_library),
            g_orig.create_pipeline_library, failures, "CreatePipelineLibrary");
    }

    if (ComPtr<ID3D12Device2> device2{}; SUCCEEDED(device.As(&device2))) {
        hook_slot(hook, slot::DEVICE2_CREATE_PIPELINE_STATE, reinterpret_cast<void*>(&create_pipeline_state), g_orig.create_pipeline_state,
            failures, "CreatePipelineState");
    }

    if (ComPtr<ID3D12Device4> device4{}; SUCCEEDED(device.As(&device4))) {
        hook_slot(hook, slot::DEVICE4_CREATE_COMMAND_LIST1, reinterpret_cast<void*>(&create_command_list1), g_orig.create_command_list1,
            failures, "CreateCommandList1");
        hook_slot(hook, slot::DEVICE4_CREATE_COMMITTED_RESOURCE1, reinterpret_cast<void*>(&create_committed_resource1),
            g_orig.create_committed_resource1, failures, "CreateCommittedResource1");
        hook_slot(hook, slot::DEVICE4_CREATE_RESERVED_RESOURCE1, reinterpret_cast<void*>(&create_reserved_resource1),
            g_orig.create_reserved_resource1, failures, "CreateReservedResource1");
    }

    if (ComPtr<ID3D12Device8> device8{}; SUCCEEDED(device.As(&device8))) {
        hook_slot(hook, slot::DEVICE8_CREATE_COMMITTED_RESOURCE2, reinterpret_cast<void*>(&create_committed_resource2),
            g_orig.create_committed_resource2, failures, "CreateCommittedResource2");
        hook_slot(hook, slot::DEVICE8_CREATE_PLACED_RESOURCE1, reinterpret_cast<void*>(&create_placed_resource1),
            g_orig.create_placed_resource1, failures, "CreatePlacedResource1");
    }

    if (ComPtr<ID3D12Device9> device9{}; SUCCEEDED(device.As(&device9))) {
        hook_slot(hook, slot::DEVICE9_CREATE_COMMAND_QUEUE1, reinterpret_cast<void*>(&create_command_queue1), g_orig.create_command_queue1,
            failures, "CreateCommandQueue1");
    }

    if (ComPtr<ID3D12Device10> device10{}; SUCCEEDED(device.As(&device10))) {
        hook_slot(hook, slot::DEVICE10_CREATE_COMMITTED_RESOURCE3, reinterpret_cast<void*>(&create_committed_resource3),
            g_orig.create_committed_resource3, failures, "CreateCommittedResource3");
        hook_slot(hook, slot::DEVICE10_CREATE_PLACED_RESOURCE2, reinterpret_cast<void*>(&create_placed_resource2),
            g_orig.create_placed_resource2, failures, "CreatePlacedResource2");
        hook_slot(hook, slot::DEVICE10_CREATE_RESERVED_RESOURCE2, reinterpret_cast<void*>(&create_reserved_resource2),
            g_orig.create_reserved_resource2, failures, "CreateReservedResource2");
    }

    const auto text = std::format("device {:x} hooked: {}", reinterpret_cast<std::uintptr_t>(device.Get()), slots_report(failures));
    set_status(g_device_status, text);
    spdlog::info("[RayReconstruction] D3D12 {}", text);

    set_up_debug_layer(device.Get());
}

HRESULT WINAPI create_device(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** out) {
    static std::once_flag debug_layer{};
    std::call_once(debug_layer, enable_debug_layer_if_asked);

    const auto hr = g_create_device_hook->get_original<D3D12CreateDeviceFn>()(adapter, level, riid, out);

    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        hook_device(static_cast<IUnknown*>(*out));
    }

    return hr;
}
} // namespace

void install(HMODULE d3d12, const core::Profile& profile) try {
    std::scoped_lock _{g_install_mutex};

    if (g_installed || d3d12 == nullptr) {
        return;
    }

    g_installed = true;
    g_registry_storage = std::make_unique<PsoRegistry>(profile);
    g_registry = g_registry_storage.get();

    const auto create_device_export = GetProcAddress(d3d12, "D3D12CreateDevice");

    if (create_device_export == nullptr) {
        set_status(g_create_device_status, "FAILED: d3d12.dll exports no D3D12CreateDevice");
        spdlog::error("[RayReconstruction] d3d12.dll exports no D3D12CreateDevice");
        return;
    }

    g_create_device_hook = std::make_unique<FunctionHook>(
        reinterpret_cast<std::uintptr_t>(create_device_export), reinterpret_cast<std::uintptr_t>(&create_device));

    if (!g_create_device_hook->create()) {
        set_status(g_create_device_status, "FAILED to hook D3D12CreateDevice");
        spdlog::error("[RayReconstruction] Failed to hook D3D12CreateDevice");
        return;
    }

    set_status(g_create_device_status, "hooked as d3d12.dll loaded");
    spdlog::info("[RayReconstruction] D3D12CreateDevice hooked as d3d12.dll loaded");
} catch (const std::exception& e) {
    spdlog::error("[RayReconstruction] Installing the D3D12 hooks: {}", e.what());
}

PsoRegistry* pso_registry() {
    return g_registry.load();
}

HRESULT create_compute_pso_outside_registry(
    std::span<const std::uint8_t> bytecode, ID3D12RootSignature* root_signature, ID3D12PipelineState** pso) {
    const auto create = g_orig.create_compute_pso.load();

    if (create == nullptr || root_signature == nullptr) {
        return E_INVALIDARG;
    }

    ComPtr<ID3D12Device> device{};

    if (const auto hr = root_signature->GetDevice(IID_PPV_ARGS(&device)); FAILED(hr)) {
        return hr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.CS = {bytecode.data(), bytecode.size()};

    ComPtr<ID3D12PipelineState> created{};

    if (const auto hr = create(device.Get(), &desc, IID_PPV_ARGS(&created)); FAILED(hr)) {
        return hr;
    }

    // The prefix of the Mod's objects, so that the debug layer's messages name it.
    created->SetName(L"RayReconstruction substitute PSO");
    *pso = created.Detach();
    return S_OK;
}

Status status() {
    Status s{};

    {
        std::scoped_lock _{g_status_mutex};
        s.create_device = g_create_device_status;
        s.device = g_device_status;
        s.debug_layer = g_debug_layer_status;
    }

    {
        std::scoped_lock _{g_queues_mutex};
        s.queues = g_queues_report;
    }

    fill_list_status(s);

    if (const auto registry = g_registry.load(); registry != nullptr) {
        s.psos = registry->counts();
    }

    {
        std::shared_lock _{g_views_mutex};
        s.views = g_views.size();
    }

    {
        std::shared_lock _{g_root_signatures_mutex};
        s.root_signatures = g_root_signatures.size();
    }

    {
        std::scoped_lock _{g_pt_cs_mutex};
        s.pt_cs = g_pt_cs;
    }

    s.root_signature_failures = g_root_signature_failures;
    s.debug_messages = g_debug_messages;
    s.debug_errors = g_debug_errors;
    s.debug_messages_naming_mod = g_debug_messages_naming_mod;
    return s;
}

std::optional<core::View> find_view(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    std::shared_lock _{g_views_mutex};
    return g_views.find(handle.ptr);
}

UINT rtv_increment() {
    return g_rtv_increment;
}

UINT srv_increment() {
    return g_srv_increment;
}

std::optional<RootSignatureInfo> find_root_signature(ID3D12RootSignature* root_signature) {
    std::shared_lock _{g_root_signatures_mutex};
    const auto it = g_root_signatures.find(root_signature);
    return it != g_root_signatures.end() ? std::optional{it->second} : std::nullopt;
}

// On the thread that records the list. The file is written once per variant, under the lock: a PSO
// created meanwhile waits for it.
void note_anchored_pt_cs(const std::optional<core::ShaderDigest>& cs) {
    std::scoped_lock _{g_pt_cs_mutex};

    if (!cs) {
        ++g_pt_cs.anchors_without_digest;
        return;
    }

    g_pt_cs.last = cs;
    g_anchored_pt_cs = cs;

    if (anchored_before(*cs)) {
        return;
    }

    auto& variant = g_pt_cs.variants.emplace_back(PtCsVariant{*cs});
    const auto path = pt_cs_path(*cs);
    const auto retained = g_retained_cs.find(cs->bytes);
    std::error_code ec{};

    if (std::filesystem::exists(path, ec)) {
        variant.dump = PtCsDump::AlreadyThere;
    } else if (retained == g_retained_cs.end()) {
        variant.dump = PtCsDump::NotRetained;
    } else {
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(retained->second.data()), static_cast<std::streamsize>(retained->second.size()));
        variant.dump = file.good() ? PtCsDump::Written : PtCsDump::WriteFailed;
    }

    if (retained != g_retained_cs.end()) {
        --g_pt_cs.retained;
        g_pt_cs.retained_bytes -= retained->second.size();
        g_retained_cs.erase(retained);
    }

    spdlog::info("[RayReconstruction] PT compute shader {} at an anchored draw for the first time: {} ({})", core::to_string(*cs),
        to_string(variant.dump), path.string());
}

std::optional<core::ShaderDigest> take_anchored_pt_cs() {
    std::scoped_lock _{g_pt_cs_mutex};
    return std::exchange(g_anchored_pt_cs, std::nullopt);
}

const char* to_string(PtCsDump dump) {
    switch (dump) {
    case PtCsDump::Written:
        return "dumped";
    case PtCsDump::AlreadyThere:
        return "dumped in an earlier session";
    case PtCsDump::NotRetained:
        return "NOT dumped: no copy was kept as its PSO was created";
    case PtCsDump::WriteFailed:
        return "NOT dumped: the file could not be written";
    }

    return "?";
}

bool debug_layer_active() {
    return g_debug_layer_active;
}
} // namespace rr::d3d12
