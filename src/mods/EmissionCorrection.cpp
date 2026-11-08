#include <algorithm>
#include <thread>
#include <variant>
#include <vector>

#include <spdlog/spdlog.h>

#include <sdk/GameIdentity.hpp>

#include "EmissionCorrection.hpp"
#include "rr/D3D12Hooks.hpp"

namespace {
const emission::Table* game_table() {
    return emission::find_table(sdk::GameIdentity::get().game_name());
}
} // namespace

std::shared_ptr<EmissionCorrection>& EmissionCorrection::get() {
    static auto mod = std::make_shared<EmissionCorrection>();
    return mod;
}

bool EmissionCorrection::is_supported() {
    return game_table() != nullptr;
}

void EmissionCorrection::on_d3d12_loaded() {
    const auto table = game_table();

    if (table == nullptr) {
        return;
    }

    std::vector<rr::core::ShaderDigest> digests(table->variants.size());
    std::transform(table->variants.begin(), table->variants.end(), digests.begin(), [](const emission::Variant& v) { return v.digest; });
    rr::d3d12::watch_compute_shaders(digests);
}

std::optional<std::string> EmissionCorrection::on_initialize() {
    m_table = game_table();

    std::scoped_lock _{m_mutex};
    m_decision.emplace(m_table->variants, m_enabled->value());
    return std::nullopt;
}

void EmissionCorrection::on_frame() {
    const auto bound = rr::d3d12::take_bound_shaders();
    const auto ran = rr::d3d12::take_anchored_pt_cs();

    if (bound.empty() && !ran) {
        return;
    }

    std::vector<rr::d3d12::BoundShader> to_prepare{};

    {
        std::scoped_lock _{m_mutex};

        for (const auto& shader : bound) {
            if (m_decision->on_bound(shader.digest, shader.root_signature)) {
                to_prepare.push_back(shader);
            }

            publish_binding(shader.digest);
        }

        // After them: the barrier rule says what ran, whatever else went bound.
        if (ran) {
            m_decision->on_variant_ran(*ran);
        }

        log_failures();
    }

    for (const auto& shader : to_prepare) {
        start_creation(shader);
    }
}

// Detached: nothing waits for it, and the game goes on with its PSO meanwhile.
void EmissionCorrection::start_creation(const rr::d3d12::BoundShader& shader) try {
    spdlog::info("[EmissionCorrection] Preparing the corrected PSO of {}", rr::core::to_string(shader.digest));
    std::thread{[self = get(), shader] { self->create_corrected_pso(shader); }}.detach();
} catch (const std::exception& e) {
    on_failed(shader, e.what());
}

void EmissionCorrection::create_corrected_pso(const rr::d3d12::BoundShader& shader) try {
    const auto bytecode = rr::d3d12::watched_bytecode(shader.digest);

    if (!bytecode) {
        on_failed(shader, "its shader was not seen as its PSO was created");
        return;
    }

    const auto edit = emission::correct_shader(m_table->variants, *bytecode);

    if (const auto refusal = std::get_if<emission::Refusal>(&edit); refusal != nullptr) {
        on_failed(shader, emission::to_string(*refusal));
        return;
    }

    ID3D12PipelineState* pso{};
    const auto hr = rr::d3d12::create_compute_pso_outside_registry(std::get<std::vector<std::uint8_t>>(edit), shader.root_signature, &pso);

    if (FAILED(hr)) {
        on_failed(shader, emission::creation_failure(static_cast<std::uint32_t>(hr)));
        return;
    }

    on_corrected(shader, pso);
} catch (const std::exception& e) {
    on_failed(shader, e.what());
}

void EmissionCorrection::on_corrected(const rr::d3d12::BoundShader& shader, ID3D12PipelineState* pso) {
    std::scoped_lock _{m_mutex};
    m_corrected[{shader.digest.bytes, shader.root_signature}] = {pso, shader.root_signature};
    m_decision->on_created(shader.digest, shader.root_signature);
    spdlog::info("[EmissionCorrection] Corrected PSO of {} ready", rr::core::to_string(shader.digest));
    publish_binding(shader.digest);
}

void EmissionCorrection::on_failed(const rr::d3d12::BoundShader& shader, const std::string& why) {
    std::scoped_lock _{m_mutex};
    m_decision->on_failed(shader.digest, shader.root_signature, why);
    log_failures();
}

void EmissionCorrection::log_failures() {
    for (const auto& failure : m_decision->take_failures()) {
        const auto level = failure.state == emission::VariantState::Failed ? spdlog::level::err : spdlog::level::warn;
        spdlog::log(level, "[EmissionCorrection] No correction for {}: {}. The game's PSO stays", rr::core::to_string(failure.digest), failure.why);
    }
}

void EmissionCorrection::publish_binding(const rr::core::ShaderDigest& digest) {
    const auto root_signature = m_decision->corrected_root_signature(digest);
    const auto corrected = root_signature ? m_corrected.find({digest.bytes, *root_signature}) : m_corrected.end();
    rr::d3d12::set_substitute(digest, corrected != m_corrected.end() ? &corrected->second : nullptr);
}

void EmissionCorrection::publish_bindings() {
    for (const auto& variant : m_table->variants) {
        publish_binding(variant.digest);
    }

    rr::d3d12::set_substitution_enabled(m_decision->enabled());
}

void EmissionCorrection::on_option_changed() {
    std::scoped_lock _{m_mutex};
    m_decision->on_option(m_enabled->value());
    publish_bindings();
}

void EmissionCorrection::on_draw_ui() {
    ImGui::SetNextItemOpen(false, ImGuiCond_::ImGuiCond_FirstUseEver);

    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    if (m_enabled->draw("Emission correction")) {
        on_option_changed();
        g_framework->request_save_config();
    }

    std::scoped_lock _{m_mutex};
    ImGui::TextWrapped("State: %s", m_decision->status().c_str());
    ImGui::Text("Binds of a corrected PSO: %llu", static_cast<unsigned long long>(rr::d3d12::substitute_binds()));
}

void EmissionCorrection::on_config_load(const utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_load(cfg);
    }

    on_option_changed();
}

void EmissionCorrection::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}
