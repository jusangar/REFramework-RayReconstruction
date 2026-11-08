#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace rr::core {
// The digest of a shader: the HASH part of its DXIL container, the "shader hash" PIX shows.
struct ShaderDigest {
    std::array<std::uint8_t, 16> bytes{};

    friend constexpr bool operator==(const ShaderDigest&, const ShaderDigest&) = default;
};

namespace detail {
// Not constexpr: reaching it while evaluating shader_digest makes the build fail.
inline void not_a_hex_digit() {}

consteval std::uint8_t hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }

    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }

    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }

    not_a_hex_digit();
    return 0;
}
} // namespace detail

// From the 32 hex digits the logs and PIX print, at compile time.
consteval ShaderDigest shader_digest(const char (&hex)[33]) {
    ShaderDigest digest{};

    for (std::size_t i = 0; i < digest.bytes.size(); ++i) {
        digest.bytes[i] = static_cast<std::uint8_t>(detail::hex_value(hex[2 * i]) << 4 | detail::hex_value(hex[2 * i + 1]));
    }

    return digest;
}

inline std::string to_string(const ShaderDigest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out{};

    for (const auto b : digest.bytes) {
        out += digits[b >> 4];
        out += digits[b & 0xf];
    }

    return out;
}

// The digest of the shader whose bytecode this is: the one its DXIL container keeps in the HASH part.
// None if the bytes are not a whole container or it has no HASH part, as with FXC's DXBC.
std::optional<ShaderDigest> read_shader_digest(std::span<const std::uint8_t> bytecode);

// What a shader declares at an SRV register.
enum class SrvDeclaration : std::uint8_t {
    Texture2D, // a typed SRV of kind Texture2D, alone or in a range
    Other,     // an SRV of another kind, such as a buffer or a raytracing acceleration structure
    None,      // no SRV covers the register
    Unknown,   // no PSV0 part, records without the kind, or bytes that cannot be read
};

// From the PSV0 part of the shader's DXIL container, which keeps the type and the kind of every resource
// the shader declares, as DxilPipelineStateValidation.h in DirectXShaderCompiler lays it out.
SrvDeclaration read_srv_declaration(std::span<const std::uint8_t> bytecode, std::uint32_t shader_register, std::uint32_t space);
} // namespace rr::core
