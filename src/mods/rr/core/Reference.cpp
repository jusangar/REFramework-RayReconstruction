#include "Reference.hpp"

namespace rr::core {
void ReferenceAccumulator::on_camera_update(const CameraUpdate& update) {
    if (!update.main_layer_updated) {
        return;
    }

    if (update.layer_changed) {
        m_layer_changed = true;
    }

    m_latest = Camera{update.jitter_disable.view, update.jitter_disable.projection};
}

// When several causes apply, the first one in the order of ReferenceRestart is given.
ReferenceStep ReferenceAccumulator::on_recording(std::uint64_t frame, Extent size) {
    ReferenceStep step{};

    if (!m_latest) {
        step.restart = ReferenceRestart::CameraUnknown;
    } else if (!m_last_frame || frame != *m_last_frame + 1) {
        step.restart = ReferenceRestart::PreviousFrameNotRecorded;
    } else if (m_layer_changed) {
        step.restart = ReferenceRestart::LayerChanged;
    } else if (size != m_size) {
        step.restart = ReferenceRestart::SizeChanged;
    } else if (*m_latest != m_anchor) {
        step.restart = ReferenceRestart::CameraMoved;
    }

    if (step.restart) {
        m_frames = 0;
        m_anchor = m_latest.value_or(Camera{});
        m_size = size;
        m_layer_changed = false;
    }

    step.frames_before = m_frames;
    ++m_frames;
    m_last_frame = frame;
    return step;
}

void ReferenceAccumulator::on_not_recorded() {
    m_frames = 0;
    m_last_frame.reset();
}

std::string_view to_string(ReferenceRestart restart) {
    switch (restart) {
    case ReferenceRestart::CameraUnknown:
        return "camera not updated yet";
    case ReferenceRestart::PreviousFrameNotRecorded:
        return "the frame before recorded no reference";
    case ReferenceRestart::LayerChanged:
        return "main scene layer changed";
    case ReferenceRestart::SizeChanged:
        return "T's size changed";
    case ReferenceRestart::CameraMoved:
        return "camera moved";
    }

    return "?";
}
} // namespace rr::core
