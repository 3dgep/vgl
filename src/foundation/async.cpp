// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "foundation/async.hpp"

#include "foundation/versioned.hpp"

#include <cstring>

namespace vgl::internal
{

async_core* async_core::create(const allocator& a) noexcept
{
    void* memory = a.allocate(sizeof(async_core), alignof(async_core));
    return memory == nullptr ? nullptr : ::new (memory) async_core(a);
}

async_core::async_core(const allocator& a) noexcept : allocator_(a), decompressors_(a) {}

async_core::~async_core()
{
    for (zstd_decompressor* d : decompressors_)
    {
        allocator_.destroy(d);
    }
}

void async_core::release() noexcept
{
    if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        const allocator a = allocator_;
        this->~async_core();
        a.deallocate(this, sizeof(async_core), alignof(async_core));
    }
}

bool async_core::push(async_item* item) noexcept
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!alive_)
    {
        return false;
    }
    item->next = nullptr;
    if (tail_ != nullptr)
    {
        tail_->next = item;
    }
    else
    {
        head_ = item;
    }
    tail_ = item;
    return true;
}

async_item* async_core::take_all() noexcept
{
    const std::lock_guard<std::mutex> lock(mutex_);
    async_item* items = head_;
    head_ = nullptr;
    tail_ = nullptr;
    return items;
}

bool async_core::begin_task() noexcept
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!alive_)
    {
        return false;
    }
    ++running_;
    return true;
}

bool async_core::end_task(async_item* result) noexcept
{
    // Notify while holding the lock: once it is released, shut_down may return and the core
    // may be freed (a queued result hands its reference to the queue), so this thread must not
    // touch the core after unlocking.
    const std::lock_guard<std::mutex> lock(mutex_);
    --running_;
    bool queued = false;
    if (alive_)
    {
        result->next = nullptr;
        if (tail_ != nullptr)
        {
            tail_->next = result;
        }
        else
        {
            head_ = result;
        }
        tail_ = result;
        queued = true;
    }
    idle_.notify_all();
    return queued;
}

async_item* async_core::shut_down() noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    alive_ = false;
    idle_.wait(lock, [this] { return running_ == 0; });
    async_item* items = head_;
    head_ = nullptr;
    tail_ = nullptr;
    return items;
}

zstd_decompressor* async_core::acquire_decompressor() noexcept
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!decompressors_.empty())
        {
            zstd_decompressor* d = decompressors_[decompressors_.size() - 1];
            decompressors_.pop_back();
            return d;
        }
    }
    auto* d = allocator_.create<zstd_decompressor>(allocator_);
    if (d != nullptr && !d->valid())
    {
        allocator_.destroy(d);
        return nullptr;
    }
    return d;
}

void async_core::release_decompressor(zstd_decompressor* d) noexcept
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!decompressors_.push_back(d))
    {
        allocator_.destroy(d);
    }
}

} // namespace vgl::internal

extern "C" VGL_API vgl_result vgl_io_request_complete(vgl_io_request* request,
                                                      vgl_result result,
                                                      const void* data,
                                                      std::size_t size)
{
    if (request == nullptr || (data == nullptr && size != 0))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (request->complete != nullptr)
    {
        // Made with vgl_io_request_create: its creator takes the result, and the request goes.
        request->complete(request->complete_user_data,
                          result,
                          result == VGL_RESULT_SUCCESS ? data : nullptr,
                          result == VGL_RESULT_SUCCESS ? size : 0);
        const vgl::internal::allocator memory = request->memory;
        memory.destroy(request);
        return VGL_RESULT_SUCCESS;
    }
    vgl::internal::async_core* core = request->core;
    vgl_result r = VGL_RESULT_SUCCESS;
    request->result = result;
    if (result == VGL_RESULT_SUCCESS)
    {
        if (request->data.allocate(core->memory(), size))
        {
            if (size != 0)
            {
                std::memcpy(request->data.data(), data, size);
            }
        }
        else
        {
            request->result = VGL_RESULT_OUT_OF_MEMORY;
            r = VGL_RESULT_OUT_OF_MEMORY;
        }
    }
    if (!core->push(request))
    {
        // The context is gone: discard the completion.
        core->memory().destroy(request);
        core->release();
    }
    return r;
}

extern "C" VGL_API vgl_result vgl_io_request_create(const vgl_io_request_create_info* info,
                                                    vgl_io_request** out_request)
{
    if (out_request == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_request = nullptr;
    if (info == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_io_request_create_info settings;
    vgl_result r =
        vgl::internal::read_versioned(info, sizeof(vgl_io_request_create_info), settings);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    if (settings.complete == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl::internal::allocator memory;
    if (settings.allocator != nullptr)
    {
        r = vgl::internal::validate_allocator_interface(settings.allocator);
        if (r != VGL_RESULT_SUCCESS)
        {
            return r;
        }
        memory = vgl::internal::allocator(*settings.allocator);
    }
    auto* request = memory.create<vgl_io_request>();
    if (request == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    request->complete = settings.complete;
    request->complete_user_data = settings.user_data;
    request->memory = memory;
    *out_request = request;
    return VGL_RESULT_SUCCESS;
}
