#include <algorithm>
#include <cstring>

#include "ShaderDigest.hpp"

namespace rr::core {
namespace {
// The layout of a DXIL container, as DxilContainer.h in DirectXShaderCompiler defines it. All of it
// is little-endian, like the platforms D3D12 runs on.
constexpr std::size_t HEADER_SIZE = 32;       // "DXBC", the container's own hash, version, size and part count
constexpr std::size_t SIZE_OFFSET = 24;       // uint32: the size of the whole container
constexpr std::size_t PART_COUNT_OFFSET = 28; // uint32, followed by one uint32 offset per part
constexpr std::size_t PART_HEADER_SIZE = 8;   // fourcc and the size of the part's data
constexpr std::size_t HASH_FLAGS_SIZE = 4;    // the HASH part's data: flags, then the digest

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | static_cast<std::uint32_t>(bytes[offset + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[offset + 2]) << 16 | static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
}

bool has_fourcc(std::span<const std::uint8_t> bytes, std::size_t offset, const char* fourcc) {
    return std::memcmp(bytes.data() + offset, fourcc, 4) == 0;
}

// The data of the first part with this fourcc that lies whole inside the container; none if the bytes
// are not a whole container or no such part does.
std::optional<std::span<const std::uint8_t>> find_part(std::span<const std::uint8_t> bytecode, const char* fourcc) {
    if (bytecode.size() < HEADER_SIZE || !has_fourcc(bytecode, 0, "DXBC")) {
        return std::nullopt;
    }

    // Nothing is read past the container's declared size, nor past the bytes there are.
    const auto container_size = read_u32(bytecode, SIZE_OFFSET);

    if (container_size > bytecode.size()) {
        return std::nullopt;
    }

    const auto container = bytecode.first(container_size);
    const std::size_t part_count = read_u32(bytecode, PART_COUNT_OFFSET);

    if (container.size() < HEADER_SIZE || part_count > (container.size() - HEADER_SIZE) / 4) {
        return std::nullopt;
    }

    for (std::size_t i = 0; i < part_count; ++i) {
        const std::size_t offset = read_u32(container, HEADER_SIZE + 4 * i);

        if (offset > container.size() || container.size() - offset < PART_HEADER_SIZE) {
            continue;
        }

        const std::size_t part_size = read_u32(container, offset + 4);
        const auto data = offset + PART_HEADER_SIZE;

        if (has_fourcc(container, offset, fourcc) && part_size <= container.size() - data) {
            return container.subspan(data, part_size);
        }
    }

    return std::nullopt;
}

// The PSV0 part, from DxilPipelineStateValidation.h: the size of the runtime info and the runtime info,
// the resource count and, if it is not 0, the size of each record and the records. A record is ResType,
// Space, LowerBound and UpperBound, and from PSV version 2 on, ResKind and ResFlags.
constexpr std::size_t PSV_MIN_RUNTIME_INFO_SIZE = 24; // PSVRuntimeInfo0, the smallest the reader takes
constexpr std::size_t PSV_RECORD_SIZE = 16;           // PSVResourceBindInfo0
constexpr std::size_t PSV_RECORD_WITH_KIND_SIZE = 24; // PSVResourceBindInfo1

// PSVResourceType and PSVResourceKind.
constexpr std::uint32_t PSV_SRV_TYPED = 3;
constexpr std::uint32_t PSV_SRV_RAW = 4;
constexpr std::uint32_t PSV_SRV_STRUCTURED = 5;
constexpr std::uint32_t PSV_KIND_TEXTURE2D = 2;
} // namespace

std::optional<ShaderDigest> read_shader_digest(std::span<const std::uint8_t> bytecode) {
    const auto part = find_part(bytecode, "HASH");
    ShaderDigest digest{};

    if (!part || part->size() < HASH_FLAGS_SIZE + digest.bytes.size()) {
        return std::nullopt;
    }

    std::copy_n(part->begin() + HASH_FLAGS_SIZE, digest.bytes.size(), digest.bytes.begin());
    return digest;
}

SrvDeclaration read_srv_declaration(std::span<const std::uint8_t> bytecode, std::uint32_t shader_register, std::uint32_t space) {
    const auto part = find_part(bytecode, "PSV0");

    if (!part || part->size() < 4) {
        return SrvDeclaration::Unknown;
    }

    const auto psv = *part;
    const std::size_t runtime_info_size = read_u32(psv, 0);

    if (runtime_info_size < PSV_MIN_RUNTIME_INFO_SIZE || runtime_info_size > psv.size() - 4 || psv.size() - 4 - runtime_info_size < 4) {
        return SrvDeclaration::Unknown;
    }

    auto offset = 4 + runtime_info_size;
    const std::size_t count = read_u32(psv, offset);
    offset += 4;

    if (count == 0) {
        return SrvDeclaration::None;
    }

    if (psv.size() - offset < 4) {
        return SrvDeclaration::Unknown;
    }

    const std::size_t record_size = read_u32(psv, offset);
    offset += 4;

    if (record_size < PSV_RECORD_SIZE || count > (psv.size() - offset) / record_size) {
        return SrvDeclaration::Unknown;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const auto record = psv.subspan(offset + i * record_size, record_size);
        const auto type = read_u32(record, 0);

        if ((type != PSV_SRV_TYPED && type != PSV_SRV_RAW && type != PSV_SRV_STRUCTURED) || read_u32(record, 4) != space ||
            shader_register < read_u32(record, 8) || shader_register > read_u32(record, 12)) {
            continue;
        }

        if (record_size < PSV_RECORD_WITH_KIND_SIZE) {
            return SrvDeclaration::Unknown;
        }

        return type == PSV_SRV_TYPED && read_u32(record, 16) == PSV_KIND_TEXTURE2D ? SrvDeclaration::Texture2D : SrvDeclaration::Other;
    }

    return SrvDeclaration::None;
}
} // namespace rr::core
