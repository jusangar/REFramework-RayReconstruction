#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Mod.hpp"
#include "emission/core/BindDecision.hpp"
#include "emission/core/Table.hpp"
#include "rr/PsoSubstitution.hpp"

// Removes the duplicated emission from the PT's light: edits in memory the compute shader the game hands over,
// so that no code of the game's shader has to be shipped, creates a PSO with it in the background and binds
// that one in place of the game's. Whatever does not match leaves the game's PSO. RR does not know this exists,
// and it does not depend on RR being on.
class EmissionCorrection : public Mod {
public:
    static std::shared_ptr<EmissionCorrection>& get();

    // Whether the game has a table. The Mod is registered only then.
    static bool is_supported();

    // From the same points as RayReconstruction::on_d3d12_loaded, and for the same reason: the shader only
    // exists while its PSO is created, and that comes long before the Mod exists.
    static void on_d3d12_loaded();

public:
    std::string_view get_name() const override { return "EmissionCorrection"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override;
    void on_draw_ui() override;

    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;

private:
    // Under m_mutex.
    void publish_binding(const rr::core::ShaderDigest& digest);
    void publish_bindings();
    void log_failures();

    void on_option_changed();

    void start_creation(const rr::d3d12::BoundShader& shader);

    // On a thread of its own: creating the PSO takes about 500 ms. With the root signature the decision asked for,
    // which is not always the one the game binds by the time it ends.
    void create_corrected_pso(const rr::d3d12::BoundShader& shader);

    void on_corrected(const rr::d3d12::BoundShader& shader, ID3D12PipelineState* pso);
    void on_failed(const rr::d3d12::BoundShader& shader, const std::string& why);

    const ModToggle::Ptr m_enabled{ModToggle::create(generate_name("Enabled"), true)};

    ValueList m_options{*m_enabled};

    const emission::Table* m_table{};

    // The interface, the thread that presents and the threads that create the PSOs.
    mutable std::mutex m_mutex{};
    std::optional<emission::BindDecision> m_decision{};

    // By digest and root signature. Never erased or written again, and their PSOs never released: the hooks read
    // these, and a list in flight may have the PSO bound.
    std::map<std::pair<decltype(rr::core::ShaderDigest::bytes), emission::RootSignature>, rr::d3d12::Substitute> m_corrected{};
};
