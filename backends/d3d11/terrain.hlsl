// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The terrain and sky shaders for shader model 5.0, ported from the OpenGL backend's GLSL
// (backends/gl/shaders.hpp), which documents the shading. They are compiled at build time with
// fxc, with VGL_LOGARITHMIC_DEPTH defined for the logarithmic depth convention, and embedded in
// the library (design document, section 7.6). The atmosphere's functions are the ones every
// backend shares, in GLSL, which these definitions turn into HLSL.

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define mix lerp
#define VGL_CONST static const
#include "../common/atmosphere.glsl"

// Per frame (backend.cpp, frame_constants).
cbuffer frame_constants : register(b0)
{
    float4x4 u_projection;
    float3 u_light; // Towards the light, in camera space; zero lights from the camera.
    int u_debug;    // The vgl_d3d11_debug_flag values.
    float u_log_depth; // 2 / log2(far + 1)
    int u_sea;         // 1 raises terrain below mean sea level to it.
    float u_contour_interval;
    float u_frame_pad;
    // The atmosphere (atmosphere.glsl), when u_atmosphere is 1: camera space to its frame, a
    // row in each float4, metres to kilometres included; the camera in it and its height above
    // the ground; and the sun's direction there.
    float4 u_to_planet[3];
    float3 u_camera_km;
    float u_camera_height;
    float3 u_sun_planet;
    int u_atmosphere;
    // The sky: the inverse of the projection, and the far plane's depth.
    float4x4 u_inverse_projection;
    float u_far_depth;
    float3 u_sky_pad;
    // The stars: the celestial frame to camera space, a row in each float4, and the viewport's
    // size in pixels.
    float4 u_celestial[3];
    float2 u_viewport;
    float2 u_stars_pad;
};

// Camera space to the atmosphere's frame.
float3 to_planet(float3 v)
{
    return float3(dot(u_to_planet[0].xyz, v), dot(u_to_planet[1].xyz, v), dot(u_to_planet[2].xyz, v));
}

// Per tile (backend.cpp, item_constants).
cbuffer item_constants : register(b1)
{
    float4x4 u_model_view; // Column-major, as the draw item has it.
    float4 u_geoid;  // The geoid's heights at the corners: north-west, north-east, south-west, south-east.
    float3 u_centre; // The tile's centre, along the ECEF axes; for directions only.
    float u_min_height; // The mesh's lowest terrain height; its skirts hang below it.
    float4 u_layer_uv[4]; // Scale and offset of each layer's texture coordinates.
    float4 u_layer_opacity;
    int u_layer_count;
    float u_level;           // The tile's level.
    float u_imagery_coarser; // Levels by which the top layer is coarser than the tile.
    float u_cells;           // The mesh's grid cells along a side, for the wireframe.
    float u_sample_cells;    // Samples along a side, less one.
    float3 u_item_pad;
};

Texture2D u_layer0 : register(t0);
Texture2D u_layer1 : register(t1);
Texture2D u_layer2 : register(t2);
Texture2D u_layer3 : register(t3);
// The mesh's samples (docs/render-interface.md, section 6): per texel, the height above the
// ellipsoid as float bits and the normal as two signed normalized 16-bit halves.
Texture2D<uint2> u_samples : register(t4);
SamplerState u_sampler : register(s0);

struct vertex_input
{
    float3 position : POSITION; // Relative to the tile's centre, along the ECEF axes.
    float height : HEIGHT;      // Above the ellipsoid, in metres.
    float2 uv : TEXCOORD;       // Over the tile: u east, v south.
};

struct varyings
{
    float4 clip : SV_Position;
    float3 position : POSITION; // Camera space.
    float3 up : NORMAL;         // The vertical, in camera space.
    float skirt : TEXCOORD1;    // Above 0 on the skirts, with the sea surface.
    float2 uv : TEXCOORD0;
#ifdef VGL_LOGARITHMIC_DEPTH
    float log_depth : TEXCOORD2; // 1 + the camera distance
#endif
};

varyings vs_main(vertex_input v)
{
    varyings o;
    // Mean sea level under the vertex, from the geoid at the tile's corners.
    float geoid = lerp(lerp(u_geoid.x, u_geoid.y, v.uv.x), lerp(u_geoid.z, u_geoid.w, v.uv.x), v.uv.y);
    float height = v.height - geoid;
    o.skirt = u_sea != 0 && v.height < u_min_height ? 1.0 : 0.0;
    float3 up = normalize(u_centre + v.position);
    float3 position = v.position;
    if (u_sea != 0 && v.height < u_min_height)
    {
        // A skirt: raised as far as the tile's lowest point is, so that it still hangs below the
        // raised surface and covers the cracks between tiles of different levels.
        position += up * max(geoid - u_min_height, 0.0);
    }
    else if (u_sea != 0 && height < 0.0)
    {
        position += up * -height; // Up to the sea surface.
    }
    float4 p = mul(u_model_view, float4(position, 1.0));
    o.position = p.xyz;
    o.up = mul((float3x3)u_model_view, up);
    o.uv = v.uv;
    o.clip = mul(u_projection, p);
#ifdef VGL_LOGARITHMIC_DEPTH
    // The distance along the view axis, so orthographic projections work too. The clip-space
    // depth follows the logarithmic one, from 0 to 1 as in Direct3D, so that the near and far
    // planes clip nothing.
    o.log_depth = 1.0 + max(-p.z, 0.0);
    o.clip.z = log2(o.log_depth) * u_log_depth * 0.5 * o.clip.w;
#endif
    return o;
}

float3 decode_octahedral(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        float2 s = float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(e.yx)) * s;
    }
    return normalize(n);
}

// Two signed normalized 16-bit values, low half first. Shifting a signed int right extends its
// sign.
float2 unpack_snorm16x2(uint v)
{
    return max(float2(float(int(v << 16) >> 16), float(int(v) >> 16)) / 32767.0, -1.0);
}

// The surface at the pixel, interpolated bilinearly between the four samples around it, by
// hand: integer textures do not filter. The height is above the ellipsoid, and the normal is
// along the ECEF axes and not normalized.
void surface(float2 uv, out float height, out float3 normal)
{
    float2 g = saturate(uv) * u_sample_cells;
    float2 base = min(floor(g), float2(u_sample_cells - 1.0, u_sample_cells - 1.0));
    float2 f = g - base;
    int2 c = int2(base);
    uint2 nw = u_samples.Load(int3(c, 0));
    uint2 ne = u_samples.Load(int3(c + int2(1, 0), 0));
    uint2 sw = u_samples.Load(int3(c + int2(0, 1), 0));
    uint2 se = u_samples.Load(int3(c + int2(1, 1), 0));
    height = lerp(lerp(asfloat(nw.x), asfloat(ne.x), f.x), lerp(asfloat(sw.x), asfloat(se.x), f.x), f.y);
    normal = lerp(lerp(decode_octahedral(unpack_snorm16x2(nw.y)), decode_octahedral(unpack_snorm16x2(ne.y)), f.x),
                  lerp(decode_octahedral(unpack_snorm16x2(sw.y)), decode_octahedral(unpack_snorm16x2(se.y)), f.x), f.y);
}

// Colours by height, the conventional hypsometric tints.
static const int stop_count = 13;
static const float stop_heights[stop_count] = {
    -8000.0, -4000.0, -1500.0, -200.0, -1.0, 0.0, 300.0, 700.0, 1300.0, 2000.0, 3000.0, 4200.0,
    5500.0};
static const float3 stop_colors[stop_count] = {
    float3(0.05, 0.10, 0.28), float3(0.10, 0.27, 0.52), float3(0.22, 0.47, 0.71),
    float3(0.50, 0.73, 0.87), float3(0.73, 0.87, 0.93), float3(0.36, 0.58, 0.34),
    float3(0.58, 0.71, 0.43), float3(0.85, 0.80, 0.55), float3(0.80, 0.64, 0.42),
    float3(0.64, 0.48, 0.34), float3(0.60, 0.55, 0.52), float3(0.80, 0.79, 0.79),
    float3(0.97, 0.97, 0.98)};

float3 height_color(float h)
{
    float3 c = stop_colors[0];
    [unroll] for (int i = 1; i < stop_count; ++i)
    {
        float t = saturate((h - stop_heights[i - 1]) / (stop_heights[i] - stop_heights[i - 1]));
        c = lerp(c, stop_colors[i], t);
    }
    return c;
}

// A distinct colour per level.
float3 level_color(float level)
{
    float h = frac(level * 0.137 + 0.05) * 6.0;
    float3 c = saturate(float3(abs(h - 3.0) - 1.0, 2.0 - abs(h - 2.0), 2.0 - abs(h - 4.0)));
    return lerp(float3(1.0, 1.0, 1.0), c, 0.8);
}

// The share of a pixel covered by lines a pixel wide at every multiple of `spacing` cells
// (backends/gl/shaders.hpp, grid_lines).
float grid_lines(float g, float w, float spacing)
{
    float x = g / spacing;
    float f = w / spacing;
    float width = min(f, 1.0);
    float lo = x - 0.5 * f + 0.5 * width;
    float hi = x + 0.5 * f + 0.5 * width;
    float covered = floor(hi) * width + min(frac(hi), width) - floor(lo) * width - min(frac(lo), width);
    return saturate(covered / max(f, 1e-6)) * (1.0 - smoothstep(0.5, 1.0, f));
}

// The wireframe's lines along one coordinate (backends/gl/shaders.hpp, wire_lines).
float wire_lines(float g, float w)
{
    float top = log2(u_cells);
    float level = clamp(log2(max(w * 4.0, 1e-6)), 0.0, top);
    float k = floor(level);
    float fine = grid_lines(g, w, exp2(k));
    float coarse = grid_lines(g, w, exp2(min(k + 1.0, top)));
    return lerp(fine, coarse, level - k);
}

float3 blend(float3 below, Texture2D layer, float2 uv, float4 scale_offset, float opacity)
{
    float4 texel = layer.Sample(u_sampler, uv * scale_offset.xy + scale_offset.zw);
    return lerp(below, texel.rgb, texel.a * opacity);
}

struct pixel_output
{
    float4 color : SV_Target;
#ifdef VGL_LOGARITHMIC_DEPTH
    float depth : SV_Depth;
#endif
};

pixel_output ps_main(varyings i)
{
    float height;
    float3 normal;
    surface(i.uv, height, normal);
    // Above mean sea level, from the geoid at the tile's corners.
    height -= lerp(lerp(u_geoid.x, u_geoid.y, i.uv.x), lerp(u_geoid.z, u_geoid.w, i.uv.x), i.uv.y);
    float3 n = normalize(mul((float3x3)u_model_view, normal));
    float3 l = dot(u_light, u_light) > 0.0 ? u_light : normalize(-i.position);
    float diffuse = max(dot(n, l), 0.0);
    // The height colours show where no imagery covers the terrain.
    float3 albedo = height_color(height);
    bool imagery = (u_debug & 8) == 0;
    if (u_layer_count > 0 && imagery)
    {
        albedo = blend(albedo, u_layer0, i.uv, u_layer_uv[0], u_layer_opacity.x);
    }
    if (u_layer_count > 1 && imagery)
    {
        albedo = blend(albedo, u_layer1, i.uv, u_layer_uv[1], u_layer_opacity.y);
    }
    if (u_layer_count > 2 && imagery)
    {
        albedo = blend(albedo, u_layer2, i.uv, u_layer_uv[2], u_layer_opacity.z);
    }
    if (u_layer_count > 3 && imagery)
    {
        albedo = blend(albedo, u_layer3, i.uv, u_layer_uv[3], u_layer_opacity.w);
    }
    if ((u_debug & 2) != 0)
    {
        albedo = lerp(albedo, level_color(u_level), 0.5);
        // The tile's border, about a pixel and a half wide.
        float2 edge = min(i.uv, 1.0 - i.uv) / max(fwidth(i.uv), float2(1e-6, 1e-6));
        if (min(edge.x, edge.y) < 1.5)
        {
            albedo = float3(0.05, 0.05, 0.05);
        }
    }
    if ((u_debug & 4) != 0)
    {
        if (u_layer_count == 0)
        {
            albedo = lerp(albedo, float3(1.0, 0.0, 1.0), 0.6);
        }
        else if (u_imagery_coarser > 0.0)
        {
            albedo = lerp(albedo, float3(1.0, 0.5, 0.0), min(0.25 + 0.2 * u_imagery_coarser, 0.8));
        }
    }
    if ((u_debug & 16) != 0)
    {
        // Contours, minor and every fifth major (backends/gl/shaders.hpp).
        float x = height / u_contour_interval;
        float dx = max(fwidth(x), 1e-6);
        float minor_pixels = abs(frac(x - 0.5) - 0.5) / dx;
        float minor = (1.0 - smoothstep(0.3, 1.0, minor_pixels)) * (1.0 - smoothstep(0.06, 0.16, dx));
        float y = x / 5.0;
        float dy = dx / 5.0;
        float major_pixels = abs(frac(y - 0.5) - 0.5) / dy;
        float major = (1.0 - smoothstep(0.6, 1.4, major_pixels)) * (1.0 - smoothstep(0.06, 0.16, dy));
        albedo = lerp(albedo, float3(0.22, 0.17, 0.12), minor * 0.55);
        albedo = lerp(albedo, float3(0.08, 0.05, 0.03), major * 0.85);
    }
    // The sunlight and the ambient light, reddened and dimmed by the atmosphere
    // (backends/gl/shaders.hpp).
    float3 sunlight = float3(1.0, 1.0, 1.0);
    float ambient = 1.0;
    float3 offset = to_planet(i.position);
    if (u_atmosphere != 0)
    {
        float3 p = u_camera_km + offset;
        float r = length(p);
        float mu_s = dot(p, u_sun_planet) / r;
        sunlight = vgl_sun_transmittance(r, mu_s) / vgl_zenith_transmittance;
        ambient = vgl_daylight(mu_s);
    }
    float3 lit = albedo * (0.3 * ambient + 0.7 * diffuse * sunlight);
    // Skirts take the colour of the edge above them, as without the sea.
    if (u_sea != 0 && height < 0.0 && i.skirt == 0.0)
    {
        float3 water = u_layer_count > 0 && imagery ? lerp(albedo, float3(0.04, 0.16, 0.36), 0.35)
                                                    : float3(0.06, 0.2, 0.42);
        float3 n_sea = normalize(i.up);
        float d = max(dot(n_sea, l), 0.0);
        float glint = pow(max(dot(reflect(-l, n_sea), normalize(-i.position)), 0.0), 80.0);
        lit = water * (0.35 * ambient + 0.65 * d * sunlight) + float3(0.5, 0.5, 0.5) * glint * sunlight;
    }
    if (u_atmosphere != 0)
    {
        // The haze between the camera and the terrain.
        float distance = length(offset);
        float3 inscatter;
        float3 transmittance;
        vgl_atmosphere(u_camera_km, u_camera_height, offset / max(distance, 1e-9), distance,
                       u_sun_planet, 8, inscatter, transmittance);
        lit = lit * transmittance + vgl_expose(inscatter * vgl_sky_brightness);
    }
    pixel_output o;
    o.color = float4(lit, 1.0);
    if ((u_debug & 1) != 0 && u_cells > 0.0)
    {
        // The wireframe (backends/gl/shaders.hpp): the grid's cells along each texture
        // coordinate, and the tile's boundary in yellow.
        float2 g = i.uv * u_cells;
        float2 w = float2(length(float2(ddx(g.x), ddy(g.x))), length(float2(ddx(g.y), ddy(g.y))));
        float2 along = step(1e-3 * float2(w.y, w.x), w);
        float2 on = float2(wire_lines(g.x, w.x), wire_lines(g.y, w.y)) * along;
        o.color.rgb = lerp(o.color.rgb, float3(0.3, 1.0, 0.95), max(on.x, on.y));
        float2 pixels = min(g, u_cells - g) / max(w, float2(1e-9, 1e-9));
        float2 edge = saturate(2.0 - pixels) * along;
        o.color.rgb = lerp(o.color.rgb, float3(1.0, 0.85, 0.1), max(edge.x, edge.y));
    }
#ifdef VGL_LOGARITHMIC_DEPTH
    o.depth = log2(i.log_depth) * u_log_depth * 0.5;
#endif
    return o;
}

// The sky (backends/gl/shaders.hpp): one triangle over the whole viewport at the far depth, so
// that it shows only where nothing was drawn before it.
struct sky_varyings
{
    float4 clip : SV_Position;
    float2 ndc : TEXCOORD0;
};

sky_varyings sky_vs_main(uint id : SV_VertexID)
{
    sky_varyings o;
    o.ndc = float2(float((id << 1) & 2), float(id & 2)) * 2.0 - 1.0;
    o.clip = float4(o.ndc, u_far_depth, 1.0);
    return o;
}

float4 sky_ps_main(sky_varyings i) : SV_Target
{
    // The view ray through the pixel, from a point on it in camera space, half way in depth.
    float4 q = mul(u_inverse_projection, float4(i.ndc, 0.5, 1.0));
    float3 direction = normalize(to_planet(q.xyz / q.w));
    float3 inscatter;
    float3 transmittance;
    vgl_atmosphere(u_camera_km, u_camera_height, direction, 1.0e9, u_sun_planet, 16, inscatter,
                   transmittance);
    float disc = smoothstep(0.999955, 0.999975, dot(direction, u_sun_planet));
    float3 color = vgl_expose(inscatter * vgl_sky_brightness) + transmittance * disc;
    return float4(color, (transmittance.r + transmittance.g + transmittance.b) / 3.0);
}

// The stars (backends/gl/shaders.hpp): a quad per star at the far depth, widened in pixels
// around the star's direction.
struct star_input
{
    float3 direction : TEXCOORD0; // Celestial frame.
    float2 corner : TEXCOORD1;    // -1 or 1 on each axis.
    float2 light : TEXCOORD2;     // Intensity, radius in pixels.
    float3 color : TEXCOORD3;
};

struct star_varyings
{
    float4 clip : SV_Position;
    float2 corner : TEXCOORD0;
    float3 color : TEXCOORD1;
};

star_varyings stars_vs_main(star_input v)
{
    star_varyings o;
    float3 d = float3(dot(u_celestial[0].xyz, v.direction), dot(u_celestial[1].xyz, v.direction),
                      dot(u_celestial[2].xyz, v.direction));
    float4 clip = mul(u_projection, float4(d, 0.0)); // A direction: at infinity.
    float visible = 1.0;
    if (u_atmosphere != 0)
    {
        visible = vgl_star_visibility(u_camera_km, u_camera_height, normalize(to_planet(d)),
                                      u_sun_planet);
    }
    o.color = v.color * (v.light.x * visible);
    o.corner = v.corner;
    clip.xy += v.corner * v.light.y * 2.0 / u_viewport * clip.w;
    clip.z = u_far_depth * clip.w;
    // Behind the camera: outside the clip volume, so nothing is drawn.
    o.clip = clip.w > 0.0 ? clip : float4(0.0, 0.0, 2.0, 1.0);
    return o;
}

float4 stars_ps_main(star_varyings i) : SV_Target
{
    float glow = exp(-3.0 * dot(i.corner, i.corner));
    return float4(i.color * glow, 0.0);
}
