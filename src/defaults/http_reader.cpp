// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The HTTP reader (vgl/defaults.h). With libcurl: range requests on its multi interface, driven
// by one thread of the reader's own. Reads arrive from any thread, wait in a list under the
// mutex and are started, cancelled and completed by that thread, which alone touches libcurl's
// handles. On Emscripten: range requests on the browser's fetch, on the main thread.

#include "common/host_memory.hpp"

#include <vgl/defaults.h>

#if defined(VGL_DEFAULTS_HAS_CURL)

#    include <curl/curl.h>

#    include <array>
#    include <cmath>
#    include <cstdint>
#    include <cstring>
#    include <mutex>
#    include <thread>

using vgl::common::memory;

struct vgl_http_reader;

namespace
{

constexpr std::uint32_t default_max_connections = 8;
constexpr double default_timeout = 30.0;
constexpr int poll_milliseconds = 100;
constexpr const char* default_user_agent = "vgl/" VGL_VERSION_STRING;

// Whether a URI is read over HTTP: it starts with "http://" or "https://", in any case.
bool is_http(const char* uri) noexcept
{
    const auto starts = [uri](const char* scheme) noexcept {
        for (std::size_t i = 0; scheme[i] != '\0'; ++i)
        {
            const char c = uri[i];
            if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != scheme[i])
            {
                return false;
            }
        }
        return true;
    };
    return starts("http://") || starts("https://");
}

// A range for a Range header: "first-last", or "first-" to the end, terminated.
using range_text = std::array<char, 48>;

range_text make_range(std::uint64_t offset, std::uint64_t size) noexcept
{
    range_text out{};
    std::size_t n = 0;
    const auto put = [&out, &n](std::uint64_t v) noexcept {
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
    };
    put(offset);
    out[n++] = '-';
    if (size != VGL_IO_SIZE_TO_END)
    {
        put(offset + size - 1);
    }
    return out;
}

char* copy_string(const memory& m, const char* s, std::size_t& out_size) noexcept
{
    out_size = std::strlen(s) + 1;
    auto* copy = static_cast<char*>(m.allocate(out_size, 1));
    if (copy != nullptr)
    {
        std::memcpy(copy, s, out_size);
    }
    return copy;
}

// A read from the time it arrives until it completes.
struct http_read
{
    vgl_http_reader* reader = nullptr;
    vgl_io_request* request = nullptr;
    http_read* previous = nullptr;
    http_read* next = nullptr;
    char* uri = nullptr;
    std::size_t uri_size = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    // Under the reader's mutex.
    bool cancelled = false;

    // The reader's thread only.
    CURL* easy = nullptr;
    unsigned char* data = nullptr;
    std::size_t capacity = 0;
    std::size_t length = 0;
    std::uint64_t body = 0; // Bytes of the body received, for a server that ignores ranges.
    bool out_of_memory = false;
    bool have_all = false; // The range is complete; the rest of the body was refused.
};

} // namespace

struct vgl_http_reader
{
    explicit vgl_http_reader(const memory& m) noexcept : allocator(m) {}

    memory allocator;
    vgl_io_interface files{};
    bool has_files = false;
    std::uint32_t max_connections = default_max_connections;
    long timeout_ms = 0;
    char* user_agent = nullptr;
    std::size_t user_agent_size = 0;
    CURLM* multi = nullptr;
    std::thread thread;
    bool thread_started = false;

    std::mutex mutex;
    http_read* incoming = nullptr; // Reads not yet started, newest first.
    http_read* active = nullptr;   // Reads with a transfer.
    bool cancel_pending = false;
    bool stopping = false;

    void free_read(http_read* p) const noexcept
    {
        allocator.deallocate(p->data, p->capacity, 16);
        allocator.deallocate(p->uri, p->uri_size, 1);
        allocator.destroy(p);
    }

    static void unlink(http_read*& list, http_read* p) noexcept
    {
        if (p->previous != nullptr)
        {
            p->previous->next = p->next;
        }
        else
        {
            list = p->next;
        }
        if (p->next != nullptr)
        {
            p->next->previous = p->previous;
        }
        p->previous = nullptr;
        p->next = nullptr;
    }

    static void push(http_read*& list, http_read* p) noexcept
    {
        p->previous = nullptr;
        p->next = list;
        if (list != nullptr)
        {
            list->previous = p;
        }
        list = p;
    }

    // Completes a read that is in no list, and frees it.
    void complete(http_read* p, vgl_result result, const void* data, std::size_t size) const
    {
        static_cast<void>(vgl_io_request_complete(p->request, result, data, size));
        free_read(p);
    }

    // Grows a read's buffer to hold at least count bytes.
    bool reserve(http_read* p, std::size_t count) const noexcept
    {
        if (count <= p->capacity)
        {
            return true;
        }
        std::size_t capacity = p->capacity < 4096 ? 4096 : p->capacity;
        while (capacity < count)
        {
            capacity = capacity > SIZE_MAX / 2 ? count : capacity * 2;
        }
        if (p->size != VGL_IO_SIZE_TO_END && capacity > p->size)
        {
            capacity = static_cast<std::size_t>(p->size);
        }
        auto* data = static_cast<unsigned char*>(allocator.allocate(capacity, 16));
        if (data == nullptr)
        {
            return false;
        }
        if (p->length != 0)
        {
            std::memcpy(data, p->data, p->length);
        }
        allocator.deallocate(p->data, p->capacity, 16);
        p->data = data;
        p->capacity = capacity;
        return true;
    }

    // Keeps the part of a chunk of the body that falls in the range.
    static std::size_t write(char* chunk, std::size_t one, std::size_t count, void* user_data)
    {
        auto* p = static_cast<http_read*>(user_data);
        const std::size_t n = one * count;
        long status = 0;
        static_cast<void>(curl_easy_getinfo(p->easy, CURLINFO_RESPONSE_CODE, &status));
        if (status != 200 && status != 206)
        {
            return n; // An error page; the status decides the result.
        }
        // A 206 body starts at the offset; a 200 one, from a server that ignored the range, at
        // the start of the file.
        std::uint64_t skip = 0;
        if (status == 200)
        {
            const std::uint64_t at = p->body;
            p->body += n;
            if (at + n <= p->offset)
            {
                return n;
            }
            skip = at < p->offset ? p->offset - at : 0;
        }
        std::uint64_t keep = n - skip;
        const std::uint64_t room = p->size == VGL_IO_SIZE_TO_END ? UINT64_MAX : p->size - p->length;
        if (keep >= room)
        {
            keep = room;
            p->have_all = true;
        }
        if (keep != 0)
        {
            if (p->length + keep > SIZE_MAX ||
                !p->reader->reserve(p, static_cast<std::size_t>(p->length + keep)))
            {
                p->out_of_memory = true;
                return CURL_WRITEFUNC_ERROR;
            }
            std::memcpy(p->data + p->length, chunk + skip, static_cast<std::size_t>(keep));
            p->length += static_cast<std::size_t>(keep);
        }
        // Refusing the rest of a whole file ends the transfer early, which finish takes as
        // success.
        return status == 200 && p->have_all ? CURL_WRITEFUNC_ERROR : n;
    }

    // Sets up a read's transfer. Returns false if libcurl could not.
    bool start(http_read* p) const noexcept
    {
        p->easy = curl_easy_init();
        if (p->easy == nullptr)
        {
            return false;
        }
        const range_text range = make_range(p->offset, p->size);
        CURL* e = p->easy;
        bool ok = curl_easy_setopt(e, CURLOPT_URL, p->uri) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_RANGE, range.data()) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &write) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_WRITEDATA, p) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_PRIVATE, p) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_MAXREDIRS, 8L) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, timeout_ms) == CURLE_OK;
        ok = ok && curl_easy_setopt(e, CURLOPT_USERAGENT, user_agent) == CURLE_OK;
        ok = ok && curl_multi_add_handle(multi, e) == CURLM_OK;
        if (!ok)
        {
            curl_easy_cleanup(e);
            p->easy = nullptr;
        }
        return ok;
    }

    // Ends a transfer and frees its handle.
    void stop(http_read* p) const noexcept
    {
        static_cast<void>(curl_multi_remove_handle(multi, p->easy));
        curl_easy_cleanup(p->easy);
        p->easy = nullptr;
    }

    // Completes a read whose transfer is done.
    void finish(http_read* p, CURLcode code) const
    {
        long status = 0;
        static_cast<void>(curl_easy_getinfo(p->easy, CURLINFO_RESPONSE_CODE, &status));
        vgl_result result = VGL_RESULT_IO_ERROR;
        bool deliver = false;
        if (p->out_of_memory)
        {
            result = VGL_RESULT_OUT_OF_MEMORY;
        }
        else if (code == CURLE_OK || (code == CURLE_WRITE_ERROR && p->have_all))
        {
            if (status == 200 || status == 206)
            {
                result = VGL_RESULT_SUCCESS;
                deliver = true;
            }
            else if (status == 404 || status == 410)
            {
                result = VGL_RESULT_NOT_FOUND;
            }
            else if (status == 416)
            {
                result = VGL_RESULT_SUCCESS; // The range starts past the end of the file.
            }
        }
        stop(p);
        complete(p, result, deliver ? p->data : nullptr, deliver ? p->length : 0);
    }

    void run()
    {
        for (;;)
        {
            http_read* started = nullptr;
            http_read* cancelled = nullptr;
            bool stop_now = false;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                stop_now = stopping;
                while (incoming != nullptr)
                {
                    http_read* p = incoming;
                    unlink(incoming, p);
                    push(p->cancelled || stop_now ? cancelled : started, p);
                }
                if (cancel_pending || stop_now)
                {
                    cancel_pending = false;
                    for (http_read* p = active; p != nullptr;)
                    {
                        http_read* following = p->next;
                        if (p->cancelled || stop_now)
                        {
                            unlink(active, p);
                            push(cancelled, p);
                        }
                        p = following;
                    }
                }
            }
            while (cancelled != nullptr)
            {
                http_read* p = cancelled;
                unlink(cancelled, p);
                if (p->easy != nullptr)
                {
                    stop(p);
                }
                complete(p, VGL_RESULT_CANCELLED, nullptr, 0);
            }
            if (stop_now)
            {
                return;
            }
            while (started != nullptr)
            {
                http_read* p = started;
                unlink(started, p);
                if (!start(p))
                {
                    complete(p, VGL_RESULT_IO_ERROR, nullptr, 0);
                    continue;
                }
                const std::lock_guard<std::mutex> lock(mutex);
                push(active, p);
            }

            int running = 0;
            static_cast<void>(curl_multi_perform(multi, &running));
            int queued = 0;
            while (CURLMsg* message = curl_multi_info_read(multi, &queued))
            {
                if (message->msg != CURLMSG_DONE)
                {
                    continue;
                }
                void* data = nullptr;
                static_cast<void>(curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &data));
                auto* p = static_cast<http_read*>(data);
                const CURLcode code = message->data.result;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    unlink(active, p);
                }
                finish(p, code);
            }
            static_cast<void>(curl_multi_poll(multi, nullptr, 0, poll_milliseconds, nullptr));
        }
    }

    static void read(void* user_data, vgl_io_request* request, const vgl_io_read* r)
    {
        auto* self = static_cast<vgl_http_reader*>(user_data);
        if (!is_http(r->uri))
        {
            if (self->has_files)
            {
                self->files.read(self->files.user_data, request, r);
            }
            else
            {
                static_cast<void>(
                    vgl_io_request_complete(request, VGL_RESULT_NOT_FOUND, nullptr, 0));
            }
            return;
        }
        if (r->size == 0)
        {
            static_cast<void>(vgl_io_request_complete(request, VGL_RESULT_SUCCESS, nullptr, 0));
            return;
        }
        if (r->size != VGL_IO_SIZE_TO_END &&
            (r->size > SIZE_MAX || r->offset > UINT64_MAX - r->size))
        {
            static_cast<void>(
                vgl_io_request_complete(request, VGL_RESULT_OUT_OF_MEMORY, nullptr, 0));
            return;
        }
        auto* p = self->allocator.create<http_read>();
        if (p != nullptr)
        {
            p->uri = copy_string(self->allocator, r->uri, p->uri_size);
        }
        if (p == nullptr || p->uri == nullptr)
        {
            self->allocator.destroy(p);
            static_cast<void>(
                vgl_io_request_complete(request, VGL_RESULT_OUT_OF_MEMORY, nullptr, 0));
            return;
        }
        p->reader = self;
        p->request = request;
        p->offset = r->offset;
        p->size = r->size;
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            push(self->incoming, p);
        }
        static_cast<void>(curl_multi_wakeup(self->multi));
    }

    static void cancel(void* user_data, vgl_io_request* request)
    {
        auto* self = static_cast<vgl_http_reader*>(user_data);
        bool found = false;
        {
            const std::lock_guard<std::mutex> lock(self->mutex);
            for (http_read* list : {self->incoming, self->active})
            {
                for (http_read* p = list; p != nullptr; p = p->next)
                {
                    if (p->request == request)
                    {
                        p->cancelled = true;
                        self->cancel_pending = true;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (found)
        {
            static_cast<void>(curl_multi_wakeup(self->multi));
        }
        // The read may be the file reader's; cancelling one it does not have does nothing.
        if (self->has_files && self->files.cancel != nullptr)
        {
            self->files.cancel(self->files.user_data, request);
        }
    }
};

namespace
{

// Starts the reader's thread. Returns false if it could not be started.
bool start_thread(vgl_http_reader* reader) noexcept
{
#    if defined(__cpp_exceptions)
    try
    {
        reader->thread = std::thread([reader] { reader->run(); });
    }
    catch (...)
    {
        return false;
    }
#    else
    reader->thread = std::thread([reader] { reader->run(); });
#    endif
    return true;
}

// Frees a reader whose thread is not running.
void free_reader(vgl_http_reader* reader) noexcept
{
    if (reader->multi != nullptr)
    {
        static_cast<void>(curl_multi_cleanup(reader->multi));
    }
    curl_global_cleanup();
    const memory m = reader->allocator;
    m.deallocate(reader->user_agent, reader->user_agent_size, 1);
    m.destroy(reader);
}

} // namespace

extern "C" VGL_DEFAULTS_API vgl_result
vgl_http_reader_create(const vgl_http_reader_create_info* info_in, vgl_http_reader** out_reader)
{
    if (out_reader == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_reader = nullptr;
    vgl_http_reader_create_info info;
    vgl_result r = vgl::common::read_versioned(info_in, sizeof(vgl_http_reader_create_info), info);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    if (!std::isfinite(info.timeout) || info.timeout < 0.0)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    memory m;
    r = vgl::common::make_memory(info.allocator, m);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        return VGL_RESULT_IO_ERROR;
    }
    auto* reader = m.create<vgl_http_reader>(m);
    if (reader == nullptr)
    {
        curl_global_cleanup();
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    if (info.files != nullptr)
    {
        reader->has_files = true;
        static_cast<void>(vgl_file_reader_io_interface_get(info.files, &reader->files));
    }
    if (info.max_connections != 0)
    {
        reader->max_connections = info.max_connections;
    }
    const double timeout = info.timeout == 0.0 ? default_timeout : info.timeout;
    // A day at most, which keeps the milliseconds in a 32-bit long.
    reader->timeout_ms =
        static_cast<long>(std::ceil((timeout < 86400.0 ? timeout : 86400.0) * 1000.0));
    reader->user_agent =
        copy_string(m,
                    info.user_agent != nullptr ? info.user_agent : default_user_agent,
                    reader->user_agent_size);
    reader->multi = curl_multi_init();
    if (reader->user_agent == nullptr || reader->multi == nullptr)
    {
        const bool memory_failed = reader->user_agent == nullptr;
        free_reader(reader);
        return memory_failed ? VGL_RESULT_OUT_OF_MEMORY : VGL_RESULT_IO_ERROR;
    }
    static_cast<void>(curl_multi_setopt(
        reader->multi, CURLMOPT_MAX_HOST_CONNECTIONS, static_cast<long>(reader->max_connections)));
    if (!start_thread(reader))
    {
        free_reader(reader);
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    *out_reader = reader;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result vgl_http_reader_destroy(vgl_http_reader* reader)
{
    if (reader == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    {
        const std::lock_guard<std::mutex> lock(reader->mutex);
        reader->stopping = true;
    }
    static_cast<void>(curl_multi_wakeup(reader->multi));
    reader->thread.join();
    free_reader(reader);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result
vgl_http_reader_io_interface_get(vgl_http_reader* reader, vgl_io_interface* out_interface)
{
    if (reader == nullptr || out_interface == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_interface = vgl_io_interface{VGL_IO_INTERFACE_VERSION,
                                      0,
                                      reader,
                                      &vgl_http_reader::read,
                                      &vgl_http_reader::cancel,
                                      nullptr};
    return VGL_RESULT_SUCCESS;
}

#elif defined(VGL_DEFAULTS_HAS_FETCH)

// On Emscripten, reads go through the browser's fetch (emscripten_fetch), which needs no thread
// of the reader's own: fetches start and finish on the main runtime thread, between frames, and
// reads and cancels that arrive on other threads are proxied to it.

#    include <emscripten/eventloop.h>
#    include <emscripten/fetch.h>
#    include <emscripten/proxying.h>
#    include <emscripten/threading.h>

#    include <array>
#    include <cmath>
#    include <cstdint>
#    include <cstring>

using vgl::common::memory;

struct vgl_http_reader;

namespace
{

constexpr double default_timeout = 30.0;
// Tries for a read that fails without an HTTP status, and the wait before the next, which grows
// with each.
constexpr std::uint32_t max_attempts = 4;
constexpr double retry_milliseconds = 50.0;

bool is_http(const char* uri) noexcept
{
    const auto starts = [uri](const char* scheme) noexcept {
        for (std::size_t i = 0; scheme[i] != '\0'; ++i)
        {
            const char c = uri[i];
            if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != scheme[i])
            {
                return false;
            }
        }
        return true;
    };
    return starts("http://") || starts("https://");
}

using range_text = std::array<char, 48>;

// A range for a Range header: "first-last", or "first-" to the end, terminated.
range_text make_range(std::uint64_t offset, std::uint64_t size) noexcept
{
    range_text out{};
    std::size_t n = 0;
    const auto put = [&out, &n](std::uint64_t v) noexcept {
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
    };
    put(offset);
    out[n++] = '-';
    if (size != VGL_IO_SIZE_TO_END)
    {
        put(offset + size - 1);
    }
    return out;
}

// A read from the time it arrives until it completes. Touched on the main thread only, once
// it has been handed there.
struct fetch_read
{
    vgl_http_reader* reader = nullptr;
    vgl_io_request* request = nullptr;
    fetch_read* previous = nullptr;
    fetch_read* next = nullptr;
    char* uri = nullptr;
    std::size_t uri_size = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::array<char, 54> range{}; // "bytes=" and the range.
    std::array<const char*, 3> headers{};
    emscripten_fetch_t* fetch = nullptr;
    bool cancelled = false;
    bool closing = false; // emscripten_fetch_close is running, and reports the end itself.
    std::uint32_t attempts = 1;
    int retry_timer = 0; // While waiting to try again, with no fetch.
};

// A cancel on its way to the main thread.
struct fetch_cancel
{
    vgl_http_reader* reader = nullptr;
    vgl_io_request* request = nullptr;
};

// Runs f(arg) on the main runtime thread: at once when called there, and otherwise queued, or,
// with wait, queued and waited for.
void on_main(void (*f)(void*), void* arg, bool wait) noexcept
{
    if (emscripten_is_main_runtime_thread())
    {
        f(arg);
        return;
    }
    em_proxying_queue* q = emscripten_proxy_get_system_queue();
    const pthread_t main = emscripten_main_runtime_thread_id();
    static_cast<void>(wait ? emscripten_proxy_sync(q, main, f, arg)
                           : emscripten_proxy_async(q, main, f, arg));
}

} // namespace

struct vgl_http_reader
{
    explicit vgl_http_reader(const memory& m) noexcept : allocator(m) {}

    memory allocator;
    vgl_io_interface files{};
    bool has_files = false;
    std::uint32_t timeout_ms = 0;
    fetch_read* active = nullptr; // Reads with a fetch, on the main thread.

    void free_read(fetch_read* p) const noexcept
    {
        allocator.deallocate(p->uri, p->uri_size, 1);
        allocator.destroy(p);
    }

    void unlink(fetch_read* p) noexcept
    {
        if (p->previous != nullptr)
        {
            p->previous->next = p->next;
        }
        else
        {
            active = p->next;
        }
        if (p->next != nullptr)
        {
            p->next->previous = p->previous;
        }
        p->previous = nullptr;
        p->next = nullptr;
    }

    [[nodiscard]] fetch_read* find(vgl_io_request* request) const noexcept
    {
        for (fetch_read* p = active; p != nullptr; p = p->next)
        {
            if (p->request == request)
            {
                return p;
            }
        }
        return nullptr;
    }

    // Completes a read when its fetch ends, however it ends.
    static void done(emscripten_fetch_t* f)
    {
        auto* p = static_cast<fetch_read*>(f->userData);
        vgl_http_reader* self = p->reader;
        if (!p->cancelled && f->status == 0 && p->attempts < max_attempts)
        {
            // No answer: a lost connection, or a browser cache that refuses a range of a file
            // it is still storing for another read, as Chrome's may. Try again shortly; the read
            // stays listed, so that cancelling it finds it.
            static_cast<void>(emscripten_fetch_close(f));
            p->fetch = nullptr;
            p->retry_timer = emscripten_set_timeout(&retry, retry_milliseconds * p->attempts, p);
            ++p->attempts;
            return;
        }
        self->unlink(p);
        vgl_result result = VGL_RESULT_IO_ERROR;
        const void* data = nullptr;
        std::size_t size = 0;
        if (p->cancelled)
        {
            result = VGL_RESULT_CANCELLED;
        }
        else if (f->status == 200 || f->status == 206)
        {
            // A 206 body starts at the offset; a 200 one, from a server that ignored the
            // range, at the start of the file.
            std::uint64_t first = f->status == 200 ? p->offset : 0;
            first = first < f->numBytes ? first : f->numBytes;
            std::uint64_t count = f->numBytes - first;
            if (p->size != VGL_IO_SIZE_TO_END && count > p->size)
            {
                count = p->size;
            }
            result = VGL_RESULT_SUCCESS;
            data = count == 0 ? nullptr : f->data + first;
            size = static_cast<std::size_t>(count);
        }
        else if (f->status == 404 || f->status == 410)
        {
            result = VGL_RESULT_NOT_FOUND;
        }
        else if (f->status == 416)
        {
            result = VGL_RESULT_SUCCESS; // The range starts past the end of the file.
        }
        static_cast<void>(vgl_io_request_complete(p->request, result, data, size));
        if (!p->closing)
        {
            static_cast<void>(emscripten_fetch_close(f));
        }
        self->free_read(p);
    }

    // Lists a read and starts its fetch, on the main thread.
    static void start(void* data)
    {
        auto* p = static_cast<fetch_read*>(data);
        vgl_http_reader* self = p->reader;
        p->next = self->active;
        if (self->active != nullptr)
        {
            self->active->previous = p;
        }
        self->active = p;
        fetch(p);
    }

    // Tries a listed read again.
    static void retry(void* data)
    {
        auto* p = static_cast<fetch_read*>(data);
        p->retry_timer = 0;
        fetch(p);
    }

    static void fetch(fetch_read* p)
    {
        const vgl_http_reader* self = p->reader;
        const range_text range = make_range(p->offset, p->size);
        std::memcpy(p->range.data(), "bytes=", 6);
        std::memcpy(p->range.data() + 6, range.data(), p->range.size() - 6);
        p->headers = {"Range", p->range.data(), nullptr};
        emscripten_fetch_attr_t attr;
        emscripten_fetch_attr_init(&attr);
        std::memcpy(attr.requestMethod, "GET", 4);
        attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
        attr.requestHeaders = p->headers.data();
        attr.timeoutMSecs = self->timeout_ms;
        attr.userData = p;
        attr.onsuccess = &done;
        attr.onerror = &done;
        p->fetch = emscripten_fetch(&attr, p->uri);
    }

    // Ends a read's fetch early, on the main thread. Closing a fetch in progress reports its end
    // at once, which completes the read.
    static void stop(fetch_read* p)
    {
        vgl_http_reader* self = p->reader;
        vgl_io_request* request = p->request;
        p->cancelled = true;
        p->closing = true;
        if (p->fetch == nullptr)
        {
            emscripten_clear_timeout(p->retry_timer); // It was waiting to try again.
        }
        else
        {
            static_cast<void>(emscripten_fetch_close(p->fetch));
        }
        fetch_read* left = self->find(request);
        if (left != nullptr)
        {
            // The fetch ended without a report.
            self->unlink(left);
            static_cast<void>(vgl_io_request_complete(request, VGL_RESULT_CANCELLED, nullptr, 0));
            self->free_read(left);
        }
    }

    static void cancel_on_main(void* data)
    {
        auto* c = static_cast<fetch_cancel*>(data);
        vgl_http_reader* self = c->reader;
        if (fetch_read* p = self->find(c->request); p != nullptr)
        {
            stop(p);
        }
        self->allocator.destroy(c);
    }

    static void read(void* user_data, vgl_io_request* request, const vgl_io_read* r)
    {
        auto* self = static_cast<vgl_http_reader*>(user_data);
        if (!is_http(r->uri) && self->has_files)
        {
            self->files.read(self->files.user_data, request, r);
            return;
        }
        if (r->size == 0)
        {
            static_cast<void>(vgl_io_request_complete(request, VGL_RESULT_SUCCESS, nullptr, 0));
            return;
        }
        if (r->size != VGL_IO_SIZE_TO_END &&
            (r->size > SIZE_MAX || r->offset > UINT64_MAX - r->size))
        {
            static_cast<void>(
                vgl_io_request_complete(request, VGL_RESULT_OUT_OF_MEMORY, nullptr, 0));
            return;
        }
        auto* p = self->allocator.create<fetch_read>();
        if (p != nullptr)
        {
            p->uri_size = std::strlen(r->uri) + 1;
            p->uri = static_cast<char*>(self->allocator.allocate(p->uri_size, 1));
        }
        if (p == nullptr || p->uri == nullptr)
        {
            self->allocator.destroy(p);
            static_cast<void>(
                vgl_io_request_complete(request, VGL_RESULT_OUT_OF_MEMORY, nullptr, 0));
            return;
        }
        std::memcpy(p->uri, r->uri, p->uri_size);
        p->reader = self;
        p->request = request;
        p->offset = r->offset;
        p->size = r->size;
        on_main(&start, p, false);
    }

    static void cancel(void* user_data, vgl_io_request* request)
    {
        auto* self = static_cast<vgl_http_reader*>(user_data);
        if (self->has_files && self->files.cancel != nullptr)
        {
            self->files.cancel(self->files.user_data, request);
        }
        auto* c = self->allocator.create<fetch_cancel>();
        if (c == nullptr)
        {
            return; // The read completes in its own time.
        }
        c->reader = self;
        c->request = request;
        on_main(&cancel_on_main, c, false);
    }

    static void stop_all(void* data)
    {
        auto* self = static_cast<vgl_http_reader*>(data);
        while (self->active != nullptr)
        {
            stop(self->active);
        }
    }
};

extern "C" VGL_DEFAULTS_API vgl_result
vgl_http_reader_create(const vgl_http_reader_create_info* info_in, vgl_http_reader** out_reader)
{
    if (out_reader == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_reader = nullptr;
    vgl_http_reader_create_info info;
    vgl_result r = vgl::common::read_versioned(info_in, sizeof(vgl_http_reader_create_info), info);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    if (!std::isfinite(info.timeout) || info.timeout < 0.0)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    memory m;
    r = vgl::common::make_memory(info.allocator, m);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    auto* reader = m.create<vgl_http_reader>(m);
    if (reader == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    if (info.files != nullptr)
    {
        reader->has_files = true;
        static_cast<void>(vgl_file_reader_io_interface_get(info.files, &reader->files));
    }
    // max_connections and user_agent are the browser's to decide.
    const double timeout = info.timeout == 0.0 ? default_timeout : info.timeout;
    reader->timeout_ms =
        static_cast<std::uint32_t>(std::ceil((timeout < 86400.0 ? timeout : 86400.0) * 1000.0));
    *out_reader = reader;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result vgl_http_reader_destroy(vgl_http_reader* reader)
{
    if (reader == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    on_main(&vgl_http_reader::stop_all, reader, true);
    const memory m = reader->allocator;
    m.destroy(reader);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result
vgl_http_reader_io_interface_get(vgl_http_reader* reader, vgl_io_interface* out_interface)
{
    if (reader == nullptr || out_interface == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_interface = vgl_io_interface{VGL_IO_INTERFACE_VERSION,
                                      0,
                                      reader,
                                      &vgl_http_reader::read,
                                      &vgl_http_reader::cancel,
                                      nullptr};
    return VGL_RESULT_SUCCESS;
}

#else // VGL_DEFAULTS_HAS_FETCH

// Without libcurl or fetch, the reader cannot be created; the other functions see only null
// readers.

extern "C" VGL_DEFAULTS_API vgl_result
vgl_http_reader_create(const vgl_http_reader_create_info* /*info*/, vgl_http_reader** out_reader)
{
    if (out_reader == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_reader = nullptr;
    return VGL_RESULT_NOT_SUPPORTED;
}

extern "C" VGL_DEFAULTS_API vgl_result vgl_http_reader_destroy(vgl_http_reader* /*reader*/)
{
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_DEFAULTS_API vgl_result
vgl_http_reader_io_interface_get(vgl_http_reader* /*reader*/, vgl_io_interface* /*out_interface*/)
{
    return VGL_RESULT_INVALID_ARGUMENT;
}

#endif // VGL_DEFAULTS_HAS_CURL, VGL_DEFAULTS_HAS_FETCH
