#include <rr_bridge.h>

#include "Adapter.hpp"

namespace {
#ifdef RR_BRIDGE_TEST_WRONG_ABI
// A build that reports another ABI version, to check by hand that the Mod says so (the CMake option
// RR_BRIDGE_TEST_WRONG_ABI).
constexpr uint32_t REPORTED_ABI_VERSION = RR_BRIDGE_ABI_VERSION + 1;
#else
constexpr uint32_t REPORTED_ABI_VERSION = RR_BRIDGE_ABI_VERSION;
#endif

const rr_bridge_api API{
    .abi_version = REPORTED_ABI_VERSION,
    .flags = rr::bridge::FLAGS,
    .init = &rr::bridge::init,
    .query_availability = &rr::bridge::query_availability,
    .create_feature = &rr::bridge::create_feature,
    .release_feature = &rr::bridge::release_feature,
    .evaluate = &rr::bridge::evaluate,
    .last_error = &rr::bridge::last_error,
};
} // namespace

extern "C" const rr_bridge_api* rr_bridge_get_api(void) {
    return &API;
}
