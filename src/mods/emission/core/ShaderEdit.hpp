#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "../../rr/core/ShaderDigest.hpp"

namespace emission {
// The emission correction rewrites operands of the shader's bitcode where they are, in memory, so that no code of
// the game's shader has to be shipped: the table keeps positions and values. Each operand is a VBR6 number of three
// chunks, kept here as its 18 raw bits, continuation bits included, so that the edit needs no bitcode reader: it
// compares and rewrites bits, and the shader keeps its size.
inline constexpr std::uint32_t FIELD_WIDTH = 18;

// Bits count from the first byte of the container, the lowest bit of each byte first, as LLVM's bitstream
// lays them out.
struct Field {
    std::uint32_t bit{};
    std::uint32_t expected{};
    std::uint32_t corrected{};
};

// A variant of the PT: its digest and the operand of each channel of the sum that duplicates the emission.
struct Variant {
    rr::core::ShaderDigest digest{};
    std::array<Field, 3> fields{};
};

// The variant with this digest, or nullptr.
const Variant* find_variant(std::span<const Variant> variants, const rr::core::ShaderDigest& digest);

// Why a shader is not corrected. Whichever it is, the game's PSO stays.
enum class Refusal : std::uint8_t {
    NotInTable,       // its digest is not in the table
    InvalidContainer, // not a whole DXIL container with a digest
    WrongSum,         // its sum is not the one this computes, so the sum written after the edit would not be valid either
    UnexpectedValue,  // a field does not hold its expected value: the shader is not the one the table knows
};

const char* to_string(Refusal refusal);

using ShaderEdit = std::variant<std::vector<std::uint8_t>, Refusal>;

// The bytes of the shader with the fields of its variant rewritten and its sum computed again, or why not.
// Nothing is delivered unless all of it applies. The HASH part stays, so the result keeps the original's digest.
ShaderEdit correct_shader(std::span<const Variant> variants, std::span<const std::uint8_t> bytecode);
} // namespace emission
