#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rr::core {
// The types of descriptor a table holds, with the values of D3D12_DESCRIPTOR_RANGE_TYPE.
enum class DescriptorType : std::uint8_t {
    Srv = 0,
    Uav = 1,
    Cbv = 2,
    Sampler = 3,
};

// D3D12's NumDescriptors of an unbounded range, and its D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND.
inline constexpr std::uint32_t UNBOUNDED_RANGE = 0xffffffff;
inline constexpr std::uint32_t APPEND_RANGE = 0xffffffff;

// A range of a descriptor table, as D3D12_DESCRIPTOR_RANGE gives it.
struct TableRange {
    DescriptorType type{};
    std::uint32_t count{}; // UNBOUNDED_RANGE: every register from the base
    std::uint32_t base_register{};
    std::uint32_t space{};
    std::uint32_t offset{}; // descriptors from the table's start, or APPEND_RANGE: right after the previous range
};

// A root parameter of a root signature, as D3D12_ROOT_PARAMETER gives it, so that the core does not need
// d3d12.h. Without its shader visibility: a compute pipeline sees every parameter.
struct RootParameter {
    enum class Kind : std::uint8_t { Table, Constants, Cbv, Srv, Uav };

    Kind kind{};
    std::vector<TableRange> ranges{}; // a table's
    std::uint32_t shader_register{};  // root constants' and a root view's
    std::uint32_t space{};
};

// Where a root signature binds a shader register.
struct RegisterBinding {
    std::uint32_t parameter{};
    RootParameter::Kind kind{};
    std::uint32_t offset{}; // in a table: descriptors from its start
};

// The parameter that binds a register of a type, which root constants count as a CBV. None if the
// parameters bind nothing there.
std::optional<RegisterBinding> find_register(
    std::span<const RootParameter> parameters, DescriptorType type, std::uint32_t shader_register, std::uint32_t space);
} // namespace rr::core
