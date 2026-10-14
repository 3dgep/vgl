// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "foundation/crc32c.hpp"

#include <array>

namespace vgl::internal
{

namespace
{

// Reflected Castagnoli polynomial.
constexpr std::uint32_t polynomial = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> make_table() noexcept
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i)
    {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit)
        {
            c = (c & 1u) != 0 ? (c >> 1) ^ polynomial : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> table = make_table();

} // namespace

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t crc) noexcept
{
    std::uint32_t c = ~crc;
    for (const std::byte b : data)
    {
        c = table[(c ^ static_cast<std::uint32_t>(b)) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

} // namespace vgl::internal
