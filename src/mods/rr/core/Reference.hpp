#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "FramePolicy.hpp"
#include "Geometry.hpp"

namespace rr::core {
// Why the accumulated reference starts its mean again.
enum class ReferenceRestart : std::uint8_t {
    // No camera update has come yet, so nothing says the camera is still.
    CameraUnknown,
    // The frame before recorded no reference: this is the first recording, the mode was off, or the
    // frame was not identified, as in the menus.
    PreviousFrameNotRecorded,
    // The main scene layer is another one.
    LayerChanged,
    // T's size changed.
    SizeChanged,
    // JitterDisable's view or projection is no longer the one the mean started with.
    CameraMoved,
};

// From the last cause, which a new one has to replace here.
inline constexpr std::size_t REFERENCE_RESTART_COUNT = static_cast<std::size_t>(ReferenceRestart::CameraMoved) + 1;

// What one recording of the reference does.
struct ReferenceStep {
    std::uint32_t frames_before{};             // the frames the mean already holds: 0 starts it with this one
    std::optional<ReferenceRestart> restart{}; // why it starts again, if it does
};

// The accumulated reference of the measurement interface: with the camera still, the mean of T over
// hundreds of frames, which the Mod writes into O the way RR's output goes. This decides how many frames
// the mean holds; the GPU keeps the mean.
//
// The camera is still while JitterDisable's view and projection stay equal, element by element and with
// no tolerance, to those of the update the mean started with. They carry no jitter, so the own jitter,
// or the game's, moves the samples within the pixel and the mean comes out antialiased. A jitter probe in
// the game kept three trims with the camera still, and in them JitterDisable's projection and Scene's
// old_view_projection, which carry no jitter, repeat bit for bit for 48 updates, so a tolerance would
// only let a slow drift blur the mean. A camera that sways at rest would never let it converge; the
// interface then says "camera moved" at every frame.
//
// Single-threaded, like the policy: the Mod calls it under the same mutex.
class ReferenceAccumulator {
public:
    // Every update of the main scene layer, as the policy gets it, also while the reference is off. One
    // whose layer did not update leaves the camera where it was.
    void on_camera_update(const CameraUpdate& update);

    // The reference is recorded at the evaluation point of the hooks' frame `frame`, with T at `size`.
    ReferenceStep on_recording(std::uint64_t frame, Extent size);

    // The Mod could not record the step on_recording gave: the GPU's mean does not have that frame, so the
    // next recording starts the mean again, as after a frame without one.
    void on_not_recorded();

    // In the mean after the last recording.
    std::uint32_t frames() const { return m_frames; }

private:
    struct Camera {
        Matrix4 view{};
        Matrix4 projection{};

        friend bool operator==(const Camera&, const Camera&) = default;
    };

    std::optional<Camera> m_latest{};
    bool m_layer_changed{};

    // The mean: its camera, T's size, the frames it holds and the last frame recorded.
    Camera m_anchor{};
    Extent m_size{};
    std::uint32_t m_frames{};
    std::optional<std::uint64_t> m_last_frame{};
};

std::string_view to_string(ReferenceRestart restart);
} // namespace rr::core
