// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Constants and plain structures of the pack format, version 1.0 (docs/pack-format.md). Shared
// by the readers in this module and the writers in the baker.

#ifndef VGL_PROVIDERS_PACK_FORMAT_HPP
#define VGL_PROVIDERS_PACK_FORMAT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vgl::internal::pack
{

inline constexpr std::uint16_t major_version = 1;
inline constexpr std::uint16_t minor_version = 0;

inline constexpr std::array<std::byte, 4> manifest_magic = {
    std::byte{'V'}, std::byte{'G'}, std::byte{'L'}, std::byte{'M'}};
inline constexpr std::array<std::byte, 4> shard_magic = {
    std::byte{'V'}, std::byte{'G'}, std::byte{'L'}, std::byte{'S'}};

inline constexpr std::string_view manifest_file_name = "pack.vglm";

// Sizes of fixed structures, in bytes.
inline constexpr std::size_t manifest_header_size = 32;
inline constexpr std::size_t section_entry_size = 32;
inline constexpr std::size_t layer_section_size = 64;
inline constexpr std::size_t shard_index_header_size = 24;
inline constexpr std::size_t shard_index_entry_size = 24;
inline constexpr std::size_t shard_header_size = 64;
inline constexpr std::size_t directory_entry_size = 24;
inline constexpr std::size_t tile_common_header_size = 8;
inline constexpr std::size_t elevation_tile_header_size = 56;

// Section types (section 4.2). Bit 31 marks a section as optional.
inline constexpr std::uint32_t section_layer = 0x00000001;
inline constexpr std::uint32_t section_height_steps = 0x00000002;
inline constexpr std::uint32_t section_shard_index = 0x00000003;
inline constexpr std::uint32_t section_metadata = 0x00000004;
inline constexpr std::uint32_t section_optional_bit = 0x80000000u;
inline constexpr std::uint32_t section_flag_compressed = 0x1;

enum class layer_type : std::uint8_t
{
    elevation = 1,
    imagery = 2,
    geoid = 3,
};

[[nodiscard]] constexpr bool has_heights(layer_type t) noexcept
{
    return t == layer_type::elevation || t == layer_type::geoid;
}

inline constexpr std::uint8_t tiling_scheme_geographic = 1;
inline constexpr std::uint8_t height_reference_none = 0;
inline constexpr std::uint8_t height_reference_ellipsoid = 1;

inline constexpr std::uint8_t shard_flag_base = 0x1;

inline constexpr std::uint8_t elevation_encoding_med_zstd = 1;
inline constexpr std::uint8_t elevation_flag_has_nodata = 0x1;

enum class imagery_encoding : std::uint8_t
{
    jpeg = 1,
    png = 2,
    webp = 3,
    bc1 = 4,
    bc7 = 5,
};

// LAYER section (section 4.3).
struct layer_description
{
    layer_type type = layer_type::elevation;
    std::uint8_t tiling_scheme = tiling_scheme_geographic;
    std::uint16_t tile_size = 0;
    std::uint8_t min_level = 0;
    std::uint8_t max_level = 0;
    std::uint8_t shard_level = 0;
    std::uint8_t border = 0;
    std::uint8_t height_reference = height_reference_ellipsoid;
    double semi_major_axis = 0.0;
    double inverse_flattening = 0.0; // 0 for a sphere.
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;
};

// SHARD_INDEX entry (section 4.5).
struct shard_entry
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint64_t file_size = 0;
    std::uint32_t digest = 0;
};

// Shard header (section 5.3).
struct shard_header
{
    layer_type type = layer_type::elevation;
    std::uint8_t root_level = 0;
    std::uint8_t flags = 0;
    std::uint32_t root_x = 0;
    std::uint32_t root_y = 0;
    std::uint32_t tile_count = 0;
    std::uint64_t directory_offset = 0;
    std::uint32_t directory_stored_size = 0;
    std::uint32_t directory_crc = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t file_size = 0;
    std::uint32_t header_crc = 0; // The shard's digest.

    [[nodiscard]] bool is_base() const noexcept { return (flags & shard_flag_base) != 0; }
};

// Shard directory entry (section 5.3).
struct directory_entry
{
    std::uint64_t key = 0;
    std::uint64_t offset = 0; // Relative to data_offset.
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
};

// Tile header (section 6.1), with the elevation and geoid fields of section 6.2.
struct tile_header
{
    layer_type type = layer_type::elevation;
    std::uint8_t encoding = 0;
    std::uint8_t child_mask = 0;
    std::uint8_t flags = 0;
    std::uint16_t header_size = 0;
    double offset = 0.0;
    double scale = 0.0;
    std::array<float, 4> child_min = {};
    std::array<float, 4> child_max = {};

    [[nodiscard]] bool has_child(std::uint32_t c) const noexcept
    {
        return (child_mask & (1u << c)) != 0;
    }
};

} // namespace vgl::internal::pack

#endif // VGL_PROVIDERS_PACK_FORMAT_HPP
