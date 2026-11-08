#include <format>

#include "Settings.hpp"

namespace rr::core {
namespace {
constexpr std::array CLONE_PROBLEMS{SettingProblem::CloneTypePre, SettingProblem::CloneTypePost, SettingProblem::CloneTypeTrue};

static_assert(CLONE_PROBLEMS.size() == CLONE_TYPE_COUNT);
} // namespace

std::vector<SettingWarning> check_settings(const Profile::Settings& expected, const GameSettings& actual) {
    std::vector<SettingWarning> warnings{};

    if (!actual.ray_tracing_tweaks) {
        warnings.push_back({SettingProblem::RayTracingTweaksOff,
            "REFramework's Ray Tracing Tweaks (Graphics) are off, so nothing sets Pure Path Tracing"});
    } else {
        if (actual.ray_trace_type != expected.ray_trace_type) {
            warnings.push_back(
                {SettingProblem::RayTraceType, std::format("Ray Trace Type is {}, not {}", actual.ray_trace_type, expected.ray_trace_type)});
        }

        for (std::size_t i = 0; i < CLONE_TYPE_COUNT; ++i) {
            if (actual.clone_types[i] != expected.clone_type) {
                warnings.push_back({CLONE_PROBLEMS[i], std::format("Ray Trace Clone Type {} is {}, not {}", to_string(static_cast<CloneType>(i)),
                                                           actual.clone_types[i], expected.clone_type)});
            }
        }
    }

    // RR takes the place of the TAA's draw: without it there is no evaluation point.
    if (!actual.anti_aliasing) {
        warnings.push_back({SettingProblem::AntiAliasingUnknown, "The game's anti-aliasing could not be read"});
    } else if (expected.game_taa && !includes_taa(*actual.anti_aliasing)) {
        warnings.push_back({SettingProblem::GameTaaOff,
            std::format("The game's anti-aliasing is {}, without its TAA, whose draw RR replaces", *actual.anti_aliasing)});
    }

    return warnings;
}

std::string pt_settings_text(const GameSettings& settings) {
    return std::format("PT: {} spp, {} bounce{}{}", settings.spp, settings.bounces, settings.bounces == 1 ? "" : "s",
        settings.ray_tracing_tweaks ? "" : " in Graphics, not applied with its Ray Tracing Tweaks off");
}

bool includes_taa(std::string_view anti_aliasing) {
    return anti_aliasing.find("TAA") != std::string_view::npos;
}

std::string_view to_string(SettingProblem problem) {
    switch (problem) {
    case SettingProblem::RayTracingTweaksOff:
        return "ray tracing tweaks off";
    case SettingProblem::RayTraceType:
        return "Ray Trace Type";
    case SettingProblem::CloneTypePre:
        return "Ray Trace Clone Type Pre";
    case SettingProblem::CloneTypePost:
        return "Ray Trace Clone Type Post";
    case SettingProblem::CloneTypeTrue:
        return "Ray Trace Clone Type True";
    case SettingProblem::GameTaaOff:
        return "the game's TAA off";
    case SettingProblem::AntiAliasingUnknown:
        return "the game's anti-aliasing unknown";
    }

    return "?";
}

std::string_view to_string(CloneType clone) {
    switch (clone) {
    case CloneType::Pre:
        return "Pre";
    case CloneType::Post:
        return "Post";
    case CloneType::True:
        return "True";
    }

    return "?";
}
} // namespace rr::core
