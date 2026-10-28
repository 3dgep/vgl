// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The terrain and sky shaders, written with Ogre's unified shader macros (OgreUnifiedShader.h),
// so that one source compiles as GLSL, GLSL ES, HLSL, Vulkan's GLSL and Metal (design document,
// section 7.6). What the macros do not cover, reading the samples texel by texel and the depth
// output, has a branch per language. The shading is the OpenGL backend's
// (backends/gl/shaders.hpp), which documents it. The globe defines VGL_LOGARITHMIC_DEPTH for
// the logarithmic depth convention, and puts the atmosphere's functions
// (backends/common/atmosphere.glsl) where a fragment shader has the line //@vgl_atmosphere.

#ifndef VGL_BACKENDS_OGRE_SHADERS_HPP
#define VGL_BACKENDS_OGRE_SHADERS_HPP

namespace vgl::ogre::detail
{

inline constexpr const char* terrain_vertex_shader = R"shader(
#include <OgreUnifiedShader.h>

OGRE_UNIFORMS(
    uniform mat4 u_world_view;      // The draw item's model_view: Ogre's view is the identity.
    uniform mat4 u_world_view_proj;
    uniform vec4 u_geoid;  // The geoid's heights at the corners: north-west, north-east, south-west, south-east.
    uniform vec4 u_centre; // The tile's centre along the ECEF axes, and its lowest terrain height.
    uniform vec4 u_frame;  // The sea surface (0 or 1), log depth's 2 / log2(far + 1), unused, unused.
)

MAIN_PARAMETERS
IN(vec4 vertex, POSITION) // Relative to the tile's centre, along the ECEF axes.
IN(vec2 uv0, TEXCOORD0)   // Over the tile: u east, v south.
IN(float uv1, TEXCOORD1)  // The height above the ellipsoid, in metres.
OUT(vec3 v_position, TEXCOORD0) // Camera space.
OUT(vec3 v_up, TEXCOORD1)       // The vertical, in camera space.
OUT(vec2 v_uv, TEXCOORD2)
OUT(float v_skirt, TEXCOORD3) // Above 0 on the skirts, with the sea surface.
OUT(float v_log_depth, TEXCOORD4) // 1 + the camera distance
MAIN_DECLARATION
{
    bool sea = u_frame.x > 0.5;
    float min_height = u_centre.w;
    float geoid = mix(mix(u_geoid.x, u_geoid.y, uv0.x), mix(u_geoid.z, u_geoid.w, uv0.x), uv0.y);
    float height = uv1 - geoid;
    v_skirt = sea && uv1 < min_height ? 1.0 : 0.0;
    vec3 up = normalize(u_centre.xyz + vertex.xyz);
    vec3 position = vertex.xyz;
    if (sea && uv1 < min_height)
    {
        position += up * max(geoid - min_height, 0.0);
    }
    else if (sea && height < 0.0)
    {
        position += up * -height;
    }
    vec4 p = mul(u_world_view, vec4(position, 1.0));
    v_position = p.xyz;
    v_up = mul(u_world_view, vec4(up, 0.0)).xyz;
    v_uv = uv0;
    gl_Position = mul(u_world_view_proj, vec4(position, 1.0));
    v_log_depth = 1.0 + max(-p.z, 0.0);
#ifdef VGL_LOGARITHMIC_DEPTH
    // The clip-space depth follows the logarithmic one, so that the near and far planes clip
    // nothing: from -w to w in OpenGL, from 0 to w in Direct3D.
#   ifdef OGRE_HLSL
    gl_Position.z = log2(v_log_depth) * u_frame.y * 0.5 * gl_Position.w;
#   else
    gl_Position.z = (log2(v_log_depth) * u_frame.y - 1.0) * gl_Position.w;
#   endif
#endif
}
)shader";

inline constexpr const char* terrain_fragment_shader = R"shader(
#include <OgreUnifiedShader.h>
//@vgl_atmosphere

// The mesh's samples, as the globe converts them (globe.cpp): per texel, two floats, the
// height above the ellipsoid and the octahedral normal's two components at 12 bits each, packed
// as a whole number below 2^24, which a float holds exactly. Ogre's Direct3D 11 render system
// has no integer textures.
#ifdef OGRE_HLSL
Texture2D<float2> u_samples : register(t0);
#   define VGL_FETCH(c) u_samples.Load(int3(c, 0)).xy
#else
uniform highp sampler2D u_samples;
#   define VGL_FETCH(c) texelFetch(u_samples, c, 0).xy
#endif

// Up to four imagery layers, bottom first.
SAMPLER2D(u_layer0, 1);
SAMPLER2D(u_layer1, 2);
SAMPLER2D(u_layer2, 3);
SAMPLER2D(u_layer3, 4);
// The clouds' coverage (atmosphere.glsl), round the globe in longitude.
SAMPLER2D(u_clouds, 5);

OGRE_UNIFORMS(
    uniform mat4 u_world_view;
    uniform vec4 u_geoid;
    uniform vec4 u_light;   // Towards the light in camera space, zero to light from the camera; the debug flags.
    uniform vec4 u_frame;   // The sea surface (0 or 1), log depth's 2 / log2(far + 1), the contour interval, unused.
    uniform vec4 u_item;    // The layer count, the tile's level, levels by which the imagery is coarser, grid cells along a side.
    uniform vec4 u_samples_info; // Samples along a side, less one; unused.
    uniform vec4 u_layer_uv0;
    uniform vec4 u_layer_uv1;
    uniform vec4 u_layer_uv2;
    uniform vec4 u_layer_uv3;
    uniform vec4 u_layer_opacity;
    // The atmosphere (atmosphere.glsl): camera space to its frame, a row in each, metres to
    // kilometres included; the camera in it and its height above the ground; and the sun's
    // direction there, with 1 in w when the atmosphere is drawn.
    uniform vec4 u_to_planet0;
    uniform vec4 u_to_planet1;
    uniform vec4 u_to_planet2;
    uniform vec4 u_camera;
    uniform vec4 u_sun_planet;
    uniform vec4 u_cloud; // Drawn, the shell's height and the image's texels at the equator in kilometres.
)

vec3 to_planet(vec3 v)
{
    return vec3(dot(u_to_planet0.xyz, v), dot(u_to_planet1.xyz, v), dot(u_to_planet2.xyz, v));
}

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

vec3 unpack_normal(float bits)
{
    vec2 q = vec2(mod(bits, 4096.0), floor(bits / 4096.0));
    return decode_octahedral(q / 4095.0 * 2.0 - 1.0);
}

void surface(vec2 uv, out float height, out vec3 normal)
{
    float cells = u_samples_info.x;
    vec2 g = clamp(uv, 0.0, 1.0) * cells;
    vec2 base = min(floor(g), vec2_splat(cells - 1.0));
    vec2 f = g - base;
    ivec2 c = ivec2(base);
    vec2 nw = VGL_FETCH(c);
    vec2 ne = VGL_FETCH(c + ivec2(1, 0));
    vec2 sw = VGL_FETCH(c + ivec2(0, 1));
    vec2 se = VGL_FETCH(c + ivec2(1, 1));
    height = mix(mix(nw.x, ne.x, f.x), mix(sw.x, se.x, f.x), f.y);
    normal = mix(mix(unpack_normal(nw.y), unpack_normal(ne.y), f.x),
                 mix(unpack_normal(sw.y), unpack_normal(se.y), f.x), f.y);
}

// Colours by height, the conventional hypsometric tints, stop by stop.
vec3 height_stop(vec3 c, float h, float lo, float hi, vec3 colour)
{
    return mix(c, colour, clamp((h - lo) / (hi - lo), 0.0, 1.0));
}

vec3 height_color(float h)
{
    vec3 c = vec3(0.05, 0.10, 0.28);
    c = height_stop(c, h, -8000.0, -4000.0, vec3(0.10, 0.27, 0.52));
    c = height_stop(c, h, -4000.0, -1500.0, vec3(0.22, 0.47, 0.71));
    c = height_stop(c, h, -1500.0, -200.0, vec3(0.50, 0.73, 0.87));
    c = height_stop(c, h, -200.0, -1.0, vec3(0.73, 0.87, 0.93));
    c = height_stop(c, h, -1.0, 0.0, vec3(0.36, 0.58, 0.34));
    c = height_stop(c, h, 0.0, 300.0, vec3(0.58, 0.71, 0.43));
    c = height_stop(c, h, 300.0, 700.0, vec3(0.85, 0.80, 0.55));
    c = height_stop(c, h, 700.0, 1300.0, vec3(0.80, 0.64, 0.42));
    c = height_stop(c, h, 1300.0, 2000.0, vec3(0.64, 0.48, 0.34));
    c = height_stop(c, h, 2000.0, 3000.0, vec3(0.60, 0.55, 0.52));
    c = height_stop(c, h, 3000.0, 4200.0, vec3(0.80, 0.79, 0.79));
    c = height_stop(c, h, 4200.0, 5500.0, vec3(0.97, 0.97, 0.98));
    return c;
}

vec3 level_color(float level)
{
    float h = fract(level * 0.137 + 0.05) * 6.0;
    vec3 c = clamp(vec3(abs(h - 3.0) - 1.0, 2.0 - abs(h - 2.0), 2.0 - abs(h - 4.0)), 0.0, 1.0);
    return mix(vec3_splat(1.0), c, 0.8);
}

float grid_lines(float g, float w, float spacing)
{
    float x = g / spacing;
    float f = w / spacing;
    float width = min(f, 1.0);
    float lo = x - 0.5 * f + 0.5 * width;
    float hi = x + 0.5 * f + 0.5 * width;
    float covered = floor(hi) * width + min(fract(hi), width) - floor(lo) * width - min(fract(lo), width);
    return clamp(covered / max(f, 1e-6), 0.0, 1.0) * (1.0 - smoothstep(0.5, 1.0, f));
}

float wire_lines(float g, float w, float cells)
{
    float top = log2(cells);
    float level = clamp(log2(max(w * 4.0, 1e-6)), 0.0, top);
    float k = floor(level);
    float fine = grid_lines(g, w, exp2(k));
    float coarse = grid_lines(g, w, exp2(min(k + 1.0, top)));
    return mix(fine, coarse, level - k);
}

MAIN_PARAMETERS
IN(vec3 v_position, TEXCOORD0)
IN(vec3 v_up, TEXCOORD1)
IN(vec2 v_uv, TEXCOORD2)
IN(float v_skirt, TEXCOORD3)
IN(float v_log_depth, TEXCOORD4)
#if defined(VGL_LOGARITHMIC_DEPTH) && defined(OGRE_HLSL)
OUT(float vgl_depth, SV_Depth)
#endif
MAIN_DECLARATION
{
    int debug = int(u_light.w);
    bool sea = u_frame.x > 0.5;
    float layers = u_item.x;
    float cells = u_item.w;
    float height;
    vec3 normal;
    surface(v_uv, height, normal);
    height -= mix(mix(u_geoid.x, u_geoid.y, v_uv.x), mix(u_geoid.z, u_geoid.w, v_uv.x), v_uv.y);
    vec3 n = normalize(mul(u_world_view, vec4(normal, 0.0)).xyz);
    vec3 l = dot(u_light.xyz, u_light.xyz) > 0.0 ? u_light.xyz : normalize(-v_position);
    float diffuse = max(dot(n, l), 0.0);
    vec3 albedo = height_color(height);
    bool imagery = (debug & 8) == 0;
    if (layers > 0.5 && imagery)
    {
        vec4 t = texture2D(u_layer0, v_uv * u_layer_uv0.xy + u_layer_uv0.zw);
        albedo = mix(albedo, t.rgb, t.a * u_layer_opacity.x);
    }
    if (layers > 1.5 && imagery)
    {
        vec4 t = texture2D(u_layer1, v_uv * u_layer_uv1.xy + u_layer_uv1.zw);
        albedo = mix(albedo, t.rgb, t.a * u_layer_opacity.y);
    }
    if (layers > 2.5 && imagery)
    {
        vec4 t = texture2D(u_layer2, v_uv * u_layer_uv2.xy + u_layer_uv2.zw);
        albedo = mix(albedo, t.rgb, t.a * u_layer_opacity.z);
    }
    if (layers > 3.5 && imagery)
    {
        vec4 t = texture2D(u_layer3, v_uv * u_layer_uv3.xy + u_layer_uv3.zw);
        albedo = mix(albedo, t.rgb, t.a * u_layer_opacity.w);
    }
    if ((debug & 2) != 0)
    {
        albedo = mix(albedo, level_color(u_item.y), 0.5);
        vec2 edge = min(v_uv, 1.0 - v_uv) / max(fwidth(v_uv), vec2_splat(1e-6));
        if (min(edge.x, edge.y) < 1.5)
        {
            albedo = vec3_splat(0.05);
        }
    }
    if ((debug & 4) != 0)
    {
        if (layers < 0.5)
        {
            albedo = mix(albedo, vec3(1.0, 0.0, 1.0), 0.6);
        }
        else if (u_item.z > 0.0)
        {
            albedo = mix(albedo, vec3(1.0, 0.5, 0.0), min(0.25 + 0.2 * u_item.z, 0.8));
        }
    }
    if ((debug & 16) != 0)
    {
        float x = height / u_frame.z;
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
    bool atmosphere = u_sun_planet.w > 0.5;
    vec3 sunlight = vec3_splat(1.0);
    float ambient = 1.0;
    vec3 offset = to_planet(v_position);
    if (atmosphere)
    {
        vec3 p = u_camera.xyz + offset;
        float r = length(p);
        float mu_s = dot(p, u_sun_planet.xyz) / r;
        sunlight = vgl_sun_transmittance(r, mu_s) / vgl_zenith_transmittance;
        ambient = vgl_daylight(mu_s);
        if (u_cloud.x > 0.5)
        {
            float ts = vgl_cloud_shadow_distance(p, u_sun_planet.xyz, u_cloud.y);
            vec3 crossing = p + u_sun_planet.xyz * max(ts, 0.0);
            float lod = vgl_cloud_lod(length(fwidth(crossing)), u_cloud.z);
            float cover = vgl_cloud_alpha(texture2DLod(u_clouds, vgl_cloud_uv(crossing), lod).r)
                * vgl_cloud_fade(u_camera.w, u_cloud.y, u_cloud.z);
            sunlight *= ts > 0.0 ? 1.0 - 0.75 * cover : 1.0;
        }
    }
    vec3 lit = albedo * (0.3 * ambient + 0.7 * diffuse * sunlight);
    if (sea && height < 0.0 && v_skirt == 0.0)
    {
        vec3 water = layers > 0.5 && imagery ? mix(albedo, vec3(0.04, 0.16, 0.36), 0.35)
                                             : vec3(0.06, 0.2, 0.42);
        vec3 n_sea = normalize(v_up);
        float d = max(dot(n_sea, l), 0.0);
        float glint = pow(max(dot(reflect(-l, n_sea), normalize(-v_position)), 0.0), 80.0);
        lit = water * (0.35 * ambient + 0.65 * d * sunlight) + vec3_splat(0.5) * glint * sunlight;
    }
    if (atmosphere)
    {
        float distance = length(offset);
        vec3 inscatter;
        vec3 transmittance;
        vec3 direction = offset / max(distance, 1e-9);
        vgl_atmosphere(u_camera.xyz, u_camera.w, direction, distance, u_sun_planet.xyz, 8,
                       inscatter, transmittance);
        lit = lit * transmittance + vgl_expose(inscatter * vgl_sky_brightness);
        if (u_cloud.x > 0.5)
        {
            float tc = vgl_cloud_distance(u_camera.xyz, u_camera.w, direction, u_cloud.y, distance);
            vec3 hit = u_camera.xyz + direction * max(tc, 0.0);
            float lod = vgl_cloud_lod(length(fwidth(hit)), u_cloud.z);
            float cover = vgl_cloud_alpha(texture2DLod(u_clouds, vgl_cloud_uv(hit), lod).r)
                * vgl_cloud_fade(u_camera.w, u_cloud.y, u_cloud.z);
            if (tc > 0.0 && cover > 0.0)
            {
                vec3 near_inscatter;
                vec3 near_transmittance;
                vgl_atmosphere(u_camera.xyz, u_camera.w, direction, tc, u_sun_planet.xyz, 6,
                               near_inscatter, near_transmittance);
                vec3 cloud = vgl_cloud_light(hit, u_sun_planet.xyz, cover, u_camera.w < u_cloud.y);
                lit = mix(lit, cloud * near_transmittance + vgl_expose(near_inscatter * vgl_sky_brightness), cover);
            }
        }
    }
    vec4 colour = vec4(lit, 1.0);
    if ((debug & 1) != 0 && cells > 0.0)
    {
        vec2 g = v_uv * cells;
        vec2 w = vec2(length(vec2(dFdx(g.x), dFdy(g.x))), length(vec2(dFdx(g.y), dFdy(g.y))));
        vec2 along = step(1e-3 * vec2(w.y, w.x), w);
        vec2 on = vec2(wire_lines(g.x, w.x, cells), wire_lines(g.y, w.y, cells)) * along;
        colour.rgb = mix(colour.rgb, vec3(0.3, 1.0, 0.95), max(on.x, on.y));
        vec2 pixels = min(g, cells - g) / max(w, vec2_splat(1e-9));
        vec2 edge = clamp(2.0 - pixels, 0.0, 1.0) * along;
        colour.rgb = mix(colour.rgb, vec3(1.0, 0.85, 0.1), max(edge.x, edge.y));
    }
    gl_FragColor = colour;
#ifdef VGL_LOGARITHMIC_DEPTH
#   ifdef OGRE_HLSL
    vgl_depth = log2(v_log_depth) * u_frame.y * 0.5;
#   else
    gl_FragDepth = log2(v_log_depth) * u_frame.y * 0.5;
#   endif
#endif
}
)shader";

// The sky (backends/gl/shaders.hpp): one triangle over the whole viewport at the far depth, from
// three vertices in normalized device coordinates, so that it shows only where nothing was
// drawn before it.
inline constexpr const char* sky_vertex_shader = R"shader(
#include <OgreUnifiedShader.h>

OGRE_UNIFORMS(
    uniform vec4 u_sky; // The far plane's depth in normalized device coordinates; unused.
)

MAIN_PARAMETERS
IN(vec4 vertex, POSITION)
OUT(vec2 v_ndc, TEXCOORD0)
MAIN_DECLARATION
{
    v_ndc = vertex.xy;
    gl_Position = vec4(vertex.xy, u_sky.x, 1.0);
}
)shader";

inline constexpr const char* sky_fragment_shader = R"shader(
#include <OgreUnifiedShader.h>
//@vgl_atmosphere

SAMPLER2D(u_clouds, 0);

OGRE_UNIFORMS(
    uniform mat4 u_inverse_projection; // Ogre's, flipped for render textures as it draws them.
    uniform vec4 u_to_planet0;
    uniform vec4 u_to_planet1;
    uniform vec4 u_to_planet2;
    uniform vec4 u_camera;
    uniform vec4 u_sun_planet;
    uniform vec4 u_cloud;
)

vec3 to_planet(vec3 v)
{
    return vec3(dot(u_to_planet0.xyz, v), dot(u_to_planet1.xyz, v), dot(u_to_planet2.xyz, v));
}

MAIN_PARAMETERS
IN(vec2 v_ndc, TEXCOORD0)
MAIN_DECLARATION
{
    // The view ray through the pixel, from a point on it in camera space, between the near and
    // far planes in every depth convention.
    vec4 q = mul(u_inverse_projection, vec4(v_ndc, 0.5, 1.0));
    vec3 direction = normalize(to_planet(q.xyz / q.w));
    vec3 inscatter;
    vec3 transmittance;
    vgl_atmosphere(u_camera.xyz, u_camera.w, direction, 1.0e9, u_sun_planet.xyz, 16, inscatter,
                   transmittance);
    float disc = smoothstep(0.999955, 0.999975, dot(direction, u_sun_planet.xyz));
    vec3 colour = vgl_expose(inscatter * vgl_sky_brightness) + transmittance * disc;
    float clear = (transmittance.r + transmittance.g + transmittance.b) / 3.0;
    if (u_cloud.x > 0.5)
    {
        float tc = vgl_cloud_distance(u_camera.xyz, u_camera.w, direction, u_cloud.y, 1.0e9);
        vec3 hit = u_camera.xyz + direction * max(tc, 0.0);
        float lod = vgl_cloud_lod(length(fwidth(hit)), u_cloud.z);
        float cover = vgl_cloud_alpha(texture2DLod(u_clouds, vgl_cloud_uv(hit), lod).r)
            * vgl_cloud_fade(u_camera.w, u_cloud.y, u_cloud.z);
        if (tc > 0.0 && cover > 0.0)
        {
            vec3 near_inscatter;
            vec3 near_transmittance;
            vgl_atmosphere(u_camera.xyz, u_camera.w, direction, tc, u_sun_planet.xyz, 8,
                           near_inscatter, near_transmittance);
            vec3 cloud = vgl_cloud_light(hit, u_sun_planet.xyz, cover, u_camera.w < u_cloud.y);
            colour = mix(colour, cloud * near_transmittance + vgl_expose(near_inscatter * vgl_sky_brightness), cover);
            clear *= 1.0 - cover;
        }
    }
    gl_FragColor = vec4(colour, clear);
}
)shader";

// The stars (backends/gl/shaders.hpp): a quad per star at the far depth, widened in pixels
// around the star's direction.
inline constexpr const char* stars_vertex_shader = R"shader(
#include <OgreUnifiedShader.h>
//@vgl_atmosphere

OGRE_UNIFORMS(
    uniform mat4 u_projection; // Ogre's, flipped for render textures as it draws them.
    uniform vec4 u_viewport;   // Width and height in pixels, and their inverses.
    uniform vec4 u_celestial0; // The celestial frame to camera space, a row in each.
    uniform vec4 u_celestial1;
    uniform vec4 u_celestial2;
    uniform vec4 u_sky;        // The far plane's depth in normalized device coordinates; unused.
    uniform vec4 u_to_planet0;
    uniform vec4 u_to_planet1;
    uniform vec4 u_to_planet2;
    uniform vec4 u_camera;
    uniform vec4 u_sun_planet;
)

MAIN_PARAMETERS
IN(vec4 vertex, POSITION) // The direction, in the celestial frame.
IN(vec2 uv0, TEXCOORD0)   // The corner: -1 or 1 on each axis.
IN(vec2 uv1, TEXCOORD1)   // Intensity, radius in pixels.
IN(vec3 uv2, TEXCOORD2)   // Colour.
OUT(vec2 v_corner, TEXCOORD0)
OUT(vec3 v_color, TEXCOORD1)
MAIN_DECLARATION
{
    vec3 a = vertex.xyz;
    vec3 d = vec3(dot(u_celestial0.xyz, a), dot(u_celestial1.xyz, a), dot(u_celestial2.xyz, a));
    vec4 clip = mul(u_projection, vec4(d, 0.0));
    float visible = 1.0;
    if (u_sun_planet.w > 0.5)
    {
        vec3 p = vec3(dot(u_to_planet0.xyz, d), dot(u_to_planet1.xyz, d), dot(u_to_planet2.xyz, d));
        visible = vgl_star_visibility(u_camera.xyz, u_camera.w, normalize(p), u_sun_planet.xyz);
    }
    v_color = uv2 * (uv1.x * visible);
    v_corner = uv0;
    clip.xy += uv0 * uv1.y * 2.0 * u_viewport.zw * clip.w;
    clip.z = u_sky.x * clip.w;
    gl_Position = clip.w > 0.0 ? clip : vec4(0.0, 0.0, 2.0, 1.0);
}
)shader";

inline constexpr const char* stars_fragment_shader = R"shader(
#include <OgreUnifiedShader.h>

MAIN_PARAMETERS
IN(vec2 v_corner, TEXCOORD0)
IN(vec3 v_color, TEXCOORD1)
MAIN_DECLARATION
{
    float glow = exp(-3.0 * dot(v_corner, v_corner));
    gl_FragColor = vec4(v_color * glow, 0.0);
}
)shader";

} // namespace vgl::ogre::detail

#endif // VGL_BACKENDS_OGRE_SHADERS_HPP
