#include <algorithm>
#include <array>
#include <bit>

#include "ContainerSum.hpp"

namespace emission {
namespace {
// MD5 (RFC 1321) with another padding of the last block, as ComputeHashRetail in DxilHash of
// DirectXShaderCompiler: the size in bits goes first and 2 * size + 1 last, both of the bytes it covers.
constexpr std::size_t BLOCK_BYTES = 64;
constexpr std::size_t SIZE_BYTES = 4;
constexpr std::size_t TAIL_WITH_ROOM = BLOCK_BYTES - 2 * SIZE_BYTES; // a longer tail takes a second block
constexpr std::uint8_t PADDING_START = 0x80;

// In the header of the container, as DxilContainer.h in DirectXShaderCompiler lays it out, little-endian.
constexpr std::size_t DECLARED_SIZE_OFFSET = 24;

using State = std::array<std::uint32_t, 4>;

constexpr State INITIAL_STATE{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};

// clang-format off
constexpr std::array<int, 64> ROTATIONS{
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

constexpr std::array<std::uint32_t, 64> CONSTANTS{
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
// clang-format on

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | static_cast<std::uint32_t>(bytes[1]) << 8 | static_cast<std::uint32_t>(bytes[2]) << 16 |
           static_cast<std::uint32_t>(bytes[3]) << 24;
}

void write_u32(std::uint8_t* bytes, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void add_block(State& state, const std::uint8_t* block) {
    auto [a, b, c, d] = state;

    for (std::size_t i = 0; i < CONSTANTS.size(); ++i) {
        std::uint32_t mixed{};
        std::size_t word{};

        if (i < 16) {
            mixed = (b & c) | (~b & d);
            word = i;
        } else if (i < 32) {
            mixed = (d & b) | (~d & c);
            word = (5 * i + 1) % 16;
        } else if (i < 48) {
            mixed = b ^ c ^ d;
            word = (3 * i + 5) % 16;
        } else {
            mixed = c ^ (b | ~d);
            word = (7 * i) % 16;
        }

        const auto rotated = std::rotl(mixed + a + CONSTANTS[i] + read_u32(block + 4 * word), ROTATIONS[i]);
        a = d;
        d = c;
        c = b;
        b += rotated;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}
} // namespace

ContainerSum compute_container_sum(std::span<const std::uint8_t> container) {
    const auto covered = container.subspan(CONTAINER_SUM_END);
    const auto tail_size = covered.size() % BLOCK_BYTES;
    const auto whole_blocks = covered.size() - tail_size;
    const auto tail = covered.subspan(whole_blocks);
    const auto size = static_cast<std::uint32_t>(covered.size());

    auto state = INITIAL_STATE;

    for (std::size_t offset = 0; offset < whole_blocks; offset += BLOCK_BYTES) {
        add_block(state, covered.data() + offset);
    }

    std::array<std::uint8_t, BLOCK_BYTES> block{};
    const auto size_in_bits = size << 3;
    const auto closing_word = size << 1 | 1;

    if (tail_size < TAIL_WITH_ROOM) {
        write_u32(block.data(), size_in_bits);
        std::copy(tail.begin(), tail.end(), block.begin() + SIZE_BYTES);
        block[SIZE_BYTES + tail_size] = PADDING_START;
    } else {
        std::copy(tail.begin(), tail.end(), block.begin());
        block[tail_size] = PADDING_START;
        add_block(state, block.data());

        block.fill(0);
        write_u32(block.data(), size_in_bits);
    }

    write_u32(block.data() + BLOCK_BYTES - SIZE_BYTES, closing_word);
    add_block(state, block.data());

    ContainerSum sum{};

    for (std::size_t i = 0; i < state.size(); ++i) {
        write_u32(sum.data() + 4 * i, state[i]);
    }

    return sum;
}

ContainerSum stored_container_sum(std::span<const std::uint8_t> container) {
    ContainerSum sum{};
    std::copy_n(container.begin() + CONTAINER_SUM_OFFSET, sum.size(), sum.begin());
    return sum;
}

void store_container_sum(std::span<std::uint8_t> container, const ContainerSum& sum) {
    std::copy(sum.begin(), sum.end(), container.begin() + CONTAINER_SUM_OFFSET);
}

std::size_t declared_container_size(std::span<const std::uint8_t> container) {
    return read_u32(container.data() + DECLARED_SIZE_OFFSET);
}
} // namespace emission
