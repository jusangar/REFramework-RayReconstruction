#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr_bridge.h>

#include "BridgeLoader.hpp"
#include "FramePolicy.hpp"

namespace rr::core {
// Whether a module of the process belongs to NGX: its core (_nvngx.dll or nvngx.dll), a snippet
// (nvngx_*.dll) or a snippet NGX took from its OTA cache, under NGX\models.
bool is_ngx_module(const std::filesystem::path& path);

// How the Mod sees the modules of the process: EnumProcessModules, std::filesystem::equivalent and the
// file version of Windows, or a double in the tests.
struct ProcessModules {
    std::function<std::vector<std::filesystem::path>()> loaded{};
    std::function<bool(const std::filesystem::path& a, const std::filesystem::path& b)> same_file{};
    // Like "310.9.16.0", or nothing when the module has no version that can be read.
    std::function<std::optional<std::string>(const std::filesystem::path& path)> file_version{};
};

// Where the RR snippet NGX loaded comes from, from the most to the least concrete.
enum class SnippetOrigin : std::uint8_t {
    // The same file as the installed snippet, the one next to the bridge.
    Installed,
    // Under the dlssd folder of NGX's OTA cache, where NGX takes it from while the NVIDIA App override of
    // RR is on.
    OtaCache,
    // Anywhere else, like the driver's ngx-builtins.
    Other,
};

std::string_view to_string(SnippetOrigin origin);

// The RR snippet NGX loaded, which is the one that evaluates.
struct LoadedSnippet {
    std::filesystem::path path{};
    SnippetOrigin origin{};
    std::optional<std::string> version{}; // nothing when it could not be read
};

// For the log and the interface: the path, the origin and the version, or "unknown version".
std::string to_string(const LoadedSnippet& snippet);

// The D3D12 objects of one evaluation, as opaque pointers.
struct EvaluationResources {
    void* command_list{}; // ID3D12GraphicsCommandList*: the game's, at the TAA draw
    void* color{};        // T, without a copy
    void* output{};       // O
    // RR's own textures, which the guide pass writes.
    void* depth{};
    void* motion_vectors{};
    void* normals_roughness{};
    void* diffuse_albedo{};
    void* specular_albedo{};
};

// What the Mod does with the NGX bridge when the per-frame policy asks: start NGX, and create,
// recreate or release the feature. It holds the feature and keeps what the interface and the policy are
// told of the bridge. The D3D12 work is the Mod's: it gives the session its own command list, executes it
// and fences it.
//
// Single-threaded, like the policy: the Mod calls both under the same mutex. It never releases the
// feature on its own when destroyed, since at the end of the process the device may already be gone.
class BridgeSession {
public:
    BridgeSession(const BridgeLoad& load, ProcessModules modules);

    Availability availability() const { return m_availability; }

    // For the log and the interface: why RR is not available, or what was checked when it is.
    const std::string& detail() const { return m_detail; }

    // What the policy is told.
    BridgeState state() const { return to_bridge_state(m_availability); }

    // What the policy is told at the evaluation point. A feature is pending from the moment it is
    // requested, so that the policy never asks for it twice.
    FeatureState feature() const;

    // FeatureAction::InitNgx. Only while the bridge is loaded: NGX starts once per process. RR is
    // disabled as another NGX consumer if NGX or a snippet was already loaded; otherwise the bridge starts
    // NGX on device, with snippet_dir, where the installed snippet has to be, as the first entry of
    // PathListInfo and its logs in log_dir, both absolute, and says whether RR is available.
    void start_ngx(void* device, const std::filesystem::path& snippet_dir, const std::filesystem::path& log_dir);

    // FeatureAction::Create and Recreate: from now on the feature is pending, at size, until the Mod
    // records its creation.
    void request_feature(Extent size);
    bool creation_requested() const { return m_requested; }

    // Records the creation of the requested feature on command_list, the Mod's own and open, after
    // releasing the feature it replaces. Returns true if the Mod has to execute and fence the list, with
    // the only RR snippet NGX has loaded accepted, wherever NGX took it from. Returns false if it has to
    // discard the list unexecuted: the session then has no feature, and its availability says why. That
    // includes NGX having no RR snippet loaded after the creation, or more than one, since then it is not
    // known which one evaluates.
    bool record_creation(void* command_list);

    // Once the Mod's fence has passed the creation recorded last.
    void on_creation_executed();

    // FeatureAction::Release. Forgets a request not recorded yet too. The GPU must be done with the
    // feature.
    void release_feature();

    // Records the evaluation of RR on the game's list, with the constants of a decision that evaluates:
    // InJitterOffset, MVScale 1 and InReset for any reset cause. The inputs are in NON_PIXEL_SHADER_RESOURCE
    // and O in UNORDERED_ACCESS. Returns false if nothing was evaluated: the feature is not ready at the
    // decision's render size, or the bridge failed, and then RR is no longer available and the detail says
    // why.
    bool evaluate(const EvaluationResources& resources, const Decision& decision);

    // Evaluations the bridge recorded.
    std::uint64_t evaluations() const { return m_evaluations; }

    // The RR snippet accepted at the last creation that found exactly one: nothing before, or once a
    // creation found none or more than one. A failure of NGX keeps it, since the snippet stays loaded and a
    // report of that failure needs it.
    const std::optional<LoadedSnippet>& loaded_snippet() const { return m_loaded; }

    std::uint64_t features_created() const { return m_created; }
    std::uint64_t features_released() const { return m_released; }

private:
    void release_current();
    void fail(rr_bridge_result result, const std::string& context);
    bool accept_snippet();

    const rr_bridge_api* m_api{};
    ProcessModules m_modules;
    Availability m_availability{};
    std::string m_detail{};
    std::filesystem::path m_installed{}; // where the installed snippet has to be, once NGX started
    std::optional<LoadedSnippet> m_loaded{};

    // The feature the bridge created, and whether the Mod's list that creates it has run.
    rr_bridge_feature* m_feature{};
    Extent m_feature_size{};
    bool m_executed{};

    // A requested feature that waits for record_creation.
    bool m_requested{};
    Extent m_requested_size{};

    std::uint64_t m_created{};
    std::uint64_t m_released{};
    std::uint64_t m_evaluations{};
};
} // namespace rr::core
