#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rr::core {
struct Extent {
    std::uint32_t width{};
    std::uint32_t height{};

    friend constexpr bool operator==(const Extent&, const Extent&) = default;
};

struct Float2 {
    float x{};
    float y{};

    friend constexpr bool operator==(const Float2&, const Float2&) = default;
};

// A 4x4 matrix with glm's layout, so that the Mod copies a glm::mat4 as it is: m[c][r] is column c and
// row r, the float c * 4 + r. projection[2][0] and [2][1] are floats 8 and 9.
struct Matrix4 {
    std::array<std::array<float, 4>, 4> m{};

    static constexpr Matrix4 identity() {
        Matrix4 out{};

        for (std::size_t i = 0; i < 4; ++i) {
            out.m[i][i] = 1.0f;
        }

        return out;
    }

    std::array<float, 4>& operator[](std::size_t column) { return m[column]; }
    const std::array<float, 4>& operator[](std::size_t column) const { return m[column]; }

    friend constexpr bool operator==(const Matrix4&, const Matrix4&) = default;
};

static_assert(sizeof(Matrix4) == 64);

// glm's product: a * b applies b first. The sums run in double.
Matrix4 operator*(const Matrix4& a, const Matrix4& b);

// Computed in double: a reversed depth of about 0.003 leaves little room for float error.
Matrix4 inverse(const Matrix4& m);
} // namespace rr::core
