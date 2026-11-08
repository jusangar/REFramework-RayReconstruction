#include <algorithm>
#include <utility>

#include "BridgeSession.hpp"

namespace rr::core {
namespace {
// UTF-8 on every platform: path::string() throws on Windows for a character the code page lacks.
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
    });
    return text;
}

// Whether path is under NGX\models, the OTA cache of NGX, and under that feature's folder if one is
// given: ProgramData\NVIDIA\NGX\models\<feature>\versions\<N>\files\<file>.bin.
bool in_ota_cache(const std::filesystem::path& path, std::string_view feature = {}) {
    std::vector<std::string> parts{};

    for (const auto& part : path) {
        parts.push_back(lower(utf8(part)));
    }

    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == "ngx" && parts[i + 1] == "models") {
            return feature.empty() || (i + 2 < parts.size() && parts[i + 2] == feature);
        }
    }

    return false;
}

// A snippet of RR, wherever NGX took it from.
bool is_rr_snippet(const std::filesystem::path& path) {
    return lower(utf8(path.filename())) == RR_BRIDGE_SNIPPET_FILE_NAME || in_ota_cache(path, "dlssd");
}

std::string with_origin(const std::filesystem::path& path, SnippetOrigin origin) {
    return utf8(path) + " (" + std::string{to_string(origin)} + ")";
}

std::string size_text(Extent size) {
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

std::string join(const std::vector<std::string>& items) {
    std::string out{};

    for (const auto& item : items) {
        out += out.empty() ? item : ", " + item;
    }

    return out;
}

Availability from_result(rr_bridge_result result) {
    switch (result) {
    case RR_BRIDGE_NO_SDK:
        return Availability::BuiltWithoutSdk;
    case RR_BRIDGE_UNSUPPORTED:
        return Availability::Unsupported;
    case RR_BRIDGE_NO_SNIPPET:
        return Availability::SnippetMissing;
    case RR_BRIDGE_OTHER_NGX_CONSUMER:
        return Availability::OtherNgxConsumer;
    default:
        return Availability::NgxError;
    }
}
} // namespace

bool is_ngx_module(const std::filesystem::path& path) {
    const auto name = lower(utf8(path.filename()));

    if (name == "_nvngx.dll" || name == "nvngx.dll") {
        return true;
    }

    if (name.starts_with("nvngx_") && name.ends_with(".dll")) {
        return true;
    }

    return in_ota_cache(path);
}

std::string_view to_string(SnippetOrigin origin) {
    switch (origin) {
    case SnippetOrigin::Installed:
        return "installed";
    case SnippetOrigin::OtaCache:
        return "from NGX's OTA cache, by the NVIDIA App override";
    case SnippetOrigin::Other:
        return "other";
    }

    return "?";
}

std::string to_string(const LoadedSnippet& snippet) {
    return utf8(snippet.path) + " (" + std::string{to_string(snippet.origin)} + ", " +
           (snippet.version ? "version " + *snippet.version : std::string{"unknown version"}) + ")";
}

BridgeSession::BridgeSession(const BridgeLoad& load, ProcessModules modules)
    : m_api{load.api},
      m_modules{std::move(modules)},
      m_availability{load.availability},
      m_detail{load.detail} {
}

void BridgeSession::start_ngx(void* device, const std::filesystem::path& snippet_dir, const std::filesystem::path& log_dir) {
    if (m_availability != Availability::Loaded || m_api == nullptr) {
        return;
    }

    // Before NGX starts, so that its own modules are never taken for another consumer.
    std::vector<std::string> others{};

    for (const auto& module : m_modules.loaded()) {
        if (is_ngx_module(module)) {
            others.push_back(utf8(module));
        }
    }

    if (!others.empty()) {
        m_availability = Availability::OtherNgxConsumer;
        m_detail = "loaded before NGX started: " + join(others);
        return;
    }

    m_installed = snippet_dir / RR_BRIDGE_SNIPPET_FILE_NAME;

    const auto snippet_dir_text = snippet_dir.wstring();
    const auto log_dir_text = log_dir.wstring();
    const rr_bridge_init_params params{device, snippet_dir_text.c_str(), log_dir_text.c_str()};

    if (const auto result = m_api->init(&params); result != RR_BRIDGE_OK) {
        fail(result, "starting NGX");
        return;
    }

    if (const auto result = m_api->query_availability(); result != RR_BRIDGE_OK) {
        fail(result, "asking NGX for RR");
        return;
    }

    m_availability = Availability::Available;
    m_detail = "no NGX module was loaded before NGX started; NGX looks first for the installed snippet, " + utf8(m_installed) +
               ", and the one it loads is known once it creates the feature";
}

void BridgeSession::request_feature(Extent size) {
    m_requested = true;
    m_requested_size = size;
}

FeatureState BridgeSession::feature() const {
    if (m_requested) {
        return {FeatureState::Status::Pending, m_requested_size};
    }

    if (m_feature == nullptr) {
        return {};
    }

    return {m_executed ? FeatureState::Status::Ready : FeatureState::Status::Pending, m_feature_size};
}

bool BridgeSession::record_creation(void* command_list) {
    if (!m_requested) {
        return false;
    }

    m_requested = false;

    if (m_availability != Availability::Available) {
        return false;
    }

    release_current();

    const auto size = m_requested_size;
    const rr_bridge_feature_params params{command_list, size.width, size.height};
    rr_bridge_feature* created{};

    if (const auto result = m_api->create_feature(&params, &created); result != RR_BRIDGE_OK) {
        fail(result, "creating the feature at " + size_text(size));
        return false;
    }

    // Unexecuted, the list only holds commands for a feature that is released here.
    if (!accept_snippet()) {
        m_api->release_feature(created);
        return false;
    }

    m_feature = created;
    m_feature_size = size;
    m_executed = false;
    ++m_created;
    return true;
}

void BridgeSession::on_creation_executed() {
    if (m_feature != nullptr) {
        m_executed = true;
    }
}

void BridgeSession::release_feature() {
    m_requested = false;
    release_current();
}

bool BridgeSession::evaluate(const EvaluationResources& resources, const Decision& decision) {
    const auto feature = this->feature();
    const auto size = decision.render_size;

    if (feature.status != FeatureState::Status::Ready || feature.size != size || !decision.camera) {
        return false;
    }

    const auto& camera = *decision.camera;
    const rr_bridge_eval_params params{
        .command_list = resources.command_list,
        .color = resources.color,
        .output = resources.output,
        .depth = resources.depth,
        .motion_vectors = resources.motion_vectors,
        .normals_roughness = resources.normals_roughness,
        .diffuse_albedo = resources.diffuse_albedo,
        .specular_albedo = resources.specular_albedo,
        .width = size.width,
        .height = size.height,
        .jitter = {camera.jitter_offset.x, camera.jitter_offset.y},
        // The guide pass writes the MVs in render pixels already.
        .mv_scale = {1.0f, 1.0f},
        .reset = decision.reset != ResetCause::None ? 1u : 0u,
    };

    if (const auto result = m_api->evaluate(m_feature, &params); result != RR_BRIDGE_OK) {
        fail(result, "evaluating RR at " + size_text(size));
        return false;
    }

    ++m_evaluations;
    return true;
}

void BridgeSession::release_current() {
    if (m_feature == nullptr) {
        return;
    }

    m_api->release_feature(m_feature);
    m_feature = nullptr;
    m_executed = false;
    ++m_released;
}

void BridgeSession::fail(rr_bridge_result result, const std::string& context) {
    const auto* error = m_api->last_error();
    m_availability = from_result(result);
    m_detail = context + ": " + (error != nullptr ? error : "no error given");
}

// After the creation: the only RR snippet NGX has loaded is accepted, wherever NGX took it from. NGX
// ignores PathListInfo and takes it from its OTA cache while the NVIDIA App override of RR is on, as seen
// in RE2, and that override is the player's choice. With none, or with more than one, RR is disabled as
// another NGX consumer, the detail says why, and it returns false.
bool BridgeSession::accept_snippet() {
    std::vector<LoadedSnippet> snippets{};

    // The same file as the installed snippet first, whatever its spelling.
    for (const auto& module : m_modules.loaded()) {
        if (m_modules.same_file(module, m_installed)) {
            snippets.push_back({module, SnippetOrigin::Installed, std::nullopt});
        } else if (is_rr_snippet(module)) {
            snippets.push_back({module, in_ota_cache(module, "dlssd") ? SnippetOrigin::OtaCache : SnippetOrigin::Other, std::nullopt});
        }
    }

    if (snippets.size() == 1) {
        auto& snippet = snippets.front();
        snippet.version = m_modules.file_version(snippet.path);
        m_detail = "after creating the feature, NGX has the RR snippet " + to_string(snippet) + " loaded";
        m_loaded = std::move(snippet);
        return true;
    }

    m_availability = Availability::OtherNgxConsumer;
    m_loaded.reset();

    if (snippets.empty()) {
        m_detail = "after creating the feature, " + utf8(m_installed) + " is not loaded, nor any other RR snippet";
        return false;
    }

    std::vector<std::string> names{};

    for (const auto& snippet : snippets) {
        names.push_back(with_origin(snippet.path, snippet.origin));
    }

    m_detail =
        "after creating the feature, NGX has more than one RR snippet loaded, so it is not known which one evaluates: " + join(names);
    return false;
}
} // namespace rr::core
