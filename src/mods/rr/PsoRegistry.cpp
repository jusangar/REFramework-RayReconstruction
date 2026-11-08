#include <algorithm>
#include <format>
#include <mutex>

#include <spdlog/spdlog.h>

#include "PsoRegistry.hpp"

namespace rr::d3d12 {
const char* to_string(PsoSource source) {
    switch (source) {
    case PsoSource::Graphics:
        return "CreateGraphicsPipelineState";
    case PsoSource::Compute:
        return "CreateComputePipelineState";
    case PsoSource::Stream:
        return "CreatePipelineState";
    case PsoSource::LibraryGraphics:
        return "LoadGraphicsPipeline";
    case PsoSource::LibraryCompute:
        return "LoadComputePipeline";
    case PsoSource::LibraryStream:
        return "LoadPipeline";
    default:
        return "?";
    }
}

std::optional<core::ShaderDigest> digest_of(const D3D12_SHADER_BYTECODE& bytecode) {
    if (bytecode.pShaderBytecode == nullptr || bytecode.BytecodeLength == 0) {
        return std::nullopt;
    }

    return core::read_shader_digest({static_cast<const std::uint8_t*>(bytecode.pShaderBytecode), bytecode.BytecodeLength});
}

bool lacks_digest(const D3D12_SHADER_BYTECODE& bytecode) {
    return bytecode.pShaderBytecode != nullptr && bytecode.BytecodeLength != 0 && !digest_of(bytecode);
}

PsRole ps_role(const core::Profile& profile, const std::optional<core::ShaderDigest>& ps) {
    if (ps && std::find(profile.admitted_ps.begin(), profile.admitted_ps.end(), *ps) != profile.admitted_ps.end()) {
        return PsRole::Admitted;
    }

    if (ps && std::find(profile.excluded_ps.begin(), profile.excluded_ps.end(), *ps) != profile.excluded_ps.end()) {
        return PsRole::Excluded;
    }

    return PsRole::NotInProfile;
}

const char* to_string(PsRole role) {
    switch (role) {
    case PsRole::Admitted:
        return "admitted";
    case PsRole::Excluded:
        return "excluded";
    default:
        return "not in the profile";
    }
}

std::string to_string(const std::optional<core::ShaderDigest>& digest) {
    return digest ? core::to_string(*digest) : std::string{"-"};
}

void PsoRegistry::add(ID3D12PipelineState* pso, PsoSource source, const PsoShaders& shaders, bool without_digest) {
    if (pso == nullptr) {
        return;
    }

    const auto taa = shaders.vs == m_profile.taa_vs;
    const auto pt = shaders.cs == m_profile.pt_cs;
    bool discovery{};

    {
        std::unique_lock _{m_mutex};
        m_psos[pso] = {source, shaders};
        ++m_counts.by_source[static_cast<std::size_t>(source)];
        m_counts.without_digest += without_digest ? 1 : 0;
        m_counts.taa_vs += taa ? 1 : 0;
        m_counts.pt_cs += pt ? 1 : 0;

        if (taa && std::find(m_taa_ps.begin(), m_taa_ps.end(), shaders.ps) == m_taa_ps.end()) {
            m_taa_ps.push_back(shaders.ps);
            m_counts.taa_vs_ps = m_taa_ps.size();
        }

        discovery = m_discovery_session != 0;
    }

    // The few PSOs the profile names are always logged.
    if (taa) {
        spdlog::info("[RayReconstruction] PSO {:x} with the TAA vertex shader {}: pixel shader {} ({}), from {}",
            reinterpret_cast<std::uintptr_t>(pso), to_string(shaders.vs), to_string(shaders.ps), to_string(ps_role(m_profile, shaders.ps)),
            to_string(source));
    } else if (pt) {
        spdlog::info("[RayReconstruction] PSO {:x} with the PT compute shader {}, from {}", reinterpret_cast<std::uintptr_t>(pso),
            to_string(shaders.cs), to_string(source));
    } else if (discovery) {
        log(pso, {source, shaders});
    }
}

std::optional<PsoShaders> PsoRegistry::find(ID3D12PipelineState* pso) const {
    std::shared_lock _{m_mutex};

    if (const auto it = m_psos.find(pso); it != m_psos.end()) {
        return it->second.second;
    }

    return std::nullopt;
}

void PsoRegistry::note_stream_parse_failure() {
    std::unique_lock _{m_mutex};
    ++m_counts.stream_parse_failures;
}

void PsoRegistry::note_library(bool created) {
    std::unique_lock _{m_mutex};
    ++(created ? m_counts.libraries : m_counts.library_failures);
}

PsoRegistry::Counts PsoRegistry::counts() const {
    std::shared_lock _{m_mutex};
    return m_counts;
}

void PsoRegistry::set_discovery(bool on) {
    std::vector<std::pair<ID3D12PipelineState*, Entry>> so_far{};

    {
        std::unique_lock _{m_mutex};

        if (on == (m_discovery_session != 0)) {
            return;
        }

        m_discovery_session = on ? ++m_last_session : 0;

        // Copied in the same lock that switches discovery on: a PSO created after it is logged by add,
        // so none is lost or logged twice, and the hooks do not wait for the log.
        if (on) {
            so_far.assign(m_psos.begin(), m_psos.end());
        }
    }

    if (!on) {
        spdlog::info("[RayReconstruction] Digest discovery off");
        return;
    }

    spdlog::info("[RayReconstruction] Digest discovery on: the {} PSOs created so far", so_far.size());

    for (const auto& [pso, entry] : so_far) {
        log(pso, entry);
    }
}

std::uint32_t PsoRegistry::discovery_session() const {
    std::shared_lock _{m_mutex};
    return m_discovery_session;
}

void PsoRegistry::log(ID3D12PipelineState* pso, const Entry& entry) {
    const auto& [source, shaders] = entry;
    std::string others{};

    // Only the other stages the PSO has.
    for (const auto& [name, digest] : {std::pair{"DS", &shaders.ds}, std::pair{"HS", &shaders.hs}, std::pair{"GS", &shaders.gs},
             std::pair{"AS", &shaders.as}, std::pair{"MS", &shaders.ms}}) {
        if (*digest) {
            others += std::format(" {} {}", name, to_string(*digest));
        }
    }

    spdlog::info("[RayReconstruction] Digests of PSO {:x}: VS {} PS {} CS {}{}, from {}", reinterpret_cast<std::uintptr_t>(pso),
        to_string(shaders.vs), to_string(shaders.ps), to_string(shaders.cs), others, to_string(source));
}
} // namespace rr::d3d12
