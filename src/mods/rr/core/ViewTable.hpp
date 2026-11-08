#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

namespace rr::core {
// What a render target or depth-stencil view descriptor holds, as far as RR needs it.
struct View {
    void* resource{};      // the ID3D12Resource, opaque here; null for a null descriptor
    std::uint32_t flags{}; // the D3D12_DSV_FLAGS of a depth-stencil view; 0 for a render target view

    friend bool operator==(const View&, const View&) = default;
};

// A run of consecutive descriptors in a copy: the CPU handle of the first one and how many there are.
struct DescriptorRange {
    std::uint64_t start{};
    std::uint32_t count{};
};

// The render target and depth-stencil views the game has written, by CPU descriptor handle, so that
// OMSetRenderTargets can name the resources it binds. Not thread-safe.
class ViewTable {
public:
    // CreateRenderTargetView and CreateDepthStencilView.
    void write(std::uint64_t handle, View view);

    // CopyDescriptors and CopyDescriptorsSimple. Both sides list the same descriptors, in ranges that
    // need not line up; increment is the descriptor size of the heap type.
    void copy(std::span<const DescriptorRange> dst, std::span<const DescriptorRange> src, std::uint32_t increment);

    // None if no view was ever written or copied at this handle.
    std::optional<View> find(std::uint64_t handle) const;

    std::size_t size() const { return m_views.size(); }

private:
    std::unordered_map<std::uint64_t, View> m_views{};
};
} // namespace rr::core
