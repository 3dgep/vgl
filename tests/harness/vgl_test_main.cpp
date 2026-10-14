// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "vgl_test.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace vgl::test
{

namespace
{

struct test_case
{
    const char* name;
    test_function function;
};

std::vector<test_case>& registry()
{
    static std::vector<test_case> cases;
    return cases;
}

int failures_in_current_case = 0;

} // namespace

registrar::registrar(const char* name, test_function function)
{
    registry().push_back({name, function});
}

void report_failure(const char* file, int line, const std::string& message)
{
    std::fprintf(stderr, "%s(%d): check failed: %s\n", file, line, message.c_str());
    ++failures_in_current_case;
}

} // namespace vgl::test

int main(int argc, char** argv)
{
    using namespace vgl::test;

    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failed = 0;
    for (const test_case& tc : registry())
    {
        if (filter != nullptr && std::strstr(tc.name, filter) == nullptr)
        {
            continue;
        }
        failures_in_current_case = 0;
        tc.function();
        ++run;
        if (failures_in_current_case != 0)
        {
            ++failed;
            std::fprintf(stderr, "FAILED: %s (%d check(s))\n", tc.name, failures_in_current_case);
        }
    }

    if (run == 0)
    {
        std::fprintf(stderr, "no test cases match\n");
        return 1;
    }
    std::printf("%d of %d test case(s) passed\n", run - failed, run);
    return failed == 0 ? 0 : 1;
}
