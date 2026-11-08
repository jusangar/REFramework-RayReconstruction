#pragma once

#include <rr_bridge.h>

// What an adapter of the NGX bridge implements: the operations of the table in rr_bridge.h. There are
// two adapters behind it, and the build links one: the real one, with the NGX SDK, and the one built
// without it, so that the bridge also builds where the SDK is not.
namespace rr::bridge {
extern const uint32_t FLAGS;

rr_bridge_result init(const rr_bridge_init_params* params);
rr_bridge_result query_availability();
rr_bridge_result create_feature(const rr_bridge_feature_params* params, rr_bridge_feature** feature);
void release_feature(rr_bridge_feature* feature);
rr_bridge_result evaluate(rr_bridge_feature* feature, const rr_bridge_eval_params* params);
const char* last_error();
} // namespace rr::bridge
