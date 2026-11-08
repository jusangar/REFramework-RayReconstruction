#pragma once

#include <span>
#include <string_view>

#include "ShaderEdit.hpp"

namespace emission {
// What the correction knows of one game: the variants of its PT it can edit. Positions and numbers, not code of
// the game's. The game's shader only takes the edit if it is the one the table was made from.
struct Table {
    std::string_view game{}; // as sdk::GameIdentity::game_name() gives it
    std::span<const Variant> variants{};
};

// The table of a game, or nullptr if the correction does not support it.
const Table* find_table(std::string_view game);
} // namespace emission
