#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "D3D12Hooks.hpp"
#include "core/FramePolicy.hpp"
#include "core/Measurements.hpp"
#include "core/ResourceStates.hpp"

namespace rr::d3d12 {
// What the guide pass reads at one evaluation point.
struct GuideInputs {
    ID3D12GraphicsCommandList* list{}; // the TAA's
    core::GBuffer gbuffer{};           // the one the anchored draw used, which the hooks keep alive
    core::Extent size{};               // T's: the render size
    core::CameraConstants camera{};    // from the decision at that draw

    // Where the registry predicted the reads at the draw. With round_trip, the pass declares their
    // transition to NON_PIXEL_SHADER_RESOURCE and back, so that the debug layer checks the states the
    // registry deduced.
    core::ReadStates states{};
    bool round_trip{};

    // Whether the pass redoes the saturated velocity by reprojection. Off, for the comparison of the
    // validation protocol, the MVs keep the game's velocity everywhere.
    bool reproject_saturated{true};

    // Whether the TAA reads the game's velocity. In a frame without velocity it does not: every MV comes
    // from the camera's reprojection, whatever reproject_saturated says, and the pass binds a null
    // descriptor in place of the G-buffer's velocity, which it neither reads nor transitions.
    bool has_velocity{true};

    // The game's AmbientBRDF LUT. With use_lut and the LUT located, the pass copies it through the PT's
    // descriptor and samples the copy; a texture of another size than lut_size, the profile's, is not the
    // LUT. Otherwise, and in every pixel then, the specular albedo comes from the GGX fit, env_brdf_fit.
    AmbientBrdfDescriptor ambient_brdf{};
    core::Extent lut_size{};
    bool use_lut{};

    std::uint64_t frame{}; // the hooks' frame of the draw
};

// RR's own textures, which the guide pass writes.
struct Guides {
    ID3D12Resource* normals_roughness{};
    ID3D12Resource* diffuse_albedo{};
    ID3D12Resource* specular_albedo{};
    ID3D12Resource* depth{};
    ID3D12Resource* motion_vectors{};
};

// What the accumulated reference records at an evaluation point.
struct ReferenceInputs {
    ID3D12GraphicsCommandList* list{}; // the TAA's
    ID3D12Resource* t{};               // read in its state at the draw, which a compute pass can read
    ID3D12Resource* o{};
    core::Extent size{};               // T's, which is O's
    std::uint32_t frames_before{};     // the frames the mean holds: 0 starts it again
    std::uint64_t frame{};             // the hooks' frame of the draw
};

// The intervals the measurement interface times on the TAA's list, with timestamps.
enum class Segment : std::uint8_t {
    GuidePass,      // the copy of the AmbientBRDF LUT and the guide pass
    Evaluation,     // RR's barriers and its evaluation by the bridge
    DebugView,      // the debug view, after the guide pass
    Reference,      // the accumulated reference
    NonFiniteCount, // the count of non-finite values in O, after RR's evaluation
    TaaDraw,        // the game's TAA draw, where it reaches the runtime
};

// From the last segment, which a new one has to replace here.
inline constexpr std::size_t SEGMENT_COUNT = static_cast<std::size_t>(Segment::TaaDraw) + 1;

const char* to_string(Segment segment);

// What one recording measured, read back once the GPU is surely done with it.
struct Measurement {
    std::uint64_t frame{}; // the hooks' frame of the recording

    // Each segment the recording timed: its two timestamps, in ticks of the queue's frequency.
    std::array<std::optional<std::array<std::uint64_t, 2>>, SEGMENT_COUNT> segments{};

    // After RR's evaluation: the GPU's running totals of NaN and infinite pixels in O.
    std::optional<core::NonFiniteTotals> nonfinite_totals{};
};

// A copy of the LUT for the check of its axis, read back from the GPU.
struct LutReadback {
    std::uint64_t frame{};       // when it was recorded
    core::Extent found{};        // the size of the texture at the PT's register
    std::vector<float> texels{}; // as judge_ambient_brdf_lut takes them; all 0 if found is not the profile's size
};

// The guide pass: a compute pass of its own, recorded on the TAA's list with its own heap, root signature
// and PSO, that turns the game's G-buffer, depth and velocity into the five textures NGX takes. Then
// either RR's evaluation, or the debug view, which writes one of the textures into O in place of the TAA,
// the way RR's output does.
//
// It also records the measurement interface: the accumulated reference, which writes the mean of T into O
// the same way without the guide pass; the count of non-finite values in O after RR's evaluation; and
// timestamps around each part, and around the game's TAA draw where RR does not replace it, for RR's net
// cost, all read back as many frames later as the LUT's copy.
//
// Called on the thread that records the TAA's list, where it sets up its D3D12 objects the first time.
// The caller restores the state of the list after it.
class GuidePass {
public:
    // Records the copy of the AmbientBRDF LUT if it takes it, the guide pass, then the debug view: O goes
    // from RENDER_TARGET to UNORDERED_ACCESS and back, and the view writes into it the guide DEBUG_VIEW_*
    // of Guides.hlsli, times scale. Returns false if it recorded nothing, as when its D3D12 objects cannot
    // be set up.
    bool record_debug_view(const GuideInputs& in, ID3D12Resource* o, int guide, float scale);

    struct Evaluation {
        bool recorded{};  // the guide pass and the barriers
        bool evaluated{}; // evaluate recorded RR, which wrote O in place of the TAA
    };

    // RR's evaluation at the TAA draw: the copy of the AmbientBRDF LUT if it takes it and the guide pass;
    // the barriers that take T from NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE and RR's textures
    // to NON_PIXEL_SHADER_RESOURCE, and O from RENDER_TARGET to UNORDERED_ACCESS; evaluate, which records
    // RR on the list and says whether it did; and the barriers back, from where NGX leaves its inputs and
    // its output. evaluate runs under a ReentryGuard, so that the hooks never see what NGX records.
    //
    // Once RR wrote O, O goes to NON_PIXEL_SHADER_RESOURCE for the count of its non-finite values, and then
    // back to RENDER_TARGET. forced_nonfinite, if not 0, gives the bits of the value pixel (0, 0) takes for
    // the count alone, never in O: the debug test of the count.
    Evaluation record_evaluation(const GuideInputs& in, ID3D12Resource* t, ID3D12Resource* o,
        const std::function<bool(const Guides&)>& evaluate, std::uint32_t forced_nonfinite = 0);

    // The accumulated reference at the TAA draw: O goes from RENDER_TARGET to UNORDERED_ACCESS and
    // back, and gets the mean of T, which this frame's T joins. Returns false if it recorded nothing.
    bool record_reference(const ReferenceInputs& in);

    // The timer of the game's TAA draw, which RR's evaluation omits, for RR's net cost. begin_taa_draw
    // records the first timestamp of a measurement on the TAA's list, right before the draw reaches the
    // runtime, and returns whether it did; end_taa_draw records the second one right after it. Only once
    // the guide pass is set up, by RR, the debug view or the reference: with RR off since the start, the
    // Mod creates nothing on the device.
    bool begin_taa_draw(ID3D12GraphicsCommandList* list, std::uint64_t frame);
    void end_taa_draw(ID3D12GraphicsCommandList* list);

    // What the recordings measured, in their order, once the GPU is surely done with them: RING frames
    // after them. frame: the hooks' current frame.
    std::vector<Measurement> take_measurements(std::uint64_t frame);

    // The next recording that copies the LUT, for the debug view or for RR's evaluation, also copies it to
    // a readback buffer.
    void request_lut_readback();

    // That copy, once the GPU is surely done with it: far more frames later than the game keeps in
    // flight. frame: the hooks' current frame.
    std::optional<LutReadback> take_lut_readback(std::uint64_t frame);

    struct Status {
        std::string setup{"not set up yet"};
        core::Extent textures{}; // the size of RR's own textures
        std::uint64_t recorded{};
        std::uint64_t texture_sets{}; // created since the start

        // The AmbientBRDF LUT at the last recording, and the recordings that copied it.
        std::string lut{"not recorded yet"};
        std::uint64_t lut_copies{};
        std::string readback{"none asked"};

        // The timestamps and the readback of the measurement interface, and its recordings.
        std::string measurement{"not set up yet"};
        std::uint64_t references{};
        std::uint64_t counts{};
        std::uint64_t timed_taa_draws{};
    };

    Status status() const;

private:
    // RR's own textures, in the order of the pass's UAVs.
    enum class Texture : std::uint8_t {
        NormalRoughness,
        Diffuse,
        Specular,
        Depth,
        MotionVectors,
    };

    // From the last texture, which a new one has to replace here.
    static constexpr std::size_t TEXTURE_COUNT = static_cast<std::size_t>(Texture::MotionVectors) + 1;

    // A set of RR's own textures at one size, in UNORDERED_ACCESS between recordings.
    struct Textures {
        core::Extent size{};
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, TEXTURE_COUNT> resources{};
        std::uint64_t retired_at{}; // the recording at which a set of another size replaced it
    };

    // The texture of a guide of the debug view, DEBUG_VIEW_* of Guides.hlsli.
    static Texture texture_of(int guide);

    // A buffer replaced by a larger one, kept until no list in flight can use it.
    struct RetiredBuffer {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource{};
        std::uint64_t retired_at{};
    };

    // A recording's timestamps and the totals of its count, until they are read back. Each slot holds
    // TIMESTAMPS_PER_SLOT timestamps in the query heap, and SLOT_BYTES in the readback buffer: the
    // timestamps, then the two totals.
    struct MeasurementSlot {
        std::uint64_t sequence{}; // the recordings' order
        std::uint64_t frame{};
        bool pending{};           // resolved on its list, not read back yet
        std::uint32_t timestamps{};
        std::array<std::optional<std::array<std::uint32_t, 2>>, SEGMENT_COUNT> segments{}; // timestamp indices
        bool counted{};
    };

    static constexpr std::uint32_t TIMESTAMPS_PER_SLOT = 4;
    static constexpr std::uint64_t TOTALS_OFFSET = TIMESTAMPS_PER_SLOT * sizeof(std::uint64_t);
    static constexpr std::uint64_t SLOT_BYTES = TOTALS_OFFSET + sizeof(core::NonFiniteTotals);

    // ResolveQueryData writes each slot's timestamps at a multiple of 8 bytes.
    static_assert(SLOT_BYTES % 8 == 0);

    bool set_up(ID3D12Device* device);

    // Under m_mutex: the device of the TAA's list, and every object the first time. False if they cannot
    // be set up.
    bool ready(ID3D12GraphicsCommandList* list);

    bool ensure_textures(core::Extent size);
    bool ensure_lut(core::Extent size);
    bool ensure_lut_readback();
    bool ensure_reference(core::Extent size);
    bool ensure_nonfinite_totals();
    bool ensure_measurements();

    // The next slot of the heap: each recording writes its descriptors into its own, so that the lists in
    // flight keep theirs. Returns the index of its first descriptor.
    UINT next_slot();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor(UINT index) const;

    D3D12_GPU_DESCRIPTOR_HANDLE write_descriptors(const GuideInputs& in, ID3D12Resource* o, Texture shown);
    bool record_lut_copy(const GuideInputs& in, bool read_back);

    // Under m_mutex: the measurement of a recording. start_measurement takes the next slot and records its
    // first timestamp; stamp_segment records another, which ends the segment begun at the one before;
    // finish_measurement resolves them into the readback buffer. Without the query heap they do nothing.
    void start_measurement(ID3D12GraphicsCommandList* list, std::uint64_t frame);
    void stamp_segment(ID3D12GraphicsCommandList* list, Segment segment);
    void finish_measurement(ID3D12GraphicsCommandList* list);
    void read_slot(std::uint32_t slot);

    // Under m_mutex, with O in NON_PIXEL_SHADER_RESOURCE: the count of its non-finite values into the
    // running totals, which then go to the readback buffer.
    void record_count(ID3D12GraphicsCommandList* list, ID3D12Resource* o, core::Extent size, std::uint32_t forced);

    // Under m_mutex and a ReentryGuard: the copy of the LUT if it takes it, then the guide pass, which
    // leaves RR's textures in UNORDERED_ACCESS, between the first two timestamps of the recording's
    // measurement. Returns the table of the recording's descriptors, followed by the debug view's if o is
    // given, or nothing if it recorded nothing.
    std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> record_pass(const GuideInputs& in, ID3D12Resource* o, Texture shown);

    mutable std::mutex m_mutex{};

    Microsoft::WRL::ComPtr<ID3D12Device> m_device{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pass_root_signature{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_view_root_signature{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_lut_root_signature{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_reference_root_signature{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_count_root_signature{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pass_pso{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_view_pso{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_lut_pso{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_reference_pso{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_count_pso{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap{};

    // The copy of the LUT, in COMMON between recordings: a buffer goes back to it at the end of every
    // submission anyway. And the buffer it is read back through, created when a check asks for it.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lut{};
    core::Extent m_lut_size{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lut_readback{};

    // A readback asked for, not recorded yet; and the frame it was recorded at, until it is taken.
    bool m_readback_asked{};
    std::optional<std::uint64_t> m_readback_frame{};
    UINT m_descriptor_size{};
    bool m_failed{};

    // Each recording writes its descriptors into the next slot of the heap, so that the lists in flight
    // keep theirs. m_slot_first is the first descriptor of the current one.
    std::uint32_t m_slot{};
    UINT m_slot_first{};

    // The mean of the accumulated reference, in COMMON between recordings, as large as the largest T seen.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_reference_mean{};
    UINT64 m_reference_bytes{};
    UINT64 m_failed_reference_bytes{};
    std::vector<RetiredBuffer> m_retired_means{};

    // The running totals of the count, never cleared: a committed buffer starts at 0. In COMMON between
    // recordings.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_nonfinite_totals{};
    bool m_nonfinite_failed{};

    // The timestamps, their readback with the totals, and the slots of the recordings.
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestamps{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_measurement_readback{};
    bool m_measurement_failed{};
    std::vector<MeasurementSlot> m_slots{};
    std::uint64_t m_measurement_sequence{};
    std::optional<std::uint32_t> m_measuring{}; // the slot of the recording in progress
    std::optional<std::uint32_t> m_taa_draw{};  // the slot begin_taa_draw took, until end_taa_draw
    std::vector<Measurement> m_measured{};      // read back, not taken yet

    Textures m_textures{};
    std::vector<Textures> m_retired{}; // until no list in flight can use them
    core::Extent m_failed_size{};      // the size whose textures could not be created, until a size in use is back

    Status m_status{};
};
} // namespace rr::d3d12
