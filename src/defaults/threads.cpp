// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The defaults module's threads (threads.hpp): Win32 on Windows, POSIX threads elsewhere.

#include "defaults/threads.hpp"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <unistd.h>
#endif

namespace vgl::defaults
{

namespace
{

#if defined(_WIN32)
DWORD WINAPI entry(LPVOID self)
{
    static_cast<const native_thread*>(self)->run();
    return 0;
}
#else
void* entry(void* self)
{
    static_cast<const native_thread*>(self)->run();
    return nullptr;
}
#endif

} // namespace

bool native_thread::start(function f, void* user_data) noexcept
{
    f_ = f;
    user_data_ = user_data;
#if defined(_WIN32)
    handle_ = CreateThread(nullptr, 0, &entry, this, 0, nullptr);
    started_ = handle_ != nullptr;
#else
    started_ = pthread_create(&handle_, nullptr, &entry, this) == 0;
#endif
    return started_;
}

void native_thread::join() noexcept
{
    if (!started_)
    {
        return;
    }
#if defined(_WIN32)
    WaitForSingleObject(handle_, INFINITE);
    CloseHandle(handle_);
    handle_ = nullptr;
#else
    static_cast<void>(pthread_join(handle_, nullptr));
#endif
    started_ = false;
}

std::uint32_t native_thread::hardware_count() noexcept
{
#if defined(_WIN32)
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return static_cast<std::uint32_t>(n);
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<std::uint32_t>(n) : 0;
#endif
}

} // namespace vgl::defaults
