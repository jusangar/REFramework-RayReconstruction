#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Profile.hpp"

namespace rr::core {
// The Ray Trace Clone Types of REFramework's Graphics, in the order of its interface.
enum class CloneType : std::uint8_t {
    Pre,
    Post,
    True,
};

// From the last clone type, which a new one has to replace here.
inline constexpr std::size_t CLONE_TYPE_COUNT = static_cast<std::size_t>(CloneType::True) + 1;

// The game's settings the profile checks, as the Mod reads them, and the PT settings, which it never
// checks: RR works with any.
struct GameSettings {
    // REFramework's "Enable Ray Tracing Tweaks". Without it, Graphics applies none of the next four, so
    // nothing sets Pure PT.
    bool ray_tracing_tweaks{};

    // By the names of Graphics' combos.
    std::string ray_trace_type{};
    std::array<std::string, CLONE_TYPE_COUNT> clone_types{};
    std::uint32_t spp{};     // Samples Per Pixel
    std::uint32_t bounces{}; // Bounce Count

    // The game's anti-aliasing, by the name of its value in via.render.RenderConfig.AntiAliasingType, or
    // nothing if it could not be read.
    std::optional<std::string> anti_aliasing{};
};

enum class SettingProblem : std::uint8_t {
    RayTracingTweaksOff,
    RayTraceType,
    CloneTypePre,
    CloneTypePost,
    CloneTypeTrue,
    GameTaaOff,
    AntiAliasingUnknown,
};

struct SettingWarning {
    SettingProblem problem{};
    std::string text{}; // what the interface and the log show
};

// What differs from the profile's settings, in the order of SettingProblem. With the ray tracing tweaks
// off only that is said: Graphics then applies no ray tracing setting.
std::vector<SettingWarning> check_settings(const Profile::Settings& expected, const GameSettings& actual);

// The PT settings as information, never as a warning: "PT: 4 spp, 1 bounce". With the ray tracing tweaks
// off it says that Graphics does not apply them.
std::string pt_settings_text(const GameSettings& settings);

// Whether an anti-aliasing value of the game includes its TAA: TAA and FXAA_TAA.
bool includes_taa(std::string_view anti_aliasing);

std::string_view to_string(SettingProblem problem);
std::string_view to_string(CloneType clone);
} // namespace rr::core
