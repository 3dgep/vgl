// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The default thread pool (vgl/defaults.h): worker threads taking tasks from one queue.

#include "defaults/defaults_memory.hpp"

#include <vgl/defaults.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

using vgl::defaults::memory;

namespace
{

struct task
{
    task* next = nullptr;
    vgl_task_fn function = nullptr;
    void* data = nullptr;
};

// Starts a thread in place. Returns false if it could not be started.
template <typename F>
bool start_thread(std::thread* where, F&& f) noexcept
{
#if defined(__cpp_exceptions)
    try
    {
        ::new (where) std::thread(std::forward<F>(f));
    }
    catch (...)
    {
        return false;
    }
#else
    ::new (where) std::thread(std::forward<F>(f));
#endif
    return true;
}

} // namespace

struct vgl_thread_pool
{
    explicit vgl_thread_pool(const memory& m) noexcept : allocator(m) {}

    memory allocator;
    std::mutex mutex;
    std::condition_variable wake;
    task* head = nullptr;
    task* tail = nullptr;
    bool stopping = false;
    std::thread* threads = nullptr;
    std::uint32_t thread_count = 0;

    void run() noexcept
    {
        for (;;)
        {
            task* t = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping || head != nullptr; });
                if (head == nullptr)
                {
                    return; // Stopping, and the queue is empty.
                }
                t = head;
                head = t->next;
                if (head == nullptr)
                {
                    tail = nullptr;
                }
            }
            const vgl_task_fn function = t->function;
            void* data = t->data;
            allocator.destroy(t);
            function(data);
        }
    }

    // Stops and joins the first count threads.
    void stop(std::uint32_t count) noexcept
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            threads[i].join();
            threads[i].~thread();
        }
        allocator.deallocate(threads, sizeof(std::thread) * thread_count, alignof(std::thread));
        threads = nullptr;
    }

    static vgl_result submit(void* user_data, vgl_task_fn function, void* data)
    {
        auto* pool = static_cast<vgl_thread_pool*>(user_data);
        if (function == nullptr)
        {
            return VGL_RESULT_INVALID_ARGUMENT;
        }
        auto* t = pool->allocator.create<task>();
        if (t == nullptr)
        {
            return VGL_RESULT_OUT_OF_MEMORY;
        }
        t->function = function;
        t->data = data;
        {
            const std::lock_guard<std::mutex> lock(pool->mutex);
            if (pool->tail != nullptr)
            {
                pool->tail->next = t;
            }
            else
            {
                pool->head = t;
            }
            pool->tail = t;
        }
        pool->wake.notify_one();
        return VGL_RESULT_SUCCESS;
    }
};

extern "C" VGL_DEFAULTS_API vgl_result
vgl_thread_pool_create(const vgl_thread_pool_create_info* info_in, vgl_thread_pool** out_pool)
{
    if (out_pool == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_pool = nullptr;
    vgl_thread_pool_create_info info;
    vgl_result r =
        vgl::defaults::read_versioned(info_in, sizeof(vgl_thread_pool_create_info), info);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    memory m;
    r = vgl::defaults::make_memory(info.allocator, m);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    std::uint32_t count = info.thread_count;
    if (count == 0)
    {
        const unsigned hardware = std::thread::hardware_concurrency();
        count = hardware > 2 ? hardware - 1 : 1;
    }

    auto* pool = m.create<vgl_thread_pool>(m);
    if (pool == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    pool->thread_count = count;
    pool->threads =
        static_cast<std::thread*>(m.allocate(sizeof(std::thread) * count, alignof(std::thread)));
    if (pool->threads == nullptr)
    {
        m.destroy(pool);
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (!start_thread(&pool->threads[i], [pool] { pool->run(); }))
        {
            pool->stop(i);
            m.destroy(pool);
            return VGL_RESULT_OUT_OF_MEMORY;
        }
    }
    *out_pool = pool;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result vgl_thread_pool_destroy(vgl_thread_pool* pool)
{
    if (pool == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    // The threads drain the queue before they return.
    pool->stop(pool->thread_count);
    const memory m = pool->allocator;
    m.destroy(pool);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result
vgl_thread_pool_task_interface_get(vgl_thread_pool* pool, vgl_task_interface* out_interface)
{
    if (pool == nullptr || out_interface == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_interface =
        vgl_task_interface{VGL_TASK_INTERFACE_VERSION, 0, pool, &vgl_thread_pool::submit, nullptr};
    return VGL_RESULT_SUCCESS;
}
