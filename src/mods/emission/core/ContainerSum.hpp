#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace emission {
// The sum of a DXIL container: the 16 bytes after "DXBC". The runtime of D3D12 recomputes it as it creates a PSO
// and refuses a shader whose sum does not match. dxil.dll writes it; this computes the same one without it.
using ContainerSum = std::array<std::uint8_t, 16>;

inline constexpr std::size_t CONTAINER_SUM_OFFSET = 4;
inline constexpr std::size_t CONTAINER_SUM_END = CONTAINER_SUM_OFFSET + std::tuple_size_v<ContainerSum>; // what it covers starts here

// All of these take a whole container, with its header.
ContainerSum compute_container_sum(std::span<const std::uint8_t> container);
ContainerSum stored_container_sum(std::span<const std::uint8_t> container);
void store_container_sum(std::span<std::uint8_t> container, const ContainerSum& sum);

// The size its header declares.
std::size_t declared_container_size(std::span<const std::uint8_t> container);
} // namespace emission
