#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include <rr_bridge.h>

#include "FramePolicy.hpp"

namespace rr::core {
// Whether RR can run with the NGX bridge in reframework/rr/, and why not. The interface shows it as is.
// load_bridge gives Loaded or why the bridge is not usable; BridgeSession gives the rest, from Available
// on, once NGX starts.
enum class Availability : std::uint8_t {
    // The bridge has the NGX SDK. NGX starts lazily, at the first identified frame.
    Loaded,
    BridgeMissing,
    // The file is there, but the system does not load it: another architecture, a missing dependency or
    // a broken build.
    BridgeNotLoadable,
    // The module is there, but it is not a bridge of this ABI version.
    AbiMismatch,
    BuiltWithoutSdk,
    // NGX started, and RR is available.
    Available,
    // No RTX GPU, or the driver does not support RR.
    Unsupported,
    // nvngx_dlssd.dll is not in reframework/rr/.
    SnippetMissing,
    // NGX or an NGX snippet was loaded before NGX started, or NGX has no RR snippet loaded after creating
    // the feature, or more than one.
    OtherNgxConsumer,
    // Any other failure of NGX: the detail gives the bridge's last error.
    NgxError,
};

// How the Mod opens a module and finds its exports: LoadLibraryW, GetProcAddress and FreeLibrary, or a
// double in the tests.
struct ModuleLoader {
    // Returns nullptr when the module does not load, and says why in error.
    std::function<void*(const std::filesystem::path& path, std::string& error)> open{};
    std::function<bool(const std::filesystem::path& path)> exists{};
    std::function<void*(void* module, const char* name)> find{};
    std::function<void(void* module)> close{};
};

struct BridgeLoad {
    Availability availability{Availability::BridgeMissing};

    // For the log and the interface: where the bridge was looked for, or what did not match.
    std::string detail{};

    // Only when Loaded: the bridge stays loaded for the life of the process.
    void* module{};
    const rr_bridge_api* api{};
};

// Loads RR_BRIDGE_FILE_NAME from dir, which has to be absolute.
BridgeLoad load_bridge(const std::filesystem::path& dir, const ModuleLoader& loader);

// What the per-frame policy is told of the bridge.
BridgeState to_bridge_state(Availability availability);

std::string_view to_string(Availability availability);
} // namespace rr::core
