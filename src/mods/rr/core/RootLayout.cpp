#include "RootLayout.hpp"

namespace rr::core {
namespace {
// The type of register a root parameter that is not a table binds.
DescriptorType type_of(RootParameter::Kind kind) {
    switch (kind) {
    case RootParameter::Kind::Srv:
        return DescriptorType::Srv;
    case RootParameter::Kind::Uav:
        return DescriptorType::Uav;
    default:
        return DescriptorType::Cbv; // root constants too
    }
}

bool holds(const TableRange& range, std::uint32_t shader_register) {
    return shader_register >= range.base_register &&
        (range.count == UNBOUNDED_RANGE || shader_register - range.base_register < range.count);
}
} // namespace

std::optional<RegisterBinding> find_register(
    std::span<const RootParameter> parameters, DescriptorType type, std::uint32_t shader_register, std::uint32_t space) {
    for (std::uint32_t i = 0; i < parameters.size(); ++i) {
        const auto& p = parameters[i];

        if (p.kind != RootParameter::Kind::Table) {
            if (type_of(p.kind) == type && p.shader_register == shader_register && p.space == space) {
                return RegisterBinding{.parameter = i, .kind = p.kind};
            }

            continue;
        }

        // Where an appended range starts. D3D12 does not let a range be appended after an unbounded one.
        std::uint32_t next = 0;

        for (const auto& range : p.ranges) {
            const auto start = range.offset == APPEND_RANGE ? next : range.offset;

            if (range.type == type && range.space == space && holds(range, shader_register)) {
                return RegisterBinding{.parameter = i, .kind = p.kind, .offset = start + (shader_register - range.base_register)};
            }

            next = range.count == UNBOUNDED_RANGE ? start : start + range.count;
        }
    }

    return std::nullopt;
}
} // namespace rr::core
