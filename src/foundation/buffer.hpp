// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Fixed-size, move-only array of trivially copyable values in memory from a vgl allocator.

#ifndef VGL_FOUNDATION_BUFFER_HPP
#define VGL_FOUNDATION_BUFFER_HPP

#include "foundation/allocator.hpp"

#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

namespace vgl::internal
{

template <typename T>
class buffer
{
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                  "buffer holds plain data only");

public:
    buffer() noexcept = default;

    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;

    buffer(buffer&& other) noexcept
        : allocator_(other.allocator_), data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0))
    {
    }

    buffer& operator=(buffer&& other) noexcept
    {
        if (this != &other)
        {
            release();
            allocator_ = other.allocator_;
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~buffer() { release(); }

    // Allocates count zero-initialized elements, replacing the current contents. Returns false
    // if the allocation fails, leaving the buffer empty. A count of 0 always succeeds.
    [[nodiscard]] bool allocate(const allocator& a, std::size_t count) noexcept
    {
        release();
        allocator_ = a;
        if (count == 0)
        {
            return true;
        }
        if (count > static_cast<std::size_t>(-1) / sizeof(T))
        {
            return false;
        }
        void* memory = allocator_.allocate(count * sizeof(T), alignof(T));
        if (memory == nullptr)
        {
            return false;
        }
        data_ = static_cast<T*>(memory);
        size_ = count;
        for (std::size_t i = 0; i < count; ++i)
        {
            ::new (static_cast<void*>(data_ + i)) T();
        }
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] std::span<T> span() noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<const T> span() const noexcept { return {data_, size_}; }

private:
    void release() noexcept
    {
        if (data_ != nullptr)
        {
            allocator_.deallocate(data_, size_ * sizeof(T), alignof(T));
            data_ = nullptr;
            size_ = 0;
        }
    }

    allocator allocator_;
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace vgl::internal

#endif // VGL_FOUNDATION_BUFFER_HPP
