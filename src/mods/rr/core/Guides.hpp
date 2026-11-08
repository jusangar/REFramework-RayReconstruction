#pragma once

// Guides.hlsli compiled as C++: the tests of the RR core run the same code as the guide pass.

// The min and max macros of windows.h, where the Mod includes it first, would replace the header's own,
// which return the operand that is not NaN as the GPU does.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

#include "HlslCompat.hpp"

namespace rr::core::guides {
using namespace rr::core::hlsl;

#include "Guides.hlsli"
} // namespace rr::core::guides

#pragma pop_macro("max")
#pragma pop_macro("min")
