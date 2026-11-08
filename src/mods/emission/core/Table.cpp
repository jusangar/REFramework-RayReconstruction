#include <array>

#include "Table.hpp"

namespace emission {
namespace {
// RE2, DX12, current Steam build: the 48 Pure variants with bounces. The table is generated from the shaders of the
// game's pak, so an update of the game that changes them calls for generating it again. Each edit makes the hit edge
// of the phi that collects the duplicated emission take the operand of the miss edge. The 12 variants with 0 bounces
// collect none, and the correction leaves Hybrid out.
constexpr std::array RE2_VARIANTS{
#include "Re2Variants.inc"
};

constexpr Table RE2{"re2", RE2_VARIANTS};

constexpr std::array TABLES{&RE2};
} // namespace

const Table* find_table(std::string_view game) {
    for (const auto table : TABLES) {
        if (table->game == game) {
            return table;
        }
    }

    return nullptr;
}
} // namespace emission
