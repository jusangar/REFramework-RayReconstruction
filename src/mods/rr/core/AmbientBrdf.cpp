#include <algorithm>
#include <cmath>
#include <limits>

#include "AmbientBrdf.hpp"
#include "Guides.hpp"

namespace rr::core {
const char* to_string(LutLocation location) {
    switch (location) {
    case LutLocation::Located:
        return "located";
    case LutLocation::NoPtDispatch:
        return "no PT sequence before the draw";
    case LutLocation::DeclarationUnknown:
        return "what the PT's compute shader declares at the register is unknown";
    case LutLocation::NotTexture2D:
        return "the PT's compute shader declares no Texture2D at the register";
    case LutLocation::RootSignatureUnknown:
        return "the PT's root signature is unknown";
    case LutLocation::RegisterUnbound:
        return "the PT's root signature does not bind the register";
    case LutLocation::RootView:
        return "the PT's root signature binds the register as a root view";
    case LutLocation::TableUnset:
        return "the table with the register is not set";
    case LutLocation::HeapUnbound:
        return "no descriptor heap bound at the PT's Dispatch";
    case LutLocation::OutsideHeap:
        return "the descriptor is outside the heap bound";
    }

    return "?";
}

LutDescriptor locate_lut(SrvDeclaration declaration, std::optional<RegisterBinding> binding, std::optional<std::uint64_t> table,
    std::optional<HeapRange> heap, std::uint32_t increment) {
    if (declaration == SrvDeclaration::Unknown) {
        return {.location = LutLocation::DeclarationUnknown};
    }

    if (declaration != SrvDeclaration::Texture2D) {
        return {.location = LutLocation::NotTexture2D};
    }

    if (!binding) {
        return {.location = LutLocation::RegisterUnbound};
    }

    LutDescriptor lut{.location = LutLocation::Located, .parameter = binding->parameter, .offset = binding->offset};

    if (binding->kind != RootParameter::Kind::Table) {
        lut.location = LutLocation::RootView;
    } else if (!table) {
        lut.location = LutLocation::TableUnset;
    } else if (!heap) {
        lut.location = LutLocation::HeapUnbound;
    } else {
        lut.handle = *table + std::uint64_t{binding->offset} * increment;
        const auto end = heap->start + std::uint64_t{heap->descriptors} * increment;

        if (lut.handle < heap->start || lut.handle >= end) {
            lut.location = LutLocation::OutsideHeap;
            lut.handle = 0;
        }
    }

    return lut;
}

std::string to_string(const LutReading& reading) {
    return std::string{reading.axis == LutAxis::PerceptualRoughness ? "perceptual roughness" : "alpha = roughness^2"} + " on V, " +
        (reading.swapped ? "bias in .x and scale in .y" : "scale in .x and bias in .y");
}

float texel_center(std::uint32_t i, std::uint32_t n) {
    return (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
}

std::array<float, 2> expected_lut_texel(LutReading reading, float u, float v) {
    // With alpha on V, the roughness is its square root.
    const auto t = guides::env_brdf_fit(u, reading.axis == LutAxis::PerceptualRoughness ? v : std::sqrt(v));
    return reading.swapped ? std::array{t.y, t.x} : std::array{t.x, t.y};
}

LutJudgement judge_ambient_brdf_lut(std::span<const float> texels, Extent size) {
    // The best reading stands out if it is this close to the fit and the next one is this much further.
    constexpr double MAX_ERROR = 0.05;
    constexpr double STANDS_OUT = 2.0;

    constexpr std::array READINGS{
        LutReading{LutAxis::PerceptualRoughness, false},
        LutReading{LutAxis::Alpha, false},
        LutReading{LutAxis::PerceptualRoughness, true},
        LutReading{LutAxis::Alpha, true},
    };

    LutJudgement judgement{};

    for (std::size_t i = 0; i < READINGS.size(); ++i) {
        judgement.fits[i] = {READINGS[i], std::numeric_limits<double>::infinity()};
    }

    const auto texel_count = std::size_t{size.width} * size.height;

    if (texel_count == 0 || texels.size() < texel_count * 2) {
        return judgement;
    }

    std::array<double, READINGS.size()> sums{};
    std::size_t counted = 0;

    for (std::uint32_t y = 0; y < size.height; ++y) {
        const auto v = texel_center(y, size.height);

        if (v < JUDGED_V_MIN || v > JUDGED_V_MAX) {
            continue;
        }

        for (std::uint32_t x = 0; x < size.width; ++x) {
            const auto u = texel_center(x, size.width);
            const auto at = (std::size_t{y} * size.width + x) * 2;

            for (std::size_t i = 0; i < READINGS.size(); ++i) {
                const auto expected = expected_lut_texel(READINGS[i], u, v);
                sums[i] += std::fabs(double{texels[at]} - expected[0]) + std::fabs(double{texels[at + 1]} - expected[1]);
            }

            ++counted;
        }
    }

    if (counted == 0) {
        return judgement;
    }

    for (std::size_t i = 0; i < READINGS.size(); ++i) {
        judgement.fits[i].error = sums[i] / static_cast<double>(counted * 2);
    }

    std::stable_sort(
        judgement.fits.begin(), judgement.fits.end(), [](const LutFit& a, const LutFit& b) { return a.error < b.error; });

    const auto& best = judgement.fits[0];

    if (best.error <= MAX_ERROR && judgement.fits[1].error >= STANDS_OUT * best.error) {
        judgement.reading = best.reading;
    }

    return judgement;
}
} // namespace rr::core
