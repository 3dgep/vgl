// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Checks that the C++ wrapper compiles and maps the C API as docs/c-api.md,
// section 16, describes.

#include <vgl/vgl.hpp>

#include <cstdio>
#include <cstring>

namespace
{

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, line, expression);
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

struct versioned
{
    std::uint32_t struct_size;
    std::uint32_t value;
};

} // namespace

int main()
{
    static_assert(vgl::succeeded(vgl::result::success));
    static_assert(vgl::succeeded(vgl::result::incomplete));
    static_assert(vgl::failed(vgl::result::out_of_memory));
    static_assert(static_cast<std::int32_t>(vgl::result::internal_error) ==
                  VGL_RESULT_INTERNAL_ERROR);

    CHECK(std::strcmp(vgl::result_name_get(vgl::result::not_found), "VGL_RESULT_NOT_FOUND") == 0);
    CHECK(std::strcmp(vgl::result_name_get(static_cast<vgl::result>(-1000)),
                      "VGL_RESULT_UNKNOWN") == 0);

    constexpr versioned info = vgl::init<versioned>();
    static_assert(info.struct_size == sizeof(versioned));
    static_assert(info.value == 0);

    const vgl::expected<vgl::version> v = vgl::version_get();
    CHECK(v.has_value());
    if (v)
    {
        CHECK(v->major_version == VGL_VERSION_MAJOR);
        CHECK(v->minor_version == VGL_VERSION_MINOR);
        CHECK(v->patch_version == VGL_VERSION_PATCH);
    }

    const vgl::expected<int> error = vgl::unexpected(vgl::result::io_error);
    CHECK(!error.has_value());
    CHECK(error.error() == vgl::result::io_error);

    if (failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("vgl %s: all checks passed\n", VGL_VERSION_STRING);
    return 0;
}
