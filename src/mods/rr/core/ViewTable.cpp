#include <algorithm>

#include "ViewTable.hpp"

namespace rr::core {
void ViewTable::write(std::uint64_t handle, View view) {
    m_views[handle] = view;
}

void ViewTable::copy(std::span<const DescriptorRange> dst, std::span<const DescriptorRange> src, std::uint32_t increment) {
    std::size_t d = 0;
    std::size_t s = 0;
    std::uint32_t d_offset = 0;
    std::uint32_t s_offset = 0;

    while (d < dst.size() && s < src.size()) {
        if (d_offset >= dst[d].count) {
            ++d;
            d_offset = 0;
            continue;
        }

        if (s_offset >= src[s].count) {
            ++s;
            s_offset = 0;
            continue;
        }

        const auto n = std::min(dst[d].count - d_offset, src[s].count - s_offset);

        for (std::uint32_t i = 0; i < n; ++i) {
            const auto to = dst[d].start + std::uint64_t{d_offset + i} * increment;

            // A handle never names a resource it no longer views.
            if (const auto from = m_views.find(src[s].start + std::uint64_t{s_offset + i} * increment); from != m_views.end()) {
                m_views[to] = from->second;
            } else {
                m_views.erase(to);
            }
        }

        d_offset += n;
        s_offset += n;
    }
}

std::optional<View> ViewTable::find(std::uint64_t handle) const {
    if (const auto it = m_views.find(handle); it != m_views.end()) {
        return it->second;
    }

    return std::nullopt;
}
} // namespace rr::core
