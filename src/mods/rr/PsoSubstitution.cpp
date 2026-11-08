#include <algorithm>
#include <atomic>
#include <mutex>

#include "PsoSubstitution.hpp"

namespace rr::d3d12 {
namespace {
struct Entry {
    core::ShaderDigest digest{};
    std::atomic<const Substitute*> substitute{};              // read by every SetPipelineState of its digest
    std::atomic<ID3D12RootSignature*> bound_root_signature{}; // of the last PSO bound
    std::atomic<bool> bound{};                                // since the last take_bound_shaders

    // Under g_mutex.
    std::vector<std::uint8_t> bytecode{};

    // Every root signature the game created it with, with a reference of the hooks': their addresses stay theirs,
    // so that comparing them means something. Raw, so that no destructor releases them as the process exits.
    std::vector<ID3D12RootSignature*> root_signatures{};
};

// Sorted by digest, and never destroyed: every recording thread reads it.
struct Watch {
    explicit Watch(std::size_t count)
        : entries(count) {}

    std::vector<Entry> entries;
};

std::atomic<Watch*> g_watch{};
std::atomic<bool> g_enabled{};
std::atomic<std::uint64_t> g_substitute_binds{};
std::mutex g_mutex{};

Entry* find_entry(const core::ShaderDigest& digest) {
    const auto watch = g_watch.load(std::memory_order_acquire);

    if (watch == nullptr) {
        return nullptr;
    }

    auto& entries = watch->entries;
    const auto it = std::lower_bound(
        entries.begin(), entries.end(), digest, [](const Entry& e, const core::ShaderDigest& d) { return e.digest.bytes < d.bytes; });

    return it != entries.end() && it->digest == digest ? &*it : nullptr;
}
} // namespace

void watch_compute_shaders(std::span<const core::ShaderDigest> digests) {
    std::vector<core::ShaderDigest> sorted(digests.begin(), digests.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.bytes < b.bytes; });

    auto watch = new Watch{sorted.size()};

    for (std::size_t i = 0; i < sorted.size(); ++i) {
        watch->entries[i].digest = sorted[i];
    }

    Watch* none{};

    if (!g_watch.compare_exchange_strong(none, watch, std::memory_order_acq_rel)) {
        delete watch;
    }
}

std::optional<std::vector<std::uint8_t>> watched_bytecode(const core::ShaderDigest& digest) {
    const auto entry = find_entry(digest);

    if (entry == nullptr) {
        return std::nullopt;
    }

    std::scoped_lock _{g_mutex};

    if (entry->bytecode.empty()) {
        return std::nullopt;
    }

    return entry->bytecode;
}

void set_substitution_enabled(bool on) {
    g_enabled.store(on, std::memory_order_release);
}

std::vector<BoundShader> take_bound_shaders() {
    std::vector<BoundShader> bound{};

    if (const auto watch = g_watch.load(std::memory_order_acquire); watch != nullptr) {
        for (auto& entry : watch->entries) {
            if (entry.bound.exchange(false, std::memory_order_acquire)) {
                bound.push_back({entry.digest, entry.bound_root_signature.load(std::memory_order_relaxed)});
            }
        }
    }

    return bound;
}

void set_substitute(const core::ShaderDigest& digest, const Substitute* substitute) {
    if (const auto entry = find_entry(digest); entry != nullptr) {
        entry->substitute.store(substitute, std::memory_order_release);
    }
}

std::uint64_t substitute_binds() {
    return g_substitute_binds.load(std::memory_order_relaxed);
}

void on_compute_pso_created(const D3D12_SHADER_BYTECODE& cs, ID3D12RootSignature* root_signature) {
    if (g_watch.load(std::memory_order_acquire) == nullptr) {
        return;
    }

    const auto digest = digest_of(cs);
    const auto entry = digest ? find_entry(*digest) : nullptr;

    if (entry == nullptr) {
        return;
    }

    std::scoped_lock _{g_mutex};

    if (entry->bytecode.empty()) {
        const auto bytes = static_cast<const std::uint8_t*>(cs.pShaderBytecode);
        entry->bytecode.assign(bytes, bytes + cs.BytecodeLength);
    }

    auto& kept = entry->root_signatures;

    if (root_signature != nullptr && std::find(kept.begin(), kept.end(), root_signature) == kept.end()) {
        root_signature->AddRef();
        kept.push_back(root_signature);
    }
}

bool substitution_enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

ID3D12PipelineState* pso_to_bind(ID3D12PipelineState* pso, const std::optional<PsoShaders>& shaders) {
    const auto entry = shaders && shaders->cs ? find_entry(*shaders->cs) : nullptr;

    if (entry == nullptr) {
        return pso;
    }

    entry->bound_root_signature.store(shaders->cs_root_signature, std::memory_order_relaxed);
    entry->bound.store(true, std::memory_order_release);

    const auto substitute = entry->substitute.load(std::memory_order_acquire);

    if (substitute == nullptr || substitute->root_signature != shaders->cs_root_signature) {
        return pso;
    }

    g_substitute_binds.fetch_add(1, std::memory_order_relaxed);
    return substitute->pso;
}
} // namespace rr::d3d12
