#include <algorithm>
#include <numeric>

#include "Measurements.hpp"

namespace rr::core {
void GpuTimer::add(std::uint64_t begin, std::uint64_t end, std::uint64_t frequency) {
    if (end < begin || frequency == 0) {
        ++m_dropped;
        return;
    }

    const auto ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
    m_window[m_samples % GPU_TIMER_WINDOW] = ms;
    ++m_samples;
    m_last_ms = ms;
    m_sum_ms += ms;
    m_max_ms = std::max(m_max_ms, ms);
}

void GpuTimer::reset() {
    *this = {};
}

GpuTimer::Stats GpuTimer::stats() const {
    Stats s{.samples = m_samples, .dropped = m_dropped, .last_ms = m_last_ms, .max_ms = m_max_ms};

    if (m_samples > 0) {
        const auto in_window = std::min<std::uint64_t>(m_samples, GPU_TIMER_WINDOW);
        s.window_ms = std::accumulate(m_window.begin(), m_window.begin() + in_window, 0.0) / static_cast<double>(in_window);
        s.mean_ms = m_sum_ms / static_cast<double>(m_samples);
    }

    return s;
}

std::optional<NetCost> net_cost(const GpuTimer::Stats& guide_pass, const GpuTimer::Stats& evaluation, const GpuTimer::Stats& taa_draw) {
    if (guide_pass.samples == 0 || evaluation.samples == 0 || taa_draw.samples == 0) {
        return std::nullopt;
    }

    NetCost cost{.rr_ms = guide_pass.window_ms + evaluation.window_ms, .taa_ms = taa_draw.window_ms};
    cost.net_ms = cost.rr_ms - cost.taa_ms;
    cost.over_threshold = cost.net_ms > NET_COST_THRESHOLD_MS;
    return cost;
}

// RrOff wins over every other reason, so a frame with RR off says so.
FrameMode frame_mode(const FrameSummary& frame, bool view_or_reference) {
    if (view_or_reference) {
        return FrameMode::ViewOrReference;
    }

    return frame.skip == SkipReason::RrOff ? FrameMode::RrOff : FrameMode::RrOn;
}

std::string_view to_string(FrameMode mode) {
    switch (mode) {
    case FrameMode::RrOn:
        return "RR on";
    case FrameMode::RrOff:
        return "RR off (the game's TAA)";
    case FrameMode::ViewOrReference:
        return "the debug view or the reference";
    }

    return "?";
}

void FrameIntervals::add(FrameMode mode, double ms) {
    auto& t = m_totals[static_cast<std::size_t>(mode)];
    ++t.frames;
    t.sum_ms += ms;
    t.max_ms = std::max(t.max_ms, ms);

    if (ms > FRAME_DROP_MS) {
        ++t.drops;
    }
}

void FrameIntervals::reset() {
    *this = {};
}

FrameIntervals::Stats FrameIntervals::stats(FrameMode mode) const {
    const auto& t = m_totals[static_cast<std::size_t>(mode)];
    Stats s{.frames = t.frames, .drops = t.drops, .max_ms = t.max_ms};

    if (t.frames > 0) {
        s.mean_ms = t.sum_ms / static_cast<double>(t.frames);
    }

    return s;
}

void NonFiniteCounter::add(std::uint64_t frame, NonFiniteTotals totals) {
    // Unsigned subtraction is modulo 2^32: a frame never has that many pixels.
    const std::uint32_t nan = totals.nan - m_totals.nan;
    const std::uint32_t inf = totals.inf - m_totals.inf;
    m_totals = totals;

    auto& c = m_counts;
    ++c.frames;
    c.nan_pixels += nan;
    c.inf_pixels += inf;

    if (nan + inf > 0) {
        ++c.frames_with_nonfinite;
        c.max_per_frame = std::max(c.max_per_frame, nan + inf);
        c.last_frame_with_nonfinite = frame;
    }
}

void NonFiniteCounter::reset() {
    m_counts = {};
}
} // namespace rr::core
