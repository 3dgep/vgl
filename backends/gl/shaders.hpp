// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The terrain shaders, in the dialect shared by GLSL 3.30 and GLSL ES 3.00 (design document,
// section 7.6). The backend puts a preamble in front of each: the version, the precision for
// OpenGL ES, and VGL_LOGARITHMIC_DEPTH for the logarithmic depth convention.

#ifndef VGL_BACKENDS_GL_SHADERS_HPP
#define VGL_BACKENDS_GL_SHADERS_HPP

namespace vgl::gl
{

inline constexpr const char* desktop_preamble = "#version 330 core\n";

// OpenGL ES 3.0 guarantees highp in fragment shaders, which the logarithmic depth needs.
inline constexpr const char* es_preamble = "#version 300 es\n"
                                           "precision highp float;\n"
                                           "precision highp int;\n";

inline constexpr const char* logarithmic_define = "#define VGL_LOGARITHMIC_DEPTH 1\n";

// The vertex layout of docs/render-interface.md, section 6.
inline constexpr const char* terrain_vertex_shader = R"glsl(
layout(location = 0) in vec3 a_position; // Relative to the tile's centre, along the ECEF axes.
layout(location = 1) in float a_height;  // Above the ellipsoid, in metres.
layout(location = 2) in vec2 a_uv;       // Over the tile; for imagery, which comes later.
layout(location = 3) in vec2 a_normal;   // Octahedral encoding, along the ECEF axes.

uniform mat4 u_model_view;
uniform mat4 u_projection;

out vec3 v_position; // Camera space.
out vec3 v_normal;   // Camera space.
out float v_height;

#ifdef VGL_LOGARITHMIC_DEPTH
uniform float u_log_depth; // 2 / log2(far + 1)
out float v_log_depth;     // 1 + the camera distance
#endif

vec3 decode_octahedral(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        vec2 s = vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(e.yx)) * s;
    }
    return normalize(n);
}

void main()
{
    vec4 p = u_model_view * vec4(a_position, 1.0);
    v_position = p.xyz;
    v_normal = mat3(u_model_view) * decode_octahedral(a_normal);
    v_height = a_height;
    gl_Position = u_projection * p;
#ifdef VGL_LOGARITHMIC_DEPTH
    // The distance along the view axis, so orthographic projections work too. The clip-space
    // depth follows the logarithmic one, so that the near and far planes clip nothing.
    v_log_depth = 1.0 + max(-p.z, 0.0);
    gl_Position.z = (log2(v_log_depth) * u_log_depth - 1.0) * gl_Position.w;
#endif
}
)glsl";

inline constexpr const char* terrain_fragment_shader = R"glsl(
in vec3 v_position;
in vec3 v_normal;
in float v_height;

uniform vec3 u_light; // Towards the light, in camera space; zero lights from the camera.

layout(location = 0) out vec4 o_color;

#ifdef VGL_LOGARITHMIC_DEPTH
uniform float u_log_depth;
in float v_log_depth;
#endif

// Colors by height, from the deep ocean floor to snow.
const int stop_count = 8;
const float stop_heights[stop_count] = float[](
    -8000.0, -3000.0, -100.0, 0.0, 400.0, 1500.0, 3200.0, 5000.0);
const vec3 stop_colors[stop_count] = vec3[](
    vec3(0.04, 0.09, 0.26), vec3(0.11, 0.25, 0.53), vec3(0.36, 0.58, 0.80),
    vec3(0.33, 0.52, 0.29), vec3(0.50, 0.62, 0.33), vec3(0.66, 0.58, 0.40),
    vec3(0.52, 0.44, 0.36), vec3(0.96, 0.96, 0.98));

vec3 height_color(float h)
{
    vec3 c = stop_colors[0];
    for (int i = 1; i < stop_count; ++i)
    {
        float t = clamp((h - stop_heights[i - 1]) / (stop_heights[i] - stop_heights[i - 1]),
                        0.0, 1.0);
        c = mix(c, stop_colors[i], t);
    }
    return c;
}

void main()
{
    vec3 n = normalize(v_normal);
    vec3 l = dot(u_light, u_light) > 0.0 ? u_light : normalize(-v_position);
    float diffuse = max(dot(n, l), 0.0);
    o_color = vec4(height_color(v_height) * (0.3 + 0.7 * diffuse), 1.0);
#ifdef VGL_LOGARITHMIC_DEPTH
    gl_FragDepth = log2(v_log_depth) * u_log_depth * 0.5;
#endif
}
)glsl";

} // namespace vgl::gl

#endif // VGL_BACKENDS_GL_SHADERS_HPP
