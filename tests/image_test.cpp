// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of imagery decoding (src/decoders/image_decoder.hpp): stb_image through the vgl
// allocator, size checks, and mip levels.

#include "decoders/image_decoder.hpp"
#include "test_packs.hpp"
#include "vgl_test.hpp"

#include <cstdint>
#include <vector>

using namespace vgl::internal;
using vgl::test::encode_test_png;
using vgl::test::test_allocator;

namespace
{

std::vector<std::uint8_t> gradient(std::uint32_t side)
{
    std::vector<std::uint8_t> rgba(std::size_t{side} * side * 4);
    for (std::uint32_t j = 0; j < side; ++j)
    {
        for (std::uint32_t i = 0; i < side; ++i)
        {
            std::uint8_t* p = &rgba[(std::size_t{j} * side + i) * 4];
            p[0] = static_cast<std::uint8_t>(i * 7);
            p[1] = static_cast<std::uint8_t>(j * 5);
            p[2] = 200;
            p[3] = 255;
        }
    }
    return rgba;
}

} // namespace

TEST_CASE(imagery_decodes_into_a_full_mip_chain)
{
    test_allocator a;
    {
        const std::vector<std::uint8_t> rgba = gradient(18);
        const std::vector<std::byte> png = encode_test_png(18, 18, rgba);
        mip_chain chain;
        REQUIRE(decode_imagery(allocator(a.table), png, 18, chain) == VGL_RESULT_SUCCESS);
        CHECK_EQ(chain.width, 18u);
        CHECK_EQ(chain.levels, 5u); // 18, 9, 4, 2, 1.
        CHECK_EQ(chain.sizes[0], std::size_t{18 * 18 * 4});
        CHECK_EQ(chain.sizes[1], std::size_t{9 * 9 * 4});
        CHECK_EQ(chain.sizes[4], std::size_t{4});
        CHECK(std::equal(rgba.begin(), rgba.end(), chain.pixels.data()));
        // Level 1, pixel (1, 2): the mean of pixels (2..3, 4..5) of level 0.
        const std::uint8_t* p = chain.pixels.data() + chain.offsets[1] + (2 * 9 + 1) * 4;
        CHECK_EQ(p[0], static_cast<std::uint8_t>((2 * 7 + 3 * 7 + 1) / 2));
        CHECK_EQ(p[1], static_cast<std::uint8_t>((4 * 5 + 5 * 5 + 1) / 2));
        CHECK_EQ(p[2], 200);
        CHECK_EQ(p[3], 255);
        // stb_image allocated through the allocator, and freed what it allocated.
        CHECK(a.count.load() > 1);
    }
    CHECK_EQ(a.live.load(), 0);
}

TEST_CASE(imagery_of_the_wrong_kind_or_size_is_rejected)
{
    test_allocator a;
    const std::vector<std::byte> png = encode_test_png(18, 18, gradient(18));
    mip_chain chain;
    CHECK_EQ(decode_imagery(allocator(a.table), png, 16, chain), VGL_RESULT_CORRUPT_DATA);
    const std::vector<std::byte> text = {std::byte{'h'}, std::byte{'i'}, std::byte{'!'}};
    CHECK_EQ(decode_imagery(allocator(a.table), text, 18, chain), VGL_RESULT_NOT_SUPPORTED);
    // A truncated file.
    const std::vector<std::byte> cut(png.begin(), png.begin() + 60);
    CHECK_EQ(decode_imagery(allocator(a.table), cut, 18, chain), VGL_RESULT_CORRUPT_DATA);
    CHECK_EQ(a.live.load(), 0);
}

TEST_CASE(imagery_decoding_survives_allocation_failures)
{
    const std::vector<std::byte> png = encode_test_png(18, 18, gradient(18));
    for (long fail = 0; fail < 40; ++fail)
    {
        test_allocator a;
        a.fail_at = fail;
        {
            mip_chain chain;
            const vgl_result r = decode_imagery(allocator(a.table), png, 18, chain);
            CHECK(r == VGL_RESULT_SUCCESS || r == VGL_RESULT_OUT_OF_MEMORY ||
                  r == VGL_RESULT_CORRUPT_DATA);
        }
        CHECK_EQ(a.live.load(), 0);
    }
}

TEST_CASE(mip_levels_weight_colour_by_alpha)
{
    test_allocator a;
    mip_chain chain;
    chain.width = 2;
    chain.height = 2;
    chain.levels = 2;
    chain.offsets = {0, 16};
    chain.sizes = {16, 4};
    REQUIRE(chain.pixels.allocate(allocator(a.table), 20));
    // One opaque red pixel and three transparent black ones: the colour stays red.
    const std::uint8_t level0[16] = {255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::copy(std::begin(level0), std::end(level0), chain.pixels.data());
    generate_mips(chain);
    const std::uint8_t* p = chain.pixels.data() + 16;
    CHECK_EQ(p[0], 255);
    CHECK_EQ(p[1], 0);
    CHECK_EQ(p[3], 64);
}
