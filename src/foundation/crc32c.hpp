// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// CRC-32C (Castagnoli), as used by the pack format (docs/pack-format.md, section 3).

#ifndef VGL_FOUNDATION_CRC32C_HPP
#define VGL_FOUNDATION_CRC32C_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace vgl::internal
{

// CRC-32C of data. To checksum data in pieces, pass the result of the previous piece as crc.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t crc = 0) noexcept;

} // namespace vgl::internal

#endif // VGL_FOUNDATION_CRC32C_HPP
