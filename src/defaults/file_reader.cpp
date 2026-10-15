// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The default filesystem reader (vgl/defaults.h). Each read opens the file, reads the range
// and closes it again, so the reader keeps no file handles between reads.

#include "defaults/defaults_memory.hpp"

#include <vgl/defaults.h>

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

using vgl::defaults::memory;

namespace
{

constexpr std::size_t file_scheme_length = 7; // "file://"

// The path of a URI: without a "file://" prefix, and on Windows without the slash before a
// drive letter ("file:///C:/data").
const char* uri_path(const char* uri) noexcept
{
    if (std::strncmp(uri, "file://", file_scheme_length) != 0)
    {
        return uri;
    }
    const char* path = uri + file_scheme_length;
#if defined(_WIN32)
    if (path[0] == '/' && path[1] != '\0' && path[2] == ':')
    {
        ++path;
    }
#endif
    return path;
}

#if defined(_WIN32)

// Converts UTF-8 to UTF-16 for _wfopen. Returns false for invalid UTF-8.
bool utf8_to_utf16(const char* in, const memory& m, wchar_t*& out, std::size_t& out_count) noexcept
{
    const std::size_t length = std::strlen(in);
    out_count = length + 1; // UTF-16 never needs more units than UTF-8 has bytes.
    out = static_cast<wchar_t*>(m.allocate(out_count * sizeof(wchar_t), alignof(wchar_t)));
    if (out == nullptr)
    {
        return false;
    }
    std::size_t o = 0;
    for (std::size_t i = 0; i < length;)
    {
        const auto b = static_cast<unsigned char>(in[i]);
        std::uint32_t cp = 0;
        std::size_t extra = 0;
        if (b < 0x80)
        {
            cp = b;
        }
        else if ((b & 0xE0) == 0xC0)
        {
            cp = b & 0x1Fu;
            extra = 1;
        }
        else if ((b & 0xF0) == 0xE0)
        {
            cp = b & 0x0Fu;
            extra = 2;
        }
        else if ((b & 0xF8) == 0xF0)
        {
            cp = b & 0x07u;
            extra = 3;
        }
        else
        {
            return false;
        }
        if (extra != 0 && i + extra >= length)
        {
            return false; // Truncated sequence.
        }
        for (std::size_t k = 1; k <= extra; ++k)
        {
            const auto c = static_cast<unsigned char>(in[i + k]);
            if ((c & 0xC0) != 0x80)
            {
                return false;
            }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        i += extra + 1;
        if (cp > 0x10FFFF)
        {
            return false;
        }
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out[o++] = static_cast<wchar_t>(0xD800 + (cp >> 10));
            out[o++] = static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        }
        else
        {
            out[o++] = static_cast<wchar_t>(cp);
        }
    }
    out[o] = L'\0';
    return true;
}

std::FILE* open_file(const char* path, const memory& m, vgl_result& error) noexcept
{
    wchar_t* wide = nullptr;
    std::size_t count = 0;
    std::FILE* f = nullptr;
    if (!utf8_to_utf16(path, m, wide, count))
    {
        error = wide == nullptr ? VGL_RESULT_OUT_OF_MEMORY : VGL_RESULT_IO_ERROR;
    }
    else if (_wfopen_s(&f, wide, L"rb") != 0)
    {
        f = nullptr;
        error = errno == ENOENT ? VGL_RESULT_NOT_FOUND : VGL_RESULT_IO_ERROR;
    }
    m.deallocate(wide, count * sizeof(wchar_t), alignof(wchar_t));
    return f;
}

int seek(std::FILE* f, std::uint64_t offset, int origin) noexcept
{
    return _fseeki64(f, static_cast<long long>(offset), origin);
}

std::int64_t tell(std::FILE* f) noexcept
{
    return _ftelli64(f);
}

#else

std::FILE* open_file(const char* path, const memory& /*m*/, vgl_result& error) noexcept
{
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        error = errno == ENOENT || errno == ENOTDIR ? VGL_RESULT_NOT_FOUND : VGL_RESULT_IO_ERROR;
    }
    return f;
}

int seek(std::FILE* f, std::uint64_t offset, int origin) noexcept
{
    return fseeko(f, static_cast<off_t>(offset), origin);
}

std::int64_t tell(std::FILE* f) noexcept
{
    return ftello(f);
}

#endif

// Reads the range and completes the request.
void read_and_complete(const memory& m, vgl_io_request* request, const vgl_io_read& r) noexcept
{
    vgl_result error = VGL_RESULT_SUCCESS;
    std::FILE* f = open_file(uri_path(r.uri), m, error);
    if (f == nullptr)
    {
        static_cast<void>(vgl_io_request_complete(request, error, nullptr, 0));
        return;
    }
    std::int64_t file_size = -1;
    if (seek(f, 0, SEEK_END) == 0)
    {
        file_size = tell(f);
    }
    if (file_size < 0)
    {
        static_cast<void>(std::fclose(f));
        static_cast<void>(vgl_io_request_complete(request, VGL_RESULT_IO_ERROR, nullptr, 0));
        return;
    }
    const auto end = static_cast<std::uint64_t>(file_size);
    const std::uint64_t offset = r.offset < end ? r.offset : end;
    const std::uint64_t size = r.size < end - offset ? r.size : end - offset;
    if (size > SIZE_MAX)
    {
        static_cast<void>(std::fclose(f));
        static_cast<void>(vgl_io_request_complete(request, VGL_RESULT_OUT_OF_MEMORY, nullptr, 0));
        return;
    }
    const auto count = static_cast<std::size_t>(size);
    void* data = count == 0 ? nullptr : m.allocate(count, 16);
    vgl_result result = VGL_RESULT_SUCCESS;
    if (count != 0 && data == nullptr)
    {
        result = VGL_RESULT_OUT_OF_MEMORY;
    }
    else if (count != 0 &&
             (seek(f, offset, SEEK_SET) != 0 || std::fread(data, 1, count, f) != count))
    {
        result = VGL_RESULT_IO_ERROR;
    }
    static_cast<void>(std::fclose(f));
    static_cast<void>(vgl_io_request_complete(request,
                                              result,
                                              result == VGL_RESULT_SUCCESS ? data : nullptr,
                                              result == VGL_RESULT_SUCCESS ? count : 0));
    m.deallocate(data, count, 16);
}

} // namespace

struct vgl_file_reader;

namespace
{

// A read waiting for, or running on, the pool.
struct pooled_read
{
    vgl_file_reader* reader = nullptr;
    vgl_io_request* request = nullptr;
    pooled_read* previous = nullptr;
    pooled_read* next = nullptr;
    bool cancelled = false;
    vgl_io_read read{};
    char* uri = nullptr;
    std::size_t uri_size = 0;
};

} // namespace

struct vgl_file_reader
{
    explicit vgl_file_reader(const memory& m) noexcept : allocator(m) {}

    memory allocator;
    bool has_pool = false;
    vgl_task_interface pool{};
    std::mutex mutex;
    std::condition_variable idle;
    pooled_read* queued = nullptr; // Reads not yet started, for cancel.
    std::uint32_t active = 0;      // Reads submitted and not finished.

    void free_read(pooled_read* p) const noexcept
    {
        allocator.deallocate(p->uri, p->uri_size, 1);
        allocator.destroy(p);
    }

    static void run(void* data) noexcept
    {
        auto* p = static_cast<pooled_read*>(data);
        vgl_file_reader* self = p->reader;
        bool cancelled = false;
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            cancelled = p->cancelled;
            if (p->previous != nullptr)
            {
                p->previous->next = p->next;
            }
            else
            {
                self->queued = p->next;
            }
            if (p->next != nullptr)
            {
                p->next->previous = p->previous;
            }
        }
        if (cancelled)
        {
            static_cast<void>(
                vgl_io_request_complete(p->request, VGL_RESULT_CANCELLED, nullptr, 0));
        }
        else
        {
            read_and_complete(self->allocator, p->request, p->read);
        }
        self->free_read(p);
        // Notify under the lock: once it is released, destroy may free the reader.
        const std::lock_guard<std::mutex> lock(self->mutex);
        --self->active;
        self->idle.notify_all();
    }

    static void read(void* user_data, vgl_io_request* request, const vgl_io_read* r)
    {
        auto* self = static_cast<vgl_file_reader*>(user_data);
        if (!self->has_pool)
        {
            read_and_complete(self->allocator, request, *r);
            return;
        }
        // The read description stays valid until completion, but copy the URI anyway: the
        // pool may run the read after the library has moved on.
        auto* p = self->allocator.create<pooled_read>();
        const std::size_t uri_size = std::strlen(r->uri) + 1;
        char* uri =
            p == nullptr ? nullptr : static_cast<char*>(self->allocator.allocate(uri_size, 1));
        if (uri == nullptr)
        {
            self->allocator.destroy(p);
            read_and_complete(self->allocator, request, *r);
            return;
        }
        std::memcpy(uri, r->uri, uri_size);
        p->reader = self;
        p->request = request;
        p->uri = uri;
        p->uri_size = uri_size;
        p->read = {uri, r->offset, r->size};
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            p->next = self->queued;
            if (self->queued != nullptr)
            {
                self->queued->previous = p;
            }
            self->queued = p;
            ++self->active;
        }
        if (self->pool.submit(self->pool.user_data, &run, p) != VGL_RESULT_SUCCESS)
        {
            // Read here instead; run unlinks the read and does the bookkeeping.
            run(p);
        }
    }

    static void cancel(void* user_data, vgl_io_request* request)
    {
        auto* self = static_cast<vgl_file_reader*>(user_data);
        const std::lock_guard<std::mutex> lock(self->mutex);
        for (pooled_read* p = self->queued; p != nullptr; p = p->next)
        {
            if (p->request == request)
            {
                p->cancelled = true;
                return;
            }
        }
    }
};

extern "C" VGL_DEFAULTS_API vgl_result
vgl_file_reader_create(const vgl_file_reader_create_info* info_in, vgl_file_reader** out_reader)
{
    if (out_reader == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_reader = nullptr;
    vgl_file_reader_create_info info;
    vgl_result r =
        vgl::defaults::read_versioned(info_in, sizeof(vgl_file_reader_create_info), info);
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
    auto* reader = m.create<vgl_file_reader>(m);
    if (reader == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    if (info.pool != nullptr)
    {
        reader->has_pool = true;
        static_cast<void>(vgl_thread_pool_task_interface_get(info.pool, &reader->pool));
    }
    *out_reader = reader;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result vgl_file_reader_destroy(vgl_file_reader* reader)
{
    if (reader == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    {
        std::unique_lock<std::mutex> lock(reader->mutex);
        reader->idle.wait(lock, [reader] { return reader->active == 0; });
    }
    const memory m = reader->allocator;
    m.destroy(reader);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result
vgl_file_reader_io_interface_get(vgl_file_reader* reader, vgl_io_interface* out_interface)
{
    if (reader == nullptr || out_interface == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_interface = vgl_io_interface{VGL_IO_INTERFACE_VERSION,
                                      0,
                                      reader,
                                      &vgl_file_reader::read,
                                      &vgl_file_reader::cancel,
                                      nullptr};
    return VGL_RESULT_SUCCESS;
}
