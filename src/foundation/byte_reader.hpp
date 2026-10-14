// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Bounds-checked reader of little-endian binary data. Reading past the end never touches
// memory outside the data: it returns zero and marks the reader as failed, so a parser can
// read a whole structure and check ok() once.

#ifndef VGL_FOUNDATION_BYTE_READER_HPP
#define VGL_FOUNDATION_BYTE_READER_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace vgl::internal
{

class byte_reader
{
public:
    explicit byte_reader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }

    [[nodiscard]] std::uint8_t u8() noexcept { return read<std::uint8_t>(); }
    [[nodiscard]] std::uint16_t u16() noexcept { return read<std::uint16_t>(); }
    [[nodiscard]] std::uint32_t u32() noexcept { return read<std::uint32_t>(); }
    [[nodiscard]] std::uint64_t u64() noexcept { return read<std::uint64_t>(); }
    [[nodiscard]] float f32() noexcept { return std::bit_cast<float>(read<std::uint32_t>()); }
    [[nodiscard]] double f64() noexcept { return std::bit_cast<double>(read<std::uint64_t>()); }

    // The next count bytes, or an empty span (and failure) if fewer remain.
    [[nodiscard]] std::span<const std::byte> bytes(std::size_t count) noexcept
    {
        if (!ok_ || count > remaining())
        {
            ok_ = false;
            return {};
        }
        const std::span<const std::byte> result = data_.subspan(position_, count);
        position_ += count;
        return result;
    }

    void skip(std::size_t count) noexcept { static_cast<void>(bytes(count)); }

    // Moves to an absolute position. Fails if the position is past the end.
    void seek(std::size_t position) noexcept
    {
        if (position > data_.size())
        {
            ok_ = false;
            return;
        }
        position_ = position;
    }

private:
    template <typename T>
    T read() noexcept
    {
        const std::span<const std::byte> b = bytes(sizeof(T));
        if (b.empty())
        {
            return 0;
        }
        T value = 0;
        std::memcpy(&value, b.data(), sizeof(T));
        if constexpr (std::endian::native == std::endian::big)
        {
            value = byteswap(value);
        }
        return value;
    }

    template <typename T>
    static T byteswap(T value) noexcept
    {
        T result = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            result = static_cast<T>((result << 8) | ((value >> (8 * i)) & 0xFFu));
        }
        return result;
    }

    std::span<const std::byte> data_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

} // namespace vgl::internal

#endif // VGL_FOUNDATION_BYTE_READER_HPP
