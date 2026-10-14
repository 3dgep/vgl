// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Elevation tile encoding 1: median edge detector prediction, zigzag residuals and byte planes
// (docs/pack-format.md, section 6.2). Zstandard compression of the planes is done separately.
// The encoder in the baker and the decoder in the runtime share this code.

#ifndef VGL_DECODERS_ELEVATION_CODEC_HPP
#define VGL_DECODERS_ELEVATION_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace vgl::internal
{

// Quantized value of a nodata sample.
inline constexpr std::int16_t elevation_nodata = -32768;

// Largest magnitude of a valid quantized sample.
inline constexpr std::int32_t elevation_max_quantized = 32767;

// Median edge detector of LOCO-I, from the samples to the left (a), above (b) and above-left (c).
[[nodiscard]] constexpr std::int32_t
med_predict(std::int32_t a, std::int32_t b, std::int32_t c) noexcept
{
    const std::int32_t lo = a < b ? a : b;
    const std::int32_t hi = a < b ? b : a;
    if (c >= hi)
    {
        return lo;
    }
    if (c <= lo)
    {
        return hi;
    }
    return a + b - c;
}

// Prediction for sample (i, j) of a tile of size samples per side, in row-major order.
[[nodiscard]] constexpr std::int32_t elevation_prediction(std::span<const std::int16_t> q,
                                                          std::uint32_t size,
                                                          std::uint32_t i,
                                                          std::uint32_t j) noexcept
{
    const std::size_t k = static_cast<std::size_t>(j) * size + i;
    if (j == 0)
    {
        return i == 0 ? 0 : q[k - 1];
    }
    if (i == 0)
    {
        return q[k - size];
    }
    return med_predict(q[k - 1], q[k - size], q[k - size - 1]);
}

[[nodiscard]] constexpr std::uint16_t zigzag(std::int16_t r) noexcept
{
    const auto u = static_cast<std::uint16_t>(r);
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(u << 1) ^ (r < 0 ? 0xFFFFu : 0u));
}

[[nodiscard]] constexpr std::int16_t unzigzag(std::uint16_t z) noexcept
{
    return static_cast<std::int16_t>(
        static_cast<std::uint16_t>((z >> 1) ^ ((z & 1u) != 0 ? 0xFFFFu : 0u)));
}

// Writes the byte planes of the residuals of q (size * size samples) to planes
// (2 * size * size bytes): first the low bytes of every zigzagged residual, then the high bytes.
inline void elevation_encode_planes(std::span<const std::int16_t> q,
                                    std::uint32_t size,
                                    std::span<std::byte> planes) noexcept
{
    const std::size_t count = static_cast<std::size_t>(size) * size;
    for (std::uint32_t j = 0; j < size; ++j)
    {
        for (std::uint32_t i = 0; i < size; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * size + i;
            const std::int32_t p = elevation_prediction(q, size, i, j);
            const auto r = static_cast<std::int16_t>(static_cast<std::uint16_t>(q[k] - p));
            const std::uint16_t z = zigzag(r);
            planes[k] = static_cast<std::byte>(z & 0xFFu);
            planes[count + k] = static_cast<std::byte>(z >> 8);
        }
    }
}

// Reverses elevation_encode_planes.
inline void elevation_decode_planes(std::span<const std::byte> planes,
                                    std::uint32_t size,
                                    std::span<std::int16_t> q) noexcept
{
    const std::size_t count = static_cast<std::size_t>(size) * size;
    for (std::uint32_t j = 0; j < size; ++j)
    {
        for (std::uint32_t i = 0; i < size; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * size + i;
            const auto z =
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(planes[k]) |
                                           (static_cast<std::uint16_t>(planes[count + k]) << 8));
            const std::int32_t p = elevation_prediction(q, size, i, j);
            q[k] = static_cast<std::int16_t>(static_cast<std::uint16_t>(p + unzigzag(z)));
        }
    }
}

} // namespace vgl::internal

#endif // VGL_DECODERS_ELEVATION_CODEC_HPP
