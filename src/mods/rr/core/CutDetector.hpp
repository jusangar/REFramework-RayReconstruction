#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "Geometry.hpp"
#include "Profile.hpp"

namespace rr::core {
// The residuals of an update against the prediction, in the units of Profile::Cuts.
struct CutResiduals {
    double pivot{};    // m
    double rotation{}; // degrees
    double focal{};    // relative
};

// An update the detector evaluated: its residuals, so that the validation pass can see how close the
// updates without a cut come to the thresholds.
struct CutCheck {
    CutResiduals residuals{};
    bool cut{};

    // No cut, but one of the residuals went over NEAR_MISS_FRACTION of its threshold: what the checks of
    // the detector at 30 fps, aiming while turning and in the game's 180-degree turn look for.
    bool near_miss{};

    // A camera push, as when the camera is pushed against a wall: the pivot alone went over its
    // threshold, but along the view's axis and without a turn or a zoom (Profile::Cuts). Neither a cut
    // nor a near miss.
    bool push{};

    // The fraction of the pivot residual along axis 2 of the update's inverse view, from 0 to 1: the
    // direction of the residual, not that of the displacement. 0 if the pivot residual is 0.
    double axis_fraction{};
};

inline constexpr double NEAR_MISS_FRACTION = 0.5;

// What the interface shows of the detector since its counters were reset.
struct CutStatistics {
    std::uint64_t checked{}; // updates evaluated
    std::uint64_t cuts{};
    std::uint64_t pushes{};
    std::uint64_t near_misses{};

    // The largest of each residual among the updates without a cut or a push, each one on its own. A
    // push's pivot goes over its threshold, and would hide how close the other updates come to it.
    CutResiduals peak{};

    void add(const CutCheck& check);
};

// The camera cut detector by residuals. The game gives no signal at a cut, so each update of the camera
// is set against a constant-velocity prediction made with the two updates before it:
// - pivot: |q[i] - (2 q[i-1] - q[i-2])|, with q the point pivot_distance in front of the camera. The
//   camera of RE2 orbits Leon, and an orbit around that point leaves it in place;
// - rotation: the angle between R[i] and R[i-1] R[i-2]^T R[i-1];
// - focal: |f[i] - f[i-1]|, with f the relative change of projection[1][1] in one update.
//
// The pivot alone does not make a cut if its residual lies along the view's axis, without a turn or a
// zoom: a camera push, the camera against a wall. It is real camera motion, which the game's motion
// vectors already carry, so it resets nothing.
//
// The update after a cut is not evaluated, since its residuals still carry the cut. Setting it against
// zero velocity instead chained 16 resets through a dolly at 1.2 m per update. So a camera change in
// two updates, the projection first and the view next, is caught one update late. Neither is the update
// after a push: a step of the camera gives the same residual twice.
class CutDetector {
public:
    explicit CutDetector(const Profile::Cuts& thresholds);

    // The prediction starts again: nothing is evaluated until two updates in a row. For a frame where
    // the main scene layer did not update, and before the first update of another main layer.
    void restart();

    // An update of the camera, from JitterDisable's matrices. Returns its residuals and whether it is a
    // cut or a push, or nothing for an update that is not evaluated: the first two after a restart and
    // the one after a cut or a push. A view that cannot be inverted is no camera: no cut, and the
    // prediction starts again.
    std::optional<CutCheck> on_update(const Matrix4& view, const Matrix4& projection);

    // Whether the prediction has taken no update since it started again. Every update it takes counts, so
    // right after on_update, only a view that cannot be inverted gives true.
    bool restarted() const { return m_count == 0; }

private:
    using Vector3 = std::array<double, 3>;
    using Rotation = std::array<Vector3, 3>; // by columns, as Matrix4

    struct Sample {
        Vector3 pivot{};
        Rotation rotation{};
        double p11{};
        double focal{}; // the relative change of p11 since the update before
    };

    Profile::Cuts m_thresholds{};

    // The last two updates, the older first.
    std::array<Sample, 2> m_previous{};
    std::size_t m_count{};

    // The last update was a cut or a push, so the next one is not evaluated.
    bool m_skip_next{};
};
} // namespace rr::core
