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

// I/O that has no files, completing every read inside read().
class no_files final : public vgl::io_interface
{
public:
    int reads = 0;

    void read(vgl::io_request* request, const vgl::io_read& /*read*/) noexcept override
    {
        ++reads;
        static_cast<void>(vgl::io_request_complete(request, vgl::result::not_found));
    }
};

// Declines every task, so the context decodes in its update.
class no_tasks final : public vgl::task_interface
{
public:
    vgl::result submit(vgl::task_fn /*function*/, void* /*task_data*/) noexcept override
    {
        return vgl::result::not_supported;
    }
};

void check_context()
{
    no_files files;
    no_tasks tasks;
    const vgl_io_interface io = files.c_interface();
    const vgl_task_interface task_table = tasks.c_interface();
    auto info = vgl::init<vgl_context_create_info>();
    info.io = &io;
    info.tasks = &task_table;
    vgl::expected<vgl::context> ctx = vgl::context::create(&info);
    CHECK(ctx.has_value());
    if (!ctx)
    {
        return;
    }
    vgl::expected<vgl::pack> p = vgl::pack::create(*ctx, "nowhere");
    CHECK(p.has_value());
    CHECK(files.reads == 1);
    CHECK(ctx->update() == vgl::result::success);
    const vgl::expected<vgl::pack_status> status = p->state_get();
    CHECK(status && status->state == vgl::pack_state::failed &&
          status->failure == vgl::result::not_found);

    const vgl::expected<vgl::height_sample> h = ctx->height_query(0.0, 0.0);
    CHECK(!h && h.error() == vgl::result::not_found);
    const vgl::expected<vgl::height_sample> msl =
        ctx->height_query(0.0, 0.0, vgl::height_reference::mean_sea_level);
    CHECK(!msl && msl.error() == vgl::result::invalid_operation);
    {
        // Without a view, a frame draws nothing; it ends when it goes out of scope.
        vgl::expected<vgl::frame> f = vgl::frame::begin(*ctx);
        CHECK(f.has_value());
        if (f)
        {
            CHECK(f->draw_list().empty());
            CHECK(f->destroys().empty());
            CHECK(f->meshes().empty());
        }
    }
    CHECK(ctx->resources_reset() == vgl::result::success);
    p->reset();
}

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

    check_context();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("vgl %s: all checks passed\n", VGL_VERSION_STRING);
    return 0;
}
