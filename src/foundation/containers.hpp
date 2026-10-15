// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Containers for the library core. Standard containers cannot be used there: they allocate
// through std::allocator rather than the host allocator (NFR-03), and they cannot report a
// failed allocation without exceptions (NFR-05). These allocate through a vgl allocator, and
// every operation that allocates returns false when the allocation fails, leaving the
// container unchanged.

#ifndef VGL_FOUNDATION_CONTAINERS_HPP
#define VGL_FOUNDATION_CONTAINERS_HPP

#include "foundation/allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace vgl::internal
{

// T and V may be pointer types, for which sizeof(T) and the conversion of T* to void* are
// intended.
// NOLINTBEGIN(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)

// Growable array. T must be nothrow move-constructible.
template <typename T>
class vector
{
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>,
                  "vector elements must move without exceptions");

public:
    vector() noexcept = default;
    explicit vector(const allocator& a) noexcept : allocator_(a) {}
    vector(const vector&) = delete;
    vector& operator=(const vector&) = delete;

    vector(vector&& o) noexcept
        : allocator_(o.allocator_), data_(std::exchange(o.data_, nullptr)),
          size_(std::exchange(o.size_, 0)), capacity_(std::exchange(o.capacity_, 0))
    {
    }

    vector& operator=(vector&& o) noexcept
    {
        if (this != &o)
        {
            release();
            allocator_ = o.allocator_;
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
            capacity_ = std::exchange(o.capacity_, 0);
        }
        return *this;
    }

    ~vector() { release(); }

    [[nodiscard]] bool reserve(std::size_t capacity) noexcept
    {
        if (capacity <= capacity_)
        {
            return true;
        }
        if (capacity > static_cast<std::size_t>(-1) / sizeof(T))
        {
            return false;
        }
        T* data = static_cast<T*>(allocator_.allocate(capacity * sizeof(T), alignof(T)));
        if (data == nullptr)
        {
            return false;
        }
        for (std::size_t i = 0; i < size_; ++i)
        {
            ::new (static_cast<void*>(data + i)) T(std::move(data_[i]));
            data_[i].~T();
        }
        if (data_ != nullptr)
        {
            allocator_.deallocate(data_, capacity_ * sizeof(T), alignof(T));
        }
        data_ = data;
        capacity_ = capacity;
        return true;
    }

    template <typename... Args>
    [[nodiscard]] bool emplace_back(Args&&... args) noexcept
    {
        static_assert(std::is_nothrow_constructible_v<T, Args...>,
                      "vector elements must construct without exceptions");
        if (size_ == capacity_ && !reserve(capacity_ < 8 ? 8 : capacity_ * 2))
        {
            return false;
        }
        ::new (static_cast<void*>(data_ + size_)) T(std::forward<Args>(args)...);
        ++size_;
        return true;
    }

    [[nodiscard]] bool push_back(T value) noexcept { return emplace_back(std::move(value)); }

    void pop_back() noexcept
    {
        --size_;
        data_[size_].~T();
    }

    // Removes element i by moving the last element into its place.
    void erase_unordered(std::size_t i) noexcept
    {
        if (i + 1 != size_)
        {
            data_[i] = std::move(data_[size_ - 1]);
        }
        pop_back();
    }

    void clear() noexcept
    {
        while (size_ != 0)
        {
            pop_back();
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] T& operator[](std::size_t i) noexcept { return data_[i]; }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return data_[i]; }
    [[nodiscard]] T* begin() noexcept { return data_; }
    [[nodiscard]] T* end() noexcept { return data_ + size_; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] const T* end() const noexcept { return data_ + size_; }

private:
    void release() noexcept
    {
        clear();
        if (data_ != nullptr)
        {
            allocator_.deallocate(data_, capacity_ * sizeof(T), alignof(T));
            data_ = nullptr;
            capacity_ = 0;
        }
    }

    allocator allocator_;
    T* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};

// Hash map from 64-bit keys to values, with open addressing and linear probing. V must be
// nothrow move-constructible. Pointers to values stay valid until the next insertion.
template <typename V>
class u64_map
{
    static_assert(std::is_nothrow_move_constructible_v<V> && std::is_nothrow_destructible_v<V>,
                  "map values must move without exceptions");

public:
    u64_map() noexcept = default;
    explicit u64_map(const allocator& a) noexcept : allocator_(a) {}
    u64_map(const u64_map&) = delete;
    u64_map& operator=(const u64_map&) = delete;
    u64_map(u64_map&&) = delete;
    u64_map& operator=(u64_map&&) = delete;
    ~u64_map() { release(); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] V* find(std::uint64_t key) noexcept
    {
        const std::size_t i = locate(key);
        return i == npos ? nullptr : value(i);
    }

    [[nodiscard]] const V* find(std::uint64_t key) const noexcept
    {
        const std::size_t i = locate(key);
        return i == npos ? nullptr : value(i);
    }

    // Inserts a value for a key that is not in the map. Returns the stored value, or null if
    // allocation failed.
    [[nodiscard]] V* insert(std::uint64_t key, V v) noexcept
    {
        if ((size_ + tombstones_ + 1) * 4 > capacity_ * 3 &&
            !rehash(capacity_ < 16 ? 16 : capacity_ * 2))
        {
            return nullptr;
        }
        std::size_t i = hash(key) & (capacity_ - 1);
        while (states_[i] == full)
        {
            i = (i + 1) & (capacity_ - 1);
        }
        if (states_[i] == deleted)
        {
            --tombstones_;
        }
        states_[i] = full;
        keys_[i] = key;
        ::new (static_cast<void*>(value(i))) V(std::move(v));
        ++size_;
        return value(i);
    }

    // Removes a key. Returns false if it was not in the map.
    bool erase(std::uint64_t key) noexcept
    {
        const std::size_t i = locate(key);
        if (i == npos)
        {
            return false;
        }
        value(i)->~V();
        states_[i] = deleted;
        --size_;
        ++tombstones_;
        return true;
    }

    // Calls f(key, value) for every entry. f must not insert or erase.
    template <typename F>
    void for_each(const F& f) noexcept
    {
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            if (states_[i] == full)
            {
                f(keys_[i], *value(i));
            }
        }
    }

    void clear() noexcept
    {
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            if (states_[i] == full)
            {
                value(i)->~V();
            }
            states_[i] = empty;
        }
        size_ = 0;
        tombstones_ = 0;
    }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    static constexpr std::uint8_t empty = 0;
    static constexpr std::uint8_t full = 1;
    static constexpr std::uint8_t deleted = 2;

    static std::size_t hash(std::uint64_t k) noexcept
    {
        // splitmix64 finalizer.
        k ^= k >> 30;
        k *= 0xBF58476D1CE4E5B9ull;
        k ^= k >> 27;
        k *= 0x94D049BB133111EBull;
        k ^= k >> 31;
        return static_cast<std::size_t>(k);
    }

    [[nodiscard]] V* value(std::size_t i) const noexcept
    {
        return std::launder(reinterpret_cast<V*>(values_ + i * sizeof(V)));
    }

    [[nodiscard]] std::size_t locate(std::uint64_t key) const noexcept
    {
        if (capacity_ == 0)
        {
            return npos;
        }
        std::size_t i = hash(key) & (capacity_ - 1);
        for (std::size_t probes = 0; probes < capacity_ && states_[i] != empty; ++probes)
        {
            if (states_[i] == full && keys_[i] == key)
            {
                return i;
            }
            i = (i + 1) & (capacity_ - 1);
        }
        return npos;
    }

    [[nodiscard]] bool rehash(std::size_t capacity) noexcept
    {
        auto* states = static_cast<std::uint8_t*>(allocator_.allocate(capacity, 1));
        auto* keys = static_cast<std::uint64_t*>(
            allocator_.allocate(capacity * sizeof(std::uint64_t), alignof(std::uint64_t)));
        auto* values =
            static_cast<unsigned char*>(allocator_.allocate(capacity * sizeof(V), alignof(V)));
        if (states == nullptr || keys == nullptr || values == nullptr)
        {
            allocator_.deallocate(states, capacity, 1);
            allocator_.deallocate(keys, capacity * sizeof(std::uint64_t), alignof(std::uint64_t));
            allocator_.deallocate(values, capacity * sizeof(V), alignof(V));
            return false;
        }
        for (std::size_t i = 0; i < capacity; ++i)
        {
            states[i] = empty;
        }
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            if (states_[i] != full)
            {
                continue;
            }
            std::size_t j = hash(keys_[i]) & (capacity - 1);
            while (states[j] == full)
            {
                j = (j + 1) & (capacity - 1);
            }
            states[j] = full;
            keys[j] = keys_[i];
            ::new (static_cast<void*>(values + j * sizeof(V))) V(std::move(*value(i)));
            value(i)->~V();
        }
        free_storage();
        states_ = states;
        keys_ = keys;
        values_ = values;
        capacity_ = capacity;
        tombstones_ = 0;
        return true;
    }

    void free_storage() noexcept
    {
        if (capacity_ != 0)
        {
            allocator_.deallocate(states_, capacity_, 1);
            allocator_.deallocate(keys_, capacity_ * sizeof(std::uint64_t), alignof(std::uint64_t));
            allocator_.deallocate(values_, capacity_ * sizeof(V), alignof(V));
        }
    }

    void release() noexcept
    {
        clear();
        free_storage();
        states_ = nullptr;
        keys_ = nullptr;
        values_ = nullptr;
        capacity_ = 0;
    }

    allocator allocator_;
    std::uint8_t* states_ = nullptr;
    std::uint64_t* keys_ = nullptr;
    unsigned char* values_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    std::size_t tombstones_ = 0;
};

// NOLINTEND(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)

} // namespace vgl::internal

#endif // VGL_FOUNDATION_CONTAINERS_HPP
