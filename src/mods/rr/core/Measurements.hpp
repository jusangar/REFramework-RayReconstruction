#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "FramePolicy.hpp"

namespace rr::core {
// The samples a GPU timer's recent mean takes: about two seconds at 60 frames per second.
inline constexpr std::size_t GPU_TIMER_WINDOW = 120;

// A GPU timer of the measurement interface: the time between two timestamps that the Mod records on the
// TAA's list, around the guide pass or RR's evaluation, read back once the GPU is done with them.
class GpuTimer {
public:
    // A sample from its two timestamps, in ticks of the queue's timestamp frequency (ticks per second).
    // One whose end comes before its begin, or without a frequency, is dropped.
    void add(std::uint64_t begin, std::uint64_t end, std::uint64_t frequency);

    void reset();

    struct Stats {
        std::uint64_t samples{};
        std::uint64_t dropped{};
        double last_ms{};
        double window_ms{}; // the mean of the last GPU_TIMER_WINDOW samples
        double mean_ms{};   // since the reset
        double max_ms{};    // since the reset
    };

    Stats stats() const;

private:
    std::uint64_t m_samples{};
    std::uint64_t m_dropped{};
    double m_last_ms{};
    double m_sum_ms{};
    double m_max_ms{};
    std::array<double, GPU_TIMER_WINDOW> m_window{};
};

// The threshold of RR's net cost, in the worst of the scenes of the validation protocol. Going over it
// leaves the implementation valid, and opens the effort of the upscaling.
inline constexpr double NET_COST_THRESHOLD_MS = 2.0;

struct NetCost {
    double rr_ms{};  // the guide pass and RR's evaluation
    double taa_ms{}; // the game's TAA draw, which RR's evaluation omits
    double net_ms{}; // rr_ms less taa_ms
    bool over_threshold{};
};

// RR's net cost, the GPU time RR adds to the frame: what its passes cost less what omitting the game's TAA
// saves. From the GPU timers of the guide pass, of RR's evaluation and of the TAA draw, each as the mean of
// its last GPU_TIMER_WINDOW samples. Empty while any of the three has no samples.
std::optional<NetCost> net_cost(const GpuTimer::Stats& guide_pass, const GpuTimer::Stats& evaluation, const GpuTimer::Stats& taa_draw);

// What the Mod was set to put in place of the game's TAA in a frame, whatever its evaluation point did: the
// drops of a mode include those of its omitted frames, as the creation of the feature, which waits for their
// Present, and those of the GPU work it left in flight.
enum class FrameMode : std::uint8_t {
    RrOn,
    RrOff,           // the game's TAA
    ViewOrReference, // the debug view or the accumulated reference, which win over RR
};

// From the last mode, which a new one has to replace here.
inline constexpr std::size_t FRAME_MODE_COUNT = static_cast<std::size_t>(FrameMode::ViewOrReference) + 1;

// The mode of a closed frame, from the policy's summary, which says whether RR was on, and whether the debug
// view or the accumulated reference is on.
FrameMode frame_mode(const FrameSummary& frame, bool view_or_reference);
std::string_view to_string(FrameMode mode);

// An interval between two Presents longer than this is a drop below 60 fps: one and a half refreshes at
// 60 Hz. With VSync, as in the validation protocol, a frame that misses its refresh lasts two, 33.3 ms,
// while the Presents of the frames that make it scatter only a little around 16.7 ms.
inline constexpr double FRAME_DROP_MS = 1.5 * 1000.0 / 60.0;

// The intervals between Presents by the mode of the frame, so that the validation protocol records the drops
// below 60 fps that RR brings and the game's TAA did not have.
class FrameIntervals {
public:
    // The interval, in ms, that ended at the Present of a frame in this mode.
    void add(FrameMode mode, double ms);

    void reset();

    struct Stats {
        std::uint64_t frames{};
        std::uint64_t drops{}; // longer than FRAME_DROP_MS
        double mean_ms{};
        double max_ms{};
    };

    Stats stats(FrameMode mode) const;

private:
    struct Totals {
        std::uint64_t frames{};
        std::uint64_t drops{};
        double sum_ms{};
        double max_ms{};
    };

    std::array<Totals, FRAME_MODE_COUNT> m_totals{};
};

// The running totals of the count of non-finite pixels in O, as the GPU keeps them: 32 bits each, never
// cleared, in this order.
struct NonFiniteTotals {
    std::uint32_t nan{}; // pixels with a NaN channel, whatever the others hold
    std::uint32_t inf{}; // pixels with an infinite channel and none NaN
};

static_assert(sizeof(NonFiniteTotals) == 2 * sizeof(std::uint32_t));

// The count of the pixels of O with a channel that is NaN or infinite, for the objective gate of the
// validation protocol. A reduction on the GPU adds each frame's pixels to its NonFiniteTotals, and every
// recording copies them back. This takes them in the order of the recordings: what a frame counted is how
// much they grew, modulo 2^32.
class NonFiniteCounter {
public:
    // The totals the GPU had after the count of the hooks' frame `frame`.
    void add(std::uint64_t frame, NonFiniteTotals totals);

    // The counts start again, from the totals the GPU already has.
    void reset();

    struct Counts {
        std::uint64_t frames{};
        std::uint64_t frames_with_nonfinite{};
        std::uint64_t nan_pixels{}; // a pixel with a NaN channel counts as NaN, whatever the others hold
        std::uint64_t inf_pixels{};
        std::uint32_t max_per_frame{};
        std::optional<std::uint64_t> last_frame_with_nonfinite{};
    };

    const Counts& counts() const { return m_counts; }

private:
    // The totals at the last reading: the GPU's buffer starts at 0.
    NonFiniteTotals m_totals{};
    Counts m_counts{};
};
} // namespace rr::core
