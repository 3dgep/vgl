// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Foundation tests: allocator, logger and math.

#include "foundation/allocator.hpp"
#include "foundation/log.hpp"
#include "foundation/math.hpp"
#include "vgl_test.hpp"

#include <vgl/foundation.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace vgl::internal;

struct counting_heap
{
    int allocations = 0;
    int deallocations = 0;
    std::size_t last_size = 0;
    std::size_t last_alignment = 0;
    bool fail = false;

    static void* allocate(void* user_data, std::size_t size, std::size_t alignment)
    {
        auto* self = static_cast<counting_heap*>(user_data);
        self->last_size = size;
        self->last_alignment = alignment;
        if (self->fail)
        {
            return nullptr;
        }
        ++self->allocations;
        return allocator().allocate(size, alignment);
    }

    static void deallocate(void* user_data, void* memory, std::size_t size, std::size_t alignment)
    {
        auto* self = static_cast<counting_heap*>(user_data);
        ++self->deallocations;
        self->last_size = size;
        self->last_alignment = alignment;
        allocator().deallocate(memory, size, alignment);
    }

    vgl_allocator_interface table()
    {
        return {VGL_ALLOCATOR_INTERFACE_VERSION, 0, this, &allocate, &deallocate};
    }
};

struct widget
{
    int value;
    explicit widget(int v) noexcept : value(v) {}
};

struct captured_log
{
    std::vector<std::pair<vgl_log_level, std::string>> messages;

    static void callback(void* user_data, vgl_log_level level, const char* message)
    {
        static_cast<captured_log*>(user_data)->messages.emplace_back(level, message);
    }
};

} // namespace

TEST_CASE(default_allocator_aligns)
{
    const allocator heap;
    for (const std::size_t alignment : {1u, 8u, 16u, 64u, 256u, 4096u})
    {
        void* p = heap.allocate(100, alignment);
        REQUIRE(p != nullptr);
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(p) % alignment, 0u);
        heap.deallocate(p, 100, alignment);
    }
    CHECK(heap.allocate(0, 8) == nullptr);
    CHECK(heap.allocate(16, 3) == nullptr);
    heap.deallocate(nullptr, 16, 8);
}

TEST_CASE(host_allocator_receives_every_call)
{
    counting_heap host;
    const vgl_allocator_interface table = host.table();
    REQUIRE(validate_allocator_interface(&table) == VGL_RESULT_SUCCESS);
    const allocator a(table);

    widget* w = a.create<widget>(42);
    REQUIRE(w != nullptr);
    CHECK_EQ(w->value, 42);
    CHECK_EQ(host.allocations, 1);
    CHECK_EQ(host.last_size, sizeof(widget));
    CHECK_EQ(host.last_alignment, alignof(widget));
    a.destroy(w);
    CHECK_EQ(host.deallocations, 1);
    a.destroy<widget>(nullptr);
    CHECK_EQ(host.deallocations, 1);

    host.fail = true;
    CHECK(a.create<widget>(1) == nullptr);
}

TEST_CASE(allocator_interface_validation)
{
    counting_heap host;
    vgl_allocator_interface table = host.table();
    CHECK_EQ(validate_allocator_interface(nullptr), VGL_RESULT_INVALID_ARGUMENT);

    table.version = 0;
    CHECK_EQ(validate_allocator_interface(&table), VGL_RESULT_VERSION_MISMATCH);
    table.version = VGL_ALLOCATOR_INTERFACE_VERSION + 1;
    CHECK_EQ(validate_allocator_interface(&table), VGL_RESULT_VERSION_MISMATCH);

    table = host.table();
    table.allocate = nullptr;
    CHECK_EQ(validate_allocator_interface(&table), VGL_RESULT_INVALID_ARGUMENT);
    table = host.table();
    table.deallocate = nullptr;
    CHECK_EQ(validate_allocator_interface(&table), VGL_RESULT_INVALID_ARGUMENT);
}

namespace
{

class tracking_allocator final : public vgl::allocator
{
public:
    int live = 0;

    void* allocate(std::size_t size, std::size_t alignment) noexcept override
    {
        ++live;
        return heap_.allocate(size, alignment);
    }

    void deallocate(void* memory, std::size_t size, std::size_t alignment) noexcept override
    {
        --live;
        heap_.deallocate(memory, size, alignment);
    }

private:
    vgl::internal::allocator heap_;
};

} // namespace

TEST_CASE(cpp_allocator_adapter)
{
    tracking_allocator host;
    const vgl_allocator_interface table = host.c_interface();
    REQUIRE(validate_allocator_interface(&table) == VGL_RESULT_SUCCESS);
    const allocator a(table);
    widget* w = a.create<widget>(7);
    REQUIRE(w != nullptr);
    CHECK_EQ(host.live, 1);
    a.destroy(w);
    CHECK_EQ(host.live, 0);
}

TEST_CASE(logger_filters_by_level)
{
    captured_log log;
    const logger quiet;
    quiet.log(VGL_LOG_LEVEL_ERROR, "never seen");
    CHECK(!quiet.enabled(VGL_LOG_LEVEL_ERROR));

    const logger warnings(&captured_log::callback, &log, VGL_LOG_LEVEL_DEFAULT);
    warnings.log(VGL_LOG_LEVEL_ERROR, "error %d", 1);
    warnings.log(VGL_LOG_LEVEL_WARNING, "warning %s", "two");
    warnings.log(VGL_LOG_LEVEL_INFO, "info");
    warnings.log(VGL_LOG_LEVEL_DEBUG, "debug");
    warnings.log(VGL_LOG_LEVEL_DEFAULT, "not a message level");
    REQUIRE(log.messages.size() == 2u);
    CHECK_EQ(log.messages[0].first, VGL_LOG_LEVEL_ERROR);
    CHECK_EQ(log.messages[0].second, std::string("error 1"));
    CHECK_EQ(log.messages[1].second, std::string("warning two"));

    log.messages.clear();
    const logger verbose(&captured_log::callback, &log, VGL_LOG_LEVEL_DEBUG);
    verbose.log(VGL_LOG_LEVEL_DEBUG, "debug");
    CHECK_EQ(log.messages.size(), 1u);
}

TEST_CASE(logger_truncates_long_messages)
{
    captured_log log;
    const logger l(&captured_log::callback, &log, VGL_LOG_LEVEL_WARNING);
    const std::string long_text(5000, 'x');
    l.log(VGL_LOG_LEVEL_ERROR, "%s", long_text.c_str());
    REQUIRE(log.messages.size() == 1u);
    CHECK_EQ(log.messages[0].second.size(), static_cast<std::size_t>(logger::max_message_size - 1));
}

TEST_CASE(matrix_operations)
{
    const dvec3 x{1, 0, 0};
    const dvec3 y{0, 1, 0};
    CHECK(cross(x, y) == (dvec3{0, 0, 1}));
    CHECK_EQ(dot(dvec3{1, 2, 3}, dvec3{4, 5, 6}), 32.0);

    // Rotation of 90 degrees about Z, then translation.
    const dmat4 m = dmat4::from_axes({0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {10, 20, 30});
    CHECK(transform_point(m, {1, 0, 0}) == (dvec3{10, 21, 30}));
    CHECK(transform_vector(m, {1, 0, 0}) == (dvec3{0, 1, 0}));
    const dmat4 inv = rigid_inverse(m);
    CHECK(transform_point(inv, {10, 21, 30}) == (dvec3{1, 0, 0}));
    CHECK(inv * m == dmat4{});

    const vgl_dmat4 c = m.to_c();
    CHECK_EQ(c.m[12], 10.0);
    CHECK(dmat4::from_c(c) == m);
}

namespace
{

// What a request made with vgl_io_request_create delivered.
struct received
{
    int calls = 0;
    vgl_result result = VGL_RESULT_SUCCESS;
    std::string data;
    bool null_data = false;

    static void callback(void* user_data, vgl_result result, const void* data, std::size_t size)
    {
        auto* self = static_cast<received*>(user_data);
        ++self->calls;
        self->result = result;
        self->null_data = data == nullptr;
        self->data.assign(static_cast<const char*>(data), size);
    }
};

// Counts live allocations.
struct counting_allocator
{
    int live = 0;
    vgl_allocator_interface table{VGL_ALLOCATOR_INTERFACE_VERSION, 0, this, &allocate, &deallocate};

    static void* allocate(void* user_data, std::size_t size, std::size_t alignment)
    {
        ++static_cast<counting_allocator*>(user_data)->live;
        return allocator().allocate(size, alignment);
    }

    static void deallocate(void* user_data, void* memory, std::size_t size, std::size_t alignment)
    {
        --static_cast<counting_allocator*>(user_data)->live;
        allocator().deallocate(memory, size, alignment);
    }
};

} // namespace

TEST_CASE(io_request_create_calls_back_on_completion)
{
    counting_allocator memory;
    received r;
    vgl_io_request_create_info info{};
    info.struct_size = sizeof(info);
    info.complete = &received::callback;
    info.user_data = &r;
    info.allocator = &memory.table;

    vgl_io_request* request = nullptr;
    REQUIRE(vgl_io_request_create(&info, &request) == VGL_RESULT_SUCCESS);
    CHECK_EQ(memory.live, 1);
    const std::string bytes = "range of a shard";
    CHECK_EQ(vgl_io_request_complete(request, VGL_RESULT_SUCCESS, bytes.data(), bytes.size()),
             VGL_RESULT_SUCCESS);
    CHECK_EQ(r.calls, 1);
    CHECK_EQ(r.result, VGL_RESULT_SUCCESS);
    CHECK(r.data == bytes);
    CHECK_EQ(memory.live, 0); // Completing frees it.

    // A failure delivers no data, whatever the completer passes.
    REQUIRE(vgl_io_request_create(&info, &request) == VGL_RESULT_SUCCESS);
    CHECK_EQ(vgl_io_request_complete(request, VGL_RESULT_NOT_FOUND, bytes.data(), bytes.size()),
             VGL_RESULT_SUCCESS);
    CHECK_EQ(r.calls, 2);
    CHECK_EQ(r.result, VGL_RESULT_NOT_FOUND);
    CHECK(r.null_data);
    CHECK(r.data.empty());
    CHECK_EQ(memory.live, 0);

    // The C++ wrapper, on the C heap.
    vgl::expected<vgl::io_request*> made = vgl::io_request_create(&received::callback, &r);
    REQUIRE(made.has_value());
    CHECK(vgl::io_request_complete(*made, vgl::result::success) == vgl::result::success);
    CHECK_EQ(r.calls, 3);
    CHECK(r.data.empty());
}

TEST_CASE(io_request_create_arguments)
{
    received r;
    vgl_io_request_create_info info{};
    info.struct_size = sizeof(info);
    info.complete = &received::callback;
    info.user_data = &r;
    vgl_io_request* request = nullptr;
    CHECK_EQ(vgl_io_request_create(&info, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_io_request_create(nullptr, &request), VGL_RESULT_INVALID_ARGUMENT);
    info.complete = nullptr;
    CHECK_EQ(vgl_io_request_create(&info, &request), VGL_RESULT_INVALID_ARGUMENT);
    CHECK(request == nullptr);
    info.complete = &received::callback;
    info.struct_size = 4;
    CHECK_EQ(vgl_io_request_create(&info, &request), VGL_RESULT_INVALID_ARGUMENT);
    info.struct_size = sizeof(info);
    vgl_allocator_interface future{
        VGL_ALLOCATOR_INTERFACE_VERSION + 1, 0, nullptr, nullptr, nullptr};
    info.allocator = &future;
    CHECK_EQ(vgl_io_request_create(&info, &request), VGL_RESULT_VERSION_MISMATCH);
    CHECK_EQ(r.calls, 0);
}
