// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The atmosphere that every backend draws (design document, section 7.6): single scattering by
// air (Rayleigh) and haze (Mie), and absorption by ozone, integrated along the view ray, with
// the sunlight's own path through the atmosphere to each point along it. It is written in the
// part of GLSL that HLSL shares once vec2, vec3, vec4 and mix are defined as float2, float3,
// float4 and lerp, and VGL_CONST as static const: the OpenGL backend includes it as it is, the
// Direct3D 11 backend's HLSL and the Ogre backend's unified shaders with those definitions.
//
// Distances are in kilometres, in a frame along the ECEF axes in which the ellipsoid is a sphere
// of its semi-major axis: the z axis is stretched by a / b. Heights in that frame differ from
// those above the ellipsoid by at most a third of a percent, which the atmosphere cannot show.

#ifndef VGL_CONST
#define VGL_CONST const
#endif

VGL_CONST float vgl_ground_radius = 6378.137;
VGL_CONST float vgl_atmosphere_height = 100.0;
VGL_CONST float vgl_top_radius = 6478.137;

// Scattering and absorption per kilometre at the ground, and the heights over which they fall
// off, as in Bruneton's and Hillaire's models of the Earth's atmosphere.
VGL_CONST vec3 vgl_rayleigh_scattering = vec3(5.802e-3, 13.558e-3, 33.1e-3);
VGL_CONST float vgl_rayleigh_height = 8.0;
VGL_CONST float vgl_mie_scattering = 3.996e-3;
VGL_CONST float vgl_mie_extinction = 4.44e-3;
VGL_CONST float vgl_mie_height = 1.2;
VGL_CONST float vgl_mie_g = 0.8;
VGL_CONST vec3 vgl_ozone_absorption = vec3(0.65e-3, 1.881e-3, 0.085e-3);
VGL_CONST float vgl_ozone_centre = 25.0; // Its layer is a tent around this height,
VGL_CONST float vgl_ozone_width = 15.0;  // falling to nothing this far above and below.

VGL_CONST float vgl_pi = 3.14159265;

// The densities of air, haze and ozone at a height, relative to their peaks.
vec3 vgl_densities(float height)
{
    float h = max(height, 0.0);
    return vec3(exp(-h / vgl_rayleigh_height),
                exp(-h / vgl_mie_height),
                max(1.0 - abs(h - vgl_ozone_centre) / vgl_ozone_width, 0.0));
}

vec3 vgl_extinction(vec3 d)
{
    return vgl_rayleigh_scattering * d.x + vec3(vgl_mie_extinction, vgl_mie_extinction, vgl_mie_extinction) * d.y +
           vgl_ozone_absorption * d.z;
}

// Where a ray from radius r, at cosine mu from the vertical, meets the sphere of radius s:
// below 0 if it does not. dr is s - r, given apart so that it keeps its precision near the
// sphere. The discriminant (s - r)(s + r) + r^2 mu^2 avoids the difference of squares of radii.
float vgl_sphere_discriminant(float r, float mu, float s, float dr)
{
    return dr * (s + r) + r * r * mu * mu;
}

// The radius at a distance t along a ray from radius r at cosine mu.
float vgl_radius_at(float r, float mu, float t)
{
    return sqrt(max(r * r + t * (t + 2.0 * r * mu), 0.0));
}

// The transmittance of the sunlight that reaches a point at radius r, with the sun at cosine
// mu_s from its vertical: none when the ground is in the way.
vec3 vgl_sun_transmittance(float r, float mu_s)
{
    float dr_ground = vgl_ground_radius - r;
    if (mu_s < 0.0 && vgl_sphere_discriminant(r, mu_s, vgl_ground_radius, dr_ground) >= 0.0)
    {
        return vec3(0.0, 0.0, 0.0);
    }
    float disc = vgl_sphere_discriminant(r, mu_s, vgl_top_radius, vgl_top_radius - r);
    float length_to_top = max(-r * mu_s + sqrt(max(disc, 0.0)), 0.0);
    float dt = length_to_top / 6.0;
    vec3 optical = vec3(0.0, 0.0, 0.0);
    for (int i = 0; i < 6; ++i)
    {
        float ri = vgl_radius_at(r, mu_s, (float(i) + 0.5) * dt);
        optical += vgl_extinction(vgl_densities(ri - vgl_ground_radius)) * dt;
    }
    return exp(-optical);
}

float vgl_rayleigh_phase(float nu)
{
    return 3.0 / (16.0 * vgl_pi) * (1.0 + nu * nu);
}

// Cornette and Shanks' phase function.
float vgl_mie_phase(float nu)
{
    float g2 = vgl_mie_g * vgl_mie_g;
    float k = 3.0 / (8.0 * vgl_pi) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + nu * nu) / pow(max(1.0 + g2 - 2.0 * vgl_mie_g * nu, 1e-4), 1.5);
}

// The light scattered towards the eye along a ray, for sunlight of irradiance 1, and the
// transmittance along it. The ray starts at origin, height above the ground, and runs along the
// unit direction for up to max_distance, or until it leaves the atmosphere or meets the ground.
// sun is the unit direction towards the sun. steps samples it.
void vgl_atmosphere(vec3 origin,
                    float height,
                    vec3 direction,
                    float max_distance,
                    vec3 sun,
                    int steps,
                    out vec3 inscatter,
                    out vec3 transmittance)
{
    inscatter = vec3(0.0, 0.0, 0.0);
    transmittance = vec3(1.0, 1.0, 1.0);
    float r = vgl_ground_radius + height;
    float mu = dot(origin, direction) / r;
    float dr_top = vgl_atmosphere_height - height;
    float disc_top = vgl_sphere_discriminant(r, mu, vgl_top_radius, dr_top);
    if (disc_top < 0.0)
    {
        return; // The ray passes the atmosphere by.
    }
    float root_top = sqrt(disc_top);
    float t_far = -r * mu + root_top;
    // From space, the ray enters the atmosphere further on.
    float t_near = dr_top < 0.0 ? -r * mu - root_top : 0.0;
    if (t_far <= 0.0)
    {
        return;
    }
    t_near = max(t_near, 0.0);
    float disc_ground = vgl_sphere_discriminant(r, mu, vgl_ground_radius, -height);
    if (mu < 0.0 && disc_ground >= 0.0)
    {
        t_far = min(t_far, max(-r * mu - sqrt(disc_ground), 0.0));
    }
    t_far = min(t_far, max_distance);
    if (t_far <= t_near)
    {
        return;
    }
    float nu = dot(direction, sun);
    float phase_r = vgl_rayleigh_phase(nu);
    float phase_m = vgl_mie_phase(nu);
    float dt = (t_far - t_near) / float(steps);
    vec3 optical = vec3(0.0, 0.0, 0.0);
    for (int i = 0; i < steps; ++i)
    {
        float t = t_near + (float(i) + 0.5) * dt;
        vec3 p = origin + direction * t;
        float ri = vgl_radius_at(r, mu, t);
        vec3 d = vgl_densities(ri - vgl_ground_radius);
        vec3 ext = vgl_extinction(d) * dt;
        vec3 to_here = exp(-(optical + 0.5 * ext));
        vec3 sunlight = vgl_sun_transmittance(ri, dot(p, sun) / ri);
        vec3 scattering = vgl_rayleigh_scattering * (d.x * phase_r) +
                          vec3(vgl_mie_scattering, vgl_mie_scattering, vgl_mie_scattering) * (d.y * phase_m);
        inscatter += to_here * sunlight * scattering * dt;
        optical += ext;
    }
    transmittance = exp(-optical);
}

// The sunlight's transmittance with the sun overhead at the ground, by which the terrain's
// lighting divides it, so that the midday sun lights the terrain as it does without the
// atmosphere: exp(-(Rayleigh 8 km + Mie 1.2 km + ozone 15 km)).
VGL_CONST vec3 vgl_zenith_transmittance = vec3(0.940, 0.868, 0.762);

// How bright the scattered light shows, for sunlight of irradiance 1.
VGL_CONST float vgl_sky_brightness = 12.0;

// How much of the terrain's ambient light remains with the sun at cosine mu_s from its vertical:
// all of it by day, fading through twilight to a little at night.
float vgl_daylight(float mu_s)
{
    return mix(0.08, 1.0, smoothstep(-0.15, 0.2, mu_s));
}

// From linear light to the display: an exponential curve that keeps dim colours as they are
// and rolls bright ones off towards white.
vec3 vgl_expose(vec3 c)
{
    return vec3(1.0, 1.0, 1.0) - exp(-c);
}

// How much of a star shows through the sky in a direction from the camera: all of it where the
// sky is dark, fading to none where it is a little brighter than a faint star, as by day. The
// sky pass then dims what shows by the transmittance.
float vgl_star_visibility(vec3 origin, float height, vec3 direction, vec3 sun)
{
    vec3 inscatter;
    vec3 transmittance;
    vgl_atmosphere(origin, height, direction, 1.0e9, sun, 8, inscatter, transmittance);
    vec3 sky = vgl_expose(inscatter * vgl_sky_brightness);
    return clamp(1.0 - 8.0 * dot(sky, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
}
