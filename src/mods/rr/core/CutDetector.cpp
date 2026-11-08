#include <algorithm>
#include <cmath>
#include <numbers>

#include "CutDetector.hpp"

namespace rr::core {
namespace {
using Vector3 = std::array<double, 3>;
using Rotation = std::array<Vector3, 3>;

Vector3 cross(const Vector3& a, const Vector3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double length(const Vector3& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

double dot(const Vector3& a, const Vector3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// a * b, which applies b first.
Rotation operator*(const Rotation& a, const Rotation& b) {
    Rotation out{};

    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t k = 0; k < 3; ++k) {
                out[c][r] += a[k][r] * b[c][k];
            }
        }
    }

    return out;
}

Rotation transpose(const Rotation& m) {
    Rotation out{};

    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            out[c][r] = m[r][c];
        }
    }

    return out;
}

// The angle of the rotation that takes a to b, in degrees. atan2 keeps the precision of small angles,
// where the cosine of the trace alone would lose it.
double angle(const Rotation& a, const Rotation& b) {
    const auto rel = transpose(a) * b;
    const double s = 0.5 * length({rel[1][2] - rel[2][1], rel[2][0] - rel[0][2], rel[0][1] - rel[1][0]});
    const double c = 0.5 * (rel[0][0] + rel[1][1] + rel[2][2] - 1.0);
    return std::atan2(s, c) * 180.0 / std::numbers::pi;
}
} // namespace

CutDetector::CutDetector(const Profile::Cuts& thresholds)
    : m_thresholds{thresholds} {
}

void CutDetector::restart() {
    m_count = 0;
    m_skip_next = false;
}

std::optional<CutCheck> CutDetector::on_update(const Matrix4& view, const Matrix4& projection) {
    // The rotation's columns are the axes of the inverse view. Axis 0 is cross(axis 1, axis 2), as
    // the analysis of the cut probe in the game built it from the probe's CSV, which only kept those two.
    // The probe inverted the view itself, so the SceneInfo's inverse_view is not read.
    const auto inverse_view = inverse(view);
    Sample sample{};

    for (std::size_t c = 1; c < 4; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            if (!std::isfinite(inverse_view[c][r])) {
                restart();
                return std::nullopt;
            }
        }
    }

    for (std::size_t i = 0; i < 3; ++i) {
        sample.rotation[1][i] = inverse_view[1][i];
        sample.rotation[2][i] = inverse_view[2][i];
        sample.pivot[i] = static_cast<double>(inverse_view[3][i]) - m_thresholds.pivot_distance * sample.rotation[2][i];
    }

    sample.rotation[0] = cross(sample.rotation[1], sample.rotation[2]);
    sample.p11 = projection[1][1];

    if (m_count > 0) {
        const auto& last = m_previous[1];
        sample.focal = last.p11 != 0.0 ? sample.p11 / last.p11 - 1.0 : 0.0;
    }

    std::optional<CutCheck> check{};

    if (m_count == 2 && !m_skip_next) {
        const auto& a = m_previous[0];
        const auto& b = m_previous[1];
        Vector3 pivot_offset{}; // from the predicted pivot

        for (std::size_t i = 0; i < 3; ++i) {
            pivot_offset[i] = sample.pivot[i] - (2.0 * b.pivot[i] - a.pivot[i]);
        }

        CutResiduals r{};
        r.pivot = length(pivot_offset);
        r.rotation = angle(b.rotation * transpose(a.rotation) * b.rotation, sample.rotation);
        r.focal = std::abs(sample.focal - b.focal);

        const auto over = [&](double fraction) {
            return r.pivot > fraction * m_thresholds.pivot_residual || r.rotation > fraction * m_thresholds.rotation_residual ||
                   r.focal > fraction * m_thresholds.focal_residual;
        };

        const double axis_fraction = r.pivot > 0.0 ? std::abs(dot(pivot_offset, sample.rotation[2])) / r.pivot : 0.0;
        const bool push = r.pivot > m_thresholds.pivot_residual && axis_fraction >= m_thresholds.push_axis_fraction &&
                          r.rotation <= m_thresholds.push_rotation_residual && r.focal <= m_thresholds.push_focal_residual;
        const bool cut = over(1.0) && !push;
        check = CutCheck{r, cut, !cut && !push && over(NEAR_MISS_FRACTION), push, axis_fraction};
    }

    m_skip_next = check && (check->cut || check->push);
    m_previous[0] = m_previous[1];
    m_previous[1] = sample;
    m_count = std::min<std::size_t>(m_count + 1, 2);
    return check;
}

void CutStatistics::add(const CutCheck& check) {
    ++checked;

    if (check.cut) {
        ++cuts;
        return;
    }

    if (check.push) {
        ++pushes;
        return;
    }

    if (check.near_miss) {
        ++near_misses;
    }

    peak.pivot = std::max(peak.pivot, check.residuals.pivot);
    peak.rotation = std::max(peak.rotation, check.residuals.rotation);
    peak.focal = std::max(peak.focal, check.residuals.focal);
}
} // namespace rr::core
