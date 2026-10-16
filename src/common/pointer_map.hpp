// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// A map from non-zero 64-bit keys to pointers, for the libraries built on the C API: the
// backends map render resource identifiers to their own objects with it. Open addressing with
// linear probing, and backward-shift deletion so that no tombstones build up. It allocates
// through vgl::common::memory and reports a failed allocation by returning false.

#ifndef VGL_COMMON_POINTER_MAP_HPP
#define VGL_COMMON_POINTER_MAP_HPP

#include "common/host_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace vgl::common
{

template <typename T>
class pointer_map
{
public:
    explicit pointer_map(const memory& m) noexcept : memory_(m) {}
    pointer_map(const pointer_map&) = delete;
    pointer_map& operator=(const pointer_map&) = delete;
    pointer_map(pointer_map&&) = delete;
    pointer_map& operator=(pointer_map&&) = delete;
    ~pointer_map() { release(); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // The value for key, or null.
    [[nodiscard]] T* find(std::uint64_t key) const noexcept
    {
        if (size_ == 0 || key == 0)
        {
            return nullptr;
        }
        for (std::size_t i = home(key);; i = (i + 1) & mask())
        {
            if (slots_[i].key == key)
            {
                return slots_[i].value;
            }
            if (slots_[i].key == 0)
            {
                return nullptr;
            }
        }
    }

    // Inserts or replaces. key must not be 0. Returns false if growing failed, leaving the
    // map unchanged.
    [[nodiscard]] bool insert(std::uint64_t key, T* value) noexcept
    {
        if ((size_ + 1) * 2 > capacity_ && !grow())
        {
            return false;
        }
        place(key, value);
        return true;
    }

    // Removes key, if present.
    void erase(std::uint64_t key) noexcept
    {
        if (size_ == 0 || key == 0)
        {
            return;
        }
        std::size_t i = home(key);
        while (slots_[i].key != key)
        {
            if (slots_[i].key == 0)
            {
                return;
            }
            i = (i + 1) & mask();
        }
        // Shift later entries of the run back into the gap, where that keeps them reachable.
        for (std::size_t j = (i + 1) & mask(); slots_[j].key != 0; j = (j + 1) & mask())
        {
            const std::size_t h = home(slots_[j].key);
            const bool movable = i <= j ? (h <= i || h > j) : (h <= i && h > j);
            if (movable)
            {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i] = slot{};
        --size_;
    }

    // Calls f(key, value) for every entry. f must not change the map.
    template <typename F>
    void for_each(const F& f) const noexcept
    {
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            if (slots_[i].key != 0)
            {
                f(slots_[i].key, slots_[i].value);
            }
        }
    }

    // Removes every entry, keeping the capacity.
    void clear() noexcept
    {
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            slots_[i] = slot{};
        }
        size_ = 0;
    }

private:
    struct slot
    {
        std::uint64_t key = 0;
        T* value = nullptr;
    };

    [[nodiscard]] std::size_t mask() const noexcept { return capacity_ - 1; }

    [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept
    {
        // The finalizer of SplitMix64: identifiers are sequential, so they need mixing.
        key ^= key >> 30;
        key *= 0xBF58476D1CE4E5B9ull;
        key ^= key >> 27;
        key *= 0x94D049BB133111EBull;
        key ^= key >> 31;
        return static_cast<std::size_t>(key) & mask();
    }

    void place(std::uint64_t key, T* value) noexcept
    {
        std::size_t i = home(key);
        while (slots_[i].key != 0 && slots_[i].key != key)
        {
            i = (i + 1) & mask();
        }
        if (slots_[i].key == 0)
        {
            ++size_;
        }
        slots_[i] = slot{key, value};
    }

    [[nodiscard]] bool grow() noexcept
    {
        const std::size_t capacity = capacity_ == 0 ? 64 : capacity_ * 2;
        auto* slots = static_cast<slot*>(memory_.allocate(sizeof(slot) * capacity, alignof(slot)));
        if (slots == nullptr)
        {
            return false;
        }
        for (std::size_t i = 0; i < capacity; ++i)
        {
            ::new (static_cast<void*>(slots + i)) slot{};
        }
        slot* old = std::exchange(slots_, slots);
        const std::size_t old_capacity = std::exchange(capacity_, capacity);
        size_ = 0;
        for (std::size_t i = 0; i < old_capacity; ++i)
        {
            if (old[i].key != 0)
            {
                place(old[i].key, old[i].value);
            }
        }
        memory_.deallocate(old, sizeof(slot) * old_capacity, alignof(slot));
        return true;
    }

    void release() noexcept
    {
        memory_.deallocate(slots_, sizeof(slot) * capacity_, alignof(slot));
        slots_ = nullptr;
        capacity_ = 0;
        size_ = 0;
    }

    memory memory_;
    slot* slots_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

} // namespace vgl::common

#endif // VGL_COMMON_POINTER_MAP_HPP
