// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "decoders/image_decoder.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

// The stb_image entry points of src/decoders/stb_image.c, and the allocation hooks it calls.
extern "C" {
int vgl_stbi_info(const unsigned char* data, int size, int* width, int* height, int* channels);
unsigned char* vgl_stbi_load(const unsigned char* data, int size, int* width, int* height);
void vgl_stbi_free(void* pixels);
void* vgl_stbi_allocate(std::size_t size);
void* vgl_stbi_reallocate(void* memory, std::size_t old_size, std::size_t new_size);
void vgl_stbi_deallocate(void* memory);
}

namespace
{

// The allocator of the decode running on this thread. stb_image's allocation macros take no
// context, so the decode sets this for its duration and clears it after: it holds nothing
// between calls, and each thread has its own.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): see above.
thread_local const vgl::internal::allocator* decode_allocator = nullptr;

// Each block starts with its size, for deallocate, padded to keep the data aligned.
constexpr std::size_t block_header = 16;

} // namespace

extern "C" void* vgl_stbi_allocate(std::size_t size)
{
    if (decode_allocator == nullptr || size > static_cast<std::size_t>(-1) - block_header)
    {
        return nullptr;
    }
    auto* block =
        static_cast<unsigned char*>(decode_allocator->allocate(size + block_header, block_header));
    if (block == nullptr)
    {
        return nullptr;
    }
    std::memcpy(block, &size, sizeof(size));
    return block + block_header;
}

extern "C" void vgl_stbi_deallocate(void* memory)
{
    if (memory == nullptr || decode_allocator == nullptr)
    {
        return;
    }
    unsigned char* block = static_cast<unsigned char*>(memory) - block_header;
    std::size_t size = 0;
    std::memcpy(&size, block, sizeof(size));
    decode_allocator->deallocate(block, size + block_header, block_header);
}

extern "C" void* vgl_stbi_reallocate(void* memory, std::size_t old_size, std::size_t new_size)
{
    void* fresh = vgl_stbi_allocate(new_size);
    if (fresh != nullptr && memory != nullptr)
    {
        std::memcpy(fresh, memory, std::min(old_size, new_size));
        vgl_stbi_deallocate(memory);
    }
    return fresh;
}

namespace vgl::internal
{

namespace
{

// Sets the decode allocator for a scope.
class allocator_scope
{
public:
    explicit allocator_scope(const allocator& a) noexcept { decode_allocator = &a; }
    allocator_scope(const allocator_scope&) = delete;
    allocator_scope& operator=(const allocator_scope&) = delete;
    allocator_scope(allocator_scope&&) = delete;
    allocator_scope& operator=(allocator_scope&&) = delete;
    ~allocator_scope() { decode_allocator = nullptr; }
};

bool is_jpeg(std::span<const std::byte> f) noexcept
{
    return f.size() >= 3 && f[0] == std::byte{0xFF} && f[1] == std::byte{0xD8} &&
           f[2] == std::byte{0xFF};
}

bool is_png(std::span<const std::byte> f) noexcept
{
    constexpr std::array<unsigned char, 8> signature = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return f.size() >= signature.size() && std::memcmp(f.data(), signature.data(), 8) == 0;
}

} // namespace

std::uint32_t mip_level_count(std::uint32_t width, std::uint32_t height) noexcept
{
    std::uint32_t levels = 1;
    for (std::uint32_t side = std::max(width, height); side > 1; side /= 2)
    {
        ++levels;
    }
    return levels;
}

vgl_result decode_imagery(const allocator& a,
                          std::span<const std::byte> file,
                          std::uint32_t side,
                          mip_chain& out) noexcept
{
    if (!is_jpeg(file) && !is_png(file))
    {
        return VGL_RESULT_NOT_SUPPORTED;
    }
    if (file.size() > static_cast<std::size_t>(INT_MAX) || side == 0 || side > 65536)
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(file.data());
    const int size = static_cast<int>(file.size());
    const allocator_scope scope(a);
    // The header first, so that a file of the wrong size is not decoded at all.
    int width = 0;
    int height = 0;
    int channels = 0;
    if (vgl_stbi_info(data, size, &width, &height, &channels) == 0 ||
        width != static_cast<int>(side) || height != static_cast<int>(side))
    {
        return VGL_RESULT_CORRUPT_DATA;
    }
    unsigned char* pixels = vgl_stbi_load(data, size, &width, &height);
    if (pixels == nullptr)
    {
        // stb_image does not say why; a failed allocation is as likely as bad data.
        return VGL_RESULT_CORRUPT_DATA;
    }

    out.width = side;
    out.height = side;
    out.levels = mip_level_count(side, side);
    std::size_t total = 0;
    for (std::uint32_t level = 0, w = side, h = side; level < out.levels; ++level)
    {
        out.offsets[level] = total;
        out.sizes[level] = std::size_t{w} * h * 4;
        total += out.sizes[level];
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    if (!out.pixels.allocate(a, total))
    {
        vgl_stbi_free(pixels);
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    std::memcpy(out.pixels.data(), pixels, out.sizes[0]);
    vgl_stbi_free(pixels);
    generate_mips(out);
    return VGL_RESULT_SUCCESS;
}

void generate_mips(mip_chain& chain) noexcept
{
    std::uint32_t w = chain.width;
    std::uint32_t h = chain.height;
    for (std::uint32_t level = 1; level < chain.levels; ++level)
    {
        const std::uint8_t* src = chain.pixels.data() + chain.offsets[level - 1];
        std::uint8_t* dst = chain.pixels.data() + chain.offsets[level];
        const std::uint32_t dw = std::max(1u, w / 2);
        const std::uint32_t dh = std::max(1u, h / 2);
        for (std::uint32_t y = 0; y < dh; ++y)
        {
            for (std::uint32_t x = 0; x < dw; ++x)
            {
                // The 2 x 2 block, or fewer pixels where a side is already 1.
                std::array<std::uint32_t, 3> colour = {};
                std::uint32_t alpha = 0;
                std::uint32_t count = 0;
                for (std::uint32_t sy = 2 * y; sy < std::min(2 * y + 2, h); ++sy)
                {
                    for (std::uint32_t sx = 2 * x; sx < std::min(2 * x + 2, w); ++sx)
                    {
                        const std::uint8_t* p = src + (std::size_t{sy} * w + sx) * 4;
                        for (std::uint32_t c = 0; c < 3; ++c)
                        {
                            colour[c] += std::uint32_t{p[c]} * p[3];
                        }
                        alpha += p[3];
                        ++count;
                    }
                }
                std::uint8_t* q = dst + (std::size_t{y} * dw + x) * 4;
                for (std::uint32_t c = 0; c < 3; ++c)
                {
                    q[c] =
                        alpha == 0 ? 0 : static_cast<std::uint8_t>((colour[c] + alpha / 2) / alpha);
                }
                q[3] = static_cast<std::uint8_t>((alpha + count / 2) / count);
            }
        }
        w = dw;
        h = dh;
    }
}

} // namespace vgl::internal
