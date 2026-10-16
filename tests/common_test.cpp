// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the helpers for libraries built on the C API (src/common): the backends and the
// defaults module.

#include "common/host_memory.hpp"
#include "common/pointer_map.hpp"
#include "test_packs.hpp"
#include "vgl_test.hpp"

#include <cstdint>
#include <map>
#include <random>
#include <vector>

using vgl::common::memory;
using vgl::common::pointer_map;
using vgl::test::test_allocator;

TEST_CASE(pointer_map_finds_what_it_holds)
{
    test_allocator a;
    {
        pointer_map<int> map{memory(a.table)};
        std::vector<int> values(5000);
        CHECK(map.find(1) == nullptr);
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            REQUIRE(map.insert(i + 1, &values[i]));
        }
        CHECK_EQ(map.size(), values.size());
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            CHECK(map.find(i + 1) == &values[i]);
        }
        CHECK(map.find(0) == nullptr);
        CHECK(map.find(values.size() + 1) == nullptr);

        // Replacing keeps the size.
        int other = 0;
        REQUIRE(map.insert(7, &other));
        CHECK(map.find(7) == &other);
        CHECK_EQ(map.size(), values.size());

        std::size_t visited = 0;
        map.for_each([&](std::uint64_t key, int* value) {
            CHECK(key != 0 && value != nullptr);
            ++visited;
        });
        CHECK_EQ(visited, values.size());
        map.clear();
        CHECK_EQ(map.size(), 0u);
        CHECK(map.find(7) == nullptr);
    }
    CHECK_EQ(a.live.load(), 0);
}

TEST_CASE(pointer_map_erases_without_losing_other_keys)
{
    // Random inserts and erases, compared with std::map, so that erasing from the middle of a
    // run of collisions is exercised, including runs that wrap around the end of the table.
    pointer_map<int> map{memory()};
    std::map<std::uint64_t, int*> reference;
    std::vector<int> values(512);
    std::mt19937_64 random(1234);
    for (int step = 0; step < 200000; ++step)
    {
        const std::uint64_t key = random() % 600 + 1;
        if (random() % 3 == 0)
        {
            map.erase(key);
            reference.erase(key);
        }
        else
        {
            int* value = &values[key % values.size()];
            REQUIRE(map.insert(key, value));
            reference[key] = value;
        }
        if (step % 1000 == 0)
        {
            REQUIRE(map.size() == reference.size());
            for (std::uint64_t k = 1; k <= 600; ++k)
            {
                const auto it = reference.find(k);
                REQUIRE(map.find(k) == (it == reference.end() ? nullptr : it->second));
            }
        }
    }
}

TEST_CASE(pointer_map_reports_a_failed_allocation)
{
    test_allocator a;
    a.fail_at = 1; // The first growth succeeds, the second fails.
    {
        pointer_map<int> map{memory(a.table)};
        int value = 0;
        std::uint64_t key = 1;
        while (map.insert(key, &value))
        {
            ++key;
        }
        CHECK(a.failed.load());
        // The map is unchanged and still works.
        CHECK_EQ(map.size(), key - 1);
        CHECK(map.find(1) == &value);
        CHECK(map.find(key) == nullptr);
        a.fail_at = -1;
        CHECK(map.insert(key, &value));
    }
    CHECK_EQ(a.live.load(), 0);
}
