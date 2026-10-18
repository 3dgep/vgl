// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The filesystem operations of the defaults module, on UTF-8 paths: the C runtime's files, and
// the platform's directories, which the C and C++ standard libraries have no allocator-aware
// way to list. Temporary paths use the module's allocator.

#ifndef VGL_DEFAULTS_FILES_HPP
#define VGL_DEFAULTS_FILES_HPP

#include "common/host_memory.hpp"

#include <cstdint>
#include <cstdio>

namespace vgl::defaults
{

#if defined(_WIN32)
// Converts UTF-8 to UTF-16 into memory from m, of out_count units, which the caller frees.
// Returns false for invalid UTF-8, with out null only if allocation failed.
bool utf8_to_utf16(const char* in,
                   const vgl::common::memory& m,
                   wchar_t*& out,
                   std::size_t& out_count) noexcept;
#endif

// Opens a file with a mode of fopen ("rb", "wb"). Returns null on failure, with error set to
// VGL_RESULT_NOT_FOUND if it does not exist, and otherwise VGL_RESULT_IO_ERROR or
// VGL_RESULT_OUT_OF_MEMORY.
std::FILE* file_open(const char* path,
                     const char* mode,
                     const vgl::common::memory& m,
                     vgl_result& error) noexcept;

// Creates a directory and those above it that are missing. True if it exists afterwards.
bool directories_create(const char* path, const vgl::common::memory& m) noexcept;

// What a directory listing reports for each entry, other than "." and "..".
struct directory_entry
{
    const char* name = nullptr; // UTF-8, valid during the call.
    bool directory = false;
    std::uint64_t size = 0;
    std::int64_t modified = 0; // Seconds since 1970.
};

using directory_fn = void (*)(void* user_data, const directory_entry& entry);

// Lists a directory. Returns false if it cannot be read.
bool directory_list(const char* path,
                    const vgl::common::memory& m,
                    directory_fn fn,
                    void* user_data) noexcept;

// Moves a file over another, at once where the platform can.
bool file_replace(const char* from, const char* to, const vgl::common::memory& m) noexcept;

// Removes a file. True if it is gone afterwards.
bool file_remove(const char* path, const vgl::common::memory& m) noexcept;

// Sets a file's modification time to now.
void file_touch(const char* path, const vgl::common::memory& m) noexcept;

} // namespace vgl::defaults

#endif // VGL_DEFAULTS_FILES_HPP
