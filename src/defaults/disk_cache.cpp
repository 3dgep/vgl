// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The disk cache (vgl/defaults.h). Each cached read is a file, <directory>/<hh>/<hash>.vglc,
// named by a 64-bit hash of its URI and range, holding that key, so that a collision reads as a
// miss, and a checksum. An index of the entries, by hash, with their sizes and when they were
// last used, is built from the directory at creation and kept in memory; the files'
// modification times carry the use across runs.
//
// A read that misses goes to the source through a request of the cache's own
// (vgl_io_request_create), whose completion delivers the data to the context's request at once
// and then stores it. Every read in progress holds a reference to the cache, so a source may
// complete reads after the cache is destroyed: they deliver, and store nothing.

#include "common/host_memory.hpp"
#include "defaults/files.hpp"

#include <vgl/defaults.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

using vgl::common::memory;

struct vgl_disk_cache;

namespace
{

constexpr std::uint64_t default_budget = 1ull * 1024 * 1024 * 1024;
constexpr std::uint32_t entry_magic = 0x434C4756u; // "VGLC", in its bytes.
constexpr std::uint32_t entry_version = 1;
constexpr std::size_t header_size = 32;
constexpr std::size_t hash_digits = 16;
constexpr const char* entry_suffix = ".vglc";
constexpr std::size_t entry_suffix_length = 5;

bool starts_with_scheme(const char* uri, const char* scheme) noexcept
{
    for (std::size_t i = 0; scheme[i] != '\0'; ++i)
    {
        const char c = uri[i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != scheme[i])
        {
            return false;
        }
    }
    return true;
}

bool is_http(const char* uri) noexcept
{
    return starts_with_scheme(uri, "http://") || starts_with_scheme(uri, "https://");
}

// The manifest is the one file of a pack that changes in place (pack format, section 5.2).
bool is_manifest(const char* uri) noexcept
{
    constexpr std::size_t n = 10; // "/pack.vglm"
    const std::size_t length = std::strlen(uri);
    return length >= n && std::memcmp(uri + length - n, "/pack.vglm", n) == 0;
}

constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t h = fnv_offset) noexcept
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        h = (h ^ p[i]) * fnv_prime;
    }
    return h;
}

void put32(unsigned char* p, std::uint32_t v) noexcept
{
    for (int i = 0; i < 4; ++i)
    {
        p[i] = static_cast<unsigned char>(v >> (8 * i));
    }
}

void put64(unsigned char* p, std::uint64_t v) noexcept
{
    for (int i = 0; i < 8; ++i)
    {
        p[i] = static_cast<unsigned char>(v >> (8 * i));
    }
}

std::uint32_t get32(const unsigned char* p) noexcept
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

std::uint64_t get64(const unsigned char* p) noexcept
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

// Appends v in decimal at out[n], which must have room.
void put_decimal(char* out, std::size_t& n, std::uint64_t v) noexcept
{
    std::array<char, 20> digits{};
    std::size_t k = 0;
    for (;;)
    {
        digits[k++] = static_cast<char>('0' + v % 10);
        v /= 10;
        if (v == 0)
        {
            break;
        }
    }
    while (k != 0)
    {
        out[n++] = digits[--k];
    }
}

bool parse_hex(const char* s, std::size_t n, std::uint64_t& out) noexcept
{
    out = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const char c = s[i];
        std::uint64_t d = 0;
        if (c >= '0' && c <= '9')
        {
            d = static_cast<std::uint64_t>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            d = static_cast<std::uint64_t>(c - 'a') + 10;
        }
        else
        {
            return false;
        }
        out = (out << 4) | d;
    }
    return true;
}

void put_hex(char* out, std::uint64_t v, std::size_t digits) noexcept
{
    constexpr const char* hex = "0123456789abcdef";
    for (std::size_t i = 0; i < digits; ++i)
    {
        out[digits - 1 - i] = hex[(v >> (4 * i)) & 0xF];
    }
}

std::int64_t now() noexcept
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

// The entries, by hash: open addressing with linear probing.
class entry_index
{
public:
    struct slot
    {
        std::uint64_t hash = 0;
        std::uint64_t size = 0; // Of the file.
        std::int64_t used = 0;  // Seconds since 1970.
        bool full = false;
    };

    explicit entry_index(const memory& m) noexcept : m_(m) {}
    entry_index(const entry_index&) = delete;
    entry_index& operator=(const entry_index&) = delete;
    entry_index(entry_index&&) = delete;
    entry_index& operator=(entry_index&&) = delete;
    ~entry_index() { free_slots(slots_, capacity_); }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    [[nodiscard]] slot* find(std::uint64_t hash) const noexcept
    {
        if (capacity_ == 0)
        {
            return nullptr;
        }
        for (std::size_t i = home(hash);; i = (i + 1) & (capacity_ - 1))
        {
            if (!slots_[i].full)
            {
                return nullptr;
            }
            if (slots_[i].hash == hash)
            {
                return &slots_[i];
            }
        }
    }

    // The slot for hash, new and zeroed but for its hash if it was not there. Null if the
    // index could not grow.
    [[nodiscard]] slot* insert(std::uint64_t hash) noexcept
    {
        if (slot* s = find(hash); s != nullptr)
        {
            return s;
        }
        if ((count_ + 1) * 10 > capacity_ * 7 && !grow())
        {
            return nullptr;
        }
        std::size_t i = home(hash);
        while (slots_[i].full)
        {
            i = (i + 1) & (capacity_ - 1);
        }
        slots_[i] = slot{hash, 0, 0, true};
        ++count_;
        return &slots_[i];
    }

    void erase(std::uint64_t hash) noexcept
    {
        slot* s = find(hash);
        if (s == nullptr)
        {
            return;
        }
        // Backward-shift deletion keeps every probe sequence unbroken.
        auto i = static_cast<std::size_t>(s - slots_);
        slots_[i].full = false;
        --count_;
        for (std::size_t j = (i + 1) & (capacity_ - 1); slots_[j].full;
             j = (j + 1) & (capacity_ - 1))
        {
            const std::size_t k = home(slots_[j].hash);
            const bool moves = i <= j ? (k <= i || k > j) : (k <= i && k > j);
            if (moves)
            {
                slots_[i] = slots_[j];
                slots_[j].full = false;
                i = j;
            }
        }
    }

    void clear() noexcept
    {
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            slots_[i].full = false;
        }
        count_ = 0;
    }

    template <typename F>
    void for_each(const F& f) const
    {
        for (std::size_t i = 0; i < capacity_; ++i)
        {
            if (slots_[i].full)
            {
                f(slots_[i]);
            }
        }
    }

private:
    [[nodiscard]] std::size_t home(std::uint64_t hash) const noexcept
    {
        return static_cast<std::size_t>(hash ^ (hash >> 32)) & (capacity_ - 1);
    }

    void free_slots(slot* s, std::size_t n) const noexcept
    {
        m_.deallocate(s, n * sizeof(slot), alignof(slot));
    }

    bool grow() noexcept
    {
        const std::size_t capacity = capacity_ == 0 ? 256 : capacity_ * 2;
        auto* slots = static_cast<slot*>(m_.allocate(capacity * sizeof(slot), alignof(slot)));
        if (slots == nullptr)
        {
            return false;
        }
        for (std::size_t i = 0; i < capacity; ++i)
        {
            ::new (&slots[i]) slot();
        }
        slot* old = slots_;
        const std::size_t old_capacity = capacity_;
        slots_ = slots;
        capacity_ = capacity;
        count_ = 0;
        for (std::size_t i = 0; i < old_capacity; ++i)
        {
            if (old[i].full)
            {
                std::size_t j = home(old[i].hash);
                while (slots_[j].full)
                {
                    j = (j + 1) & (capacity_ - 1);
                }
                slots_[j] = old[i];
                ++count_;
            }
        }
        free_slots(old, old_capacity);
        return true;
    }

    const memory& m_;
    slot* slots_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t count_ = 0;
};

// A read from the time it arrives until it is delivered and, if it went to the source,
// stored.
struct cache_op
{
    vgl_disk_cache* cache = nullptr;
    vgl_io_request* outer = nullptr; // The context's request.
    cache_op* previous = nullptr;    // In the list of reads not yet delivered.
    cache_op* next = nullptr;
    bool delivered = false; // Under the cache's mutex.
    char* uri = nullptr;
    std::size_t uri_size = 0;
    vgl_io_read read{};
    std::uint64_t hash = 0;
    char* key = nullptr; // The entry's key: "<uri>\n<offset>\n<size>".
    std::size_t key_allocation = 0;
    std::size_t key_length = 0;
    bool manifest = false;
    // The source's failure, for a manifest served from the disk instead.
    vgl_result source_error = VGL_RESULT_SUCCESS;
    // What the source delivered, to store.
    unsigned char* data = nullptr;
    std::size_t data_allocation = 0;
    std::size_t data_size = 0;
};

} // namespace

struct vgl_disk_cache
{
    explicit vgl_disk_cache(const memory& m) noexcept : allocator(m), index(allocator) {}

    memory allocator;
    vgl_allocator_interface allocator_table{}; // For the source requests; empty for the C heap.
    bool has_host_allocator = false;
    std::atomic<std::uint32_t> references{1};
    char* directory = nullptr;
    std::size_t directory_size = 0;
    vgl_io_interface source{};
    std::uint64_t budget = default_budget;
    bool has_pool = false;
    vgl_task_interface pool{};

    std::mutex mutex;
    std::condition_variable disk_idle;
    entry_index index;           // Under the mutex, as everything below.
    std::uint32_t disk_work = 0; // Disk work queued or running, which destroy waits for.
    std::uint64_t bytes = 0;
    bool alive = true;
    cache_op* undelivered = nullptr;
    std::uint64_t temporaries = 0; // Names of files being written.
    vgl_disk_cache_statistics counts{};

    void retain() noexcept { references.fetch_add(1, std::memory_order_relaxed); }

    void release() noexcept
    {
        if (references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            const memory m = allocator;
            m.deallocate(directory, directory_size, 1);
            m.destroy(this);
        }
    }

    // The path of an entry, or of a file next to it with suffix; freed with free_path.
    [[nodiscard]] char* path_of(std::uint64_t hash, const char* suffix, std::size_t& out_size) const
    {
        const std::size_t base = directory_size - 1;
        const std::size_t suffix_length = std::strlen(suffix);
        out_size = base + 1 + 2 + 1 + hash_digits + suffix_length + 1;
        auto* p = static_cast<char*>(allocator.allocate(out_size, 1));
        if (p == nullptr)
        {
            return nullptr;
        }
        std::memcpy(p, directory, base);
        std::size_t n = base;
        p[n++] = '/';
        put_hex(p + n, hash >> 56, 2);
        n += 2;
        p[n++] = '/';
        put_hex(p + n, hash, hash_digits);
        n += hash_digits;
        std::memcpy(p + n, suffix, suffix_length + 1);
        return p;
    }

    void free_path(char* p, std::size_t size) const noexcept { allocator.deallocate(p, size, 1); }

    void free_op(cache_op* op) const noexcept
    {
        allocator.deallocate(op->data, op->data_allocation, 16);
        allocator.deallocate(op->key, op->key_allocation, 1);
        allocator.deallocate(op->uri, op->uri_size, 1);
        allocator.destroy(op);
    }

    // Ends a read's work, dropping its reference.
    static void finish(cache_op* op) noexcept
    {
        vgl_disk_cache* self = op->cache;
        self->free_op(op);
        self->release();
    }

    // Completes the context's request, once: a cancel may have done so already.
    static void deliver(cache_op* op, vgl_result result, const void* data, std::size_t size)
    {
        vgl_disk_cache* self = op->cache;
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            if (op->delivered)
            {
                return;
            }
            op->delivered = true;
            self->unlink(op);
        }
        static_cast<void>(vgl_io_request_complete(op->outer, result, data, size));
    }

    void unlink(cache_op* op) noexcept
    {
        if (op->previous != nullptr)
        {
            op->previous->next = op->next;
        }
        else
        {
            undelivered = op->next;
        }
        if (op->next != nullptr)
        {
            op->next->previous = op->previous;
        }
        op->previous = nullptr;
        op->next = nullptr;
    }

    // Runs disk work on the pool, or here. The work ends with disk_done.
    void on_disk(void (*fn)(void*), cache_op* op)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            ++disk_work;
        }
        if (!has_pool || pool.submit(pool.user_data, fn, op) != VGL_RESULT_SUCCESS)
        {
            fn(op);
        }
    }

    void disk_done() noexcept
    {
        const std::lock_guard<std::mutex> lock(mutex);
        --disk_work;
        disk_idle.notify_all();
    }

    // Removes an entry that is missing or does not hold its key; under the mutex.
    void forget(std::uint64_t hash) noexcept
    {
        if (const entry_index::slot* s = index.find(hash); s != nullptr)
        {
            bytes -= s->size;
            index.erase(hash);
        }
    }

    // Reads an entry's data into memory, of out_allocation bytes, to free. Returns false if it
    // is missing or not what it should be.
    bool load(const cache_op* op,
              unsigned char*& out,
              std::size_t& out_size,
              std::size_t& out_allocation) const
    {
        out = nullptr;
        out_size = 0;
        out_allocation = 0;
        std::size_t path_size = 0;
        char* path = path_of(op->hash, entry_suffix, path_size);
        if (path == nullptr)
        {
            return false;
        }
        vgl_result error = VGL_RESULT_SUCCESS;
        std::FILE* f = vgl::defaults::file_open(path, "rb", allocator, error);
        free_path(path, path_size);
        if (f == nullptr)
        {
            return false;
        }
        std::array<unsigned char, header_size> header{};
        bool ok = std::fread(header.data(), 1, header.size(), f) == header.size() &&
                  get32(header.data()) == entry_magic &&
                  get32(header.data() + 4) == entry_version &&
                  get32(header.data() + 8) == op->key_length;
        const std::uint64_t size = ok ? get64(header.data() + 16) : 0;
        ok = ok && size <= SIZE_MAX - op->key_length;
        const std::size_t total = ok ? op->key_length + static_cast<std::size_t>(size) : 0;
        const std::size_t allocation = total == 0 ? 1 : total;
        unsigned char* data =
            ok ? static_cast<unsigned char*>(allocator.allocate(allocation, 16)) : nullptr;
        ok = ok && data != nullptr && std::fread(data, 1, total, f) == total &&
             std::memcmp(data, op->key, op->key_length) == 0 &&
             fnv1a(data, total) == get64(header.data() + 24) && std::fgetc(f) == EOF;
        static_cast<void>(std::fclose(f));
        if (!ok)
        {
            allocator.deallocate(data, allocation, 16);
            return false;
        }
        // The key comes first; move the data to the front.
        std::memmove(data, data + op->key_length, static_cast<std::size_t>(size));
        out = data;
        out_size = static_cast<std::size_t>(size);
        out_allocation = allocation;
        return true;
    }

    // Serves a read from its entry, on the pool or here; if that fails, a shard goes to the
    // source and a manifest whose source failed gets that failure.
    static void load_task(void* data)
    {
        auto* op = static_cast<cache_op*>(data);
        vgl_disk_cache* self = op->cache;
        unsigned char* bytes = nullptr;
        std::size_t size = 0;
        std::size_t allocation = 0;
        if (self->load(op, bytes, size, allocation))
        {
            {
                const std::lock_guard<std::mutex> lock(self->mutex);
                if (op->manifest)
                {
                    ++self->counts.offline_hits;
                }
                else
                {
                    ++self->counts.hits;
                }
                if (entry_index::slot* s = self->index.find(op->hash); s != nullptr)
                {
                    s->used = now();
                }
            }
            deliver(op, VGL_RESULT_SUCCESS, size == 0 ? nullptr : bytes, size);
            self->allocator.deallocate(bytes, allocation, 16);
            std::size_t path_size = 0;
            if (char* path = self->path_of(op->hash, entry_suffix, path_size); path != nullptr)
            {
                vgl::defaults::file_touch(path, self->allocator);
                self->free_path(path, path_size);
            }
            self->disk_done();
            finish(op);
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            self->forget(op->hash);
        }
        self->disk_done();
        if (op->manifest)
        {
            deliver(op, op->source_error, nullptr, 0);
            finish(op);
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            ++self->counts.misses;
        }
        fetch(op);
    }

    // Passes a read to the source.
    static void fetch(cache_op* op)
    {
        vgl_disk_cache* self = op->cache;
        vgl_io_request_create_info info{};
        info.struct_size = sizeof(info);
        info.complete = &fetched;
        info.user_data = op;
        info.allocator = self->has_host_allocator ? &self->allocator_table : nullptr;
        vgl_io_request* inner = nullptr;
        if (vgl_io_request_create(&info, &inner) != VGL_RESULT_SUCCESS)
        {
            deliver(op, VGL_RESULT_OUT_OF_MEMORY, nullptr, 0);
            finish(op);
            return;
        }
        self->source.read(self->source.user_data, inner, &op->read);
    }

    // What the source delivered: to the context at once, then to the disk.
    static void fetched(void* user_data, vgl_result result, const void* data, std::size_t size)
    {
        auto* op = static_cast<cache_op*>(user_data);
        vgl_disk_cache* self = op->cache;
        if (result == VGL_RESULT_SUCCESS)
        {
            deliver(op, VGL_RESULT_SUCCESS, data, size);
            bool alive = false;
            {
                const std::lock_guard<std::mutex> lock(self->mutex);
                alive = self->alive;
            }
            if (alive)
            {
                op->data_allocation = size == 0 ? 1 : size;
                op->data =
                    static_cast<unsigned char*>(self->allocator.allocate(op->data_allocation, 16));
                if (op->data != nullptr)
                {
                    if (size != 0)
                    {
                        std::memcpy(op->data, data, size);
                    }
                    op->data_size = size;
                    self->on_disk(&store_task, op);
                    return;
                }
                op->data_allocation = 0;
            }
            finish(op);
            return;
        }
        // A manifest the server could not give: the stored one, if there is one. A missing
        // manifest is missing, not offline.
        if (op->manifest && result != VGL_RESULT_NOT_FOUND)
        {
            bool stored = false;
            {
                const std::lock_guard<std::mutex> lock(self->mutex);
                stored = self->alive && self->index.find(op->hash) != nullptr;
            }
            if (stored)
            {
                op->source_error = result;
                self->on_disk(&load_task, op);
                return;
            }
        }
        deliver(op, result, nullptr, 0);
        finish(op);
    }

    // Writes what the source delivered as an entry, then keeps to the budget.
    static void store_task(void* data)
    {
        auto* op = static_cast<cache_op*>(data);
        op->cache->store(op);
        op->cache->disk_done();
        finish(op);
    }

    void store(const cache_op* op)
    {
        const std::size_t size = op->data_size;
        std::uint64_t temporary = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            temporary = ++temporaries;
        }
        std::array<char, 24> suffix{};
        std::size_t n = 0;
        suffix[n++] = '.';
        put_decimal(suffix.data(), n, temporary);
        std::memcpy(suffix.data() + n, ".tmp", 5);
        std::size_t temp_size = 0;
        std::size_t path_size = 0;
        char* temp = path_of(op->hash, suffix.data(), temp_size);
        char* path = path_of(op->hash, entry_suffix, path_size);
        bool ok = temp != nullptr && path != nullptr;
        if (ok)
        {
            // The bucket's directory, made on first use.
            const std::size_t bucket = directory_size - 1 + 3;
            const char saved = temp[bucket];
            temp[bucket] = '\0';
            ok = vgl::defaults::directories_create(temp, allocator);
            temp[bucket] = saved;
        }
        vgl_result error = VGL_RESULT_SUCCESS;
        std::FILE* f = ok ? vgl::defaults::file_open(temp, "wb", allocator, error) : nullptr;
        if (f != nullptr)
        {
            std::array<unsigned char, header_size> header{};
            put32(header.data(), entry_magic);
            put32(header.data() + 4, entry_version);
            put32(header.data() + 8, static_cast<std::uint32_t>(op->key_length));
            put64(header.data() + 16, size);
            const std::uint64_t check = fnv1a(op->data, size, fnv1a(op->key, op->key_length));
            put64(header.data() + 24, check);
            ok = std::fwrite(header.data(), 1, header.size(), f) == header.size() &&
                 std::fwrite(op->key, 1, op->key_length, f) == op->key_length &&
                 (size == 0 || std::fwrite(op->data, 1, size, f) == size);
            ok = std::fclose(f) == 0 && ok;
            ok = ok && vgl::defaults::file_replace(temp, path, allocator);
            if (!ok)
            {
                static_cast<void>(vgl::defaults::file_remove(temp, allocator));
            }
        }
        else
        {
            ok = false;
        }
        free_path(temp, temp_size);
        free_path(path, path_size);
        if (!ok)
        {
            return;
        }
        const std::uint64_t file_size = header_size + op->key_length + size;
        hash_list victims;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (!alive)
            {
                return;
            }
            entry_index::slot* s = index.insert(op->hash);
            if (s == nullptr)
            {
                return; // Not indexed: found again by the next run's scan.
            }
            bytes = bytes - s->size + file_size;
            s->size = file_size;
            s->used = now();
            if (bytes > budget)
            {
                victims = choose_victims();
            }
        }
        // The files go after the lock is released: removing thousands of them takes long, and
        // the context's thread takes the lock for every read.
        remove_files(victims);
    }

    // Hashes of entries whose files are to be removed.
    struct hash_list
    {
        std::uint64_t* hashes = nullptr;
        std::size_t count = 0;
        std::size_t capacity = 0;
    };

    // Removes the files of the entries listed, and frees the list. Returns false if some could
    // not be removed.
    bool remove_files(hash_list& list) const
    {
        bool removed = true;
        for (std::size_t i = 0; i < list.count; ++i)
        {
            std::size_t path_size = 0;
            char* path = path_of(list.hashes[i], entry_suffix, path_size);
            removed = removed && path != nullptr && vgl::defaults::file_remove(path, allocator);
            free_path(path, path_size);
        }
        allocator.deallocate(
            list.hashes, list.capacity * sizeof(std::uint64_t), alignof(std::uint64_t));
        list = hash_list{};
        return removed;
    }

    // Takes the least recently used entries out of the index until the rest fit in nine tenths
    // of the budget, and lists them for remove_files; under the mutex. An entry stored again
    // before its file goes is lost with it, and read again when next wanted.
    [[nodiscard]] hash_list choose_victims()
    {
        struct victim
        {
            std::int64_t used;
            std::uint64_t hash;
            std::uint64_t size;
        };
        hash_list list;
        const std::size_t count = index.size();
        auto* all =
            static_cast<victim*>(allocator.allocate(count * sizeof(victim), alignof(victim)));
        list.hashes = static_cast<std::uint64_t*>(
            allocator.allocate(count * sizeof(std::uint64_t), alignof(std::uint64_t)));
        if (all == nullptr || list.hashes == nullptr)
        {
            allocator.deallocate(all, count * sizeof(victim), alignof(victim));
            allocator.deallocate(
                list.hashes, count * sizeof(std::uint64_t), alignof(std::uint64_t));
            return hash_list{};
        }
        list.capacity = count;
        std::size_t n = 0;
        index.for_each(
            [&](const entry_index::slot& e) { all[n++] = victim{e.used, e.hash, e.size}; });
        std::sort(all, all + n, [](const victim& x, const victim& y) { return x.used < y.used; });
        const std::uint64_t target = budget / 10 * 9;
        for (std::size_t i = 0; i < n && bytes > target; ++i)
        {
            forget(all[i].hash);
            list.hashes[list.count++] = all[i].hash;
            ++counts.evictions;
        }
        allocator.deallocate(all, count * sizeof(victim), alignof(victim));
        return list;
    }

    // Indexes the entries in the directory, and removes files left half-written.
    struct scan
    {
        vgl_disk_cache* self = nullptr;
        const char* bucket = nullptr; // While listing one.
        bool failed = false;
    };

    static void scan_bucket_entry(void* user_data, const vgl::defaults::directory_entry& e)
    {
        auto* s = static_cast<scan*>(user_data);
        vgl_disk_cache* self = s->self;
        const std::size_t length = std::strlen(e.name);
        std::uint64_t hash = 0;
        if (!e.directory && length == hash_digits + entry_suffix_length &&
            std::memcmp(e.name + hash_digits, entry_suffix, entry_suffix_length) == 0 &&
            parse_hex(e.name, hash_digits, hash))
        {
            if (entry_index::slot* slot = self->index.insert(hash); slot != nullptr)
            {
                self->bytes += e.size;
                slot->size = e.size;
                slot->used = e.modified;
            }
            else
            {
                s->failed = true;
            }
            return;
        }
        if (!e.directory && length > 4 && std::memcmp(e.name + length - 4, ".tmp", 4) == 0)
        {
            // A write that never finished.
            const std::size_t bucket_length = std::strlen(s->bucket);
            const std::size_t size = bucket_length + 1 + length + 1;
            auto* path = static_cast<char*>(self->allocator.allocate(size, 1));
            if (path != nullptr)
            {
                std::memcpy(path, s->bucket, bucket_length);
                path[bucket_length] = '/';
                std::memcpy(path + bucket_length + 1, e.name, length + 1);
                static_cast<void>(vgl::defaults::file_remove(path, self->allocator));
                self->allocator.deallocate(path, size, 1);
            }
        }
    }

    static void scan_root_entry(void* user_data, const vgl::defaults::directory_entry& e)
    {
        auto* s = static_cast<scan*>(user_data);
        vgl_disk_cache* self = s->self;
        std::uint64_t ignored = 0;
        if (!e.directory || std::strlen(e.name) != 2 || !parse_hex(e.name, 2, ignored))
        {
            return;
        }
        const std::size_t base = self->directory_size - 1;
        const std::size_t size = base + 4;
        auto* bucket = static_cast<char*>(self->allocator.allocate(size, 1));
        if (bucket == nullptr)
        {
            s->failed = true;
            return;
        }
        std::memcpy(bucket, self->directory, base);
        bucket[base] = '/';
        bucket[base + 1] = e.name[0];
        bucket[base + 2] = e.name[1];
        bucket[base + 3] = '\0';
        scan inner{self, bucket, false};
        if (!vgl::defaults::directory_list(bucket, self->allocator, &scan_bucket_entry, &inner) ||
            inner.failed)
        {
            s->failed = true;
        }
        self->allocator.deallocate(bucket, size, 1);
    }

    // The interface's callbacks.

    static void read(void* user_data, vgl_io_request* outer, const vgl_io_read* r)
    {
        auto* self = static_cast<vgl_disk_cache*>(user_data);
        if (!is_http(r->uri))
        {
            self->source.read(self->source.user_data, outer, r);
            return;
        }
        auto* op = self->allocator.create<cache_op>();
        if (op == nullptr)
        {
            static_cast<void>(vgl_io_request_complete(outer, VGL_RESULT_OUT_OF_MEMORY, nullptr, 0));
            return;
        }
        op->cache = self;
        op->outer = outer;
        op->manifest = is_manifest(r->uri);
        op->uri_size = std::strlen(r->uri) + 1;
        op->uri = static_cast<char*>(self->allocator.allocate(op->uri_size, 1));
        // Room for the URI and two numbers of up to 20 digits, each after a newline.
        op->key_allocation = op->uri_size - 1 + std::size_t{2} * 21;
        op->key = static_cast<char*>(self->allocator.allocate(op->key_allocation, 1));
        if (op->uri == nullptr || op->key == nullptr)
        {
            self->free_op(op);
            static_cast<void>(vgl_io_request_complete(outer, VGL_RESULT_OUT_OF_MEMORY, nullptr, 0));
            return;
        }
        std::memcpy(op->uri, r->uri, op->uri_size);
        op->read = {};
        op->read.uri = op->uri;
        op->read.offset = r->offset;
        op->read.size = r->size;
        std::size_t n = op->uri_size - 1;
        std::memcpy(op->key, r->uri, n);
        op->key[n++] = '\n';
        put_decimal(op->key, n, r->offset);
        op->key[n++] = '\n';
        put_decimal(op->key, n, r->size);
        op->key_length = n;
        op->hash = fnv1a(op->key, op->key_length);
        self->retain();
        bool stored = false;
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            op->next = self->undelivered;
            if (self->undelivered != nullptr)
            {
                self->undelivered->previous = op;
            }
            self->undelivered = op;
            stored = !op->manifest && self->index.find(op->hash) != nullptr;
            if (!op->manifest && !stored)
            {
                ++self->counts.misses;
            }
        }
        if (stored)
        {
            self->on_disk(&load_task, op);
        }
        else
        {
            fetch(op);
        }
    }

    static void cancel(void* user_data, vgl_io_request* outer)
    {
        auto* self = static_cast<vgl_disk_cache*>(user_data);
        cache_op* found = nullptr;
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            for (cache_op* op = self->undelivered; op != nullptr; op = op->next)
            {
                if (op->outer == outer)
                {
                    found = op;
                    found->delivered = true;
                    self->unlink(found);
                    break;
                }
            }
        }
        if (found != nullptr)
        {
            // The download goes on, to be stored; the context hears now.
            static_cast<void>(vgl_io_request_complete(outer, VGL_RESULT_CANCELLED, nullptr, 0));
            return;
        }
        // Not one of the cache's: the source's own.
        if (self->source.cancel != nullptr)
        {
            self->source.cancel(self->source.user_data, outer);
        }
    }
};

extern "C" VGL_DEFAULTS_API vgl_result
vgl_disk_cache_create(const vgl_disk_cache_create_info* info_in, vgl_disk_cache** out_cache)
{
    if (out_cache == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_cache = nullptr;
    if (info_in == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_disk_cache_create_info info;
    vgl_result r = vgl::common::read_versioned(info_in, sizeof(vgl_disk_cache_create_info), info);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    if (info.directory == nullptr || info.directory[0] == '\0' || info.source == nullptr ||
        info.source->read == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (info.source->version == 0 || info.source->version > VGL_IO_INTERFACE_VERSION)
    {
        return VGL_RESULT_VERSION_MISMATCH;
    }
    memory m;
    r = vgl::common::make_memory(info.allocator, m);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    auto* cache = m.create<vgl_disk_cache>(m);
    if (cache == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    if (info.allocator != nullptr)
    {
        cache->allocator_table = *info.allocator;
        cache->has_host_allocator = true;
    }
    cache->source = *info.source;
    cache->budget = info.budget == 0 ? default_budget : info.budget;
    if (info.pool != nullptr)
    {
        cache->has_pool = true;
        static_cast<void>(vgl_thread_pool_task_interface_get(info.pool, &cache->pool));
    }
    // The directory without a trailing separator.
    std::size_t length = std::strlen(info.directory);
    while (length > 1 && (info.directory[length - 1] == '/' || info.directory[length - 1] == '\\'))
    {
        --length;
    }
    cache->directory_size = length + 1;
    cache->directory = static_cast<char*>(m.allocate(cache->directory_size, 1));
    if (cache->directory == nullptr)
    {
        cache->release();
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    std::memcpy(cache->directory, info.directory, length);
    cache->directory[length] = '\0';
    if (!vgl::defaults::directories_create(cache->directory, m))
    {
        cache->release();
        return VGL_RESULT_IO_ERROR;
    }
    vgl_disk_cache::scan s{cache, nullptr, false};
    if (!vgl::defaults::directory_list(cache->directory, m, &vgl_disk_cache::scan_root_entry, &s) ||
        s.failed)
    {
        cache->release();
        return VGL_RESULT_IO_ERROR;
    }
    *out_cache = cache;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result vgl_disk_cache_destroy(vgl_disk_cache* cache)
{
    if (cache == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    {
        std::unique_lock<std::mutex> lock(cache->mutex);
        cache->alive = false;
        cache->disk_idle.wait(lock, [cache] { return cache->disk_work == 0; });
    }
    cache->release();
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result
vgl_disk_cache_io_interface_get(vgl_disk_cache* cache, vgl_io_interface* out_interface)
{
    if (cache == nullptr || out_interface == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_interface = vgl_io_interface{VGL_IO_INTERFACE_VERSION,
                                      0,
                                      cache,
                                      &vgl_disk_cache::read,
                                      &vgl_disk_cache::cancel,
                                      nullptr};
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result
vgl_disk_cache_statistics_get(vgl_disk_cache* cache, vgl_disk_cache_statistics* out_statistics)
{
    if (cache == nullptr || out_statistics == nullptr || out_statistics->struct_size < 8)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_disk_cache_statistics s{};
    {
        const std::lock_guard<std::mutex> lock(cache->mutex);
        s = cache->counts;
        s.entries = cache->index.size();
        s.bytes = cache->bytes;
        s.budget = cache->budget;
    }
    const std::uint32_t size = out_statistics->struct_size;
    s.struct_size = size < sizeof(s) ? size : static_cast<std::uint32_t>(sizeof(s));
    std::memcpy(out_statistics, &s, s.struct_size);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result vgl_disk_cache_clear(vgl_disk_cache* cache)
{
    if (cache == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_disk_cache::hash_list all;
    {
        const std::lock_guard<std::mutex> lock(cache->mutex);
        const std::size_t count = cache->index.size();
        all.hashes = static_cast<std::uint64_t*>(
            cache->allocator.allocate(count * sizeof(std::uint64_t), alignof(std::uint64_t)));
        if (count != 0 && all.hashes == nullptr)
        {
            return VGL_RESULT_OUT_OF_MEMORY;
        }
        all.capacity = count;
        cache->index.for_each(
            [&](const entry_index::slot& e) { all.hashes[all.count++] = e.hash; });
        cache->index.clear();
        cache->bytes = 0;
    }
    // Files that remain are found again by the next scan.
    return cache->remove_files(all) ? VGL_RESULT_SUCCESS : VGL_RESULT_IO_ERROR;
}
