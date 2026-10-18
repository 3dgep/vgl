// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The defaults module's filesystem operations (files.hpp): Win32 and the C runtime's wide
// functions on Windows, POSIX elsewhere.

#include "defaults/files.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <sys/utime.h>

#    include <direct.h>
#    include <windows.h>
#else
#    include <sys/stat.h>

#    include <dirent.h>
#    include <unistd.h>
#    include <utime.h>
#endif

using vgl::common::memory;

namespace vgl::defaults
{

namespace
{

// A copy of a UTF-8 string in memory from m, freed with it.
class string_copy
{
public:
    string_copy(const char* s, std::size_t length, const memory& m) noexcept
        : m_(m), data_(static_cast<char*>(m.allocate(length + 1, 1)))
    {
        if (data_ != nullptr)
        {
            std::memcpy(data_, s, length);
            data_[length] = '\0';
            size_ = length + 1;
        }
    }

    string_copy(const string_copy&) = delete;
    string_copy& operator=(const string_copy&) = delete;
    string_copy(string_copy&&) = delete;
    string_copy& operator=(string_copy&&) = delete;

    ~string_copy() { m_.deallocate(data_, size_, 1); }

    [[nodiscard]] char* get() const noexcept { return data_; }

private:
    const memory& m_;
    char* data_ = nullptr;
    std::size_t size_ = 0;
};

bool is_separator(char c) noexcept
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

} // namespace

#if defined(_WIN32)

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

namespace
{

// A path as UTF-16, freed with it.
class wide_path
{
public:
    wide_path(const char* path, const memory& m) noexcept : m_(m)
    {
        if (!utf8_to_utf16(path, m, data_, count_))
        {
            m_.deallocate(data_, count_ * sizeof(wchar_t), alignof(wchar_t));
            data_ = nullptr;
        }
    }

    wide_path(const wide_path&) = delete;
    wide_path& operator=(const wide_path&) = delete;
    wide_path(wide_path&&) = delete;
    wide_path& operator=(wide_path&&) = delete;

    ~wide_path() { m_.deallocate(data_, count_ * sizeof(wchar_t), alignof(wchar_t)); }

    [[nodiscard]] const wchar_t* get() const noexcept { return data_; }

private:
    const memory& m_;
    wchar_t* data_ = nullptr;
    std::size_t count_ = 0;
};

bool directory_exists(const wchar_t* path) noexcept
{
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool make_directory(const char* path, const memory& m) noexcept
{
    const wide_path w(path, m);
    if (w.get() == nullptr)
    {
        return false;
    }
    return CreateDirectoryW(w.get(), nullptr) != 0 || directory_exists(w.get());
}

} // namespace

std::FILE*
file_open(const char* path, const char* mode, const memory& m, vgl_result& error) noexcept
{
    const wide_path w(path, m);
    if (w.get() == nullptr)
    {
        error = VGL_RESULT_OUT_OF_MEMORY;
        return nullptr;
    }
    std::array<wchar_t, 8> wide_mode{};
    for (std::size_t i = 0; mode[i] != '\0' && i + 1 < wide_mode.size(); ++i)
    {
        wide_mode[i] = static_cast<wchar_t>(mode[i]);
    }
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, w.get(), wide_mode.data()) != 0)
    {
        error = errno == ENOENT ? VGL_RESULT_NOT_FOUND : VGL_RESULT_IO_ERROR;
        return nullptr;
    }
    return f;
}

bool directory_list(const char* path, const memory& m, directory_fn fn, void* user_data) noexcept
{
    const std::size_t length = std::strlen(path);
    // "<path>/*"
    auto* p = static_cast<char*>(m.allocate(length + 3, 1));
    if (p == nullptr)
    {
        return false;
    }
    std::memcpy(p, path, length);
    p[length] = '/';
    p[length + 1] = '*';
    p[length + 2] = '\0';
    const wide_path w(p, m);
    m.deallocate(p, length + 3, 1);
    if (w.get() == nullptr)
    {
        return false;
    }
    WIN32_FIND_DATAW found{};
    HANDLE h = FindFirstFileExW(w.get(),
                                FindExInfoBasic,
                                &found,
                                FindExSearchNameMatch,
                                nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
    {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    for (;;)
    {
        // The names the module writes are ASCII; others are reported with '?' for the rest.
        std::array<char, MAX_PATH> name{};
        std::size_t n = 0;
        for (; found.cFileName[n] != L'\0' && n + 1 < name.size(); ++n)
        {
            const wchar_t c = found.cFileName[n];
            name[n] = c < 0x80 ? static_cast<char>(c) : '?';
        }
        const bool dot = std::strcmp(name.data(), ".") == 0 || std::strcmp(name.data(), "..") == 0;
        if (!dot)
        {
            directory_entry e;
            e.name = name.data();
            e.directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = (std::uint64_t{found.nFileSizeHigh} << 32) | found.nFileSizeLow;
            // FILETIME counts 100 ns since 1601.
            const std::uint64_t ticks =
                (std::uint64_t{found.ftLastWriteTime.dwHighDateTime} << 32) |
                found.ftLastWriteTime.dwLowDateTime;
            e.modified = static_cast<std::int64_t>(ticks / 10000000u) - 11644473600;
            fn(user_data, e);
        }
        if (FindNextFileW(h, &found) == 0)
        {
            break;
        }
    }
    FindClose(h);
    return true;
}

bool file_replace(const char* from, const char* to, const memory& m) noexcept
{
    const wide_path a(from, m);
    const wide_path b(to, m);
    return a.get() != nullptr && b.get() != nullptr &&
           MoveFileExW(a.get(), b.get(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool file_remove(const char* path, const memory& m) noexcept
{
    const wide_path w(path, m);
    if (w.get() == nullptr)
    {
        return false;
    }
    return DeleteFileW(w.get()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
}

void file_touch(const char* path, const memory& m) noexcept
{
    const wide_path w(path, m);
    if (w.get() != nullptr)
    {
        static_cast<void>(_wutime(w.get(), nullptr));
    }
}

#else

namespace
{

bool make_directory(const char* path, const memory& /*m*/) noexcept
{
    if (mkdir(path, 0777) == 0)
    {
        return true;
    }
    struct stat s
    {
    };
    return errno == EEXIST && stat(path, &s) == 0 && S_ISDIR(s.st_mode);
}

} // namespace

std::FILE*
file_open(const char* path, const char* mode, const memory& /*m*/, vgl_result& error) noexcept
{
    std::FILE* f = std::fopen(path, mode);
    if (f == nullptr)
    {
        error = errno == ENOENT || errno == ENOTDIR ? VGL_RESULT_NOT_FOUND : VGL_RESULT_IO_ERROR;
    }
    return f;
}

bool directory_list(const char* path, const memory& m, directory_fn fn, void* user_data) noexcept
{
    DIR* d = opendir(path);
    if (d == nullptr)
    {
        return errno == ENOENT;
    }
    const std::size_t length = std::strlen(path);
    // readdir is safe on a stream of the caller's own, which this one is.
    while (const dirent* found = readdir(d)) // NOLINT(concurrency-mt-unsafe)
    {
        const char* name = &found->d_name[0];
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
        {
            continue;
        }
        const std::size_t name_length = std::strlen(name);
        auto* full = static_cast<char*>(m.allocate(length + name_length + 2, 1));
        if (full == nullptr)
        {
            closedir(d);
            return false;
        }
        std::memcpy(full, path, length);
        full[length] = '/';
        std::memcpy(full + length + 1, name, name_length + 1);
        struct stat s
        {
        };
        if (stat(full, &s) == 0)
        {
            directory_entry e;
            e.name = name;
            e.directory = S_ISDIR(s.st_mode);
            e.size = static_cast<std::uint64_t>(s.st_size);
            e.modified = static_cast<std::int64_t>(s.st_mtime);
            fn(user_data, e);
        }
        m.deallocate(full, length + name_length + 2, 1);
    }
    closedir(d);
    return true;
}

bool file_replace(const char* from, const char* to, const memory& /*m*/) noexcept
{
    return std::rename(from, to) == 0;
}

bool file_remove(const char* path, const memory& /*m*/) noexcept
{
    return std::remove(path) == 0 || errno == ENOENT;
}

void file_touch(const char* path, const memory& /*m*/) noexcept
{
    static_cast<void>(utime(path, nullptr));
}

#endif

bool directories_create(const char* path, const memory& m) noexcept
{
    const std::size_t length = std::strlen(path);
    const string_copy copy(path, length, m);
    char* p = copy.get();
    if (p == nullptr)
    {
        return false;
    }
    // Each component in turn, skipping a leading separator and, on Windows, a drive.
    std::size_t i = 1;
#if defined(_WIN32)
    if (length >= 2 && p[1] == ':')
    {
        i = 3;
    }
#endif
    for (; i < length; ++i)
    {
        if (is_separator(p[i]))
        {
            const char saved = p[i];
            p[i] = '\0';
            static_cast<void>(make_directory(p, m));
            p[i] = saved;
        }
    }
    return make_directory(p, m);
}

} // namespace vgl::defaults
