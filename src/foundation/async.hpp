// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// State shared between a context and its asynchronous work: I/O requests the host completes
// on any thread, and tasks it runs on any thread (docs/c-api.md, section 12).
//
// Completions are queued under a mutex and processed by the context on its own thread. The
// core is reference counted: every outstanding request and task holds a reference, so a host
// may complete a request or run a task after the context is destroyed. The work then finds
// the core shut down, frees itself and drops its reference. Shutting down waits only for tasks
// that are running, never for the host.

#ifndef VGL_FOUNDATION_ASYNC_HPP
#define VGL_FOUNDATION_ASYNC_HPP

#include "decoders/zstd_decompressor.hpp"
#include "foundation/allocator.hpp"
#include "foundation/buffer.hpp"
#include "foundation/containers.hpp"

#include <vgl/foundation.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace vgl::internal
{

class async_core;

// Something that completes asynchronously and is queued back to the context.
struct async_item
{
    enum class kind : std::uint8_t
    {
        io,
        task,    // A decode job.
        mesh,    // A mesh job.
        texture, // A texture job.
    };

    explicit async_item(kind k) noexcept : item_kind(k) {}

    async_item* next = nullptr;
    kind item_kind;
};

class async_core
{
public:
    // Creates a core with one reference, for the context. Returns null on failure.
    [[nodiscard]] static async_core* create(const allocator& a) noexcept;

    async_core(const async_core&) = delete;
    async_core& operator=(const async_core&) = delete;
    async_core(async_core&&) = delete;
    async_core& operator=(async_core&&) = delete;

    void retain() noexcept { references_.fetch_add(1, std::memory_order_relaxed); }

    // Drops a reference; the last one frees the core.
    void release() noexcept;

    [[nodiscard]] const allocator& memory() const noexcept { return allocator_; }

    // Queues a completed item. Returns false if the core is shut down; the caller then frees
    // the item and releases its reference.
    [[nodiscard]] bool push(async_item* item) noexcept;

    // Takes every queued item, oldest first.
    [[nodiscard]] async_item* take_all() noexcept;

    // Brackets a task's work. begin returns false if the core is shut down; the task then
    // frees itself. end queues the result, or returns false if the core was shut down
    // meanwhile, in which case the task frees itself.
    [[nodiscard]] bool begin_task() noexcept;
    [[nodiscard]] bool end_task(async_item* result) noexcept;

    // Stops accepting completions and waits until no task is running. Queued items are
    // returned for the caller to free.
    [[nodiscard]] async_item* shut_down() noexcept;

    // A decompressor for one decode, from a pool. Returns null on failure.
    [[nodiscard]] zstd_decompressor* acquire_decompressor() noexcept;
    void release_decompressor(zstd_decompressor* d) noexcept;

private:
    explicit async_core(const allocator& a) noexcept;
    ~async_core();

    allocator allocator_;
    std::atomic<std::uint32_t> references_{1};
    std::mutex mutex_;
    std::condition_variable idle_;
    bool alive_ = true;
    std::uint32_t running_ = 0;
    async_item* head_ = nullptr;
    async_item* tail_ = nullptr;
    vector<zstd_decompressor*> decompressors_;
};

// What an I/O request is for, so that its completion can be routed.
struct io_target
{
    enum class kind : std::uint8_t
    {
        manifest,
        shard_head,
        shard_directory,
        tile,
    };

    kind target_kind = kind::manifest;
    std::uint32_t pack_id = 0;
    std::uint64_t shard_key = 0;
    std::uint64_t tile_key = 0;
};

} // namespace vgl::internal

// The public request handle (vgl/foundation.h).
struct vgl_io_request : vgl::internal::async_item
{
    vgl_io_request() noexcept : async_item(kind::io) {}

    vgl::internal::async_core* core = nullptr;
    std::uint64_t id = 0;
    vgl::internal::io_target target;
    vgl::internal::buffer<char> uri;
    vgl_io_read read{};
    vgl_result result = VGL_RESULT_SUCCESS;
    vgl::internal::buffer<std::byte> data;
};

#endif // VGL_FOUNDATION_ASYNC_HPP
