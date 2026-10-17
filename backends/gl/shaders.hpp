// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The terrain and sky shaders, in the dialect shared by GLSL 3.30 and GLSL ES 3.00 (design
// document, section 7.6). The backend puts a preamble in front of each: the version, the
// precision for OpenGL ES, VGL_LOGARITHMIC_DEPTH for the logarithmic depth convention, and the
// atmosphere's functions (backends/common/atmosphere.glsl).

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

// The vertex layout of docs/render-interface.md, section 6. The vertices place the surface;
// the fragment shader shades it from the mesh's samples, which may be finer.
inline constexpr const char* terrain_vertex_shader = R"glsl(
layout(location = 0) in vec3 a_position; // Relative to the tile's centre, along the ECEF axes.
layout(location = 1) in float a_height;  // Above the ellipsoid, in metres.
layout(location = 2) in vec2 a_uv;       // Over the tile: u east, v south.
layout(location = 3) in vec2 a_normal;   // Octahedral encoding, along the ECEF axes.

uniform mat4 u_model_view;
uniform mat4 u_projection;
uniform vec4 u_geoid;  // The geoid's heights at the corners: north-west, north-east, south-west, south-east.
uniform vec3 u_centre; // The tile's centre, along the ECEF axes; for directions only.
uniform int u_sea;     // 1 raises terrain below mean sea level to it.
uniform float u_min_height; // The mesh's lowest terrain height; its skirts hang below it.

out vec3 v_position; // Camera space.
out vec3 v_up;       // The vertical, in camera space.
out float v_skirt;   // Above 0 on the skirts, with the sea surface.
out vec2 v_uv;

#ifdef VGL_LOGARITHMIC_DEPTH
uniform float u_log_depth; // 2 / log2(far + 1)
out float v_log_depth;     // 1 + the camera distance
#endif

void main()
{
    // Mean sea level under the vertex, from the geoid at the tile's corners.
    float geoid = mix(mix(u_geoid.x, u_geoid.y, a_uv.x), mix(u_geoid.z, u_geoid.w, a_uv.x), a_uv.y);
    float height = a_height - geoid;
    v_skirt = u_sea != 0 && a_height < u_min_height ? 1.0 : 0.0;
    vec3 up = normalize(u_centre + a_position);
    vec3 position = a_position;
    if (u_sea != 0 && a_height < u_min_height)
    {
        // A skirt: raised as far as the tile's lowest point is, so that it still hangs below the
        // raised surface and covers the cracks between tiles of different levels.
        position += up * max(geoid - u_min_height, 0.0);
    }
    else if (u_sea != 0 && height < 0.0)
    {
        position += up * -height; // Up to the sea surface.
    }
    vec4 p = u_model_view * vec4(position, 1.0);
    v_position = p.xyz;
    v_up = mat3(u_model_view) * up;
    v_uv = a_uv;
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
in vec3 v_up;
in float v_skirt;
in vec2 v_uv;
uniform int u_sea;
uniform mat4 u_model_view;
uniform vec4 u_geoid;

// The mesh's samples (docs/render-interface.md, section 6): per texel, the height above the
// ellipsoid as float bits and the normal as two signed normalized 16-bit halves.
uniform highp usampler2D u_samples;
uniform float u_sample_cells; // Samples along a side, less one.

uniform vec3 u_light; // Towards the light, in camera space; zero lights from the camera.

// The atmosphere (atmosphere.glsl), when u_atmosphere is 1: camera space to its frame, metres to
// kilometres included, the camera in it and its height above the ground, and the sun's
// direction there.
uniform int u_atmosphere;
uniform mat3 u_to_planet;
uniform vec3 u_camera_km;
uniform float u_camera_height;
uniform vec3 u_sun_planet;

// Up to four imagery layers, bottom first (docs/render-interface.md, section 5). Separate
// samplers rather than an array: GLSL ES 3.00 indexes sampler arrays by constants only.
uniform int u_layer_count;
uniform vec4 u_layer_uv[4];  // Scale and offset of the texture coordinates.
uniform vec4 u_layer_opacity;
uniform sampler2D u_layer0;
uniform sampler2D u_layer1;
uniform sampler2D u_layer2;
uniform sampler2D u_layer3;

// Debug views (vgl_gl_debug_flag), and what they show.
uniform int u_debug;              // The flags.
uniform float u_level;            // The tile's level.
uniform float u_imagery_coarser;  // Levels by which the top layer is coarser than the tile.
uniform float u_cells;            // The mesh's grid cells along a side, for the wireframe.
uniform float u_contour_interval; // Metres between contour lines.

layout(location = 0) out vec4 o_color;

#ifdef VGL_LOGARITHMIC_DEPTH
uniform float u_log_depth;
in float v_log_depth;
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

// Two signed normalized 16-bit values, low half first, as unpackSnorm2x16 does, which GLSL
// 3.30 lacks. Converting to int keeps the bits, and shifting a signed int right extends its sign.
vec2 unpack_snorm16x2(uint v)
{
    return max(vec2(float(int(v << 16u) >> 16), float(int(v) >> 16)) / 32767.0, -1.0);
}

// The surface at the fragment, interpolated bilinearly between the four samples around it, by
// hand: integer textures do not filter. The height is above the ellipsoid, and the normal is
// along the ECEF axes and not normalized.
void surface(out float height, out vec3 normal)
{
    vec2 g = clamp(v_uv, 0.0, 1.0) * u_sample_cells;
    vec2 base = min(floor(g), vec2(u_sample_cells - 1.0));
    vec2 f = g - base;
    ivec2 c = ivec2(base);
    uvec2 nw = texelFetch(u_samples, c, 0).xy;
    uvec2 ne = texelFetch(u_samples, c + ivec2(1, 0), 0).xy;
    uvec2 sw = texelFetch(u_samples, c + ivec2(0, 1), 0).xy;
    uvec2 se = texelFetch(u_samples, c + ivec2(1, 1), 0).xy;
    height = mix(mix(uintBitsToFloat(nw.x), uintBitsToFloat(ne.x), f.x),
                 mix(uintBitsToFloat(sw.x), uintBitsToFloat(se.x), f.x), f.y);
    normal = mix(mix(decode_octahedral(unpack_snorm16x2(nw.y)), decode_octahedral(unpack_snorm16x2(ne.y)), f.x),
                 mix(decode_octahedral(unpack_snorm16x2(sw.y)), decode_octahedral(unpack_snorm16x2(se.y)), f.x), f.y);
}

// Colours by height, the conventional hypsometric tints: blues from the deep ocean floor to
// the shelf, then lowland green, yellow, browns, grey rock and snow.
const int stop_count = 13;
const float stop_heights[stop_count] = float[](
    -8000.0, -4000.0, -1500.0, -200.0, -1.0, 0.0, 300.0, 700.0, 1300.0, 2000.0, 3000.0, 4200.0,
    5500.0);
const vec3 stop_colors[stop_count] = vec3[](
    vec3(0.05, 0.10, 0.28), vec3(0.10, 0.27, 0.52), vec3(0.22, 0.47, 0.71),
    vec3(0.50, 0.73, 0.87), vec3(0.73, 0.87, 0.93), vec3(0.36, 0.58, 0.34),
    vec3(0.58, 0.71, 0.43), vec3(0.85, 0.80, 0.55), vec3(0.80, 0.64, 0.42),
    vec3(0.64, 0.48, 0.34), vec3(0.60, 0.55, 0.52), vec3(0.80, 0.79, 0.79),
    vec3(0.97, 0.97, 0.98));

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

// A distinct colour per level.
vec3 level_color(float level)
{
    float h = fract(level * 0.137 + 0.05) * 6.0;
    vec3 c = clamp(vec3(abs(h - 3.0) - 1.0, 2.0 - abs(h - 2.0), 2.0 - abs(h - 4.0)), 0.0, 1.0);
    return mix(vec3(1.0), c, 0.8);
}

// Blends a layer over the colour below it, by its alpha and opacity.
// The share of a pixel covered by lines a pixel wide at every multiple of `spacing` cells, for a
// coordinate g in cells that changes by w cells per pixel: the part of the pixel's footprint
// along the coordinate's screen gradient that the lines overlap, a box filter, so that crowded
// lines blend into a tint instead of shimmering. Lines closer than two pixels fade, and are gone
// at one.
float grid_lines(float g, float w, float spacing)
{
    float x = g / spacing;
    float f = w / spacing; // The footprint, in spacings.
    float width = min(f, 1.0);
    // The lines' length within [0, t], with each line centred on a whole number.
    float lo = x - 0.5 * f + 0.5 * width;
    float hi = x + 0.5 * f + 0.5 * width;
    float covered = floor(hi) * width + min(fract(hi), width) - floor(lo) * width -
                    min(fract(lo), width);
    return clamp(covered / max(f, 1e-6), 0.0, 1.0) * (1.0 - smoothstep(0.5, 1.0, f));
}

// The wireframe's lines along one coordinate: every cell's edge where they are at least four
// pixels apart; where they crowd, every second, fourth, eighth line and so on, a power of two of
// cells that keeps them that far apart, blending into the next as they crowd further.
float wire_lines(float g, float w)
{
    float top = log2(u_cells);
    float level = clamp(log2(max(w * 4.0, 1e-6)), 0.0, top);
    float k = floor(level);
    float fine = grid_lines(g, w, exp2(k));
    float coarse = grid_lines(g, w, exp2(min(k + 1.0, top)));
    return mix(fine, coarse, level - k);
}

vec3 blend(vec3 below, sampler2D layer, vec4 uv, float opacity)
{
    vec4 texel = texture(layer, v_uv * uv.xy + uv.zw);
    return mix(below, texel.rgb, texel.a * opacity);
}

void main()
{
    float height;
    vec3 normal;
    surface(height, normal);
    // Above mean sea level, from the geoid at the tile's corners.
    height -= mix(mix(u_geoid.x, u_geoid.y, v_uv.x), mix(u_geoid.z, u_geoid.w, v_uv.x), v_uv.y);
    vec3 n = normalize(mat3(u_model_view) * normal);
    vec3 l = dot(u_light, u_light) > 0.0 ? u_light : normalize(-v_position);
    float diffuse = max(dot(n, l), 0.0);
    // The height colours show where no imagery covers the terrain.
    vec3 albedo = height_color(height);
    if (u_layer_count > 0 && (u_debug & 8) == 0)
    {
        albedo = blend(albedo, u_layer0, u_layer_uv[0], u_layer_opacity.x);
    }
    if (u_layer_count > 1 && (u_debug & 8) == 0)
    {
        albedo = blend(albedo, u_layer1, u_layer_uv[1], u_layer_opacity.y);
    }
    if (u_layer_count > 2 && (u_debug & 8) == 0)
    {
        albedo = blend(albedo, u_layer2, u_layer_uv[2], u_layer_opacity.z);
    }
    if (u_layer_count > 3 && (u_debug & 8) == 0)
    {
        albedo = blend(albedo, u_layer3, u_layer_uv[3], u_layer_opacity.w);
    }
    if ((u_debug & 2) != 0)
    {
        albedo = mix(albedo, level_color(u_level), 0.5);
        // The tile's border, about a pixel and a half wide.
        vec2 edge = min(v_uv, 1.0 - v_uv) / max(fwidth(v_uv), vec2(1e-6));
        if (min(edge.x, edge.y) < 1.5)
        {
            albedo = vec3(0.05);
        }
    }
    if ((u_debug & 4) != 0)
    {
        if (u_layer_count == 0)
        {
            albedo = mix(albedo, vec3(1.0, 0.0, 1.0), 0.6);
        }
        else if (u_imagery_coarser > 0.0)
        {
            albedo = mix(albedo, vec3(1.0, 0.5, 0.0), min(0.25 + 0.2 * u_imagery_coarser, 0.8));
        }
    }
    if ((u_debug & 16) != 0)
    {
        // Contours, minor and every fifth major. From the height's screen gradient: the distance
        // to the nearest line in pixels, so that lines keep their width at every distance and
        // are anti-aliased, and how closely lines crowd, so that they fade out before they turn
        // into noise (from 16 pixels apart down to 6), the major ones later than the minor.
        float x = height / u_contour_interval;
        float dx = max(fwidth(x), 1e-6);
        float minor_pixels = abs(fract(x - 0.5) - 0.5) / dx;
        float minor = (1.0 - smoothstep(0.3, 1.0, minor_pixels)) * (1.0 - smoothstep(0.06, 0.16, dx));
        float y = x / 5.0;
        float dy = dx / 5.0;
        float major_pixels = abs(fract(y - 0.5) - 0.5) / dy;
        float major = (1.0 - smoothstep(0.6, 1.4, major_pixels)) * (1.0 - smoothstep(0.06, 0.16, dy));
        albedo = mix(albedo, vec3(0.22, 0.17, 0.12), minor * 0.55);
        albedo = mix(albedo, vec3(0.08, 0.05, 0.03), major * 0.85);
    }
    // The sunlight and the ambient light: with the atmosphere, the sunlight that reaches the
    // terrain, reddened and dimmed by its path, and the daylight left as the sun sets.
    vec3 sunlight = vec3(1.0);
    float ambient = 1.0;
    vec3 offset = u_to_planet * v_position; // From the camera, in the atmosphere's frame.
    if (u_atmosphere != 0)
    {
        vec3 p = u_camera_km + offset;
        float r = length(p);
        float mu_s = dot(p, u_sun_planet) / r;
        sunlight = vgl_sun_transmittance(r, mu_s) / vgl_zenith_transmittance;
        ambient = vgl_daylight(mu_s);
    }
    vec3 lit = albedo * (0.3 * ambient + 0.7 * diffuse * sunlight);
    // Skirts take the colour of the edge above them, as without the sea, so that they do not
    // show as water where they fill a crack on land.
    if (u_sea != 0 && height < 0.0 && v_skirt == 0.0)
    {
        // Water: the imagery's ocean, or a deep blue, under a level surface with a glint of
        // the light.
        vec3 water = u_layer_count > 0 && (u_debug & 8) == 0 ? mix(albedo, vec3(0.04, 0.16, 0.36), 0.35)
                                                           : vec3(0.06, 0.2, 0.42);
        vec3 n_sea = normalize(v_up);
        float d = max(dot(n_sea, l), 0.0);
        float glint = pow(max(dot(reflect(-l, n_sea), normalize(-v_position)), 0.0), 80.0);
        lit = water * (0.35 * ambient + 0.65 * d * sunlight) + vec3(0.5) * glint * sunlight;
    }
    if (u_atmosphere != 0)
    {
        // The haze between the camera and the terrain.
        float distance = length(offset);
        vec3 inscatter;
        vec3 transmittance;
        vgl_atmosphere(u_camera_km, u_camera_height, offset / max(distance, 1e-9), distance,
                       u_sun_planet, 8, inscatter, transmittance);
        lit = lit * transmittance + vgl_expose(inscatter * vgl_sky_brightness);
    }
    o_color = vec4(lit, 1.0);
    if ((u_debug & 1) != 0 && u_cells > 0.0)
    {
        // The wireframe: the grid's cells, without the diagonals that split them into triangles,
        // along each texture coordinate. A coordinate that barely changes across a fragment
        // compared with the other, as on the skirts that hang from a tile's edges, where it is
        // constant but for rounding, draws none. Bright cyan: clear on dark and light ground.
        vec2 g = v_uv * u_cells;
        vec2 w = vec2(length(vec2(dFdx(g.x), dFdy(g.x))), length(vec2(dFdx(g.y), dFdy(g.y))));
        vec2 along = step(1e-3 * vec2(w.y, w.x), w);
        vec2 on = vec2(wire_lines(g.x, w.x), wire_lines(g.y, w.y)) * along;
        o_color.rgb = mix(o_color.rgb, vec3(0.3, 1.0, 0.95), max(on.x, on.y));
        // The tile's boundary in yellow: a pixel solid and a pixel fading inside each tile, so
        // about two pixels where two tiles meet, from the distance to the edge in pixels.
        vec2 pixels = min(g, u_cells - g) / max(w, vec2(1e-9));
        vec2 edge = clamp(2.0 - pixels, 0.0, 1.0) * along;
        o_color.rgb = mix(o_color.rgb, vec3(1.0, 0.85, 0.1), max(edge.x, edge.y));
    }
#ifdef VGL_LOGARITHMIC_DEPTH
    gl_FragDepth = log2(v_log_depth) * u_log_depth * 0.5;
#endif
}
)glsl";

// The sky: one triangle over the whole viewport at the far depth, so that it shows only where
// nothing was drawn before it.
inline constexpr const char* sky_vertex_shader = R"glsl(
uniform float u_far_depth; // The far plane's depth in normalized device coordinates.
out vec2 v_ndc;

void main()
{
    v_ndc = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, u_far_depth, 1.0);
}
)glsl";

inline constexpr const char* sky_fragment_shader = R"glsl(
in vec2 v_ndc;
uniform mat4 u_inverse_projection;
uniform mat3 u_to_planet;
uniform vec3 u_camera_km;
uniform float u_camera_height;
uniform vec3 u_sun_planet;
uniform float u_mid_depth; // A depth between the near and far planes.

layout(location = 0) out vec4 o_color;

void main()
{
    // The view ray through the pixel, from a point on it in camera space.
    vec4 q = u_inverse_projection * vec4(v_ndc, u_mid_depth, 1.0);
    vec3 direction = normalize(u_to_planet * (q.xyz / q.w));
    vec3 inscatter;
    vec3 transmittance;
    vgl_atmosphere(u_camera_km, u_camera_height, direction, 1.0e9, u_sun_planet, 16, inscatter,
                   transmittance);
    // The sun's disc, half a degree across, softened over a pixel or so.
    float disc = smoothstep(0.999955, 0.999975, dot(direction, u_sun_planet));
    vec3 color = vgl_expose(inscatter * vgl_sky_brightness) + transmittance * disc;
    // Over what the framebuffer holds, which shows through as far as the sky is clear.
    o_color = vec4(color, (transmittance.r + transmittance.g + transmittance.b) / 3.0);
}
)glsl";

} // namespace vgl::gl

#endif // VGL_BACKENDS_GL_SHADERS_HPP
