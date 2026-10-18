// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Threads of the defaults module, on the platform's own functions (CreateThread, pthread_create)
// rather than std::thread, which reports failing to start only by an exception: in a build
// without exceptions, that would end the process (docs/c-api.md, section 7.4).

#ifndef VGL_DEFAULTS_THREADS_HPP
#define VGL_DEFAULTS_THREADS_HPP

#include <cstdint>

#if !defined(_WIN32)
#    include <pthread.h>
#endif

namespace vgl::defaults
{

class native_thread
{
public:
    using function = void (*)(void* user_data);

    native_thread() noexcept = default;
    native_thread(const native_thread&) = delete;
    native_thread& operator=(const native_thread&) = delete;
    native_thread(native_thread&&) = delete;
    native_thread& operator=(native_thread&&) = delete;
    ~native_thread() { join(); }

    // Starts the thread, running f(user_data). Returns false if it could not start. The object
    // must stay where it is until the thread is joined.
    [[nodiscard]] bool start(function f, void* user_data) noexcept;

    // Waits for the thread to end. Does nothing if it was not started, or was joined.
    void join() noexcept;

    // The number of threads the hardware runs at once, or 0 if unknown.
    [[nodiscard]] static std::uint32_t hardware_count() noexcept;

    // The thread's entry, on the platform's thread: runs f(user_data).
    void run() const noexcept { f_(user_data_); }

private:
    function f_ = nullptr;
    void* user_data_ = nullptr;
#if defined(_WIN32)
    void* handle_ = nullptr; // A HANDLE.
#else
    pthread_t handle_{};
#endif
    bool started_ = false;
};

} // namespace vgl::defaults

#endif // VGL_DEFAULTS_THREADS_HPP
