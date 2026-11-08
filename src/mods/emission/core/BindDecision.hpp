#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ShaderEdit.hpp"

namespace emission {
// What the correction has for a digest, with the root signature the game last bound it with.
enum class VariantState : std::uint8_t {
    NoCorrection, // not in the table, or not prepared yet
    Preparing,    // its corrected PSO is being created
    Active,       // its corrected PSO is there
    Failed,       // it could not be created, and it is not tried again in this session
};

const char* to_string(VariantState state);

// A root signature, only to tell it from another. A PSO runs with the one it was created with, so each one takes a
// corrected PSO of its own.
using RootSignature = const void*;

// A digest that went without its correction, for the log. Its state says whether something broke: Failed, or
// NoCorrection for a variant outside the table, which is what 0 bounces and Hybrid run.
struct Failure {
    rr::core::ShaderDigest digest{};
    VariantState state{};
    std::string why{};
};

// The reason of a corrected PSO the runtime did not create.
std::string creation_failure(std::uint32_t hresult);

// Which PSO goes bound for each digest: the corrected one or the game's. Pure: the Mod feeds it what happens and
// acts on its answers. The option does not throw anything away, so turning it off and on binds again without
// creating again. Nothing here depends on RR.
class BindDecision {
public:
    BindDecision(std::span<const Variant> variants, bool enabled);

    void on_option(bool enabled);

    // The game bound a shader with this digest and this root signature. True if the Mod has to create its
    // corrected PSO now: a variant is prepared the first time it appears with the option on, and again the first
    // time the game binds it with another root signature.
    [[nodiscard]] bool on_bound(const rr::core::ShaderDigest& digest, RootSignature root_signature);

    // The PT ran this variant, whether the table has it or not. What the game binds only says that for the
    // digests of the table, so this is where a variant without a correction shows up.
    void on_variant_ran(const rr::core::ShaderDigest& digest);

    // The creation a true from on_bound asked for ended.
    void on_created(const rr::core::ShaderDigest& digest, RootSignature root_signature);
    void on_failed(const rr::core::ShaderDigest& digest, RootSignature root_signature, const std::string& why);

    bool enabled() const { return m_enabled; }

    VariantState state(const rr::core::ShaderDigest& digest) const;

    // The root signature whose corrected PSO goes bound in place of the game's, or none for the game's PSO.
    std::optional<RootSignature> corrected_root_signature(const rr::core::ShaderDigest& digest) const;

    // The digest of the last on_bound or on_variant_ran with the option on.
    std::optional<rr::core::ShaderDigest> current() const { return m_current; }

    // The failures since the last call. Each one comes out once in the session, so that one line of the log per
    // failure takes no bookkeeping in the Mod.
    std::vector<Failure> take_failures();

    // The line of the interface: the state of the current digest, with the reason if it failed.
    std::string status() const;

private:
    struct Attempt {
        VariantState state{};
        std::string why{}; // when it failed
    };

    struct Record {
        RootSignature bound_root_signature{};        // the last one the game bound it with
        std::map<RootSignature, Attempt> attempts{}; // never erased: a failure is not tried again
    };

    // The digest is the current one from now on. A digest outside the table is a failure, the first time.
    Record& see(const rr::core::ShaderDigest& digest);

    const Attempt* bound_attempt(const rr::core::ShaderDigest& digest) const;
    Attempt* attempt_under_way(const rr::core::ShaderDigest& digest, RootSignature root_signature);

    std::span<const Variant> m_variants{};
    bool m_enabled{};
    std::optional<rr::core::ShaderDigest> m_current{};
    std::map<decltype(rr::core::ShaderDigest::bytes), Record> m_records{}; // only the digests seen with the option on
    std::vector<Failure> m_failures{};
};
} // namespace emission
