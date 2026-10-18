// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The draw features and debug views every backend takes (vgl/render.h), and the bits the
// shared shaders read: the debug views, with contour lines as one more.

#ifndef VGL_BACKENDS_COMMON_DRAW_FLAGS_HPP
#define VGL_BACKENDS_COMMON_DRAW_FLAGS_HPP

#include <vgl/render.h>

#include <cstdint>

namespace vgl::backends
{

inline constexpr std::uint32_t known_draw_features =
    VGL_DRAW_FEATURE_SEA_SURFACE | VGL_DRAW_FEATURE_ATMOSPHERE | VGL_DRAW_FEATURE_STARS |
    VGL_DRAW_FEATURE_CLOUDS | VGL_DRAW_FEATURE_CONTOURS;

inline constexpr std::uint32_t known_debug_views =
    VGL_DEBUG_VIEW_WIREFRAME | VGL_DEBUG_VIEW_LEVEL_COLORS | VGL_DEBUG_VIEW_IMAGERY_SOURCE |
    VGL_DEBUG_VIEW_HEIGHT_COLORS;

// The shaders' bit for contour lines, after the debug views' bits.
inline constexpr std::uint32_t shader_contours = 0x10;

// The bits the shaders' debug value has: the debug views, and the contour lines.
[[nodiscard]] constexpr std::uint32_t shader_debug_bits(vgl_debug_views views,
                                                        vgl_draw_features features) noexcept
{
    return views | ((features & VGL_DRAW_FEATURE_CONTOURS) != 0 ? shader_contours : 0u);
}

[[nodiscard]] constexpr bool has(vgl_draw_features features, vgl_draw_feature feature) noexcept
{
    return (features & static_cast<std::uint32_t>(feature)) != 0;
}

} // namespace vgl::backends

#endif // VGL_BACKENDS_COMMON_DRAW_FLAGS_HPP
