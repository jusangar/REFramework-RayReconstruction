#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <d3dcompiler.h>

#include <spdlog/spdlog.h>

#include "D3D12Hooks.hpp"
#include "GuidePass.hpp"
#include "RrShaders.inc"
#include "core/Guides.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler")

using Microsoft::WRL::ComPtr;

namespace rr::d3d12 {
namespace {
// How many recordings a slot of the heap, or a set of textures replaced, waits before it is used again
// or freed: far more frames than the game keeps in flight.
constexpr std::uint32_t RING = 16;

// The descriptors of one recording: the guide pass's table, then the debug view's, the count's and the
// accumulated reference's.
namespace descriptor {
constexpr UINT ALBEDO = 0;
constexpr UINT NORMALS = 1;
constexpr UINT VELOCITY = 2;
constexpr UINT DEPTH = 3;
constexpr UINT AMBIENT_BRDF = 4; // the copy of the game's LUT
constexpr UINT PASS_SRVS = 5;
constexpr UINT TEXTURES = 5; // the UAVs of RR's own textures, in the order of GuidePass::Texture
constexpr UINT VIEW_TABLE = 10; // the guide shown, then O
constexpr UINT COUNT_SOURCE = 12; // O, for the count of its non-finite values
constexpr UINT REFERENCE_TABLE = 13; // T, then O
constexpr UINT PER_SLOT = 15;
} // namespace descriptor

// GuideConstants of GuidePass.hlsl, as FXC lays it out: each matrix as it is in memory, with the render
// size and j between them, then where the MVs come from, the LUT's size and whether it was copied. A
// cbuffer takes whole registers of 4 DWORDs: FXC declares CB0[10], which the last three fill.
struct GuideConstants {
    core::Matrix4 inverse_view_projection{};
    core::Float2 render_size{};
    core::Float2 jitter{};
    core::Matrix4 old_view_projection{};
    std::uint32_t motion{}; // MOTION_* of Guides.hlsli
    core::Extent lut_size{};
    std::uint32_t lut{};
};

static_assert(offsetof(GuideConstants, render_size) == 64 && offsetof(GuideConstants, jitter) == 72 &&
              offsetof(GuideConstants, old_view_projection) == 80 && offsetof(GuideConstants, motion) == 144 &&
              offsetof(GuideConstants, lut_size) == 148 && offsetof(GuideConstants, lut) == 156 && sizeof(GuideConstants) == 160);

// Where the pass takes the MVs from.
std::uint32_t motion_of(const GuideInputs& in) {
    using namespace core::guides;
    const auto motion = !in.has_velocity         ? MOTION_REPROJECT_ALL
                        : in.reproject_saturated ? MOTION_REPROJECT_SATURATED
                                                 : MOTION_GAME_VELOCITY;
    return static_cast<std::uint32_t>(motion);
}

// The root constants of GuidePass.hlsl, in DWORDs: GuideConstants, DebugConstants, LutConstants,
// ReferenceConstants and CountConstants, in whole registers as the shader declares them.
constexpr UINT PASS_CONSTANTS = sizeof(GuideConstants) / 4;
constexpr UINT VIEW_CONSTANTS = 4;
constexpr UINT LUT_CONSTANTS = 4;
constexpr UINT REFERENCE_CONSTANTS = 4;
constexpr UINT COUNT_CONSTANTS = 4;

// The running totals of the count: the NaN pixels, then the infinite ones.
constexpr UINT64 NONFINITE_TOTALS_BYTES = sizeof(core::NonFiniteTotals);


// The copy of the LUT: two floats per texel, then two for the size of the texture found.
UINT64 lut_bytes(core::Extent size) {
    return (UINT64{size.width} * size.height + 1) * 2 * sizeof(float);
}

constexpr UINT GROUP_SIZE = 8; // numthreads of both kernels

constexpr std::array<DXGI_FORMAT, 5> TEXTURE_FORMATS{
    DXGI_FORMAT_R16G16B16A16_FLOAT, // normals with the roughness
    DXGI_FORMAT_R16G16B16A16_FLOAT, // diffuse albedo
    DXGI_FORMAT_R16G16B16A16_FLOAT, // specular albedo
    DXGI_FORMAT_R32_FLOAT,          // depth
    DXGI_FORMAT_R16G16_FLOAT,       // MVs
};

constexpr std::array<const wchar_t*, 5> TEXTURE_NAMES{
    L"RayReconstruction guide: normals and roughness",
    L"RayReconstruction guide: diffuse albedo",
    L"RayReconstruction guide: specular albedo",
    L"RayReconstruction guide: depth",
    L"RayReconstruction guide: motion vectors",
};

// D3DCompile reads Guides.hlsli from dinput8.dll, as GuidePass.hlsl includes it.
class EmbeddedIncludes final : public ID3DInclude {
public:
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        if (name == nullptr || std::string_view{name} != "Guides.hlsli") {
            return E_FAIL;
        }

        *data = shaders::GUIDES_HLSLI;
        *bytes = static_cast<UINT>(sizeof(shaders::GUIDES_HLSLI) - 1);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};

ComPtr<ID3DBlob> compile(const char* kernel, std::string& error) {
    EmbeddedIncludes includes{};
    ComPtr<ID3DBlob> code{};
    ComPtr<ID3DBlob> errors{};
    const auto hr = D3DCompile(shaders::GUIDEPASS_HLSL, sizeof(shaders::GUIDEPASS_HLSL) - 1, "GuidePass.hlsl", nullptr, &includes, kernel,
        "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

    if (FAILED(hr)) {
        error = std::format("D3DCompile of {} failed ({:x}): {}", kernel, static_cast<std::uint32_t>(hr),
            errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        return nullptr;
    }

    return code;
}

ComPtr<ID3D12RootSignature> create_root_signature(
    ID3D12Device* device, std::span<const D3D12_ROOT_PARAMETER> parameters, const wchar_t* name, std::string& error) {
    const D3D12_ROOT_SIGNATURE_DESC desc{
        static_cast<UINT>(parameters.size()), parameters.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob{};
    ComPtr<ID3DBlob> errors{};
    ComPtr<ID3D12RootSignature> root_signature{};

    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))) {
        error = std::format("D3D12SerializeRootSignature failed: {}",
            errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        return nullptr;
    }

    if (const auto hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature));
        FAILED(hr)) {
        error = std::format("CreateRootSignature failed ({:x})", static_cast<std::uint32_t>(hr));
        return nullptr;
    }

    root_signature->SetName(name);
    return root_signature;
}

D3D12_ROOT_PARAMETER root_constants(UINT shader_register, UINT count) {
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants = {shader_register, 0, count};
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return p;
}

D3D12_ROOT_PARAMETER root_table(std::span<const D3D12_DESCRIPTOR_RANGE> ranges) {
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return p;
}

// Root constants at register b<constant_register>, and one table: srvs SRVs from t<srv_base>, then uavs
// UAVs from u<uav_base>.
ComPtr<ID3D12RootSignature> create_table_root_signature(ID3D12Device* device, UINT constants, UINT constant_register, UINT srv_base,
    UINT srvs, UINT uav_base, UINT uavs, const wchar_t* name, std::string& error) {
    const std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{{
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvs, srv_base, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uavs, uav_base, 0, srvs},
    }};

    const std::array parameters{root_constants(constant_register, constants), root_table(ranges)};
    return create_root_signature(device, parameters, name, error);
}

// The copy of the LUT: LutConstants at b2, the PT's descriptor at t6 in a table of its own, which points
// into the game's heap, and the copy at u6 as a root UAV, which needs no descriptor.
ComPtr<ID3D12RootSignature> create_lut_root_signature(ID3D12Device* device, std::string& error) {
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, 0};

    D3D12_ROOT_PARAMETER uav{};
    uav.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    uav.Descriptor = {6, 0};
    uav.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    const std::array parameters{root_constants(2, LUT_CONSTANTS), root_table({&range, 1}), uav};
    return create_root_signature(device, parameters, L"RayReconstruction AmbientBRDF copy root signature", error);
}

// The kernels of the measurement interface: root constants at b<constant_register>, a table with an SRV
// at t<srv_register> and, with table_uav, a UAV there; and a root UAV at u<root_uav>, which needs no
// descriptor and has no bounds.
ComPtr<ID3D12RootSignature> create_root_uav_signature(ID3D12Device* device, UINT constant_register, UINT srv_register,
    std::optional<UINT> table_uav, UINT root_uav, const wchar_t* name, std::string& error) {
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, srv_register, 0, 0}};

    if (table_uav) {
        ranges.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, *table_uav, 0, 1});
    }

    D3D12_ROOT_PARAMETER uav{};
    uav.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    uav.Descriptor = {root_uav, 0};
    uav.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    const std::array parameters{root_constants(constant_register, 4), root_table(ranges), uav};
    return create_root_signature(device, parameters, name, error);
}

ComPtr<ID3D12PipelineState> create_pso(
    ID3D12Device* device, ID3D12RootSignature* root_signature, const char* kernel, const wchar_t* name, std::string& error) {
    const auto code = compile(kernel, error);

    if (code == nullptr) {
        return nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso{};

    if (const auto hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)); FAILED(hr)) {
        error = std::format("CreateComputePipelineState of {} failed ({:x})", kernel, static_cast<std::uint32_t>(hr));
        return nullptr;
    }

    pso->SetName(name);
    return pso;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, subresource, before, after};
    return barrier;
}

// Where the copy of the LUT waits for the pass, and for a readback too.
D3D12_RESOURCE_STATES lut_read_state(bool read_back) {
    return read_back ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE
                     : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

void reverse(std::vector<D3D12_RESOURCE_BARRIER>& barriers) {
    for (auto& b : barriers) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    }
}

UINT groups(std::uint32_t pixels) {
    return (pixels + GROUP_SIZE - 1) / GROUP_SIZE;
}

ID3D12Resource* as_resource(core::ResourceId resource) {
    return static_cast<ID3D12Resource*>(const_cast<void*>(resource));
}

// A game resource, read as it is: the format of its own description.
void write_srv(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT plane, D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Texture2D.PlaneSlice = plane;
    device->CreateShaderResourceView(resource, &srv, where);
}

void write_uav(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource, nullptr, &uav, where);
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}
} // namespace

bool GuidePass::set_up(ID3D12Device* device) {
    std::string error{};

    const auto fail = [&] {
        m_failed = true;
        m_status.setup = "failed: " + error;
        spdlog::error("[RayReconstruction] Guide pass not set up: {}", error);
        return false;
    };

    // The registers of GuidePass.hlsl: GuideConstants at b0, the game's resources and the copy of the LUT
    // from t0 and RR's textures from u0; DebugConstants at b1, the guide shown at t5 and O at u5.
    m_pass_root_signature = create_table_root_signature(device, PASS_CONSTANTS, 0, 0, descriptor::PASS_SRVS, 0,
        static_cast<UINT>(TEXTURE_COUNT), L"RayReconstruction guide pass root signature", error);
    m_view_root_signature =
        create_table_root_signature(device, VIEW_CONSTANTS, 1, 5, 1, 5, 1, L"RayReconstruction debug view root signature", error);
    m_lut_root_signature = create_lut_root_signature(device, error);

    // ReferenceConstants at b3, T at t7 and O at u7, and the mean at u8; CountConstants at b4, O at t8 and
    // the totals at u9.
    m_reference_root_signature =
        create_root_uav_signature(device, 3, 7, 7, 8, L"RayReconstruction accumulated reference root signature", error);
    m_count_root_signature =
        create_root_uav_signature(device, 4, 8, std::nullopt, 9, L"RayReconstruction non-finite count root signature", error);

    if (m_pass_root_signature == nullptr || m_view_root_signature == nullptr || m_lut_root_signature == nullptr ||
        m_reference_root_signature == nullptr || m_count_root_signature == nullptr) {
        return fail();
    }

    m_pass_pso = create_pso(device, m_pass_root_signature.Get(), "guide_pass", L"RayReconstruction guide pass PSO", error);
    m_view_pso = create_pso(device, m_view_root_signature.Get(), "debug_view", L"RayReconstruction debug view PSO", error);
    m_lut_pso = create_pso(device, m_lut_root_signature.Get(), "copy_ambient_brdf", L"RayReconstruction AmbientBRDF copy PSO", error);
    m_reference_pso = create_pso(
        device, m_reference_root_signature.Get(), "accumulate_reference", L"RayReconstruction accumulated reference PSO", error);
    m_count_pso = create_pso(device, m_count_root_signature.Get(), "count_nonfinite", L"RayReconstruction non-finite count PSO", error);

    if (m_pass_pso == nullptr || m_view_pso == nullptr || m_lut_pso == nullptr || m_reference_pso == nullptr || m_count_pso == nullptr) {
        return fail();
    }

    const D3D12_DESCRIPTOR_HEAP_DESC heap{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, RING * descriptor::PER_SLOT, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (const auto hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_heap)); FAILED(hr)) {
        error = std::format("CreateDescriptorHeap failed ({:x})", static_cast<std::uint32_t>(hr));
        return fail();
    }

    m_heap->SetName(L"RayReconstruction guide pass heap");
    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device = device;
    m_status.setup = "ready";
    spdlog::info("[RayReconstruction] Guide pass set up");
    return true;
}

// A new size retires the set in use: the lists in flight may still write it.
bool GuidePass::ensure_textures(core::Extent size) {
    static_assert(TEXTURE_FORMATS.size() == TEXTURE_COUNT && TEXTURE_NAMES.size() == TEXTURE_COUNT);

    std::erase_if(m_retired, [this](const Textures& t) { return m_status.recorded >= t.retired_at + RING; });

    if (m_textures.size == size) {
        // Back at a size that works: the failure no longer holds, and the size that failed can try again.
        if (m_failed_size != core::Extent{}) {
            m_failed_size = {};
            m_status.setup = "ready";
        }

        return true;
    }

    // Once per visit to a size, so that a size the device refuses does not flood the log.
    if (m_failed_size == size) {
        return false;
    }

    Textures created{size};
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

    for (std::size_t i = 0; i < TEXTURE_COUNT; ++i) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = size.width;
        desc.Height = size.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = TEXTURE_FORMATS[i];
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (const auto hr = m_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&created.resources[i]));
            FAILED(hr)) {
            m_failed_size = size;
            m_status.setup =
                std::format("the guides could not be created at {}x{} ({:x})", size.width, size.height, static_cast<std::uint32_t>(hr));
            spdlog::error("[RayReconstruction] Guide pass: {}", m_status.setup);
            return false;
        }

        created.resources[i]->SetName(TEXTURE_NAMES[i]);
    }

    if (m_textures.resources[0] != nullptr) {
        m_textures.retired_at = m_status.recorded;
        m_retired.push_back(std::move(m_textures));
    }

    m_textures = std::move(created);
    m_failed_size = {};
    m_status.setup = "ready";
    m_status.textures = size;
    ++m_status.texture_sets;
    spdlog::info("[RayReconstruction] Guide pass: textures at {}x{}", size.width, size.height);
    return true;
}

// The profile's size never changes, so the buffer is made once: a buffer of another size could still be
// in use by the lists in flight.
bool GuidePass::ensure_lut(core::Extent size) {
    if (m_lut != nullptr || size.width == 0 || size.height == 0) {
        return m_lut != nullptr && m_lut_size == size;
    }

    // Once, so that a buffer the device refuses does not flood the log.
    if (m_lut_size == size) {
        return false;
    }

    m_lut_size = size;

    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto desc = buffer_desc(lut_bytes(size), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    if (const auto hr =
            m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_lut));
        FAILED(hr)) {
        spdlog::error("[RayReconstruction] Guide pass: the copy of the AmbientBRDF LUT could not be created ({:x})", static_cast<std::uint32_t>(hr));
        return false;
    }

    m_lut->SetName(L"RayReconstruction AmbientBRDF copy");
    return true;
}

// Once a check asks for it. A readback buffer stays in COPY_DEST.
bool GuidePass::ensure_lut_readback() {
    if (m_lut_readback != nullptr || m_lut == nullptr) {
        return m_lut_readback != nullptr;
    }

    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    auto desc = m_lut->GetDesc();
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (const auto hr = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_lut_readback));
        FAILED(hr)) {
        m_readback_asked = false;
        m_status.readback = std::format("the readback buffer could not be created ({:x})", static_cast<std::uint32_t>(hr));
        return false;
    }

    m_lut_readback->SetName(L"RayReconstruction AmbientBRDF readback");
    return true;
}

bool GuidePass::ready(ID3D12GraphicsCommandList* list) {
    if (m_failed) {
        return false;
    }

    ComPtr<ID3D12Device> device{};

    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || (m_device == nullptr && !set_up(device.Get()))) {
        return false;
    }

    // The game keeps one device for the whole process: another one would need every object again.
    if (device.Get() != m_device.Get()) {
        m_status.setup = "the TAA's list belongs to another device than the guide pass";
        return false;
    }

    return true;
}

// The mean is indexed by T's width, so a buffer for a larger T serves a smaller one. A larger T retires
// the buffer in use: the lists in flight may still write it.
bool GuidePass::ensure_reference(core::Extent size) {
    std::erase_if(m_retired_means, [this](const RetiredBuffer& b) { return m_status.recorded >= b.retired_at + RING; });

    const auto bytes = UINT64{size.width} * size.height * 4 * sizeof(float);

    if (m_reference_mean != nullptr && m_reference_bytes >= bytes) {
        // A size that fits again: the one that failed can try again.
        m_failed_reference_bytes = 0;
        return true;
    }

    // Once per visit to a size, so that a size the device refuses does not flood the log.
    if (m_failed_reference_bytes == bytes) {
        return false;
    }

    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> created{};

    if (const auto hr = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&created));
        FAILED(hr)) {
        m_failed_reference_bytes = bytes;
        spdlog::error("[RayReconstruction] The mean of the accumulated reference could not be created at {}x{} ({:x})", size.width,
            size.height, static_cast<std::uint32_t>(hr));
        return false;
    }

    created->SetName(L"RayReconstruction accumulated reference mean");

    if (m_reference_mean != nullptr) {
        m_retired_means.push_back({std::move(m_reference_mean), m_status.recorded});
    }

    m_reference_mean = std::move(created);
    m_reference_bytes = bytes;
    m_failed_reference_bytes = 0;
    spdlog::info("[RayReconstruction] Accumulated reference: mean at {}x{}", size.width, size.height);
    return true;
}

// Once, for the life of the process: the CPU follows how its totals grow, from 0.
bool GuidePass::ensure_nonfinite_totals() {
    if (m_nonfinite_totals != nullptr || m_nonfinite_failed) {
        return m_nonfinite_totals != nullptr;
    }

    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto desc = buffer_desc(NONFINITE_TOTALS_BYTES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    if (const auto hr = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_nonfinite_totals));
        FAILED(hr)) {
        m_nonfinite_failed = true;
        spdlog::error("[RayReconstruction] The totals of the non-finite count could not be created ({:x})", static_cast<std::uint32_t>(hr));
        return false;
    }

    m_nonfinite_totals->SetName(L"RayReconstruction non-finite totals");
    return true;
}

// Once: RING slots of timestamps, and a readback buffer that stays in COPY_DEST.
bool GuidePass::ensure_measurements() {
    if (m_timestamps != nullptr || m_measurement_failed) {
        return m_timestamps != nullptr;
    }

    const auto fail = [this](std::string what, HRESULT hr) {
        m_measurement_failed = true;
        m_status.measurement = std::format("failed: {} ({:x})", what, static_cast<std::uint32_t>(hr));
        spdlog::error("[RayReconstruction] Measurement interface not set up: {}", m_status.measurement);
        m_timestamps.Reset();
        return false;
    };

    const D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, RING * TIMESTAMPS_PER_SLOT, 0};

    if (const auto hr = m_device->CreateQueryHeap(&queries, IID_PPV_ARGS(&m_timestamps)); FAILED(hr)) {
        return fail("CreateQueryHeap", hr);
    }

    m_timestamps->SetName(L"RayReconstruction timestamps");

    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    const auto desc = buffer_desc(RING * SLOT_BYTES, D3D12_RESOURCE_FLAG_NONE);

    if (const auto hr = m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_measurement_readback));
        FAILED(hr)) {
        return fail("the readback buffer could not be created", hr);
    }

    m_measurement_readback->SetName(L"RayReconstruction measurement readback");
    m_slots.resize(RING);
    m_status.measurement = "ready";
    return true;
}

UINT GuidePass::next_slot() {
    m_slot = (m_slot + 1) % RING;
    m_slot_first = m_slot * descriptor::PER_SLOT;
    return m_slot_first;
}

D3D12_CPU_DESCRIPTOR_HANDLE GuidePass::cpu_descriptor(UINT index) const {
    auto handle = m_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_descriptor_size;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GuidePass::gpu_descriptor(UINT index) const {
    auto handle = m_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_descriptor_size;
    return handle;
}

// The hooks keep the G-buffer alive until the draw and for the lists in flight (core::GBufferHold), but
// the game's resources can be another set in the next recording, so every recording writes all its views
// anew.
D3D12_GPU_DESCRIPTOR_HANDLE GuidePass::write_descriptors(const GuideInputs& in, ID3D12Resource* o, Texture shown) {
    const auto first = next_slot();
    const auto cpu = [&](UINT index) { return cpu_descriptor(first + index); };

    const auto& g = in.gbuffer;
    const auto device = m_device.Get();

    for (const auto& [resource, index] : {std::pair{g.albedo, descriptor::ALBEDO}, std::pair{g.normals, descriptor::NORMALS}}) {
        write_srv(device, as_resource(resource), as_resource(resource)->GetDesc().Format, 0, cpu(index));
    }

    // A frame without velocity binds none: a null descriptor, whose view needs a format of its own, and
    // which the pass does not read.
    if (in.has_velocity) {
        write_srv(device, as_resource(g.velocity), as_resource(g.velocity)->GetDesc().Format, 0, cpu(descriptor::VELOCITY));
    } else {
        write_srv(device, nullptr, DXGI_FORMAT_R16G16B16A16_SNORM, 0, cpu(descriptor::VELOCITY));
    }

    // Plane 0 of R32G8X24_TYPELESS: the depth, unchanged.
    write_srv(device, as_resource(g.depth), DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, 0, cpu(descriptor::DEPTH));

    // The copy of the LUT as a StructuredBuffer<float2>; a null descriptor without it, which the pass then
    // does not read.
    D3D12_SHADER_RESOURCE_VIEW_DESC lut{};
    lut.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    lut.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lut.Buffer.NumElements = static_cast<UINT>(lut_bytes(m_lut_size) / (2 * sizeof(float)));
    lut.Buffer.StructureByteStride = 2 * sizeof(float);
    device->CreateShaderResourceView(m_lut.Get(), &lut, cpu(descriptor::AMBIENT_BRDF));

    for (UINT i = 0; i < TEXTURE_COUNT; ++i) {
        write_uav(device, m_textures.resources[i].Get(), TEXTURE_FORMATS[i], cpu(descriptor::TEXTURES + i));
    }

    if (o != nullptr) {
        const auto s = static_cast<std::size_t>(shown);
        write_srv(device, m_textures.resources[s].Get(), TEXTURE_FORMATS[s], 0, cpu(descriptor::VIEW_TABLE));
        write_uav(device, o, o->GetDesc().Format, cpu(descriptor::VIEW_TABLE + 1));
    }

    return gpu_descriptor(first);
}

// With the game's heap bound, as the PT read it: the copy of the LUT, which then goes to
// NON_PIXEL_SHADER_RESOURCE for the pass, and to COPY_SOURCE too for a readback. False if the pass has to
// fall back on the GGX fit.
//
// The LUT is read in whatever state the PT read it, in compute and earlier in the same list: the game
// does not transition a static texture in between. The round trip of the reads cannot declare it, since
// the hooks never learn its resource; the debug layer's GPU-based validation would see a wrong state.
bool GuidePass::record_lut_copy(const GuideInputs& in, bool read_back) {
    const auto& d = in.ambient_brdf;

    if (!in.use_lut) {
        m_status.lut = "GGX fit, as chosen";
        return false;
    }

    if (d.lut.location != core::LutLocation::Located || d.heap == nullptr) {
        m_status.lut = std::string{"GGX fit: "} + core::to_string(d.lut.location);
        return false;
    }

    // A root UAV has no bounds: the copy must be the size of the buffer.
    if (m_lut == nullptr || m_lut_size != in.lut_size) {
        m_status.lut = "GGX fit: no buffer of the LUT's size for the copy";
        return false;
    }

    const auto list = in.list;
    const auto to_uav = transition(m_lut.Get(), 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &to_uav);

    const std::array<std::uint32_t, LUT_CONSTANTS> constants{in.lut_size.width, in.lut_size.height, 0, 0};
    ID3D12DescriptorHeap* const heaps[]{d.heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_lut_root_signature.Get());
    list->SetComputeRoot32BitConstants(0, LUT_CONSTANTS, constants.data(), 0);
    list->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{d.lut.handle});
    list->SetComputeRootUnorderedAccessView(2, m_lut->GetGPUVirtualAddress());
    list->SetPipelineState(m_lut_pso.Get());
    list->Dispatch(groups(in.lut_size.width), groups(in.lut_size.height), 1);

    const auto to_read = transition(m_lut.Get(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, lut_read_state(read_back));
    list->ResourceBarrier(1, &to_read);

    ++m_status.lut_copies;
    m_status.lut = std::format("copied through the PT's descriptor: root parameter {}, descriptor {} of its table", d.lut.parameter, d.lut.offset);
    return true;
}

void GuidePass::request_lut_readback() {
    std::scoped_lock _{m_mutex};

    if (!m_readback_frame) {
        m_readback_asked = true;
        m_status.readback = "asked: the next recording that copies the LUT copies it back too";
    }
}

// No fence: the game's list carries the copy. RING frames are at least RING recordings later, as a frame
// has one evaluation point, and that is what a slot of the heap waits too.
std::optional<LutReadback> GuidePass::take_lut_readback(std::uint64_t frame) {
    std::scoped_lock _{m_mutex};

    if (!m_readback_frame || frame < *m_readback_frame + RING || m_lut_readback == nullptr) {
        return std::nullopt;
    }

    LutReadback out{*m_readback_frame};
    m_readback_frame.reset();

    const auto bytes = lut_bytes(m_lut_size);
    const D3D12_RANGE read{0, static_cast<SIZE_T>(bytes)};
    void* data{};

    if (FAILED(m_lut_readback->Map(0, &read, &data)) || data == nullptr) {
        m_status.readback = "the readback buffer could not be mapped";
        return std::nullopt;
    }

    const auto floats = static_cast<const float*>(data);
    const auto texels = std::size_t{m_lut_size.width} * m_lut_size.height * 2;
    out.texels.assign(floats, floats + texels);
    out.found = {static_cast<std::uint32_t>(floats[texels]), static_cast<std::uint32_t>(floats[texels + 1])};

    const D3D12_RANGE written{0, 0};
    m_lut_readback->Unmap(0, &written);

    m_status.readback = std::format("read back from frame {}", out.frame);
    return out;
}

// A slot is reused RING recordings later, at least RING frames, since a frame has one evaluation point and
// one recording there, RR's or the TAA draw's: far more than the game keeps in flight, so one still pending
// can be read back now. One recorded fewer than RING frames ago, which takes more than one recording per
// frame, is dropped unread: the GPU may still be writing it. That happens only when RR's evaluation fails
// after its guide pass, and the TAA draw then takes a second one.
void GuidePass::start_measurement(ID3D12GraphicsCommandList* list, std::uint64_t frame) {
    m_measuring.reset();

    if (!ensure_measurements()) {
        return;
    }

    const auto index = static_cast<std::uint32_t>(m_measurement_sequence % RING);

    if (m_slots[index].pending && frame >= m_slots[index].frame + RING) {
        read_slot(index);
    }

    m_slots[index] = MeasurementSlot{.sequence = m_measurement_sequence++, .frame = frame, .timestamps = 1};
    m_measuring = index;
    list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * TIMESTAMPS_PER_SLOT);
}

void GuidePass::stamp_segment(ID3D12GraphicsCommandList* list, Segment segment) {
    if (!m_measuring) {
        return;
    }

    auto& slot = m_slots[*m_measuring];

    if (slot.timestamps >= TIMESTAMPS_PER_SLOT) {
        return;
    }

    list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, *m_measuring * TIMESTAMPS_PER_SLOT + slot.timestamps);
    slot.segments[static_cast<std::size_t>(segment)] = std::array{slot.timestamps - 1, slot.timestamps};
    ++slot.timestamps;
}

void GuidePass::finish_measurement(ID3D12GraphicsCommandList* list) {
    if (!m_measuring) {
        return;
    }

    auto& slot = m_slots[*m_measuring];
    list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, *m_measuring * TIMESTAMPS_PER_SLOT, slot.timestamps,
        m_measurement_readback.Get(), *m_measuring * SLOT_BYTES);
    slot.pending = true;
    m_measuring.reset();
}

void GuidePass::read_slot(std::uint32_t index) {
    auto& slot = m_slots[index];
    slot.pending = false;

    const D3D12_RANGE read{static_cast<SIZE_T>(index * SLOT_BYTES), static_cast<SIZE_T>((index + 1) * SLOT_BYTES)};
    void* data{};

    if (FAILED(m_measurement_readback->Map(0, &read, &data)) || data == nullptr) {
        m_status.measurement = "the readback buffer could not be mapped";
        return;
    }

    // Map gives the start of the buffer, whatever the range.
    const auto bytes = static_cast<const std::uint8_t*>(data) + index * SLOT_BYTES;
    std::array<std::uint64_t, TIMESTAMPS_PER_SLOT> ticks{};
    core::NonFiniteTotals totals{};
    std::memcpy(ticks.data(), bytes, sizeof(ticks));
    std::memcpy(&totals, bytes + TOTALS_OFFSET, sizeof(totals));

    const D3D12_RANGE written{0, 0};
    m_measurement_readback->Unmap(0, &written);

    Measurement out{slot.frame};

    for (std::size_t i = 0; i < SEGMENT_COUNT; ++i) {
        if (const auto& segment = slot.segments[i]) {
            out.segments[i] = std::array{ticks[(*segment)[0]], ticks[(*segment)[1]]};
        }
    }

    if (slot.counted) {
        out.nonfinite_totals = totals;
    }

    m_measured.push_back(out);
}

// No fence: the game's list carries the measurement, as the copy of the LUT.
std::vector<Measurement> GuidePass::take_measurements(std::uint64_t frame) {
    std::scoped_lock _{m_mutex};
    std::vector<std::uint32_t> ready{};

    for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].pending && frame >= m_slots[i].frame + RING) {
            ready.push_back(i);
        }
    }

    std::sort(ready.begin(), ready.end(), [this](std::uint32_t a, std::uint32_t b) { return m_slots[a].sequence < m_slots[b].sequence; });

    for (const auto index : ready) {
        read_slot(index);
    }

    return std::exchange(m_measured, {});
}

// The totals go to the readback slot of the recording, and back to COMMON.
void GuidePass::record_count(ID3D12GraphicsCommandList* list, ID3D12Resource* o, core::Extent size, std::uint32_t forced) {
    if (!ensure_nonfinite_totals()) {
        return;
    }

    write_srv(m_device.Get(), o, o->GetDesc().Format, 0, cpu_descriptor(m_slot_first + descriptor::COUNT_SOURCE));

    const auto to_uav = transition(m_nonfinite_totals.Get(), 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &to_uav);

    const std::array<std::uint32_t, COUNT_CONSTANTS> constants{size.width, size.height, forced, 0};
    ID3D12DescriptorHeap* const heaps[]{m_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_count_root_signature.Get());
    list->SetComputeRoot32BitConstants(0, COUNT_CONSTANTS, constants.data(), 0);
    list->SetComputeRootDescriptorTable(1, gpu_descriptor(m_slot_first + descriptor::COUNT_SOURCE));
    list->SetComputeRootUnorderedAccessView(2, m_nonfinite_totals->GetGPUVirtualAddress());
    list->SetPipelineState(m_count_pso.Get());
    list->Dispatch(groups(size.width), groups(size.height), 1);

    const auto to_copy = transition(m_nonfinite_totals.Get(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);

    if (m_measuring) {
        list->CopyBufferRegion(
            m_measurement_readback.Get(), *m_measuring * SLOT_BYTES + TOTALS_OFFSET, m_nonfinite_totals.Get(), 0, NONFINITE_TOTALS_BYTES);
        m_slots[*m_measuring].counted = true;
    }

    const auto back = transition(m_nonfinite_totals.Get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &back);
    ++m_status.counts;
}

std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> GuidePass::record_pass(const GuideInputs& in, ID3D12Resource* o, Texture shown) {
    if (!ready(in.list) || !ensure_textures(in.size)) {
        return std::nullopt;
    }

    // The guide pass's segment starts before the copy of the LUT.
    start_measurement(in.list, in.frame);

    // Without the buffer of the copy, the pass falls back on the GGX fit.
    ensure_lut(in.lut_size);
    const auto read_back = m_readback_asked && ensure_lut_readback();
    const auto lut_copied = record_lut_copy(in, read_back);

    const auto table = write_descriptors(in, o, shown);
    const auto list = in.list;
    const auto& g = in.gbuffer;

    // The round trip of the reads, from where the registry predicted them. Plane 0 is the only one of the
    // depth the pass reads, and the velocity is none in a frame without it.
    std::vector<D3D12_RESOURCE_BARRIER> reads{};

    if (in.round_trip) {
        for (const auto& [read, resource] : {std::pair{core::Read::Albedo, g.albedo}, std::pair{core::Read::Normals, g.normals},
                 std::pair{core::Read::Velocity, g.velocity}, std::pair{core::Read::DepthPlane0, g.depth}}) {
            const auto state = in.states[static_cast<std::size_t>(read)];

            if (read == core::Read::Velocity && !in.has_velocity) {
                continue;
            }

            if (state && *state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
                reads.push_back(transition(
                    as_resource(resource), 0, static_cast<D3D12_RESOURCE_STATES>(*state), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            }
        }
    }

    if (!reads.empty()) {
        list->ResourceBarrier(static_cast<UINT>(reads.size()), reads.data());
    }

    const GuideConstants pass{
        .inverse_view_projection = in.camera.inverse_view_projection,
        .render_size = {static_cast<float>(in.size.width), static_cast<float>(in.size.height)},
        .jitter = in.camera.jitter,
        .old_view_projection = in.camera.old_view_projection,
        .motion = motion_of(in),
        .lut_size = in.lut_size,
        .lut = lut_copied ? 1u : 0u,
    };

    ID3D12DescriptorHeap* const heaps[]{m_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_pass_root_signature.Get());
    list->SetComputeRoot32BitConstants(0, PASS_CONSTANTS, &pass, 0);
    list->SetComputeRootDescriptorTable(1, table);
    list->SetPipelineState(m_pass_pso.Get());
    list->Dispatch(groups(in.size.width), groups(in.size.height), 1);

    if (!reads.empty()) {
        reverse(reads);
        list->ResourceBarrier(static_cast<UINT>(reads.size()), reads.data());
    }

    // The copy of the LUT goes back to COMMON, after its readback.
    if (lut_copied) {
        if (read_back) {
            list->CopyBufferRegion(m_lut_readback.Get(), 0, m_lut.Get(), 0, lut_bytes(m_lut_size));
            m_readback_asked = false;
            m_readback_frame = in.frame;
            m_status.readback = std::format("recorded at frame {}", in.frame);
        }

        const auto back = transition(m_lut.Get(), 0, lut_read_state(read_back), D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &back);
    }

    stamp_segment(list, Segment::GuidePass);
    return table;
}

bool GuidePass::record_debug_view(const GuideInputs& in, ID3D12Resource* o, int guide, float scale) {
    std::scoped_lock _{m_mutex};

    // Nothing RR creates or records here goes through the logic of the hooks.
    const ReentryGuard guard{};

    const auto shown = texture_of(guide);
    const auto table = record_pass(in, o, shown);

    if (!table) {
        return false;
    }

    const auto list = in.list;

    // The debug view reads the guide shown and writes O, each between the barriers that take it there and
    // back. O is a game resource: all its subresources.
    std::vector<D3D12_RESOURCE_BARRIER> view{
        transition(m_textures.resources[static_cast<std::size_t>(shown)].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(o, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };

    list->ResourceBarrier(static_cast<UINT>(view.size()), view.data());

    // DebugConstants.
    const std::array<std::uint32_t, VIEW_CONSTANTS> constants{
        static_cast<std::uint32_t>(guide), std::bit_cast<std::uint32_t>(scale), in.size.width, in.size.height};
    auto view_table = *table;
    view_table.ptr += static_cast<UINT64>(descriptor::VIEW_TABLE) * m_descriptor_size;

    list->SetComputeRootSignature(m_view_root_signature.Get());
    list->SetComputeRoot32BitConstants(0, VIEW_CONSTANTS, constants.data(), 0);
    list->SetComputeRootDescriptorTable(1, view_table);
    list->SetPipelineState(m_view_pso.Get());
    list->Dispatch(groups(in.size.width), groups(in.size.height), 1);

    reverse(view);
    list->ResourceBarrier(static_cast<UINT>(view.size()), view.data());

    stamp_segment(list, Segment::DebugView);
    finish_measurement(list);
    ++m_status.recorded;
    return true;
}

GuidePass::Evaluation GuidePass::record_evaluation(const GuideInputs& in, ID3D12Resource* t, ID3D12Resource* o,
    const std::function<bool(const Guides&)>& evaluate, std::uint32_t forced_nonfinite) {
    std::scoped_lock _{m_mutex};

    // Nothing RR creates or records here, NGX included, goes through the logic of the hooks.
    const ReentryGuard guard{};

    if (!record_pass(in, nullptr, Texture::NormalRoughness)) {
        return {};
    }

    // NGX takes its inputs in NON_PIXEL_SHADER_RESOURCE exactly, and its output in UNORDERED_ACCESS, and
    // leaves them so. T and O are game resources: all their subresources. O's barrier is the second.
    constexpr std::size_t O_BARRIER = 1;
    const auto& r = m_textures.resources;
    std::vector<D3D12_RESOURCE_BARRIER> barriers{
        transition(t, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, static_cast<D3D12_RESOURCE_STATES>(core::expected_state(core::Read::T)),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(o, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };

    for (const auto& texture : r) {
        barriers.push_back(transition(texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }

    const auto list = in.list;
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

    const auto texture = [&](Texture which) { return r[static_cast<std::size_t>(which)].Get(); };
    const auto evaluated = evaluate(Guides{
        .normals_roughness = texture(Texture::NormalRoughness),
        .diffuse_albedo = texture(Texture::Diffuse),
        .specular_albedo = texture(Texture::Specular),
        .depth = texture(Texture::Depth),
        .motion_vectors = texture(Texture::MotionVectors),
    });

    // The barriers back. Once RR wrote O, O goes to NON_PIXEL_SHADER_RESOURCE first, for the count; the
    // TAA writes it when RR did not, so then there is nothing of RR's to count. Nor to time: the segment
    // would hold the barriers alone.
    reverse(barriers);

    if (evaluated) {
        barriers[O_BARRIER].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

    if (evaluated) {
        stamp_segment(list, Segment::Evaluation);
        record_count(list, o, in.size, forced_nonfinite);

        const auto to_render_target = transition(
            o, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &to_render_target);
        stamp_segment(list, Segment::NonFiniteCount);
    }

    finish_measurement(list);
    ++m_status.recorded;
    return {.recorded = true, .evaluated = evaluated};
}

bool GuidePass::record_reference(const ReferenceInputs& in) {
    std::scoped_lock _{m_mutex};

    // Nothing RR creates or records here goes through the logic of the hooks.
    const ReentryGuard guard{};

    if (!ready(in.list) || !ensure_reference(in.size)) {
        return false;
    }

    const auto list = in.list;
    start_measurement(list, in.frame);

    // The game's resources may be freed after the draw, so their views are written anew.
    const auto first = next_slot();
    write_srv(m_device.Get(), in.t, in.t->GetDesc().Format, 0, cpu_descriptor(first + descriptor::REFERENCE_TABLE));
    write_uav(m_device.Get(), in.o, in.o->GetDesc().Format, cpu_descriptor(first + descriptor::REFERENCE_TABLE + 1));

    // T is read in its state at the draw, NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE, as the guide
    // pass reads the G-buffer. O is a game resource: all its subresources.
    std::vector<D3D12_RESOURCE_BARRIER> barriers{
        transition(m_reference_mean.Get(), 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(in.o, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };

    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

    // ReferenceConstants.
    const std::array<std::uint32_t, REFERENCE_CONSTANTS> constants{in.size.width, in.size.height, in.frames_before, 0};
    ID3D12DescriptorHeap* const heaps[]{m_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_reference_root_signature.Get());
    list->SetComputeRoot32BitConstants(0, REFERENCE_CONSTANTS, constants.data(), 0);
    list->SetComputeRootDescriptorTable(1, gpu_descriptor(first + descriptor::REFERENCE_TABLE));
    list->SetComputeRootUnorderedAccessView(2, m_reference_mean->GetGPUVirtualAddress());
    list->SetPipelineState(m_reference_pso.Get());
    list->Dispatch(groups(in.size.width), groups(in.size.height), 1);

    reverse(barriers);
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

    stamp_segment(list, Segment::Reference);
    finish_measurement(list);
    ++m_status.recorded;
    ++m_status.references;
    return true;
}

// A measurement of its own, which starts right before the draw, where RR's recordings start theirs: the
// two timers start at the same point of the list. The TAA's list is recorded by one thread, so nothing
// else records between the two calls.
bool GuidePass::begin_taa_draw(ID3D12GraphicsCommandList* list, std::uint64_t frame) {
    std::scoped_lock _{m_mutex};

    // Nothing RR records here goes through the logic of the hooks.
    const ReentryGuard guard{};
    m_taa_draw.reset();

    if (m_device == nullptr || !ready(list)) {
        return false;
    }

    start_measurement(list, frame);
    m_taa_draw = m_measuring;
    return m_taa_draw.has_value();
}

void GuidePass::end_taa_draw(ID3D12GraphicsCommandList* list) {
    std::scoped_lock _{m_mutex};
    const ReentryGuard guard{};

    if (!m_taa_draw || m_measuring != m_taa_draw) {
        m_taa_draw.reset();
        return;
    }

    m_taa_draw.reset();
    stamp_segment(list, Segment::TaaDraw);
    finish_measurement(list);
    ++m_status.timed_taa_draws;
}

GuidePass::Status GuidePass::status() const {
    std::scoped_lock _{m_mutex};
    return m_status;
}

const char* to_string(Segment segment) {
    switch (segment) {
    case Segment::GuidePass:
        return "guide pass";
    case Segment::Evaluation:
        return "RR's evaluation";
    case Segment::DebugView:
        return "debug view";
    case Segment::Reference:
        return "accumulated reference";
    case Segment::NonFiniteCount:
        return "non-finite count";
    case Segment::TaaDraw:
        return "the game's TAA draw";
    }

    return "?";
}

GuidePass::Texture GuidePass::texture_of(int guide) {
    using namespace core::guides;

    if (guide == DEBUG_VIEW_NORMALS || guide == DEBUG_VIEW_ROUGHNESS) {
        return Texture::NormalRoughness;
    }

    if (guide == DEBUG_VIEW_DIFFUSE) {
        return Texture::Diffuse;
    }

    if (guide == DEBUG_VIEW_SPECULAR) {
        return Texture::Specular;
    }

    return guide == DEBUG_VIEW_DEPTH ? Texture::Depth : Texture::MotionVectors;
}
} // namespace rr::d3d12
