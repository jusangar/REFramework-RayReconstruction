#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <psapi.h>

#include <d3d12.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd.h>

#include "Adapter.hpp"

// The feature the table hands out: the NGX handle and the parameter map it was created with, which every
// evaluation reuses.
struct rr_bridge_feature {
    NVSDK_NGX_Handle* handle{};
    NVSDK_NGX_Parameter* parameters{};
    uint32_t width{};
    uint32_t height{};
    bool first_evaluation_logged{};
};

// The adapter with the NGX SDK of NVIDIA/DLSS v310.9.16. NGX starts with a project ID of its own and the
// CUSTOM engine type, and looks for the RR snippet first in the folder the Mod gives, reframework/rr/,
// where the installed one has to be. NGX may still load another, like the one of its OTA cache while the
// NVIDIA App override of RR is on: the Mod accepts it as long as it is the only RR snippet loaded.
namespace rr::bridge {
const uint32_t FLAGS = RR_BRIDGE_FLAG_NGX_SDK;

namespace {
// The same in every run, so that NGX always sees the same application. NGX only checks that a CUSTOM
// project ID looks like a GUID.
constexpr const char* PROJECT_ID = "084d6962-0dc8-4c87-99cd-c5472ca313f0";
constexpr const char* ENGINE_VERSION = "1.0";

constexpr const wchar_t* LOG_FILE_NAME = L"rr_bridge.log";

// The Mod never calls the table concurrently; NGX calls the log callback from any thread.
std::string g_last_error{"no error"};
ID3D12Device* g_device{};

// What PathListInfo and InApplicationDataPath point to, for the life of the process.
std::wstring g_snippet_dir{};
std::wstring g_log_dir{};
const wchar_t* g_paths[1]{};

std::mutex g_log_mutex{};
std::FILE* g_log{}; // under g_log_mutex

void write_log(std::string_view text) {
    std::scoped_lock _{g_log_mutex};

    if (g_log == nullptr) {
        return;
    }

    std::fwrite(text.data(), 1, text.size(), g_log);

    if (text.empty() || text.back() != '\n') {
        std::fputc('\n', g_log);
    }

    std::fflush(g_log);
}

// NGX's own lines, among them the one that says which snippet it loaded ("feature dlssd snippet:").
void NVSDK_CONV on_ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    write_log(message != nullptr ? message : "");
}

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }

    const auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}

std::string ngx_error(const char* call, NVSDK_NGX_Result result) {
    return std::format("{}: {} ({:#x})", call, narrow(GetNGXResultAsString(result)), static_cast<std::uint32_t>(result));
}

// A snippet of RR, wherever NGX took it from: by its file name, or under the dlssd folder of NGX's OTA
// cache, ProgramData\NVIDIA\NGX\models\dlssd\, whose files have other names.
bool is_rr_snippet(const std::filesystem::path& path) {
    if (_wcsicmp(path.filename().c_str(), L"" RR_BRIDGE_SNIPPET_FILE_NAME) == 0) {
        return true;
    }

    const std::vector<std::wstring> parts(path.begin(), path.end());

    for (std::size_t i = 0; i + 2 < parts.size(); ++i) {
        if (_wcsicmp(parts[i].c_str(), L"ngx") == 0 && _wcsicmp(parts[i + 1].c_str(), L"models") == 0 &&
            _wcsicmp(parts[i + 2].c_str(), L"dlssd") == 0) {
            return true;
        }
    }

    return false;
}

// The RR snippets loaded in the process, for the log, wherever NGX took them from: NGX's own lines may not
// say it at the logging level the bridge asks for. The Mod checks them on its own.
std::string loaded_snippets() {
    std::vector<HMODULE> modules(512);
    DWORD needed{};

    // Another thread may load a module in between: then the list is taken again, larger.
    for (;;) {
        const auto bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));

        if (!EnumProcessModules(GetCurrentProcess(), modules.data(), bytes, &needed)) {
            return std::format("RR snippets unknown: EnumProcessModules error {}", GetLastError());
        }

        if (needed <= bytes) {
            break;
        }

        modules.resize(needed / sizeof(HMODULE) + 64);
    }

    modules.resize(needed / sizeof(HMODULE));
    std::wstring name(32768, L'\0');
    std::string found{};

    for (const auto module : modules) {
        const auto length = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
        const std::wstring path{name.data(), length};

        if (length > 0 && is_rr_snippet(path)) {
            found += (found.empty() ? "" : ", ") + narrow(path.c_str());
        }
    }

    return found.empty() ? "no RR snippet loaded" : "RR snippet " + found;
}

rr_bridge_result fail(rr_bridge_result code, std::string text) {
    g_last_error = std::move(text);
    write_log("[rr_bridge] " + g_last_error);
    return code;
}
} // namespace

rr_bridge_result init(const rr_bridge_init_params* params) {
    if (params == nullptr || params->device == nullptr || params->snippet_dir == nullptr || params->log_dir == nullptr) {
        return fail(RR_BRIDGE_ERROR, "init: a parameter is missing");
    }

    const auto device = static_cast<ID3D12Device*>(params->device);

    if (g_device != nullptr) {
        return g_device == device ? RR_BRIDGE_OK : fail(RR_BRIDGE_ERROR, "init: NGX already started on another device");
    }

    g_snippet_dir = params->snippet_dir;
    g_log_dir = params->log_dir;
    g_paths[0] = g_snippet_dir.c_str();

    // NGX may fail without a word if it cannot write there.
    std::error_code ec{};
    std::filesystem::create_directories(g_log_dir, ec);

    {
        std::scoped_lock _{g_log_mutex};

        if (g_log == nullptr) {
            g_log = _wfopen((std::filesystem::path{g_log_dir} / LOG_FILE_NAME).c_str(), L"w");
        }
    }

    write_log(std::format("[rr_bridge] Starting NGX: snippets from {}, logs in {}", narrow(g_snippet_dir.c_str()), narrow(g_log_dir.c_str())));

    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = g_paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &on_ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = false;

    const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_VERSION, g_log_dir.c_str(), device, &info, NVSDK_NGX_Version_API);

    if (NVSDK_NGX_FAILED(result)) {
        const auto unsupported = result == NVSDK_NGX_Result_FAIL_FeatureNotSupported || result == NVSDK_NGX_Result_FAIL_PlatformError;
        return fail(unsupported ? RR_BRIDGE_UNSUPPORTED : RR_BRIDGE_ERROR, ngx_error("NVSDK_NGX_D3D12_Init_with_ProjectID", result));
    }

    g_device = device;
    write_log("[rr_bridge] NGX started");
    return RR_BRIDGE_OK;
}

rr_bridge_result query_availability() {
    if (g_device == nullptr) {
        return fail(RR_BRIDGE_ERROR, "query_availability: NGX has not started");
    }

    // The installed snippet is required even if NGX then loads another one, as with the NVIDIA App
    // override on: with it, RR keeps working once the override is off.
    const auto snippet = std::filesystem::path{g_snippet_dir} / RR_BRIDGE_SNIPPET_FILE_NAME;

    if (std::error_code ec{}; !std::filesystem::exists(snippet, ec)) {
        return fail(RR_BRIDGE_NO_SNIPPET, narrow(snippet.c_str()) + " not found");
    }

    NVSDK_NGX_Parameter* capabilities{};

    if (const auto result = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities); NVSDK_NGX_FAILED(result)) {
        return fail(RR_BRIDGE_ERROR, ngx_error("NVSDK_NGX_D3D12_GetCapabilityParameters", result));
    }

    // The types of the DLSS Programming Guide's example for super sampling.
    int available{};
    int needs_updated_driver{};
    unsigned int min_driver_major{};
    unsigned int min_driver_minor{};
    int init_result{static_cast<int>(NVSDK_NGX_Result_Fail)};
    NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
    NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needs_updated_driver);
    NVSDK_NGX_Parameter_GetUI(capabilities, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &min_driver_major);
    NVSDK_NGX_Parameter_GetUI(capabilities, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &min_driver_minor);
    NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &init_result);
    NVSDK_NGX_D3D12_DestroyParameters(capabilities);

    if (available == 1) {
        write_log("[rr_bridge] RR available");
        return RR_BRIDGE_OK;
    }

    if (needs_updated_driver != 0) {
        return fail(RR_BRIDGE_UNSUPPORTED, std::format("RR needs driver {}.{} or later", min_driver_major, min_driver_minor));
    }

    return fail(RR_BRIDGE_UNSUPPORTED, ngx_error("RR not available", static_cast<NVSDK_NGX_Result>(init_result)));
}

rr_bridge_result create_feature(const rr_bridge_feature_params* params, rr_bridge_feature** feature) {
    if (feature != nullptr) {
        *feature = nullptr;
    }

    if (params == nullptr || feature == nullptr || params->command_list == nullptr || params->width == 0 || params->height == 0) {
        return fail(RR_BRIDGE_ERROR, "create_feature: a parameter is missing");
    }

    if (g_device == nullptr) {
        return fail(RR_BRIDGE_ERROR, "create_feature: NGX has not started");
    }

    NVSDK_NGX_Parameter* parameters{};

    if (const auto result = NVSDK_NGX_D3D12_AllocateParameters(&parameters); NVSDK_NGX_FAILED(result)) {
        return fail(RR_BRIDGE_ERROR, ngx_error("NVSDK_NGX_D3D12_AllocateParameters", result));
    }

    // DLAA at the render size: RR works 1:1 at whatever size the game renders, and the game scales to the
    // screen after it. In the roughness and depth modes, 0 is the wrong option. Never AutoExposure: O stays
    // without exposure, as with the game's TAA, and the tonemapping exposes it later. No MVJittered, since
    // the guide pass takes the jitter out of the MVs.
    NVSDK_NGX_DLSSD_Create_Params create{};
    create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
    create.InWidth = params->width;
    create.InHeight = params->height;
    create.InTargetWidth = params->width;
    create.InTargetHeight = params->height;
    create.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    create.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    create.InEnableOutputSubrects = false;

    const auto list = static_cast<ID3D12GraphicsCommandList*>(params->command_list);
    NVSDK_NGX_Handle* handle{};

    if (const auto result = NGX_D3D12_CREATE_DLSSD_EXT(list, 1, 1, &handle, parameters, &create); NVSDK_NGX_FAILED(result)) {
        NVSDK_NGX_D3D12_DestroyParameters(parameters);
        return fail(RR_BRIDGE_ERROR, ngx_error("NGX_D3D12_CREATE_DLSSD_EXT", result));
    }

    *feature = new rr_bridge_feature{handle, parameters, params->width, params->height};
    write_log(std::format("[rr_bridge] Feature created at {}x{}; {}", params->width, params->height, loaded_snippets()));
    return RR_BRIDGE_OK;
}

void release_feature(rr_bridge_feature* feature) {
    if (feature == nullptr) {
        return;
    }

    if (const auto result = NVSDK_NGX_D3D12_ReleaseFeature(feature->handle); NVSDK_NGX_FAILED(result)) {
        write_log("[rr_bridge] " + ngx_error("NVSDK_NGX_D3D12_ReleaseFeature", result));
    }

    NVSDK_NGX_D3D12_DestroyParameters(feature->parameters);
    write_log(std::format("[rr_bridge] Feature released at {}x{}", feature->width, feature->height));
    delete feature;
}

rr_bridge_result evaluate(rr_bridge_feature* feature, const rr_bridge_eval_params* params) {
    if (feature == nullptr || params == nullptr || params->command_list == nullptr || params->color == nullptr ||
        params->output == nullptr || params->depth == nullptr || params->motion_vectors == nullptr ||
        params->normals_roughness == nullptr || params->diffuse_albedo == nullptr || params->specular_albedo == nullptr) {
        return fail(RR_BRIDGE_ERROR, "evaluate: a parameter is missing");
    }

    if (params->width != feature->width || params->height != feature->height) {
        return fail(RR_BRIDGE_ERROR,
            std::format("evaluate: {}x{} with a feature created at {}x{}", params->width, params->height, feature->width, feature->height));
    }

    const auto resource = [](void* r) { return static_cast<ID3D12Resource*>(r); };

    // Everything else stays 0 or null: the optional inputs RE2 does not have, and the camera matrices
    // (rr_bridge.h says why).
    NVSDK_NGX_D3D12_DLSSD_Eval_Params eval{};
    eval.pInColor = resource(params->color);
    eval.pInOutput = resource(params->output);
    eval.pInDepth = resource(params->depth);
    eval.pInMotionVectors = resource(params->motion_vectors);
    eval.pInDiffuseAlbedo = resource(params->diffuse_albedo);
    eval.pInSpecularAlbedo = resource(params->specular_albedo);
    // NVSDK_NGX_DLSS_Roughness_Mode_Packed: the roughness is in .w, and pInRoughness stays null.
    eval.pInNormals = resource(params->normals_roughness);
    eval.InJitterOffsetX = params->jitter[0];
    eval.InJitterOffsetY = params->jitter[1];
    eval.InRenderSubrectDimensions = {params->width, params->height};
    eval.InReset = params->reset != 0 ? 1 : 0;
    eval.InMVScaleX = params->mv_scale[0];
    eval.InMVScaleY = params->mv_scale[1];
    // T arrives at the evaluation point without exposure, as checked in RE2: no exposure texture.
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;

    const auto list = static_cast<ID3D12GraphicsCommandList*>(params->command_list);

    if (const auto result = NGX_D3D12_EVALUATE_DLSSD_EXT(list, feature->handle, feature->parameters, &eval); NVSDK_NGX_FAILED(result)) {
        return fail(RR_BRIDGE_ERROR, ngx_error("NGX_D3D12_EVALUATE_DLSSD_EXT", result));
    }

    if (!feature->first_evaluation_logged) {
        feature->first_evaluation_logged = true;
        write_log(std::format("[rr_bridge] First evaluation of the feature at {}x{}", feature->width, feature->height));
    }

    return RR_BRIDGE_OK;
}

const char* last_error() {
    return g_last_error.c_str();
}
} // namespace rr::bridge
