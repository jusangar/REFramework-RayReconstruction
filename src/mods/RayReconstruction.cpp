#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <span>
#include <vector>

#include <windows.h>

#include <psapi.h>
#include <winver.h>

// GetFileVersionInfoW, for the version of the RR snippet NGX loaded.
#pragma comment(lib, "version")

#include <spdlog/spdlog.h>

#include <sdk/GameIdentity.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETypeDB.hpp>
#include <sdk/Renderer.hpp>

#include "rr/D3D12Hooks.hpp"
#include "rr/core/Guides.hpp"
#include "rr/core/Profile.hpp"

#include "Graphics.hpp"
#include "RayReconstruction.hpp"

// The camera input copies the matrices as they are.
static_assert(sizeof(sdk::renderer::SceneInfo) == sizeof(rr::core::SceneMatrices));

// The debug view's combo is "Off" and then the guides, in the order of Guides.hlsli.
static_assert(rr::core::guides::DEBUG_VIEW_NORMALS == 0 && rr::core::guides::DEBUG_VIEW_MOTION_VECTORS == 5);

namespace {
// The choices of the "Specular albedo" option, in the order of its combo. The first follows the profile.
constexpr std::array<std::optional<rr::core::AmbientBrdfSource>, 3> SPECULAR_SOURCES{
    std::nullopt, rr::core::AmbientBrdfSource::GameLut, rr::core::AmbientBrdfSource::GgxFit};

// The choices of the "Sign of InJitterOffset" option, in the order of its combo.
constexpr std::array<rr::core::Float2, 4> JITTER_OFFSET_SIGNS{{{1.0f, 1.0f}, {-1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, -1.0f}}};

// The choices of the debug test of the count, in the order of its combo: the bits of the value pixel (0,
// 0) takes, a quiet NaN or +Inf, or 0 for none.
constexpr std::array<std::uint32_t, 3> FORCED_NONFINITE{0, 0x7fc00000, 0x7f800000};

// A value of an enum of the game, size bytes wide, with its sign, as Graphics reads them.
std::int64_t enum_value(std::uint64_t raw, std::uint32_t size) {
    switch (size) {
    case 1:
        return static_cast<std::int8_t>(raw);
    case 2:
        return static_cast<std::int16_t>(raw);
    case 4:
        return static_cast<std::int32_t>(raw);
    default:
        return static_cast<std::int64_t>(raw);
    }
}

// The literals of an enum of the game with their names, in the order of its fields. They are data of the
// TDB, so they never change.
std::vector<std::pair<std::int64_t, std::string>> enum_literals(const sdk::RETypeDefinition& type) {
    const auto size = type.get_valuetype_size();
    std::vector<std::pair<std::int64_t, std::string>> literals{};

    for (const auto field : type.get_fields()) {
        const auto flags = field->get_flags();

        if ((flags & static_cast<std::uint16_t>(via::clr::FieldFlag::Static)) == 0 ||
            (flags & static_cast<std::uint16_t>(via::clr::FieldFlag::Literal)) == 0) {
            continue;
        }

        const auto data = field->get_data_raw(nullptr, true);
        std::uint64_t raw{};

        if (data != nullptr) {
            std::memcpy(&raw, data, std::min<std::size_t>(size, sizeof(raw)));
            literals.emplace_back(enum_value(raw, size), field->get_name());
        }
    }

    return literals;
}

// The game's anti-aliasing, by the name of its value in via.render.RenderConfig.AntiAliasingType: read
// with get_AntiAliasing of the RenderConfig, as VR does, and named through the literals of the enum, as
// Graphics names the ray tracing types. Nothing if any of them is missing. It runs every frame, so the
// literals are read once.
std::optional<std::string> read_anti_aliasing() {
    static const auto renderer_t = sdk::find_type_definition("via.render.Renderer");
    static const auto config_t = sdk::find_type_definition("via.render.RenderConfig");
    static const auto anti_aliasing_t = sdk::find_type_definition("via.render.RenderConfig.AntiAliasingType");
    static const auto get_config = renderer_t != nullptr ? renderer_t->get_method("get_RenderConfig") : nullptr;
    static const auto get_anti_aliasing = config_t != nullptr ? config_t->get_method("get_AntiAliasing") : nullptr;

    if (get_config == nullptr || get_anti_aliasing == nullptr || anti_aliasing_t == nullptr) {
        return std::nullopt;
    }

    const auto renderer = renderer_t->get_instance();

    if (renderer == nullptr) {
        return std::nullopt;
    }

    const auto context = sdk::get_thread_context();
    const auto config = get_config->call<::REManagedObject*>(context, renderer);

    if (config == nullptr) {
        return std::nullopt;
    }

    static const auto literals = enum_literals(*anti_aliasing_t);

    // Only the low bytes of the register hold the value.
    static const auto size = anti_aliasing_t->get_valuetype_size();
    const auto value = enum_value(get_anti_aliasing->call<std::uint64_t>(context, config), size);
    const auto literal = std::find_if(literals.begin(), literals.end(), [&](const auto& entry) { return entry.first == value; });

    return literal != literals.end() ? std::optional{literal->second} : std::nullopt;
}

// A SceneInfo variant of the scene layer, by the name the profile gives it.
sdk::renderer::SceneInfo* scene_info(sdk::renderer::layer::Scene* layer, rr::core::SceneVariant variant) {
    using rr::core::SceneVariant;

    switch (variant) {
    case SceneVariant::Scene:
        return layer->get_scene_info();
    case SceneVariant::DepthDistortion:
        return layer->get_depth_distortion_scene_info();
    case SceneVariant::Filter:
        return layer->get_filter_scene_info();
    case SceneVariant::ZPrepass:
        return layer->get_z_prepass_scene_info();
    case SceneVariant::JitterDisable:
        return layer->get_jitter_disable_scene_info();
    case SceneVariant::JitterDisablePost:
        return layer->get_jitter_disable_post_scene_info();
    }

    return nullptr;
}

// The bridge and the snippet. Never reframework/plugins, whose DLLs PluginLoader loads on its own.
std::filesystem::path rr_dir() {
    return REFramework::get_persistent_dir() / "reframework" / "rr";
}

rr::core::ProcessModules process_modules() {
    return {
        .loaded = [] {
            std::vector<std::filesystem::path> paths{};
            std::vector<HMODULE> modules(512);
            DWORD needed{};

            // Another thread may load a module in between: then the list is taken again, larger.
            for (;;) {
                const auto bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));

                if (!EnumProcessModules(GetCurrentProcess(), modules.data(), bytes, &needed)) {
                    spdlog::error("[RayReconstruction] EnumProcessModules error {}", GetLastError());
                    return paths;
                }

                if (needed <= bytes) {
                    break;
                }

                modules.resize(needed / sizeof(HMODULE) + 64);
            }

            modules.resize(needed / sizeof(HMODULE));
            std::wstring name(32768, L'\0');

            for (const auto module : modules) {
                if (const auto length = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size())); length > 0) {
                    paths.emplace_back(std::wstring_view{name.data(), length});
                }
            }

            return paths;
        },
        // Not by spelling: case, short names and the two spaces in RE2's folder.
        .same_file = [](const std::filesystem::path& a, const std::filesystem::path& b) {
            std::error_code ec{};
            return std::filesystem::equivalent(a, b, ec);
        },
        // The numbers of VS_FIXEDFILEINFO, as Explorer shows them: the string of the resource uses commas.
        .file_version = [](const std::filesystem::path& path) -> std::optional<std::string> {
            DWORD ignored{};
            const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);

            if (size == 0) {
                return std::nullopt;
            }

            std::vector<std::byte> data(size);
            VS_FIXEDFILEINFO* info{};
            UINT length{};

            if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
                !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || info == nullptr ||
                length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE) {
                return std::nullopt;
            }

            return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        },
    };
}

std::string feature_text(const rr::core::FeatureState& feature) {
    switch (feature.status) {
    case rr::core::FeatureState::Status::Absent:
        return "none";
    case rr::core::FeatureState::Status::Pending:
        return std::format("being created at {}x{}", feature.size.width, feature.size.height);
    case rr::core::FeatureState::Status::Ready:
        return std::format("ready at {}x{}", feature.size.width, feature.size.height);
    }

    return "?";
}

const rr::core::Profile* game_profile() {
    const auto profile = rr::core::find_profile(sdk::GameIdentity::get().game_name());

#ifdef RR_TEST_WRONG_TAA_VS
    // A test build: the profile with another TAA vertex shader digest, to check that RR says the digest
    // differs from the profile.
    if (profile != nullptr) {
        static const auto altered = [profile] {
            auto copy = *profile;
            copy.taa_vs.bytes.back() ^= 1;
            return copy;
        }();

        return &altered;
    }
#endif

    return profile;
}

// %p takes a void*.
const void* as_void(const void* p) {
    return p;
}

// A resource of the game, which the hooks keep as an identity.
ID3D12Resource* as_resource(rr::core::ResourceId resource) {
    return static_cast<ID3D12Resource*>(const_cast<void*>(resource));
}

std::string flags_text(std::uint32_t flags) {
    using namespace rr::core::d3d12_value;
    std::string out{};

    if ((flags & ALLOW_RENDER_TARGET) != 0) {
        out += "RT";
    }

    if ((flags & ALLOW_UNORDERED_ACCESS) != 0) {
        out += out.empty() ? "UAV" : "+UAV";
    }

    return out.empty() ? "neither RT nor UAV" : out;
}

std::string reason_text(const std::optional<rr::core::SkipReason>& reason, const char* none) {
    return reason ? std::string{rr::core::to_string(*reason)} : std::string{none};
}

// What the interface says of RR at an evaluation point that RR did not take: the decision's reason, or the
// one the Mod gave the policy afterwards.
std::string omitted_text(const rr::core::Decision& decision, rr::core::SkipReason after) {
    return "omitted: " + std::string{rr::core::to_string(decision.skip.value_or(after))};
}

std::string residuals_text(const rr::core::CutResiduals& r) {
    return std::format("pivot {:.3f} m, rotation {:.2f} deg, focal {:.2f} %", r.pivot, r.rotation, 100.0 * r.focal);
}

// An update of the cut detector: its residuals, and how much of the pivot's lies along the view's axis,
// which tells a camera push. A cut that the push rule missed shows by how much.
std::string check_text(const rr::core::CutCheck& check) {
    return std::format(
        "{}, {:.1f} % of the pivot residual along the view's axis", residuals_text(check.residuals), 100.0 * check.axis_fraction);
}

// A run of omitted frames, with its frames by reason.
std::string run_text(const rr::core::OmittedRun& run) {
    std::string reasons{};

    for (std::size_t i = 0; i < rr::core::SKIP_REASON_COUNT; ++i) {
        if (run.omitted[i] > 0) {
            reasons += std::format(
                "{}{} {}", reasons.empty() ? "" : ", ", rr::core::to_string(static_cast<rr::core::SkipReason>(i)), run.omitted[i]);
        }
    }

    return std::format("{} frames from frame {}: {}", run.frames, run.first, reasons);
}

// What the hooks saw in a frame: for the first frame of each run of omitted frames, whether the game drew
// its TAA there and why the barrier rule did not anchor it. Most loose frames of the transitions have no
// anchored draw at all, so RR cannot replace what the game shows in them.
std::string frame_hooks_text(const rr::d3d12::Counters& c) {
    using rr::d3d12::Counter;
    const auto at = [&](Counter counter) { return c[static_cast<std::size_t>(counter)]; };
    return std::format("draws {}, dispatches {}, with the TAA vertex shader {} (admitted pixel shader {}, not anchored {}), anchored {}, "
                       "attempts without the velocity {}, with two or more {}",
        at(Counter::Draws), at(Counter::Dispatches), at(Counter::TaaVsDraws), at(Counter::TaaVsAdmittedDraws),
        at(Counter::TaaVsUnanchoredDraws), at(Counter::Anchors), at(Counter::AnchorsWithoutVelocity), at(Counter::AnchorsWithTwoVelocities));
}

// A run that ended, and whether it kept RR's history or the condition that broke it.
std::string ended_run_text(const rr::core::OmittedRun& run) {
    const auto& broken = run.history_break;
    return run_text(run) + " | " + (broken ? "history reset: " + std::string{rr::core::to_string(*broken)} : std::string{"history kept"});
}

// A closed frame without velocity: evaluated, or omitted with the condition, and the Mod's reason when it
// did not evaluate RR.
std::string no_velocity_text(rr::core::NoVelocityOutcome outcome, const std::optional<rr::core::SkipReason>& skip) {
    using rr::core::NoVelocityOutcome;
    const auto text = std::string{rr::core::to_string(outcome)};

    if (rr::core::is_evaluated(outcome)) {
        return text;
    }

    return "omitted, " + text + (outcome == NoVelocityOutcome::NotEvaluated ? ": " + reason_text(skip, "") : std::string{});
}

// A run of frames evaluated reusing a G-buffer pass.
std::string reused_run_text(const rr::core::ReusedPassRun& run) {
    return std::format("frames {} to {}, which reused the G-buffer pass {}", run.first, run.last, run.pass);
}

// S_OK while the device is not removed.
HRESULT device_removed_reason() {
    const auto& hook = g_framework->get_d3d12_hook();
    const auto device = hook != nullptr ? hook->get_device() : nullptr;
    return device != nullptr ? device->GetDeviceRemovedReason() : S_OK;
}

void text(const std::string& s) {
    ImGui::TextUnformatted(s.c_str());
}

void warning(const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.8f, 0.2f, 1.0f});
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}

// The attribution that section 7.1(b) of the RTX supplement, in the NVIDIA RTX SDKs License, asks of an
// application with the DLSS SDK, since the bridge incorporates portions of its NGX SDK. It names the same
// marks as the bridge's NOTICE.
void attribution() {
    ImGui::TextWrapped("This Mod uses the NVIDIA DLSS SDK for DLSS Ray Reconstruction, which needs an NVIDIA RTX GPU. NVIDIA, RTX, "
                       "DLSS and NGX are trademarks or registered trademarks of NVIDIA Corporation in the United States and other "
                       "countries. REFramework-RayReconstruction is not sponsored or endorsed by NVIDIA.");
}

// What the registry of the resource states has seen, for the interface and the log.
std::vector<std::string> states_summary(const rr::d3d12::Status& s) {
    using rr::core::to_string;
    const auto& c = s.states;
    std::vector<std::string> lines{};

    lines.push_back(std::format("G-buffer: {} | passes {}, identity changes {}",
        s.gbuffer ? rr::d3d12::to_string(*s.gbuffer) : std::string{"not identified yet"}, c.gbuffer_passes, c.gbuffer_changes));
    lines.push_back(std::format("Command queues hooked: {}", s.queues));
    lines.push_back(std::format("Registry: {} resources created, {} lists replayed | {} transitions, {} first sights, {} mismatches ({} on "
                                "what RR reads) | not followed {}, creation not seen {}, on copy lists {}",
        c.created, c.lists, c.transitions, c.first_sights, c.mismatches, c.read_mismatches, c.not_followed, c.unseen_creation,
        c.copy_list));

    std::string problems{};

    for (std::size_t i = 0; i < c.problems.size(); ++i) {
        if (c.problems[i] > 0) {
            problems +=
                std::format("{}{} {}", problems.empty() ? "" : ", ", to_string(static_cast<rr::core::StateProblem>(i)), c.problems[i]);
        }
    }

    lines.push_back(std::format("TAA draws: {}, {} with their G-buffer pass already submitted | {}", c.draws, c.draws_pass_submitted,
        problems.empty() ? std::string{"none"} : problems));

    std::string expected{};

    for (std::size_t i = 0; i < c.expected_at_taa.size(); ++i) {
        expected += std::format("{}{} {}", i == 0 ? "" : ", ", to_string(static_cast<rr::core::Read>(i)), c.expected_at_taa[i]);
    }

    lines.push_back(std::format("TAAs confirmed at their submission: {} | in the expected state: {}", c.confirmations, expected));

    std::string order{};

    for (std::size_t i = 0; i < c.order.size(); ++i) {
        order += std::format("{}{}: {}", i == 0 ? "" : " | ", to_string(static_cast<rr::core::GBufferOrder>(i)), c.order[i]);
    }

    lines.push_back(std::format("G-buffer pass at the TAA's submission: {}", order));
    lines.push_back(std::format("Predictions the TAA contradicted: {} ({} with another G-buffer submitted than the draw used), at draws "
                                "that took the state as known: {}",
        c.disagree, c.gbuffer_differs, c.wrong));

    if (s.last_confirmation) {
        lines.push_back(std::format("Last TAA: {}", rr::d3d12::to_string(s.last_confirmation->actual)));
    }

    return lines;
}
} // namespace

std::shared_ptr<RayReconstruction>& RayReconstruction::get() {
    static auto mod = std::make_shared<RayReconstruction>();
    return mod;
}

bool RayReconstruction::is_supported() {
    return game_profile() != nullptr;
}

void RayReconstruction::on_d3d12_loaded(HMODULE d3d12) {
    if (d3d12 == nullptr) {
        return;
    }

    if (const auto profile = game_profile(); profile != nullptr) {
        rr::d3d12::install(d3d12, *profile);
    }
}

std::optional<std::string> RayReconstruction::on_initialize() {
    const auto dir = rr_dir();

    std::error_code ec{};
    std::filesystem::create_directories(dir, ec);

    const auto bridge = rr::core::load_bridge(dir, {
        .open = [](const std::filesystem::path& path, std::string& error) -> void* {
            const auto module = LoadLibraryW(path.c_str());

            if (module == nullptr) {
                error = "LoadLibraryW error " + std::to_string(GetLastError());
            }

            return module;
        },
        .exists = [](const std::filesystem::path& path) {
            std::error_code ec{};
            return std::filesystem::exists(path, ec);
        },
        .find = [](void* module, const char* name) -> void* {
            return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
        },
        .close = [](void* module) {
            FreeLibrary(static_cast<HMODULE>(module));
        },
    });

    spdlog::info("[RayReconstruction] {}: {}", rr::core::to_string(bridge.availability), bridge.detail);

    const auto hooks = rr::d3d12::status();
    spdlog::info("[RayReconstruction] D3D12CreateDevice: {} | {}", hooks.create_device, hooks.device);

    m_profile = game_profile();

    {
        std::scoped_lock _{m_policy_mutex};
        m_session = std::make_unique<rr::core::BridgeSession>(bridge, process_modules());
        m_policy = std::make_unique<rr::core::FramePolicy>(*m_profile);
        m_policy->on_bridge_state(m_session->state());
        m_policy->on_toggle(m_enabled->value());
        read_debug_options();
        read_jitter_options();
        read_measurement_options();
    }

    rr::d3d12::set_anchor_handler(&RayReconstruction::on_anchored_draw);
    rr::d3d12::set_timed_draw_handler(&RayReconstruction::on_timed_draw);

    // RR being unavailable is not an error of REFramework.
    return std::nullopt;
}

rr::d3d12::AnchorResult RayReconstruction::on_anchored_draw(const rr::d3d12::AnchoredDraw& draw) {
    return get()->on_evaluation_point(draw);
}

// Outside m_policy_mutex: the guide pass takes its own lock.
void RayReconstruction::on_timed_draw(void* list) {
    get()->m_guide_pass.end_taa_draw(static_cast<ID3D12GraphicsCommandList*>(list));
}

rr::d3d12::AnchorResult RayReconstruction::on_evaluation_point(const rr::d3d12::AnchoredDraw& draw) {
    std::scoped_lock _{m_policy_mutex};

    if (!decide(draw)) {
        return {};
    }

    auto result = replace_taa(draw);

    // The game's TAA draw, timed for RR's net cost: only in an identified frame, where the draw is the one
    // RR's evaluation omits. In the menus, the draw anchored may be another one.
    if (!result.replaces_draw && !m_frames.decision.identification) {
        result.times_draw = m_guide_pass.begin_taa_draw(static_cast<ID3D12GraphicsCommandList*>(draw.list), draw.frame);
    }

    return result;
}

// Under m_policy_mutex. The debug view wins over the accumulated reference, which wins over RR.
rr::d3d12::AnchorResult RayReconstruction::replace_taa(const rr::d3d12::AnchoredDraw& draw) {
    if (const auto work = debug_work(draw)) {
        m_frames.reference.status = m_reference_on ? "not written: the debug view wins" : "off";
        return show_debug_view(*work);
    }

    if (m_reference_on) {
        return accumulate_reference(draw);
    }

    m_frames.reference.status = "off";
    return evaluate(draw);
}

// Under m_policy_mutex. Either way RR has no history of the frame. Only a view recorded replaces the
// game's TAA, which the own jitter needs; otherwise the TAA draws, as after an evaluation not recorded.
rr::d3d12::AnchorResult RayReconstruction::show_debug_view(const DebugWork& work) {
    const auto recorded = m_guide_pass.record_debug_view(work.inputs, work.o, work.guide, work.scale);
    const auto reason = recorded ? rr::core::SkipReason::DebugView : rr::core::SkipReason::EvaluationFailed;
    m_policy->on_not_evaluated(reason);
    m_frames.evaluation = omitted_text(m_frames.decision, reason);

    if (!recorded) {
        return {};
    }

    return {.recorded = true, .replaces_draw = true};
}

// Under m_policy_mutex. The accumulated reference writes the mean of T into O in place of the TAA, and of
// RR, as the debug view does: whatever RR, the bridge and the feature say, but only in a frame whose
// evaluation point is identified. It needs no guide pass, so neither the G-buffer nor the camera
// constants: ReferenceAccumulator follows the camera.
rr::d3d12::AnchorResult RayReconstruction::accumulate_reference(const rr::d3d12::AnchoredDraw& draw) {
    const auto& d = m_frames.decision;
    auto& v = m_frames.reference;

    // An evaluation point not identified omits the frame already.
    if (d.identification) {
        v.status = "not written: " + std::string{rr::core::to_string(*d.identification)};
        m_frames.evaluation = omitted_text(d, *d.identification);
        return {};
    }

    // Without the game's heaps, the hooks could not bind them again after the reference's own. RR is not
    // evaluated either: the TAA draws.
    if (!draw.heaps_bound) {
        v.status = "not written: the game bound no descriptor heaps";
        m_policy->on_not_evaluated(rr::core::SkipReason::EvaluationFailed);
        m_frames.evaluation = omitted_text(d, rr::core::SkipReason::EvaluationFailed);
        return {};
    }

    const auto step = m_reference.on_recording(draw.frame, d.render_size);
    const auto recorded = m_guide_pass.record_reference({
        .list = static_cast<ID3D12GraphicsCommandList*>(draw.list),
        .t = as_resource(draw.anchor.t),
        .o = as_resource(draw.rt0.resource),
        .size = d.render_size,
        .frames_before = step.frames_before,
        .frame = draw.frame,
    });

    if (step.restart) {
        v.last_restart = step.restart;
        v.last_restart_frame = draw.frame;
        ++v.restarts[static_cast<std::size_t>(*step.restart)];
    }

    // The GPU's mean lacks a frame that was not recorded, so the next one starts it again.
    if (!recorded) {
        m_reference.on_not_recorded();
    }

    v.frames = m_reference.frames();

    // As with the debug view, only a reference recorded replaces the game's TAA.
    const auto reason = recorded ? rr::core::SkipReason::AccumulatedReference : rr::core::SkipReason::EvaluationFailed;
    m_policy->on_not_evaluated(reason);
    m_frames.evaluation = omitted_text(d, reason);

    if (!recorded) {
        v.status = "not written: the reference could not be recorded (see the guide pass's status)";
        return {};
    }

    v.status = std::format("written, {} frames in the mean", v.frames);
    return {.recorded = true, .replaces_draw = true};
}

bool RayReconstruction::decide(const rr::d3d12::AnchoredDraw& draw) {
    const auto registry = rr::d3d12::pso_registry();
    const auto discovery_session = registry != nullptr ? registry->discovery_session() : 0;
    const Differences differences{
        .unknown_ps = draw.point.vs == m_profile->taa_vs && draw.ps_role == rr::d3d12::PsRole::NotInProfile,
        .t_format = !draw.point.t_profile_format,
    };

    log_digests(draw, differences, discovery_session);

    // The others are counted: the pass in RE2 shows whether a frame has any.
    if (m_frame_anchors++ > 0 || m_policy == nullptr) {
        return false;
    }

    log_sizes(draw);
    poll_creation();

    auto point = draw.point;
    point.feature = m_session->feature();
    m_frames.decision = m_policy->on_evaluation_point(point);
    m_frames.last_draw = draw;
    log_states(draw);

    switch (m_frames.decision.action) {
    case rr::core::FeatureAction::InitNgx:
        m_start_ngx = true;
        break;
    case rr::core::FeatureAction::Create:
    case rr::core::FeatureAction::Recreate:
        // Pending from now on, although the creation waits for Present.
        m_session->request_feature(m_frames.decision.render_size);
        break;
    case rr::core::FeatureAction::Release:
        release_feature("asked at the evaluation point");
        break;
    case rr::core::FeatureAction::None:
        break;
    }

    if (differences.unknown_ps) {
        m_frames.unknown_ps = draw.point.ps;
    }

    if (!m_frames.decision.identification) {
        auto& by_ps = m_frames.identified_by_ps;
        const auto ps = std::find_if(by_ps.begin(), by_ps.end(), [&](const auto& entry) { return entry.first == draw.point.ps; });

        if (ps == by_ps.end()) {
            by_ps.emplace_back(draw.point.ps, 1);
        } else {
            ++ps->second;
        }

        if (draw.point.o && draw.point.o->uav && (draw.rt0.desc.flags & rr::core::d3d12_value::ALLOW_RENDER_TARGET) != 0) {
            ++m_frames.o_rt_uav;
        }
    }

    return true;
}

// Under m_policy_mutex. The debug view writes O in place of the TAA, and of RR, whatever RR, the bridge
// and the feature say, but only in a frame whose evaluation point is identified, and with the constants
// of a camera update.
std::optional<RayReconstruction::DebugWork> RayReconstruction::debug_work(const rr::d3d12::AnchoredDraw& draw) {
    const auto& d = m_frames.decision;
    auto& status = m_frames.debug_view;

    if (!m_debug.guide) {
        status = "off";
        return std::nullopt;
    }

    if (d.identification) {
        status = "not written: " + std::string{rr::core::to_string(*d.identification)};
        return std::nullopt;
    }

    std::string why{};
    const auto inputs = guide_inputs(draw, why);

    if (!inputs) {
        status = "not written: " + why;
        return std::nullopt;
    }

    // Whether it was recorded, the guide pass's status says.
    status = "asked of the guide pass";

    return DebugWork{
        .inputs = *inputs,
        .o = as_resource(draw.rt0.resource),
        .guide = *m_debug.guide,
        .scale = m_debug.scale,
    };
}

// Under m_policy_mutex, so that a release at Present cannot take the feature between the decision and
// the bridge: the guide pass, RR's barriers and its evaluation, in one sequence. The hooks restore the
// list after it, and the TAA draw goes only if RR was not evaluated.
rr::d3d12::AnchorResult RayReconstruction::evaluate(const rr::d3d12::AnchoredDraw& draw) {
    const auto& d = m_frames.decision;
    auto& status = m_frames.evaluation;

    if (d.skip) {
        status = "omitted: " + std::string{rr::core::to_string(*d.skip)};
        return {};
    }

    std::string why{};
    const auto inputs = guide_inputs(draw, why);

    if (!inputs) {
        status = "not recorded: " + why;
        m_policy->on_not_evaluated(rr::core::SkipReason::EvaluationFailed);
        return {};
    }

    // InJitterOffset with the sign of the diagnostic option. The guide pass keeps the policy's j.
    auto bridge_decision = d;
    bridge_decision.camera->jitter_offset.x *= m_jitter.offset_sign.x;
    bridge_decision.camera->jitter_offset.y *= m_jitter.offset_sign.y;

    const auto t = as_resource(draw.anchor.t);
    const auto o = as_resource(draw.rt0.resource);
    const auto record = m_guide_pass.record_evaluation(
        *inputs, t, o,
        [&](const rr::d3d12::Guides& guides) {
            m_evaluated_since_wait = true;

            return m_session->evaluate(
                {
                    .command_list = draw.list,
                    .color = t,
                    .output = o,
                    .depth = guides.depth,
                    .motion_vectors = guides.motion_vectors,
                    .normals_roughness = guides.normals_roughness,
                    .diffuse_albedo = guides.diffuse_albedo,
                    .specular_albedo = guides.specular_albedo,
                },
                bridge_decision);
        },
        m_forced_nonfinite);

    if (record.evaluated) {
        status = std::format("evaluated, reset: {}", rr::core::to_string(d.reset));
        m_frames.jitter.offset_sent = bridge_decision.camera->jitter_offset;
        return {.recorded = true, .replaces_draw = true};
    }

    m_policy->on_not_evaluated(rr::core::SkipReason::EvaluationFailed);

    if (!record.recorded) {
        status = "not recorded: the guide pass recorded nothing";
        return {};
    }

    // The feature changed since the decision, or the bridge failed.
    if (m_session->availability() == rr::core::Availability::Available) {
        status = "not recorded: the feature is no longer the one the decision saw ready";
        return {.recorded = true};
    }

    status = "not recorded: " + m_session->detail();
    spdlog::error("[RayReconstruction] RR not evaluated at frame {}: {}", draw.frame, m_session->detail());

    // The list being recorded may hold what NGX recorded before it failed: the feature goes once it is
    // submitted.
    if (m_policy->on_bridge_state(m_session->state()) == rr::core::FeatureAction::Release) {
        m_release_at_present = "the evaluation failed";
    }

    return {.recorded = true};
}

// Under m_policy_mutex. Neither of the first two happens in an identified frame of RE2: the TAA binds its
// tables, and a known state needs the G-buffer.
std::optional<rr::d3d12::GuideInputs> RayReconstruction::guide_inputs(const rr::d3d12::AnchoredDraw& draw, std::string& why) const {
    const auto& d = m_frames.decision;

    if (!draw.heaps_bound) {
        why = "the game bound no descriptor heaps";
        return std::nullopt;
    }

    if (!draw.states.gbuffer) {
        why = "no G-buffer";
        return std::nullopt;
    }

    if (!d.camera) {
        why = std::string{rr::core::to_string(rr::core::SkipReason::CameraUnknown)};
        return std::nullopt;
    }

    return rr::d3d12::GuideInputs{
        .list = static_cast<ID3D12GraphicsCommandList*>(draw.list),
        .gbuffer = *draw.states.gbuffer,
        .size = d.render_size,
        .camera = *d.camera,
        .states = draw.states.predicted,
        .round_trip = m_debug.round_trip,
        .reproject_saturated = m_debug.reproject_saturated,
        .has_velocity = draw.point.has_velocity,
        .ambient_brdf = draw.ambient_brdf,
        .lut_size = m_profile->ambient_brdf.size,
        .use_lut = m_debug.use_lut,
        .frame = draw.frame,
    };
}

// Under m_policy_mutex.
void RayReconstruction::read_debug_options() {
    // The config may hold any number.
    const auto view = std::clamp(m_debug_view->value(), 0, rr::core::guides::DEBUG_VIEW_MOTION_VECTORS + 1);
    m_debug.guide = view > 0 ? std::optional{view - 1} : std::nullopt;
    m_debug.scale = m_debug_scale->value();
    m_debug.round_trip = m_barrier_round_trip->value();
    m_debug.reproject_saturated = m_reproject_saturated->value();

    const auto last = static_cast<int>(SPECULAR_SOURCES.size()) - 1;
    auto source = SPECULAR_SOURCES[std::clamp(m_specular_source->value(), 0, last)];

    if (!source && m_profile != nullptr) {
        source = m_profile->ambient_brdf.source;
    }

    m_debug.use_lut = source == rr::core::AmbientBrdfSource::GameLut;
}

// Under m_policy_mutex.
void RayReconstruction::read_jitter_options() {
    // The config may hold any number.
    const auto follow = std::clamp(m_follow_inverses->value(), 0, 2);
    const auto last = static_cast<int>(JITTER_OFFSET_SIGNS.size()) - 1;
    m_jitter.inject = m_own_jitter->value();
    m_jitter.follow_jitter_disable = follow >= 1;
    m_jitter.follow_post = follow == 2;
    m_jitter.offset_sign = JITTER_OFFSET_SIGNS[std::clamp(m_jitter_offset_sign->value(), 0, last)];

    if (m_policy != nullptr) {
        m_policy->on_jitter_injection(m_jitter.inject);
    }
}

// Under m_policy_mutex. Neither option is saved, so they start off in every session.
void RayReconstruction::read_measurement_options() {
    const auto last = static_cast<int>(FORCED_NONFINITE.size()) - 1;
    m_reference_on = m_accumulated_reference->value();
    m_forced_nonfinite = FORCED_NONFINITE[std::clamp(m_force_nonfinite->value(), 0, last)];
}

void RayReconstruction::on_enabled_changed(std::string_view from) {
    {
        std::scoped_lock _{m_policy_mutex};

        if (m_policy != nullptr) {
            m_policy->on_toggle(m_enabled->value());
        }

        spdlog::info("[RayReconstruction] RR switched {} from {} at frame {}", m_enabled->value() ? "on" : "off", from, m_hooks_frame);
    }

    g_framework->request_save_config();
}

// Under m_policy_mutex.
void RayReconstruction::log_digests(const rr::d3d12::AnchoredDraw& draw, const Differences& differences, std::uint32_t discovery_session) {
    const auto first_time = [this](const rr::core::ShaderDigest& digest) {
        if (std::find(m_logged_digests.begin(), m_logged_digests.end(), digest) != m_logged_digests.end()) {
            return false;
        }

        m_logged_digests.push_back(digest);
        return true;
    };

    const auto& t = draw.anchor.t_desc;
    const auto& rt0 = draw.rt0.desc;

    if (differences.unknown_ps && first_time(draw.point.ps)) {
        spdlog::info("[RayReconstruction] Evaluation point with a pixel shader the profile does not have: {}",
            rr::d3d12::to_string(draw.shaders.ps));
    }

    if (differences.t_format && !m_logged_t_format) {
        m_logged_t_format = true;
        spdlog::info("[RayReconstruction] T has format {}, not the profile's {}", static_cast<std::uint32_t>(t.format),
            static_cast<std::uint32_t>(m_profile->formats.t));
    }

    if (discovery_session == 0) {
        return;
    }

    if (discovery_session != m_discovery_session) {
        m_discovered.clear();
        m_discovery_session = discovery_session;
    }

    // Once per pair of shaders and discovery, so that the log names every variant without growing with
    // every frame.
    const std::pair shaders{draw.point.vs, draw.point.ps};

    if (std::find(m_discovered.begin(), m_discovered.end(), shaders) != m_discovered.end()) {
        return;
    }

    m_discovered.push_back(shaders);
    spdlog::info("[RayReconstruction] Evaluation point: VS {} | PS {} ({}) | PT compute shader {} | T {}x{} format {} {} | RT0 {}x{} "
                 "format {} {}, {} | list {:x} thread {}",
        rr::d3d12::to_string(draw.shaders.vs), rr::d3d12::to_string(draw.shaders.ps), rr::d3d12::to_string(draw.ps_role),
        rr::d3d12::to_string(draw.anchor.pt_cs), t.size.width, t.size.height, static_cast<std::uint32_t>(t.format), flags_text(t.flags),
        rt0.size.width, rt0.size.height, static_cast<std::uint32_t>(rt0.format), flags_text(rt0.flags), rr::core::to_string(draw.o_problem),
        reinterpret_cast<std::uintptr_t>(draw.list), draw.thread);
}

// Under m_policy_mutex. The game goes through every step of a change of image quality in a few frames,
// too fast for the interface.
void RayReconstruction::log_sizes(const rr::d3d12::AnchoredDraw& draw) {
    const auto t = draw.anchor.t_desc.size;
    const auto o = draw.rt0.desc.size;

    if (t == m_logged_t_size && o == m_logged_o_size) {
        return;
    }

    m_logged_t_size = t;
    m_logged_o_size = o;
    spdlog::info("[RayReconstruction] Evaluation point at frame {}: T {}x{}, RT0 {}x{} ({})", draw.frame, t.width, t.height, o.width,
        o.height, rr::core::to_string(draw.o_problem));
}

// Under m_policy_mutex. On every change of the state problem: the frame where the G-buffer changes
// identity, and the first one whose state is known again.
void RayReconstruction::log_states(const rr::d3d12::AnchoredDraw& draw) {
    const auto& s = draw.states;

    if (s.problem == m_logged_state_problem) {
        return;
    }

    // The identification gives "resource state unknown" also with RR off or without the bridge.
    m_logged_state_problem = s.problem;
    spdlog::info(
        "[RayReconstruction] Resource states at frame {}: {} | identification: {} | decision: {} | G-buffer pass {}, {} at the draw "
        "| {}",
        draw.frame, rr::core::to_string(s.problem), reason_text(m_frames.decision.identification, "identified"),
        reason_text(m_frames.decision.skip, "evaluate"), s.gbuffer_pass, s.gbuffer_submitted ? "submitted" : "not submitted",
        rr::d3d12::to_string(s.predicted));
}

void RayReconstruction::on_present() {
    const auto now = std::chrono::steady_clock::now();
    const auto hooks_frame = rr::d3d12::end_frame();
    const auto hooks_counters = rr::d3d12::last_frame_counters();
    check_lut_axis(hooks_frame);
    take_measurements(hooks_frame);
    const auto removed = device_removed_reason();

    std::scoped_lock _{m_policy_mutex};
    const auto closed = hooks_frame - 1;
    m_hooks_frame = hooks_frame;

    // REFramework logs the HRESULT of a Present that fails; this gives the device's reason, once.
    if (removed != S_OK && !m_frames.device_removed) {
        m_frames.device_removed = removed;
        spdlog::error("[RayReconstruction] Device removed at frame {}: reason {:x}", closed, static_cast<std::uint32_t>(removed));
    }

    if (m_policy == nullptr) {
        return;
    }

    // A frame without an update of the main scene layer: the cut detector starts its prediction again.
    if (!m_camera.updated) {
        rr::core::CameraUpdate stale{};
        stale.main_layer_updated = false;
        m_policy->on_camera_update(stale);
        m_reference.on_camera_update(stale);
        ++m_frames.camera.frames_without_update;
    }

    m_camera.updated = false;
    ++m_camera.presents;

    const auto frame = m_policy->on_end_frame();
    m_frames.counters.add(frame);
    m_frames.last = frame;
    ++m_frames.anchors_per_frame[std::min(m_frame_anchors, MANY_ANCHORS)];
    m_frame_anchors = 0;

    // Every frame without velocity, one or two per transition. Its draw is the frame's evaluation point,
    // so the last one.
    if (frame.no_velocity) {
        const auto& d = m_frames.last_draw;
        spdlog::info("[RayReconstruction] Frame without velocity at frame {}: {} | VS {} PS {} | G-buffer pass {}", closed,
            no_velocity_text(*frame.no_velocity, frame.skip), d ? rr::d3d12::to_string(d->shaders.vs) : std::string{"?"},
            d ? rr::d3d12::to_string(d->shaders.ps) : std::string{"?"}, d ? d->states.gbuffer_pass : 0);
    }

    // The interval that ends here is the closed frame's, for the drops below 60 fps.
    if (m_last_present) {
        const auto ms = std::chrono::duration<double, std::milli>(now - *m_last_present).count();
        m_frames.measures.intervals.add(rr::core::frame_mode(frame, m_debug.guide.has_value() || m_reference_on), ms);
    }

    m_last_present = now;

    // The runs of omitted frames, for the objective gate that wants none in normal play: the log tells the
    // menus and the transitions from the rest by their time and their reasons, and whether each one kept
    // RR's history.
    const auto change = m_frames.runs.add(closed, frame);

    if (change.started) {
        spdlog::info("[RayReconstruction] Omitted frames from frame {}: {} | in that frame, {}", closed, reason_text(frame.skip, ""),
            frame_hooks_text(hooks_counters));
    }

    if (change.ended) {
        spdlog::info("[RayReconstruction] Omitted frames: the run ended at frame {}, {}", closed, ended_run_text(*change.ended));
        m_frames.last_run = change.ended;
        m_frames.last_run_end = closed;
    }

    // Whether the noise of a transition falls in the frames whose guides are an earlier frame's, as after
    // the three frames that open the screen that picks an object.
    if (const auto ended = m_frames.reused_passes.add(closed, frame)) {
        spdlog::info("[RayReconstruction] Frames evaluated without a G-buffer pass of their own: {}", reused_run_text(*ended));
    }

    run_bridge_work();
}

// The check of the LUT's axis: what the V axis of the game's LUT holds, so that the guide pass samples it
// with the right roughness. Outside m_policy_mutex, because the guide pass takes its own lock. The log
// gets every reading and a few texels, to check the judgement by hand.
void RayReconstruction::check_lut_axis(std::uint64_t frame) {
    const auto readback = m_guide_pass.take_lut_readback(frame);

    if (!readback || m_profile == nullptr) {
        return;
    }

    const auto& lut = m_profile->ambient_brdf;
    std::string result{};

    if (readback->found != lut.size) {
        result = std::format("the texture at the PT's t{} is {}x{}, not the profile's {}x{}, so it is not the LUT", lut.srv_register,
            readback->found.width, readback->found.height, lut.size.width, lut.size.height);
    } else {
        const auto judgement = rr::core::judge_ambient_brdf_lut(readback->texels, lut.size);
        std::string fits{};

        for (const auto& fit : judgement.fits) {
            fits += std::format("{}{} {:.4f}", fits.empty() ? "" : "; ", rr::core::to_string(fit.reading), fit.error);
        }

        result = std::format("{} | mean error per channel against the scale and bias of the GGX fit, V from {} to {}: {}",
            judgement.reading ? rr::core::to_string(*judgement.reading) : std::string{"inconclusive"}, rr::core::JUDGED_V_MIN,
            rr::core::JUDGED_V_MAX, fits);

        // A few texels on the diagonal, against the fit with the perceptual roughness on V.
        for (const auto at : {0.25f, 0.5f, 0.75f}) {
            const auto x = static_cast<std::uint32_t>(at * static_cast<float>(lut.size.width));
            const auto y = static_cast<std::uint32_t>(at * static_cast<float>(lut.size.height));
            const auto i = (std::size_t{y} * lut.size.width + x) * 2;
            const auto u = rr::core::texel_center(x, lut.size.width);
            const auto v = rr::core::texel_center(y, lut.size.height);
            const auto fit = rr::core::expected_lut_texel({rr::core::LutAxis::PerceptualRoughness, false}, u, v);
            spdlog::info("[RayReconstruction] AmbientBRDF texel ({}, {}) at u {:.4f} v {:.4f}: {:.4f}, {:.4f} | GGX fit with "
                         "roughness = v: {:.4f}, {:.4f}",
                x, y, u, v, readback->texels[i], readback->texels[i + 1], fit[0], fit[1]);
        }
    }

    spdlog::info("[RayReconstruction] AmbientBRDF LUT read back from frame {}: {}", readback->frame, result);

    std::scoped_lock _{m_policy_mutex};
    m_frames.lut_check = std::format("frame {}: {}", readback->frame, result);
}

// The measurement interface. Outside m_policy_mutex, because the guide pass takes its own lock. The
// timestamps tick at the frequency of the queue that runs the TAA's list, the one REFramework captures.
void RayReconstruction::take_measurements(std::uint64_t frame) {
    const auto measured = m_guide_pass.take_measurements(frame);

    if (measured.empty()) {
        return;
    }

    std::scoped_lock _{m_policy_mutex};
    auto& m = m_frames.measures;

    if (m.frequency == 0) {
        const auto& hook = g_framework->get_d3d12_hook();
        const auto queue = hook != nullptr ? hook->get_command_queue() : nullptr;
        UINT64 frequency{};

        if (queue != nullptr && SUCCEEDED(queue->GetTimestampFrequency(&frequency))) {
            m.frequency = frequency;
            spdlog::info("[RayReconstruction] Timestamp frequency: {} Hz", frequency);
        }
    }

    for (const auto& measurement : measured) {
        ++m.taken;

        for (std::size_t i = 0; i < rr::d3d12::SEGMENT_COUNT; ++i) {
            if (const auto& ticks = measurement.segments[i]) {
                m.timers[i].add((*ticks)[0], (*ticks)[1], m.frequency);
            }
        }

        if (!measurement.nonfinite_totals) {
            continue;
        }

        const auto before = m.nonfinite.counts().frames_with_nonfinite;
        m.nonfinite.add(measurement.frame, *measurement.nonfinite_totals);
        const auto& c = m.nonfinite.counts();
        const auto dirty = c.frames_with_nonfinite > before;

        // Each run of frames with non-finite pixels, for the gate of the validation protocol, which wants
        // none in normal play. Its start and its end, so that the log does not grow with every frame.
        if (dirty && !m.run_start) {
            m.run_start = measurement.frame;
            spdlog::warn("[RayReconstruction] Non-finite pixels in O after RR from frame {} | totals since the reset: NaN {}, infinite {}",
                measurement.frame, c.nan_pixels, c.inf_pixels);
        }

        if (dirty) {
            ++m.run_frames;
        } else if (m.run_start) {
            spdlog::warn("[RayReconstruction] Non-finite pixels in O after RR: the run from frame {} ended after {} frames counted | "
                         "totals since the reset: NaN {}, infinite {}",
                *m.run_start, m.run_frames, c.nan_pixels, c.inf_pixels);
            m.run_start.reset();
            m.run_frames = 0;
        }
    }
}

// The camera input, on the engine's thread that updated the layer: after the update of the main scene
// layer, the matrices of Scene and JitterDisable, as the game wrote them.
void RayReconstruction::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    if (layer == nullptr || !layer->is_fully_rendered()) {
        return;
    }

    const auto scene = layer->get_scene_info();
    const auto jitter_disable = layer->get_jitter_disable_scene_info();

    if (scene == nullptr || jitter_disable == nullptr) {
        return;
    }

    rr::core::CameraUpdate update{};
    std::memcpy(&update.scene, scene, sizeof(update.scene));
    std::memcpy(&update.jitter_disable, jitter_disable, sizeof(update.jitter_disable));

    std::scoped_lock _{m_policy_mutex};

    if (m_policy == nullptr) {
        return;
    }

    auto& c = m_camera;

    if (layer != c.main_layer) {
        if (c.main_layer != nullptr && c.presents <= c.main_layer_frame + MAIN_LAYER_SILENCE) {
            ++m_frames.camera.other_layer_updates;
            return;
        }

        update.layer_changed = c.main_layer != nullptr;
        c.main_layer = layer;
        ++m_frames.camera.main_layer_changes;
        spdlog::info("[RayReconstruction] Main scene layer at frame {}: {:x}", c.presents, reinterpret_cast<std::uintptr_t>(layer));
    }

    c.main_layer_frame = c.presents;
    c.updated = true;

    // An anchored draw since the last Present: the frame's evaluation point came before this update.
    ++(m_frame_anchors > 0 ? m_frames.jitter.after_point : m_frames.jitter.before_point);

    const auto decision = m_policy->on_camera_update(update);
    m_reference.on_camera_update(update);
    m_frames.camera.last_update = decision.update_number;
    ++m_frames.camera.updates;

    // The checks of the cut detector at 30 fps, aiming while turning, in the game's 180-degree turn and
    // with another mouse sensitivity, and the camera against a wall: every cut, push and near miss goes to
    // the log, with its residuals and the fraction of the pivot's along the view's axis.
    if (const auto& check = decision.cut_check) {
        auto& cuts = m_frames.cuts;
        cuts.stats.add(*check);

        if (check->cut || check->push || check->near_miss) {
            (check->cut ? cuts.last_cut : check->push ? cuts.last_push : cuts.last_near_miss) = {m_hooks_frame, *check};
            spdlog::info("[RayReconstruction] {} at frame {}, camera update {}: {}",
                check->cut ? "Camera cut" : check->push ? "Camera push" : "Near miss of the cut detector", m_hooks_frame,
                decision.update_number, check_text(*check));
        }
    }

    write_jitter(layer, update, decision);
}

// Under m_policy_mutex: RR's own jitter, written into the profile's variants, chosen by name. Each one is
// copied, injected and written back whole; old_view_projection keeps what the game wrote, without jitter,
// so that its velocity follows the injection.
void RayReconstruction::write_jitter(
    sdk::renderer::layer::Scene* layer, const rr::core::CameraUpdate& update, const rr::core::CameraDecision& decision) {
    using rr::core::SceneMatrices;
    using rr::core::SceneVariant;
    auto& v = m_frames.jitter;
    const auto post = scene_info(layer, SceneVariant::JitterDisablePost);
    SceneMatrices post_matrices{};

    // What the correction of the inverses relies on, on the matrices as the game wrote them, also while
    // nothing is injected.
    ++v.compared;
    v.jitter_disable_same += rr::core::same_inverses(update.jitter_disable, update.scene);

    if (post == nullptr) {
        ++v.post_missing;
    } else {
        std::memcpy(&post_matrices, post, sizeof(post_matrices));
        v.post_same += rr::core::same_inverses(post_matrices, update.scene);

        const auto& p = post_matrices.projection;
        const auto& jd = update.jitter_disable.projection;
        v.post_jittered += p[2][0] != jd[2][0] || p[2][1] != jd[2][1];
    }

    v.last_injected = decision.jitter_term.has_value();

    if (!decision.jitter_term) {
        return;
    }

    const auto term = *decision.jitter_term;
    const auto& jitter_disable_projection = update.jitter_disable.projection;

    // Scene from the matrices the policy got: the game gets the same ones the evaluation takes its
    // constants from.
    const auto injected_scene = rr::core::inject_jitter(update.scene, jitter_disable_projection, term);
    ++v.injected;

    for (const auto& jitter_variant : m_profile->jitter_variants) {
        const auto i = static_cast<std::size_t>(jitter_variant.variant);
        const auto info = scene_info(layer, jitter_variant.variant);

        if (info == nullptr) {
            ++v.missing[i];
            continue;
        }

        auto injected = injected_scene;

        if (jitter_variant.variant != SceneVariant::Scene) {
            SceneMatrices matrices{};
            std::memcpy(&matrices, info, sizeof(matrices));
            injected = rr::core::inject_jitter(matrices, jitter_disable_projection, term);
        }

        // The copy lies past what REFramework knows of a SceneInfo, as a probe of RE2's jitter and motion
        // vectors found. The check only says those floats are mapped; that they are the copy,
        // write_injection_with_copy checks bit for bit.
        if (!jitter_variant.view_projection_copy || IsBadReadPtr(info, rr::core::SCENE_INFO_FLOATS * sizeof(float))) {
            std::memcpy(info, &injected, sizeof(injected));
        } else {
            const std::span<float, rr::core::SCENE_INFO_FLOATS> floats{reinterpret_cast<float*>(info), rr::core::SCENE_INFO_FLOATS};
            v.copies[i] += rr::core::write_injection_with_copy(floats, injected);
        }

        ++v.written[i];
    }

    const auto jitter_disable_info = scene_info(layer, SceneVariant::JitterDisable);

    if (auto jitter_disable = update.jitter_disable; m_jitter.follow_jitter_disable && jitter_disable_info != nullptr &&
                                                     rr::core::follow_scene_inverses(jitter_disable, update.scene, injected_scene)) {
        std::memcpy(jitter_disable_info, &jitter_disable, sizeof(jitter_disable));
        ++v.jitter_disable_followed;
    }

    if (post != nullptr && m_jitter.follow_post && rr::core::follow_scene_inverses(post_matrices, update.scene, injected_scene)) {
        std::memcpy(post, &post_matrices, sizeof(post_matrices));
        ++v.post_followed;
    }
}

// The game's settings the profile checks, on the thread where Graphics applies its own every frame. The
// log gets every change of the PT settings and of the warnings.
void RayReconstruction::on_frame() {
    if (m_profile == nullptr) {
        return;
    }

    // On the thread of the interface, which draws m_enabled too.
    if (m_switch_key->is_key_down_once()) {
        m_enabled->toggle();
        on_enabled_changed("the switch key");
    }

    auto settings = read_game_settings();
    auto warnings = rr::core::check_settings(m_profile->settings, settings);
    std::vector<std::string> texts{rr::core::pt_settings_text(settings)};

    for (const auto& warning : warnings) {
        texts.push_back(warning.text);
    }

    if (texts != m_logged_settings) {
        for (const auto& text : texts) {
            spdlog::info("[RayReconstruction] Game settings: {}", text);
        }

        if (warnings.empty()) {
            spdlog::info("[RayReconstruction] Game settings: as the profile checks them");
        }

        m_logged_settings = std::move(texts);
    }

    std::scoped_lock _{m_policy_mutex};
    m_frames.settings = std::move(settings);
    m_frames.setting_warnings = std::move(warnings);
}

// What REFramework's Graphics applies, not what the game reports: Pure PT and the Clone Types only exist
// there. Without its ray tracing tweaks, Graphics applies none of them.
rr::core::GameSettings RayReconstruction::read_game_settings() {
    rr::core::GameSettings s{};

#if defined(REFRAMEWORK_UNIVERSAL) || TDB_VER >= 69
    const auto& graphics = Graphics::get();
    const auto name = [](ModCombo& combo) {
        const auto& options = combo.options();
        const auto i = combo.value();
        return i >= 0 && static_cast<std::size_t>(i) < options.size() ? std::string{options[i]} : std::string{"?"};
    };

    s.ray_tracing_tweaks = graphics->m_ray_tracing_tweaks->value();
    s.ray_trace_type = name(*graphics->m_ray_trace_type);
    s.clone_types = {name(*graphics->m_ray_trace_clone_type_pre), name(*graphics->m_ray_trace_clone_type_post),
        name(*graphics->m_ray_trace_clone_type_true)};
    s.spp = static_cast<std::uint32_t>(std::strtoul(name(*graphics->m_samples_per_pixel).c_str(), nullptr, 10));
    s.bounces = static_cast<std::uint32_t>(std::strtoul(name(*graphics->m_bounce_count).c_str(), nullptr, 10));
#endif

    s.anti_aliasing = read_anti_aliasing();
    return s;
}

// From REFramework's reset: ResizeBuffers or ResizeTarget, as with an alt-tab or a change of
// resolution or of window mode. The device stays, and so does NGX.
void RayReconstruction::on_device_reset() {
    std::scoped_lock _{m_policy_mutex};

    if (m_policy != nullptr && m_policy->on_device_reset() == rr::core::FeatureAction::Release) {
        release_feature("device reset");
    }
}

// Under m_policy_mutex, at Present.
void RayReconstruction::run_bridge_work() {
    if (m_release_at_present) {
        release_feature(*m_release_at_present);
        m_release_at_present.reset();
    }

    poll_creation();

    if (m_start_ngx) {
        m_start_ngx = false;
        start_ngx();
    }

    if (m_session->creation_requested()) {
        create_feature();
    }
}

// Under m_policy_mutex, at Present.
void RayReconstruction::start_ngx() {
    const auto dir = rr_dir();
    const auto log_dir = dir / "ngx";

    // NGX may fail without a word if it cannot write its logs.
    std::error_code ec{};
    std::filesystem::create_directories(log_dir, ec);

    {
        // NGX's own D3D12 work never goes through the logic of the hooks.
        rr::d3d12::ReentryGuard guard{};
        m_session->start_ngx(g_framework->get_d3d12_hook()->get_device(), dir, log_dir);
    }

    spdlog::info("[RayReconstruction] NGX: {}: {}", rr::core::to_string(m_session->availability()), m_session->detail());
    tell_policy_bridge_state();
}

// Under m_policy_mutex, at Present.
void RayReconstruction::create_feature() {
    rr::d3d12::ReentryGuard guard{};

    if (!m_creation_set_up) {
        if (!m_creation.setup(L"RayReconstruction feature creation")) {
            return; // the request waits for the next Present
        }

        m_creation_set_up = true;
    } else if (m_creation.waiting_for_fence) {
        // The allocator of the last creation is reused once that creation has run.
        if (!creation_executed()) {
            return;
        }

        m_creation.wait(0);
    }

    const auto size = m_session->feature().size;

    // The creation releases the feature it replaces.
    wait_for_evaluations();

    if (!m_session->record_creation(m_creation.cmd_list.Get())) {
        discard_creation();
        spdlog::info("[RayReconstruction] Feature not created at {}x{}: {}: {}", size.width, size.height,
            rr::core::to_string(m_session->availability()), m_session->detail());
        tell_policy_bridge_state();
        return;
    }

    // NGX records on the list behind the context's back.
    m_creation.has_commands = true;
    m_creation.execute();
    spdlog::info("[RayReconstruction] Creating the feature at {}x{} (creations {}, releases {})", size.width, size.height,
        m_session->features_created(), m_session->features_released());
}

// Under m_policy_mutex. The list of a creation that failed or was rejected never runs: what it holds
// refers to a feature already released.
void RayReconstruction::discard_creation() {
    m_creation.cmd_list->Close();
    m_creation.cmd_allocator->Reset();
    m_creation.cmd_list->Reset(m_creation.cmd_allocator.Get(), nullptr);
    m_creation.has_commands = false;
}

// Under m_policy_mutex.
bool RayReconstruction::creation_executed() const {
    return m_creation.waiting_for_fence && m_creation.fence->GetCompletedValue() >= m_creation.fence_value;
}

// Under m_policy_mutex: once the fence has passed the last creation, the feature is ready.
void RayReconstruction::poll_creation() {
    const auto feature = m_session->feature();

    if (feature.status != rr::core::FeatureState::Status::Pending || m_session->creation_requested() || !creation_executed()) {
        return;
    }

    m_session->on_creation_executed();

    {
        // The list is open again for the next creation, and nothing waits on the context any more, not
        // even its destructor at the end of the process.
        rr::d3d12::ReentryGuard guard{};
        m_creation.wait(0);
    }

    const auto& snippet = m_session->loaded_snippet();
    spdlog::info("[RayReconstruction] Feature ready at {}x{}; RR snippet {}", feature.size.width, feature.size.height,
        snippet ? rr::core::to_string(*snippet) : std::string{"unknown"});
}

// Under m_policy_mutex. The GPU has to be done with the feature: with its creation, and with the game's
// lists that evaluated it.
void RayReconstruction::release_feature(std::string_view why) {
    const auto feature = m_session->feature();
    rr::d3d12::ReentryGuard guard{};

    if (m_creation.waiting_for_fence) {
        m_creation.wait(2000);
    }

    wait_for_evaluations();

    const auto released = m_session->features_released();
    m_session->release_feature();

    // A request not recorded yet has no feature behind it.
    if (m_session->features_released() > released) {
        spdlog::info("[RayReconstruction] Feature released at {}x{}: {}", feature.size.width, feature.size.height, why);
    } else if (feature.status != rr::core::FeatureState::Status::Absent) {
        spdlog::info("[RayReconstruction] Request for a feature at {}x{} dropped: {}", feature.size.width, feature.size.height, why);
    }
}

// Under m_policy_mutex, before NGX frees a feature RR evaluated: no list that evaluated it may be in flight
// (DLSS_Programming_Guide_Release.pdf of the SDK 310.9.16, section 5.5, p. 54). The game's lists that
// evaluated it run on the queue REFramework captures, the swapchain's, like the TAA's list: a fence
// signaled on that queue passes once the GPU is done with every list submitted before. A list not submitted
// yet is not covered, but every release comes at Present, after the game submitted the frame's lists, or at
// a device reset, between frames. A fence of its own, not a d3d12::CommandContext: that one signals only
// after executing a list, and its wait does not say whether it timed out.
void RayReconstruction::wait_for_evaluations() {
    if (!m_evaluated_since_wait) {
        return;
    }

    m_evaluated_since_wait = false;

    const auto& hook = g_framework->get_d3d12_hook();
    const auto queue = hook != nullptr ? hook->get_command_queue() : nullptr;
    const auto device = hook != nullptr ? hook->get_device() : nullptr;

    if (queue == nullptr || device == nullptr) {
        spdlog::error("[RayReconstruction] No command queue to wait for the evaluations on: the feature goes anyway");
        return;
    }

    const rr::d3d12::ReentryGuard guard{};

    if (m_evaluation_fence == nullptr) {
        if (const auto hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_evaluation_fence)); FAILED(hr)) {
            spdlog::error("[RayReconstruction] CreateFence failed ({:x}): the feature goes without waiting for the evaluations",
                static_cast<std::uint32_t>(hr));
            return;
        }

        m_evaluation_fence->SetName(L"RayReconstruction evaluation fence");
        m_evaluation_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    if (const auto hr = queue->Signal(m_evaluation_fence.Get(), ++m_evaluation_fence_value); FAILED(hr)) {
        spdlog::error("[RayReconstruction] Signal failed ({:x}): the feature goes without waiting for the evaluations",
            static_cast<std::uint32_t>(hr));
        return;
    }

    if (m_evaluation_fence->GetCompletedValue() >= m_evaluation_fence_value) {
        return;
    }

    if (m_evaluation_event == nullptr || FAILED(m_evaluation_fence->SetEventOnCompletion(m_evaluation_fence_value, m_evaluation_event)) ||
        WaitForSingleObject(m_evaluation_event, 2000) != WAIT_OBJECT_0) {
        spdlog::error("[RayReconstruction] The GPU did not finish the evaluations in time: the feature goes anyway");
    }
}

// Under m_policy_mutex.
void RayReconstruction::tell_policy_bridge_state() {
    if (m_policy->on_bridge_state(m_session->state()) == rr::core::FeatureAction::Release) {
        release_feature("the bridge is no longer usable");
    }
}

void RayReconstruction::on_draw_ui() {
    ImGui::SetNextItemOpen(false, ImGuiCond_::ImGuiCond_FirstUseEver);
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    if (m_enabled->draw("Enabled")) {
        on_enabled_changed("the interface");
    }

    if (m_switch_key->draw("Switch key")) {
        g_framework->request_save_config();
    }

    draw_bridge_ui();
    attribution();
    draw_frame_ui();
    draw_cuts_ui();
    draw_guides_ui();
    draw_jitter_ui();
    draw_states_ui();
    draw_measurement_ui();
    draw_hooks_ui();
}

void RayReconstruction::draw_bridge_ui() {
    // A copy, so that the lock is not held while drawing.
    BridgeView b{};

    {
        std::scoped_lock _{m_policy_mutex};

        if (m_session == nullptr) {
            return;
        }

        b = {m_session->availability(), m_session->detail(), m_session->loaded_snippet(), m_session->feature(),
            m_session->features_created(), m_session->features_released(), m_session->evaluations()};
    }

    const auto availability = rr::core::to_string(b.availability);
    ImGui::Text("Availability: %.*s", static_cast<int>(availability.size()), availability.data());

    if (!b.detail.empty()) {
        ImGui::TextWrapped("%s", b.detail.c_str());
    }

    // The one that evaluates, so that a report says which one ran.
    if (b.snippet) {
        ImGui::TextWrapped("RR snippet: %s", rr::core::to_string(*b.snippet).c_str());
    }

    text(std::format(
        "Feature: {} | created {}, released {} | evaluations recorded {}", feature_text(b.feature), b.created, b.released, b.evaluations));
}

void RayReconstruction::draw_frame_ui() {
    if (m_profile == nullptr) {
        return;
    }

    // A copy, so that the lock is not held while drawing.
    Frames f{};

    {
        std::scoped_lock _{m_policy_mutex};
        f = m_frames;
    }

    ImGui::Separator();

    if (!f.last) {
        text("Frame: none closed yet");
    } else {
        text(std::format("Frame: {}", f.last->skip ? "omitted, " + reason_text(f.last->skip, "") : std::string{"evaluated"}));
        text(std::format("Identification: {}", reason_text(f.last->identification, "identified")));
    }

    text(std::format("RR at the last evaluation point: {}", f.evaluation));

    const auto& c = f.counters;

    if (const auto n = c.not_identified[static_cast<std::size_t>(rr::core::SkipReason::DigestMismatch)]; n > 0) {
        warning(std::format("No PSO carries the profile's TAA vertex shader {} ({} frames): the game may have changed. Digest discovery "
                            "logs the new digests.",
            rr::core::to_string(m_profile->taa_vs), n));
    }

    if (f.last_draw && !f.last_draw->point.t_profile_format) {
        warning(std::format("T has format {}, and the profile says {}.", static_cast<std::uint32_t>(f.last_draw->anchor.t_desc.format),
            static_cast<std::uint32_t>(m_profile->formats.t)));
    }

    if (f.unknown_ps) {
        warning(std::format("The pixel shader {} of the evaluation point is not in the profile.", rr::core::to_string(*f.unknown_ps)));
    }

    // The game's settings the profile checks, and the PT settings, which are never a warning.
    text("Game settings: " + rr::core::pt_settings_text(f.settings));

    for (const auto& setting : f.setting_warnings) {
        warning(setting.text + ".");
    }

    // The measurement interface's counters too, but not the mean of the accumulated reference.
    if (ImGui::Button("Reset counters")) {
        std::scoped_lock _{m_policy_mutex};
        m_frames.counters = {};
        m_frames.anchors_per_frame = {};
        m_frames.o_rt_uav = 0;
        m_frames.identified_by_ps.clear();
        m_frames.unknown_ps.reset();
        m_frames.jitter = {};

        auto& m = m_frames.measures;

        for (auto& timer : m.timers) {
            timer.reset();
        }

        m.intervals.reset();
        m.nonfinite.reset();
        m.run_start.reset();
        m.run_frames = 0;
        m_frames.reference.restarts = {};

        m_frames.cuts = {};

        // A run in progress starts again at the next omitted frame.
        m_frames.runs = {};
        m_frames.last_run.reset();
        m_frames.counters_frame = m_hooks_frame;
        m_frames.counters_time = std::chrono::steady_clock::now();
        spdlog::info("[RayReconstruction] Counters reset at frame {}", m_hooks_frame);
    }

    std::vector<std::string> lines{};
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - f.counters_time).count();
    lines.push_back(std::format("Counters since frame {}, {} min {:02} s ago | device {}", f.counters_frame, since / 60, since % 60,
        f.device_removed ? std::format("removed, reason {:x}", static_cast<std::uint32_t>(*f.device_removed))
                         : std::string{"not removed"}));
    lines.push_back(std::format(
        "Frames closed: {} | evaluated {} | identified {} | identified with O RT+UAV {}", c.frames, c.evaluated, c.identified, f.o_rt_uav));

    const auto& cam = f.camera;
    lines.push_back(std::format("Camera updates: {} (last {}) | frames without one {} | main layer changes {} | updates of other layers {}",
        cam.updates, cam.last_update, cam.frames_without_update, cam.main_layer_changes, cam.other_layer_updates));
    lines.push_back(std::format("Frames with 0 / 1 / 2 or more anchored draws: {} / {} / {}", f.anchors_per_frame[0],
        f.anchors_per_frame[1], f.anchors_per_frame[2]));
    lines.push_back(std::format("History resets: {} {} | {} {} | {} {}", rr::core::to_string(rr::core::ResetCause::AfterOmittedFrame),
        c.resets[static_cast<std::size_t>(rr::core::ResetCause::AfterOmittedFrame)],
        rr::core::to_string(rr::core::ResetCause::FeatureRecreated),
        c.resets[static_cast<std::size_t>(rr::core::ResetCause::FeatureRecreated)], rr::core::to_string(rr::core::ResetCause::CameraCut),
        c.resets[static_cast<std::size_t>(rr::core::ResetCause::CameraCut)]));
    lines.push_back(std::format("Runs of omitted frames: {} | in progress: {} | last ended: {}", f.runs.count(),
        f.runs.current() ? run_text(*f.runs.current()) : std::string{"none"},
        f.last_run ? std::format("at frame {}, {}", f.last_run_end, ended_run_text(*f.last_run)) : std::string{"none"}));

    // The runs that kept RR's history, and the others by the condition that broke them.
    std::string reset_by{};

    for (std::size_t i = 0; i < rr::core::HISTORY_BREAK_COUNT; ++i) {
        reset_by += std::format(
            "{}{} {}", reset_by.empty() ? "" : ", ", rr::core::to_string(static_cast<rr::core::HistoryBreak>(i)), f.runs.broken_by()[i]);
    }

    lines.push_back(std::format("Runs ended that kept RR's history: {} | history reset by: {}", f.runs.kept(), reset_by));

    // The frames without velocity evaluated, with the camera's reprojection and with a reset after a camera
    // cut, and the others by outcome.
    std::string no_velocity{};

    for (std::size_t i = 0; i < rr::core::NO_VELOCITY_OUTCOME_COUNT; ++i) {
        if (const auto outcome = static_cast<rr::core::NoVelocityOutcome>(i); !rr::core::is_evaluated(outcome) && c.no_velocity[i] > 0) {
            no_velocity += std::format("{}{} {}", no_velocity.empty() ? "" : ", ", rr::core::to_string(outcome), c.no_velocity[i]);
        }
    }

    lines.push_back(std::format("Frames without velocity: {} {} | {} {} | omitted: {}",
        rr::core::to_string(rr::core::NoVelocityOutcome::Evaluated),
        c.no_velocity[static_cast<std::size_t>(rr::core::NoVelocityOutcome::Evaluated)],
        rr::core::to_string(rr::core::NoVelocityOutcome::EvaluatedWithReset),
        c.no_velocity[static_cast<std::size_t>(rr::core::NoVelocityOutcome::EvaluatedWithReset)],
        no_velocity.empty() ? std::string{"none"} : no_velocity));

    // The frames evaluated with the guides of an earlier frame; the log has each run once it ends.
    const auto& reused = f.reused_passes.current();
    lines.push_back(std::format("Frames evaluated without a G-buffer pass of their own: {} | run in progress: {}", c.reused_gbuffer_pass,
        reused ? reused_run_text(*reused) : std::string{"none"}));

    ImGui::SameLine();

    // The table goes to the log as a line of its own.
    if (ImGui::Button("Write to the log")) {
        for (const auto& line : lines) {
            spdlog::info("[RayReconstruction] Frames: {}", line);
        }

        std::string omitted{};

        for (std::size_t i = 0; i < rr::core::SKIP_REASON_COUNT; ++i) {
            omitted += std::format("{}{} {} ({} not identified)", omitted.empty() ? "" : ", ",
                rr::core::to_string(static_cast<rr::core::SkipReason>(i)), c.omitted[i], c.not_identified[i]);
        }

        spdlog::info("[RayReconstruction] Frames omitted by reason: {}", omitted);
    }

    for (const auto& line : lines) {
        text(line);
    }

    for (const auto& [ps, n] : f.identified_by_ps) {
        ImGui::BulletText("Identified with pixel shader %s: %llu", rr::core::to_string(ps).c_str(), static_cast<unsigned long long>(n));
    }

    if (ImGui::BeginTable("##rr_reasons", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Reason");
        ImGui::TableSetupColumn("Frames omitted");
        ImGui::TableSetupColumn("Identification");
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < rr::core::SKIP_REASON_COUNT; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(std::string{rr::core::to_string(static_cast<rr::core::SkipReason>(i))});
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(c.omitted[i]));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(c.not_identified[i]));
        }

        ImGui::EndTable();
    }

    if (!f.last_draw) {
        text("Last evaluation point: none yet");
        return;
    }

    const auto& d = *f.last_draw;
    const auto& t = d.anchor.t_desc;
    ImGui::Text("Last evaluation point: frame %llu, thread %lu, list %p", static_cast<unsigned long long>(d.frame), d.thread, d.list);
    ImGui::BulletText("Decision: %s | identification: %s", reason_text(f.decision.skip, "evaluate").c_str(),
        reason_text(f.decision.identification, "identified").c_str());

    if (f.decision.no_velocity) {
        ImGui::BulletText("Frame without velocity: %s", std::string{rr::core::to_string(*f.decision.no_velocity)}.c_str());
    }
    ImGui::BulletText("Vertex shader %s (%s)", rr::d3d12::to_string(d.shaders.vs).c_str(),
        d.point.vs == m_profile->taa_vs ? "the profile's TAA" : "not the profile's TAA");
    ImGui::BulletText("Pixel shader %s (%s)", rr::d3d12::to_string(d.shaders.ps).c_str(), rr::d3d12::to_string(d.ps_role));
    ImGui::BulletText("PT compute shader %s (%s)", rr::d3d12::to_string(d.anchor.pt_cs).c_str(),
        d.anchor.pt_cs == m_profile->pt_cs ? "the profile's" : "not the profile's");
    ImGui::BulletText("T: %ux%u, format %u (%s), %s", t.size.width, t.size.height, static_cast<std::uint32_t>(t.format),
        d.point.t_profile_format ? "the profile's" : "not the profile's", flags_text(t.flags).c_str());

    const auto o = std::string{rr::core::to_string(d.o_problem)};

    if (!d.rt0.state) {
        ImGui::BulletText("O: %s", o.c_str());
    } else {
        const auto& rt0 = d.rt0.desc;
        ImGui::BulletText("O: %s | RT0 %ux%u, format %u, %s, state %x", o.c_str(), rt0.size.width, rt0.size.height,
            static_cast<std::uint32_t>(rt0.format), flags_text(rt0.flags).c_str(), *d.rt0.state);
    }

    const auto& st = d.states;
    const auto problem = std::string{rr::core::to_string(st.problem)};
    ImGui::BulletText("Resource states: %s | G-buffer pass %llu, %s at the draw", problem.c_str(),
        static_cast<unsigned long long>(st.gbuffer_pass), st.gbuffer_submitted ? "submitted" : "not submitted");
    ImGui::BulletText("At the draw: %s", rr::d3d12::to_string(st.predicted).c_str());

    const auto& lut = d.ambient_brdf.lut;

    if (lut.location == rr::core::LutLocation::Located) {
        ImGui::BulletText("AmbientBRDF LUT: located at root parameter %u, descriptor %u of its table", lut.parameter, lut.offset);
    } else {
        ImGui::BulletText("AmbientBRDF LUT: %s", rr::core::to_string(lut.location));
    }
}

// The cut detector, for its checks in the validation pass: how close the updates without a cut came to
// the thresholds, and the camera pushes. "Reset counters" sets it to 0 too.
void RayReconstruction::draw_cuts_ui() {
    if (m_profile == nullptr || !ImGui::TreeNode("Cut detector")) {
        return;
    }

    CutView v{};

    {
        std::scoped_lock _{m_policy_mutex};
        v = m_frames.cuts;
    }

    ImGui::TextWrapped("Every camera update of the main layer is set against a constant-velocity prediction, also with RR off. A "
                       "camera push is an update that only the pivot would make a cut, with its residual along the view's axis and "
                       "without a turn or a zoom, as the camera against a wall: it resets nothing. A near miss is an update without a "
                       "cut or a push where a residual went over half its threshold. Cuts, pushes and near misses go to the log with "
                       "their frame, residuals and fraction along the axis; outside the cinematics and the screen transitions, a cut "
                       "is a false positive.");

    const auto& t = m_profile->cuts;
    const auto& s = v.stats;
    const auto percent = [](double residual, float threshold) { return 100.0 * residual / static_cast<double>(threshold); };
    const auto last = [](const std::optional<std::pair<std::uint64_t, rr::core::CutCheck>>& c) {
        return c ? std::format("at frame {}, {}", c->first, check_text(c->second)) : std::string{"none"};
    };

    std::vector<std::string> lines{};
    lines.push_back(
        std::format("Thresholds of the profile: pivot {:.2f} m, at {:.2f} m in front of the camera | rotation {:.1f} deg | focal "
                    "{:.1f} %",
            t.pivot_residual, t.pivot_distance, t.rotation_residual, 100.0f * t.focal_residual));
    lines.push_back(std::format("Camera push: {:.0f} % of the pivot residual along the view's axis or more, rotation {:.1f} deg and "
                                "focal {:.1f} % at most",
        100.0f * t.push_axis_fraction, t.push_rotation_residual, 100.0f * t.push_focal_residual));
    lines.push_back(
        std::format("Updates checked: {} | cuts {} | pushes {} | near misses {}", s.checked, s.cuts, s.pushes, s.near_misses));
    lines.push_back(
        std::format("Largest residuals without a cut or a push: {} | of the thresholds: pivot {:.0f} %, rotation {:.0f} %, focal "
                    "{:.0f} %",
            residuals_text(s.peak), percent(s.peak.pivot, t.pivot_residual), percent(s.peak.rotation, t.rotation_residual),
            percent(s.peak.focal, t.focal_residual)));
    lines.push_back(std::format("Last cut: {}", last(v.last_cut)));
    lines.push_back(std::format("Last push: {}", last(v.last_push)));
    lines.push_back(std::format("Last near miss: {}", last(v.last_near_miss)));

    if (ImGui::Button("Write to the log")) {
        for (const auto& line : lines) {
            spdlog::info("[RayReconstruction] Cut detector: {}", line);
        }
    }

    for (const auto& line : lines) {
        ImGui::TextWrapped("%s", line.c_str());
    }

    ImGui::TreePop();
}

void RayReconstruction::draw_guides_ui() {
    if (!ImGui::TreeNode("Guide pass and debug view")) {
        return;
    }

    auto changed = m_debug_view->draw("Debug view");
    changed |= m_debug_scale->draw("Debug view scale");
    changed |= m_barrier_round_trip->draw("Barrier round trip of the reads");
    changed |= m_reproject_saturated->draw("Reproject the saturated velocity");
    changed |= m_specular_source->draw("Specular albedo");

    std::string status{};
    std::string lut_check{};
    bool use_lut{};

    {
        std::scoped_lock _{m_policy_mutex};

        if (changed) {
            read_debug_options();
        }

        status = m_frames.debug_view;
        lut_check = m_frames.lut_check;
        use_lut = m_debug.use_lut;
    }

    if (changed) {
        g_framework->request_save_config();
    }

    ImGui::TextWrapped("The view writes a guide into O in place of the game's TAA and of RR, in identified frames, with RR off "
                       "too. It is not saved. O goes through the exposure and the tonemapping, so the scale sets its brightness: "
                       "about 1 for the normals and the albedos, 0.05 for the MVs, which are in pixels, and a few hundred for the "
                       "depth. MVs: red to the right, blue to the left, green the vertical length.");
    ImGui::TextWrapped("In fast turns the game's velocity saturates at half the screen, and the pass redoes those pixels by "
                       "reprojection. To compare with the switch off, a scale of about 0.001 leaves half the screen (960 px at 1920 "
                       "wide) at a medium brightness: off, the saturated pixels are flat red or blue; on, they keep changing across "
                       "the screen.");

    const auto s = m_guide_pass.status();
    text(std::format("Last evaluation point: {}", status));
    text(std::format("Guide pass: {} | textures {}x{}, {} sets created | recorded {}", s.setup, s.textures.width, s.textures.height,
        s.texture_sets, s.recorded));

    ImGui::Separator();

    if (m_profile == nullptr) {
        ImGui::TreePop();
        return;
    }

    const auto& lut = m_profile->ambient_brdf;
    ImGui::TextWrapped("AmbientBRDF LUT (the PT's t%u, %ux%u): %s | specular albedo from %s | recordings that copied it: %llu",
        lut.srv_register, lut.size.width, lut.size.height, s.lut.c_str(), use_lut ? "the LUT where it is found" : "the GGX fit",
        static_cast<unsigned long long>(s.lut_copies));

    // The copy runs only while the guide pass records: for the debug view, or for RR's evaluation.
    if (ImGui::Button("Check the LUT's axis")) {
        m_guide_pass.request_lut_readback();
    }

    ImGui::SameLine();
    ImGui::TextWrapped("with the debug view on or RR evaluated, and the LUT as the specular albedo's source. Readback: %s",
        s.readback.c_str());
    ImGui::TextWrapped("Axis check: %s", lut_check.c_str());

    ImGui::TreePop();
}

// The own jitter: its options, and what its checks in the game read. "Reset counters" sets its counters
// to 0 too.
void RayReconstruction::draw_jitter_ui() {
    if (m_profile == nullptr || !ImGui::TreeNode("Own jitter")) {
        return;
    }

    auto saved = m_own_jitter->draw("Inject RR's own jitter");
    saved |= m_follow_inverses->draw("Give the injected inverses to");
    const auto changed = m_jitter_offset_sign->draw("Sign of InJitterOffset") || saved;

    JitterView v{};
    std::optional<rr::core::CameraConstants> camera{};

    {
        std::scoped_lock _{m_policy_mutex};

        if (changed) {
            read_jitter_options();
        }

        v = m_frames.jitter;
        camera = m_frames.decision.camera;
    }

    if (saved) {
        g_framework->request_save_config();
    }

    ImGui::TextWrapped("RR's own jitter, a Halton(2, 3) of 32 phases over +-0.5 px, goes into the game only while its TAA is "
                       "replaced, by RR or by the debug view, so turn RR on first. With the camera still, the debug view of the "
                       "MVs gives 0 if the game's velocity follows the injection. The sign of InJitterOffset changes only what "
                       "goes to the bridge, never the MVs, and is not saved.");

    std::vector<std::string> lines{};
    lines.push_back(std::format("Last camera update: {}", v.last_injected ? "RR's own jitter" : "the game's jitter"));

    if (camera) {
        lines.push_back(std::format("j at the last evaluation point: ({:.4f}, {:.4f}) px", camera->jitter.x, camera->jitter.y));
    }

    if (v.offset_sent) {
        lines.push_back(
            std::format("InJitterOffset at the last evaluation recorded: ({:.4f}, {:.4f}) px", v.offset_sent->x, v.offset_sent->y));
    }

    std::string variants{};

    for (const auto& jitter_variant : m_profile->jitter_variants) {
        const auto i = static_cast<std::size_t>(jitter_variant.variant);
        variants += std::format("{}{} {}", variants.empty() ? "" : ", ", rr::core::to_string(jitter_variant.variant), v.written[i]);

        if (jitter_variant.view_projection_copy) {
            variants += std::format(" (copy at float {} rewritten in {})", rr::core::VIEW_PROJECTION_COPY_FLOAT, v.copies[i]);
        }

        if (v.missing[i] > 0) {
            variants += std::format(" (missing in {})", v.missing[i]);
        }
    }

    lines.push_back(std::format("Camera updates injected: {} | variants written: {}", v.injected, variants));
    lines.push_back(std::format(
        "Camera updates before / after the frame's evaluation point, in their Present interval: {} / {}", v.before_point, v.after_point));
    lines.push_back(std::format("Inverses equal to Scene's, of {} updates: JitterDisable {}, JitterDisablePost {} (missing {}) | "
                                "JitterDisablePost with jitter {} | inverses given: JitterDisable {}, JitterDisablePost {}",
        v.compared, v.jitter_disable_same, v.post_same, v.post_missing, v.post_jittered, v.jitter_disable_followed, v.post_followed));

    if (ImGui::Button("Write to the log")) {
        for (const auto& line : lines) {
            spdlog::info("[RayReconstruction] Own jitter: {}", line);
        }
    }

    for (const auto& line : lines) {
        ImGui::TextWrapped("%s", line.c_str());
    }

    ImGui::TreePop();
}

// The measurement interface: the GPU timers, the count of non-finite values in O, the accumulated
// reference and the game's settings read. And RR's net cost and the intervals between Presents. "Reset
// counters" sets its counters to 0 too.
void RayReconstruction::draw_measurement_ui() {
    if (m_profile == nullptr || !ImGui::TreeNode("Measurement")) {
        return;
    }

    auto changed = m_accumulated_reference->draw("Accumulated reference");
    changed |= m_force_nonfinite->draw("Force a non-finite pixel in the count");

    MeasurementView m{};
    ReferenceView r{};
    rr::core::GameSettings g{};

    {
        std::scoped_lock _{m_policy_mutex};

        if (changed) {
            read_measurement_options();
        }

        m = m_frames.measures;
        r = m_frames.reference;
        g = m_frames.settings;
    }

    ImGui::TextWrapped("The timers time each part of RR's work on the TAA's list with timestamps, read back 16 frames later, and "
                       "the game's TAA draw in the identified frames where it draws, once the guide pass is set up. RR's net cost is "
                       "the guide pass and the evaluation, less the TAA draw it omits, each the mean of its last %zu samples: in "
                       "each scene, with the camera still, a few seconds with RR off and as many with RR on.",
        rr::core::GPU_TIMER_WINDOW);
    ImGui::TextWrapped("The intervals between Presents go by the mode of their frame: RR on, RR off, or the debug view or the "
                       "reference. With VSync at 60 Hz, one longer than %.1f ms is a drop below 60 fps.",
        rr::core::FRAME_DROP_MS);
    ImGui::TextWrapped("The accumulated reference writes into O, in place of the TAA and of RR, the mean of T since the camera "
                       "last moved: keep it still for a few hundred frames. The debug view wins over it, and it is not saved. The "
                       "count of non-finite pixels in O runs after every evaluation of RR; the forced pixel goes to the count alone, "
                       "never into O.");

    std::vector<std::string> lines{};
    const auto s = m_guide_pass.status();
    lines.push_back(std::format("Timestamps: {} | frequency {} Hz | measurements read back {} | TAA draws timed {}", s.measurement,
        m.frequency, m.taken, s.timed_taa_draws));

    for (std::size_t i = 0; i < rr::d3d12::SEGMENT_COUNT; ++i) {
        const auto t = m.timers[i].stats();
        lines.push_back(std::format("{}: last {:.3f} ms, mean of the last {} {:.3f} ms, mean {:.3f} ms, max {:.3f} ms ({} samples, {} dropped)",
            rr::d3d12::to_string(static_cast<rr::d3d12::Segment>(i)), t.last_ms, rr::core::GPU_TIMER_WINDOW, t.window_ms, t.mean_ms,
            t.max_ms, t.samples, t.dropped));
    }

    const auto timer = [&](rr::d3d12::Segment segment) { return m.timers[static_cast<std::size_t>(segment)].stats(); };
    const auto cost =
        rr::core::net_cost(timer(rr::d3d12::Segment::GuidePass), timer(rr::d3d12::Segment::Evaluation), timer(rr::d3d12::Segment::TaaDraw));

    if (cost) {
        lines.push_back(std::format(
            "Net cost, means of the last {}: RR {:.3f} ms less the TAA draw {:.3f} ms = {:.3f} ms, {} the threshold of {:.0f} ms",
            rr::core::GPU_TIMER_WINDOW, cost->rr_ms, cost->taa_ms, cost->net_ms, cost->over_threshold ? "over" : "within",
            rr::core::NET_COST_THRESHOLD_MS));
    } else {
        lines.push_back("Net cost: it needs samples of the guide pass, of RR's evaluation and of the game's TAA draw");
    }

    std::string intervals{};

    for (std::size_t i = 0; i < rr::core::FRAME_MODE_COUNT; ++i) {
        const auto mode = static_cast<rr::core::FrameMode>(i);
        const auto f = m.intervals.stats(mode);
        intervals += std::format("{}{}: {} frames, mean {:.2f} ms, max {:.2f} ms, {} drops", intervals.empty() ? "" : " | ",
            rr::core::to_string(mode), f.frames, f.mean_ms, f.max_ms, f.drops);
    }

    lines.push_back("Intervals between Presents: " + intervals);

    const auto& c = m.nonfinite.counts();
    lines.push_back(std::format("Non-finite pixels in O after RR: {} frames counted, {} with some | NaN {}, infinite {} | at most {} in a "
                                "frame | last in frame {}",
        c.frames, c.frames_with_nonfinite, c.nan_pixels, c.inf_pixels, c.max_per_frame,
        c.last_frame_with_nonfinite ? std::to_string(*c.last_frame_with_nonfinite) : std::string{"none"}));

    std::string restarts{};

    for (std::size_t i = 0; i < rr::core::REFERENCE_RESTART_COUNT; ++i) {
        restarts += std::format("{}{} {}", restarts.empty() ? "" : ", ", rr::core::to_string(static_cast<rr::core::ReferenceRestart>(i)),
            r.restarts[i]);
    }

    lines.push_back(std::format("Accumulated reference: {} | recorded {} | last new mean: {} | new means: {}", r.status, s.references,
        r.last_restart ? std::format("{} at frame {}", rr::core::to_string(*r.last_restart), r.last_restart_frame) : std::string{"none"},
        restarts));

    lines.push_back(std::format("Game settings read: Ray Tracing Tweaks {} | Ray Trace Type {} | Clone Types Pre {}, Post {}, True {} | "
                                "Samples Per Pixel {} | Bounce Count {} | anti-aliasing {}",
        g.ray_tracing_tweaks ? "on" : "off", g.ray_trace_type, g.clone_types[0], g.clone_types[1], g.clone_types[2], g.spp, g.bounces,
        g.anti_aliasing.value_or("not read")));

    if (ImGui::Button("Write to the log")) {
        for (const auto& line : lines) {
            spdlog::info("[RayReconstruction] Measurement: {}", line);
        }
    }

    for (const auto& line : lines) {
        ImGui::TextWrapped("%s", line.c_str());
    }

    ImGui::TreePop();
}

void RayReconstruction::draw_states_ui() {
    if (!ImGui::TreeNode("Resource states and G-buffer")) {
        return;
    }

    const auto lines = states_summary(rr::d3d12::status());

    if (ImGui::Button("Write to the log")) {
        for (const auto& line : lines) {
            spdlog::info("[RayReconstruction] {}", line);
        }
    }

    ImGui::SameLine();

    // What the registry learned stays.
    if (ImGui::Button("Reset state counters")) {
        rr::d3d12::reset_state_counters();
    }

    for (const auto& line : lines) {
        ImGui::TextWrapped("%s", line.c_str());
    }

    ImGui::TreePop();
}

void RayReconstruction::draw_hooks_ui() {
    if (!ImGui::TreeNode("D3D12 hooks")) {
        return;
    }

    if (m_digest_discovery->draw("Digest discovery (log)")) {
        if (const auto registry = rr::d3d12::pso_registry(); registry != nullptr) {
            registry->set_discovery(m_digest_discovery->value());
        }

        g_framework->request_save_config();
    }

    const auto s = rr::d3d12::status();

    ImGui::TextWrapped("D3D12CreateDevice: %s", s.create_device.c_str());
    ImGui::TextWrapped("Device: %s", s.device.c_str());
    ImGui::TextWrapped("Command list vtables: %s", s.list_vtables.c_str());
    ImGui::TextWrapped("Debug layer: %s", s.debug_layer.c_str());

    if (s.debug_messages > 0) {
        ImGui::Text("Debug layer messages: %llu | errors %llu | naming the Mod %llu", static_cast<unsigned long long>(s.debug_messages),
            static_cast<unsigned long long>(s.debug_errors), static_cast<unsigned long long>(s.debug_messages_naming_mod));
    }

    ImGui::Separator();

    const auto& psos = s.psos;
    ImGui::Text("PSOs with the profile's TAA vertex shader: %llu distinct pixel shaders, %llu created",
        static_cast<unsigned long long>(psos.taa_vs_ps), static_cast<unsigned long long>(psos.taa_vs));
    ImGui::Text("PSOs with the profile's PT compute shader: %llu created", static_cast<unsigned long long>(psos.pt_cs));
    ImGui::Text("PSOs with a shader without a HASH part: %llu | streams not parsed: %llu",
        static_cast<unsigned long long>(psos.without_digest), static_cast<unsigned long long>(psos.stream_parse_failures));
    ImGui::Text("Pipeline libraries: %llu (failed %llu) | views known: %llu", static_cast<unsigned long long>(psos.libraries),
        static_cast<unsigned long long>(psos.library_failures), static_cast<unsigned long long>(s.views));
    ImGui::Text("Root signatures read: %llu | not read: %llu", static_cast<unsigned long long>(s.root_signatures),
        static_cast<unsigned long long>(s.root_signature_failures));

    // Each PT setting is another variant: the digest is a diagnostic, and each variant anchored goes to
    // reframework/rr/ for dxc -dumpbin.
    const auto& pt = s.pt_cs;
    ImGui::Text("PT compute shader at the last anchored draw: %s", pt.last ? rr::core::to_string(*pt.last).c_str() : "none yet");
    ImGui::Text("Compute shaders kept until an anchor names the PT's: %llu, %llu KB | over the limit %llu | root signature not read %llu | "
                "anchors without a digest %llu",
        static_cast<unsigned long long>(pt.retained), static_cast<unsigned long long>(pt.retained_bytes / 1024),
        static_cast<unsigned long long>(pt.over_limit), static_cast<unsigned long long>(pt.without_root_signature),
        static_cast<unsigned long long>(pt.anchors_without_digest));

    for (const auto& v : pt.variants) {
        ImGui::BulletText("PT compute shader %s (%s): %s", rr::core::to_string(v.digest).c_str(),
            m_profile != nullptr && v.digest == m_profile->pt_cs ? "the profile's" : "another variant", rr::d3d12::to_string(v.dump));
    }

    for (std::size_t i = 0; i < psos.by_source.size(); ++i) {
        ImGui::BulletText(
            "%s: %llu", rr::d3d12::to_string(static_cast<rr::d3d12::PsoSource>(i)), static_cast<unsigned long long>(psos.by_source[i]));
    }

    ImGui::Separator();
    ImGui::Text("Frames: %llu", static_cast<unsigned long long>(s.frames));

    if (ImGui::BeginTable("##rr_counters", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Seen by the hooks");
        ImGui::TableSetupColumn("Total");
        ImGui::TableSetupColumn("Last frame");
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < s.total.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rr::d3d12::to_string(static_cast<rr::d3d12::Counter>(i)));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(s.total[i]));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(s.last_frame[i]));
        }

        ImGui::EndTable();
    }

    ImGui::Separator();

    if (!s.taa_draw) {
        ImGui::TextUnformatted("Last draw with the TAA vertex shader: none yet");
    } else {
        const auto& d = *s.taa_draw;
        const auto ps = rr::d3d12::to_string(d.shaders.ps);
        ImGui::Text("Last draw with the TAA vertex shader: frame %llu, thread %lu", static_cast<unsigned long long>(d.frame), d.thread);
        ImGui::BulletText("%s %u x %u on list %p (type %d)", d.indexed ? "DrawIndexedInstanced" : "DrawInstanced", d.count, d.instances,
            d.list, static_cast<int>(d.list_type));
        ImGui::BulletText("PSO %p, pixel shader %s (%s)", as_void(d.pso), ps.c_str(), rr::d3d12::to_string(d.ps_role));
        ImGui::BulletText("Descriptor heaps (%u): %p %p", d.heap_count, as_void(d.heaps[0]), as_void(d.heaps[1]));
        ImGui::BulletText("Graphics root signature %p, %d root arguments set (mask %llx)", as_void(d.root_signature),
            std::popcount(d.root_arguments), static_cast<unsigned long long>(d.root_arguments));
        ImGui::BulletText("Render targets: %u, RT0 %p", d.rtv_count, as_void(d.rt0));
    }

    ImGui::TreePop();
}

void RayReconstruction::on_config_load(const utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_load(cfg);
    }

    if (const auto registry = rr::d3d12::pso_registry(); registry != nullptr) {
        registry->set_discovery(m_digest_discovery->value());
    }

    std::scoped_lock _{m_policy_mutex};
    read_debug_options();
    read_jitter_options();

    if (m_policy != nullptr) {
        m_policy->on_toggle(m_enabled->value());
    }
}

void RayReconstruction::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}
