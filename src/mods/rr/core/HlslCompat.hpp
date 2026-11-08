#pragma once

// The part of HLSL that Guides.hlsli uses, so that the tests compile it as C++. It follows the semantics
// of HLSL and D3D:
//
// - a float4x4 is indexed by rows, and mul(M, v) is M * v with v a column;
// - min, max, clamp and saturate return the operand that is not NaN, as the GPU does;
// - round goes to the nearest even integer on ties, like the round_ne that FXC emits.

#include <bit>
#include <cmath>
#include <concepts>
#include <cstdint>

namespace rr::core::hlsl {
using uint = std::uint32_t;

struct float2 {
    float x{};
    float y{};

    constexpr float2() = default;
    constexpr float2(float x_, float y_) : x{x_}, y{y_} {}
};

struct float3 {
    float x{};
    float y{};
    float z{};

    constexpr float3() = default;
    constexpr float3(float x_, float y_, float z_) : x{x_}, y{y_}, z{z_} {}
};

struct float4 {
    float x{};
    float y{};
    float z{};
    float w{};

    constexpr float4() = default;
    constexpr float4(float x_, float y_, float z_, float w_) : x{x_}, y{y_}, z{z_}, w{w_} {}
    constexpr float4(float3 v, float w_) : x{v.x}, y{v.y}, z{v.z}, w{w_} {}
};

struct float4x4 {
    float4 rows[4]{};

    constexpr float4x4() = default;
    constexpr float4x4(float4 r0, float4 r1, float4 r2, float4 r3) : rows{r0, r1, r2, r3} {}

    constexpr const float4& operator[](int row) const { return rows[row]; }
};

template <typename F> constexpr float2 per_component(F f, float2 a, float2 b) {
    return {f(a.x, b.x), f(a.y, b.y)};
}

template <typename F> constexpr float3 per_component(F f, float3 a, float3 b) {
    return {f(a.x, b.x), f(a.y, b.y), f(a.z, b.z)};
}

template <typename F> constexpr float4 per_component(F f, float4 a, float4 b) {
    return {f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w)};
}

template <typename V>
concept Vector = std::same_as<V, float2> || std::same_as<V, float3> || std::same_as<V, float4>;

// A scalar operand of a vector operation applies to every component, as in HLSL.
template <Vector V> constexpr V splat(float s) {
    if constexpr (std::same_as<V, float2>) {
        return {s, s};
    } else if constexpr (std::same_as<V, float3>) {
        return {s, s, s};
    } else {
        return {s, s, s, s};
    }
}

template <Vector V> constexpr V operator+(V a, V b) {
    return per_component([](float p, float q) { return p + q; }, a, b);
}

template <Vector V> constexpr V operator-(V a, V b) {
    return per_component([](float p, float q) { return p - q; }, a, b);
}

template <Vector V> constexpr V operator*(V a, V b) {
    return per_component([](float p, float q) { return p * q; }, a, b);
}

template <Vector V> constexpr V operator/(V a, V b) {
    return per_component([](float p, float q) { return p / q; }, a, b);
}

template <Vector V> constexpr V operator+(V a, float s) { return a + splat<V>(s); }
template <Vector V> constexpr V operator-(V a, float s) { return a - splat<V>(s); }
template <Vector V> constexpr V operator*(V a, float s) { return a * splat<V>(s); }
template <Vector V> constexpr V operator/(V a, float s) { return a / splat<V>(s); }

inline float abs(float x) { return std::fabs(x); }
inline float floor(float x) { return std::floor(x); }
inline float sqrt(float x) { return std::sqrt(x); }
inline float round(float x) { return std::nearbyint(x); }
inline float min(float a, float b) { return std::fmin(a, b); }
inline float max(float a, float b) { return std::fmax(a, b); }
inline float clamp(float x, float lo, float hi) { return min(max(x, lo), hi); }
inline float saturate(float x) { return clamp(x, 0.0f, 1.0f); }
inline uint asuint(float x) { return std::bit_cast<uint>(x); }

constexpr float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
constexpr float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

template <Vector V> float length(V v) { return sqrt(dot(v, v)); }
template <Vector V> V normalize(V v) { return v / length(v); }

constexpr float4 mul(const float4x4& m, float4 v) {
    return {dot(m[0], v), dot(m[1], v), dot(m[2], v), dot(m[3], v)};
}
} // namespace rr::core::hlsl
