#include <format>
#include <utility>

#include "BindDecision.hpp"

namespace emission {
const char* to_string(VariantState state) {
    switch (state) {
    case VariantState::NoCorrection:
        return "no correction for this variant";
    case VariantState::Preparing:
        return "preparing";
    case VariantState::Active:
        return "active";
    case VariantState::Failed:
        return "failed";
    }

    return "?";
}

std::string creation_failure(std::uint32_t hresult) {
    return std::format("creating the PSO failed ({:#x})", hresult);
}

BindDecision::BindDecision(std::span<const Variant> variants, bool enabled)
    : m_variants{variants}
    , m_enabled{enabled} {
}

void BindDecision::on_option(bool enabled) {
    m_enabled = enabled;
}

BindDecision::Record& BindDecision::see(const rr::core::ShaderDigest& digest) {
    m_current = digest;

    const auto [record, first_time] = m_records.try_emplace(digest.bytes);

    if (first_time && find_variant(m_variants, digest) == nullptr) {
        m_failures.push_back({digest, VariantState::NoCorrection, to_string(Refusal::NotInTable)});
    }

    return record->second;
}

void BindDecision::on_variant_ran(const rr::core::ShaderDigest& digest) {
    if (m_enabled) {
        see(digest);
    }
}

bool BindDecision::on_bound(const rr::core::ShaderDigest& digest, RootSignature root_signature) {
    if (!m_enabled) {
        return false;
    }

    auto& record = see(digest);

    if (find_variant(m_variants, digest) == nullptr) {
        return false;
    }

    record.bound_root_signature = root_signature;

    if (record.attempts.contains(root_signature)) {
        return false;
    }

    if (root_signature == nullptr) {
        const std::string why{"the root signature it was created with is unknown"};
        record.attempts[root_signature] = {VariantState::Failed, why};
        m_failures.push_back({digest, VariantState::Failed, why});
        return false;
    }

    record.attempts[root_signature].state = VariantState::Preparing;
    return true;
}

BindDecision::Attempt* BindDecision::attempt_under_way(const rr::core::ShaderDigest& digest, RootSignature root_signature) {
    const auto record = m_records.find(digest.bytes);

    if (record == m_records.end()) {
        return nullptr;
    }

    const auto attempt = record->second.attempts.find(root_signature);
    return attempt != record->second.attempts.end() && attempt->second.state == VariantState::Preparing ? &attempt->second : nullptr;
}

void BindDecision::on_created(const rr::core::ShaderDigest& digest, RootSignature root_signature) {
    if (const auto attempt = attempt_under_way(digest, root_signature); attempt != nullptr) {
        attempt->state = VariantState::Active;
    }
}

void BindDecision::on_failed(const rr::core::ShaderDigest& digest, RootSignature root_signature, const std::string& why) {
    if (const auto attempt = attempt_under_way(digest, root_signature); attempt != nullptr) {
        attempt->state = VariantState::Failed;
        attempt->why = why;
        m_failures.push_back({digest, VariantState::Failed, why});
    }
}

std::vector<Failure> BindDecision::take_failures() {
    return std::exchange(m_failures, {});
}

const BindDecision::Attempt* BindDecision::bound_attempt(const rr::core::ShaderDigest& digest) const {
    const auto record = m_records.find(digest.bytes);

    if (record == m_records.end()) {
        return nullptr;
    }

    const auto attempt = record->second.attempts.find(record->second.bound_root_signature);
    return attempt != record->second.attempts.end() ? &attempt->second : nullptr;
}

VariantState BindDecision::state(const rr::core::ShaderDigest& digest) const {
    const auto attempt = bound_attempt(digest);
    return attempt != nullptr ? attempt->state : VariantState::NoCorrection;
}

std::optional<RootSignature> BindDecision::corrected_root_signature(const rr::core::ShaderDigest& digest) const {
    if (!m_enabled || state(digest) != VariantState::Active) {
        return std::nullopt;
    }

    return m_records.at(digest.bytes).bound_root_signature;
}

std::string BindDecision::status() const {
    if (!m_enabled) {
        return "off: the game's PSO";
    }

    if (!m_current) {
        return "waiting for a variant of the PT";
    }

    const auto digest = rr::core::to_string(*m_current);

    if (const auto attempt = bound_attempt(*m_current); attempt != nullptr && attempt->state == VariantState::Failed) {
        return std::format("{} ({}): {}", to_string(attempt->state), digest, attempt->why);
    }

    return std::format("{} ({})", to_string(state(*m_current)), digest);
}
} // namespace emission
