#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <windows.h>

#include <d3d12.h>

#include "PsoRegistry.hpp"
#include "core/ShaderDigest.hpp"

// Binding another compute PSO in place of the game's, by the digest of its shader. The hooks do not know why: a
// Mod asks them to follow some digests, makes its own PSOs and hands them over. Only the runtime sees those: the
// shadow state, the barrier rule, the LUT and the counters go on with the game's PSO, and the registry never
// hears of them.
namespace rr::d3d12 {
// The compute shaders to follow, once and before the game can create a PSO: the bytecode only exists during
// that call. From the load notification of d3d12.dll. Only the first call does anything.
void watch_compute_shaders(std::span<const core::ShaderDigest> digests);

// The bytecode of a watched compute shader. None if the digest is not watched, or the game has not created it yet.
std::optional<std::vector<std::uint8_t>> watched_bytecode(const core::ShaderDigest& digest);

// A compute PSO that never goes through the hooks, so that it stays out of the registry. It can take long: not on
// a thread that records or presents. The reference is the caller's.
HRESULT create_compute_pso_outside_registry(
    std::span<const std::uint8_t> bytecode, ID3D12RootSignature* root_signature, ID3D12PipelineState** pso);

// Whether SetPipelineState looks at the watched digests. Off, its only cost is reading one atomic.
void set_substitution_enabled(bool on);

// A watched digest the game bound, with the root signature of the last of its PSOs it bound. That one is null if the
// shader carries its own, and the hooks keep it alive for the whole session.
struct BoundShader {
    core::ShaderDigest digest{};
    ID3D12RootSignature* root_signature{};
};

// Those the game bound since the last call, with the substitution enabled.
std::vector<BoundShader> take_bound_shaders();

// A PSO to bind in place of the game's. A PSO has to run with the root signature it was created with, so it only
// stands in for the game's PSOs created with this one: any other stays the game's.
struct Substitute {
    ID3D12PipelineState* pso{};
    ID3D12RootSignature* root_signature{};
};

// What SetPipelineState binds from now on in place of the PSOs with this digest, or null for the game's. It has to
// outlive the session: a recording thread may be reading it, and a list in flight may have its PSO bound.
void set_substitute(const core::ShaderDigest& digest, const Substitute* substitute);

// The calls to SetPipelineState so far that bound a substitute.
std::uint64_t substitute_binds();

// Internal to the D3D12 hooks.

// As the game creates a compute PSO, by any of its paths, with the root signature it gave.
void on_compute_pso_created(const D3D12_SHADER_BYTECODE& cs, ID3D12RootSignature* root_signature);

bool substitution_enabled();

// What goes to the runtime for a PSO of the game's. With the substitution enabled.
ID3D12PipelineState* pso_to_bind(ID3D12PipelineState* pso, const std::optional<PsoShaders>& shaders);
} // namespace rr::d3d12
