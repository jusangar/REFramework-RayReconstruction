#include <algorithm>

#include "ContainerSum.hpp"
#include "ShaderEdit.hpp"

namespace emission {
namespace {
bool fits(const Field& field, std::span<const std::uint8_t> container) {
    return std::uint64_t{field.bit} + FIELD_WIDTH <= std::uint64_t{container.size()} * 8;
}

std::uint32_t read_field(std::span<const std::uint8_t> container, std::uint32_t bit) {
    std::uint32_t value{};

    for (std::uint32_t i = 0; i < FIELD_WIDTH; ++i) {
        const auto position = bit + i;
        value |= static_cast<std::uint32_t>((container[position / 8] >> (position % 8)) & 1) << i;
    }

    return value;
}

void write_field(std::span<std::uint8_t> container, std::uint32_t bit, std::uint32_t value) {
    for (std::uint32_t i = 0; i < FIELD_WIDTH; ++i) {
        const auto position = bit + i;
        auto& byte = container[position / 8];
        const auto mask = static_cast<std::uint8_t>(1u << (position % 8));
        byte = static_cast<std::uint8_t>(((value >> i) & 1) != 0 ? byte | mask : byte & ~mask);
    }
}
} // namespace

const char* to_string(Refusal refusal) {
    switch (refusal) {
    case Refusal::NotInTable:
        return "its digest is not in the table";
    case Refusal::InvalidContainer:
        return "it is not a whole DXIL container";
    case Refusal::WrongSum:
        return "its container sum is not the expected one";
    case Refusal::UnexpectedValue:
        return "a field does not hold its expected value";
    }

    return "?";
}

const Variant* find_variant(std::span<const Variant> variants, const rr::core::ShaderDigest& digest) {
    const auto it = std::find_if(variants.begin(), variants.end(), [&](const Variant& v) { return v.digest == digest; });
    return it != variants.end() ? &*it : nullptr;
}

ShaderEdit correct_shader(std::span<const Variant> variants, std::span<const std::uint8_t> bytecode) {
    const auto digest = rr::core::read_shader_digest(bytecode);

    if (!digest) {
        return Refusal::InvalidContainer;
    }

    const auto variant = find_variant(variants, *digest);

    if (variant == nullptr) {
        return Refusal::NotInTable;
    }

    if (declared_container_size(bytecode) != bytecode.size()) {
        return Refusal::InvalidContainer;
    }

    if (stored_container_sum(bytecode) != compute_container_sum(bytecode)) {
        return Refusal::WrongSum;
    }

    for (const auto& field : variant->fields) {
        if (!fits(field, bytecode) || read_field(bytecode, field.bit) != field.expected) {
            return Refusal::UnexpectedValue;
        }
    }

    std::vector<std::uint8_t> corrected(bytecode.begin(), bytecode.end());

    for (const auto& field : variant->fields) {
        write_field(corrected, field.bit, field.corrected);
    }

    store_container_sum(corrected, compute_container_sum(corrected));
    return corrected;
}
} // namespace emission
