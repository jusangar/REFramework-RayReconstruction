#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>

#include "core/Profile.hpp"
#include "core/ShaderDigest.hpp"

namespace rr::d3d12 {
// How the game created a PSO.
enum class PsoSource : std::uint8_t {
    Graphics,        // ID3D12Device::CreateGraphicsPipelineState
    Compute,         // ID3D12Device::CreateComputePipelineState
    Stream,          // ID3D12Device2::CreatePipelineState
    LibraryGraphics, // ID3D12PipelineLibrary::LoadGraphicsPipeline
    LibraryCompute,  // ID3D12PipelineLibrary::LoadComputePipeline
    LibraryStream,   // ID3D12PipelineLibrary1::LoadPipeline
    Count,
};

const char* to_string(PsoSource source);

// The digests of a PSO's shaders: none for a stage it does not have, or whose shader has no HASH part.
struct PsoShaders {
    std::optional<core::ShaderDigest> vs{};
    std::optional<core::ShaderDigest> ps{};
    std::optional<core::ShaderDigest> cs{};

    // What the compute shader declares at the profile's AmbientBRDF register, from its PSV0 part as the PSO
    // is created: the bytecode is gone after that.
    core::SrvDeclaration cs_lut{core::SrvDeclaration::Unknown};

    // The root signature a compute PSO was created with, only to compare: none if its shader carries its own.
    ID3D12RootSignature* cs_root_signature{};

    // The other stages, which only digest discovery logs.
    std::optional<core::ShaderDigest> ds{};
    std::optional<core::ShaderDigest> hs{};
    std::optional<core::ShaderDigest> gs{};
    std::optional<core::ShaderDigest> as{};
    std::optional<core::ShaderDigest> ms{};
};

// The digest of one shader of a PSO description; none without bytecode or without a HASH part.
std::optional<core::ShaderDigest> digest_of(const D3D12_SHADER_BYTECODE& bytecode);

// Whether there is bytecode whose container has no HASH part.
bool lacks_digest(const D3D12_SHADER_BYTECODE& bytecode);

// What the profile says of the pixel shader of a draw with the TAA's vertex shader.
enum class PsRole : std::uint8_t {
    Admitted,
    Excluded,
    NotInProfile,
};

PsRole ps_role(const core::Profile& profile, const std::optional<core::ShaderDigest>& ps);
const char* to_string(PsRole role);

std::string to_string(const std::optional<core::ShaderDigest>& digest);

// Every PSO the game created since the device hook, with the digests of its shaders, so that a draw
// can be told by the shaders of the PSO it has bound. Thread-safe.
class PsoRegistry {
public:
    struct Counts {
        std::array<std::uint64_t, static_cast<std::size_t>(PsoSource::Count)> by_source{};
        std::uint64_t without_digest{};        // with some shader that has no HASH part
        std::uint64_t taa_vs{};                // created with the profile's TAA vertex shader
        std::uint64_t taa_vs_ps{};             // distinct pixel shaders among them, which recreations do not change
        std::uint64_t pt_cs{};                 // created with the profile's PT compute shader
        std::uint64_t stream_parse_failures{}; // streams with a subobject the parser does not know
        std::uint64_t libraries{};
        std::uint64_t library_failures{};
    };

    explicit PsoRegistry(const core::Profile& profile)
        : m_profile{profile} {}

    const core::Profile& profile() const { return m_profile; }

    // At creation. A PSO at the address of a released one takes its place.
    void add(ID3D12PipelineState* pso, PsoSource source, const PsoShaders& shaders, bool without_digest);
    std::optional<PsoShaders> find(ID3D12PipelineState* pso) const;

    void note_stream_parse_failure();
    void note_library(bool created);

    Counts counts() const;

    // Digest discovery. Switching it on logs every PSO created so far, since they come in the first
    // seconds, before the Mod's config loads; while it is on, every new one is logged too.
    void set_discovery(bool on);

    // 0 while discovery is off; otherwise a number that changes every time it is switched on, so that
    // what is logged once per discovery is logged again after switching it off and on.
    std::uint32_t discovery_session() const;

private:
    using Entry = std::pair<PsoSource, PsoShaders>;

    static void log(ID3D12PipelineState* pso, const Entry& entry);

    const core::Profile& m_profile;

    mutable std::shared_mutex m_mutex{};
    std::unordered_map<ID3D12PipelineState*, Entry> m_psos{};
    std::vector<std::optional<core::ShaderDigest>> m_taa_ps{}; // the distinct pixel shaders with the TAA's vertex shader
    Counts m_counts{};
    std::uint32_t m_discovery_session{};
    std::uint32_t m_last_session{};
};
} // namespace rr::d3d12
