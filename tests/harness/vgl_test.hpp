// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Minimal test harness for the vgl tests, with no dependencies. Each test executable links
// vgl_test_main, which runs every registered test case, or those whose names contain the
// first command-line argument.
//
//     TEST_CASE(tile_keys_round_trip)
//     {
//         CHECK(condition);
//         CHECK_EQ(actual, expected);
//         CHECK_NEAR(actual, expected, tolerance);
//         REQUIRE(condition); // Stops the test case on failure.
//     }

#ifndef VGL_TEST_HPP
#define VGL_TEST_HPP

#include <cmath>
#include <sstream>
#include <string>

namespace vgl::test
{

using test_function = void (*)();

// Registers a test case. Used by TEST_CASE.
struct registrar
{
    registrar(const char* name, test_function function);
};

// Records a failed check. Used by the CHECK macros.
void report_failure(const char* file, int line, const std::string& message);

template <typename T>
std::string to_string(const T& value)
{
    if constexpr (requires(std::ostringstream& os, const T& v) { os << v; })
    {
        std::ostringstream os;
        os.precision(17);
        os << value;
        return os.str();
    }
    else
    {
        return "<value>";
    }
}

template <typename A, typename B>
bool check_eq(
    const A& actual, const B& expected, const char* expression, const char* file, int line)
{
    if (actual == expected)
    {
        return true;
    }
    report_failure(file,
                   line,
                   std::string(expression) + ": " + to_string(actual) +
                       " != " + to_string(expected));
    return false;
}

inline bool check_near(double actual,
                       double expected,
                       double tolerance,
                       const char* expression,
                       const char* file,
                       int line)
{
    if (std::fabs(actual - expected) <= tolerance)
    {
        return true;
    }
    report_failure(file,
                   line,
                   std::string(expression) + ": " + to_string(actual) + " differs from " +
                       to_string(expected) + " by " + to_string(std::fabs(actual - expected)) +
                       ", more than " + to_string(tolerance));
    return false;
}

} // namespace vgl::test

#define VGL_TEST_CONCAT_INNER(a, b) a##b
#define VGL_TEST_CONCAT(a, b)       VGL_TEST_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                            \
    static void name();                                                                            \
    static const ::vgl::test::registrar VGL_TEST_CONCAT(name, _registrar)(#name, &name);           \
    static void name()

#define CHECK(condition)                                                                           \
    ((condition) ? true : (::vgl::test::report_failure(__FILE__, __LINE__, #condition), false))

#define CHECK_EQ(actual, expected)                                                                 \
    ::vgl::test::check_eq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define CHECK_NEAR(actual, expected, tolerance)                                                    \
    ::vgl::test::check_near(                                                                       \
        (actual), (expected), (tolerance), #actual " ~ " #expected, __FILE__, __LINE__)

#define REQUIRE(condition)                                                                         \
    do                                                                                             \
    {                                                                                              \
        if (!CHECK(condition))                                                                     \
        {                                                                                          \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#endif // VGL_TEST_HPP
