#include "Adapter.hpp"

// The adapter built without the NGX SDK: every operation fails, so RR stays unavailable and REFramework
// works as without the bridge.
namespace rr::bridge {
const uint32_t FLAGS = 0;

rr_bridge_result init(const rr_bridge_init_params*) {
    return RR_BRIDGE_NO_SDK;
}

rr_bridge_result query_availability() {
    return RR_BRIDGE_NO_SDK;
}

rr_bridge_result create_feature(const rr_bridge_feature_params*, rr_bridge_feature** feature) {
    if (feature != nullptr) {
        *feature = nullptr;
    }

    return RR_BRIDGE_NO_SDK;
}

void release_feature(rr_bridge_feature*) {
}

rr_bridge_result evaluate(rr_bridge_feature*, const rr_bridge_eval_params*) {
    return RR_BRIDGE_NO_SDK;
}

const char* last_error() {
    return "built without the NGX SDK";
}
} // namespace rr::bridge
