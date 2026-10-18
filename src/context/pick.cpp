// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// vgl_context_ray_query (FR-07): picking. The ray is marched through the terrain that height
// queries see. Where it runs above the highest terrain a tile can hold, it skips ahead by its
// height above it, since a straight line's height above the ellipsoid changes by at most the
// distance along it; elsewhere it steps by about the data's sample spacing. The first step that
// ends under the terrain is bisected down to a centimetre.

#include "context/context_impl.hpp"
#include "decoders/elevation_codec.hpp"
#include "tiling/selection.hpp"
#include "tiling/tile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace vgl::internal;

namespace
{

namespace pk = vgl::internal::pack;

// Margins around the heights terrain can have, for the shells the ray is clipped to.
constexpr double shell_margin = 100.0;

// Bisection ends when the hit is known to this, in metres.
constexpr double hit_tolerance = 0.01;

// The longest step near the terrain, in metres, where its data is coarse or missing.
constexpr double max_fine_step = 1000.0;

// A bound on the work for one ray: grazing rays through fine data step the most.
constexpr int max_steps = 200000;

// The terrain at a position, from the loaded data only: no reads start.
struct probe
{
    double height = 0.0; // Above the ellipsoid; 0 where no pack has data.
    double bound = 0.0;  // No loaded height over region is higher.
    degree_region region;
    double spacing = 0.0; // Of the finest loaded samples there, in degrees.
};

// Distances along a ray where it meets the ellipsoid inflated by a height: near and far, or
// false if it misses. The inflated ellipsoid has its semi-axes grown by the height, which is
// within millimetres of the surface at that height above the ellipsoid.
bool meet_shell(const ellipsoid& e,
                const dvec3& origin,
                const dvec3& direction,
                double height,
                double& near,
                double& far) noexcept
{
    const double a = e.semi_major_axis() + height;
    const double b = e.semi_minor_axis() + height;
    const dvec3 o{origin.x / a, origin.y / a, origin.z / b};
    const dvec3 d{direction.x / a, direction.y / a, direction.z / b};
    const double qa = dot(d, d);
    const double qb = dot(o, d);
    const double qc = dot(o, o) - 1.0;
    const double disc = qb * qb - qa * qc;
    if (disc < 0.0)
    {
        return false;
    }
    const double root = std::sqrt(disc);
    near = (-qb - root) / qa;
    far = (-qb + root) / qa;
    return true;
}

// The terrain at a position in degrees, as height queries see it but without starting reads,
// with a bound on the heights around it.
probe probe_at(vgl_context& ctx, double lat, double lon) noexcept
{
    probe out;
    bool found = false;
    std::uint32_t finest = 0; // The level the bound's region lies at.
    out.spacing = tile_span(0);
    for (vgl_pack* p : ctx.packs)
    {
        if (p->state != VGL_PACK_STATE_READY ||
            p->manifest.layer().type != pk::layer_type::elevation)
        {
            continue;
        }
        const pk::layer_description& l = p->manifest.layer();
        // A regional pack's tiles start at its first level: a region no deeper than that
        // could hold one of them that does not contain the position.
        finest = std::max<std::uint32_t>(finest, l.min_level);
        if (lat < l.south || lat > l.north || (lon < l.west && lon + 360.0 > l.east) ||
            (lon > l.east && lon - 360.0 < l.west))
        {
            continue;
        }
        const std::uint32_t n = l.tile_size;
        const bool usable = !found; // The highest-priority data at the position gives the height.
        for (std::uint32_t level = l.min_level; level <= l.max_level; ++level)
        {
            const tile_id id = tile_at(lat, lon, level);
            tile_state* t = nullptr;
            vgl_result error = VGL_RESULT_SUCCESS;
            if (find_tile(ctx, *p, id, t, error, false) != availability::ready)
            {
                break;
            }
            // Every loaded tile over the position bounds the terrain around it: nodata
            // samples take their heights from coarser tiles or other packs.
            if (t->min_height <= t->max_height)
            {
                out.bound = std::max(out.bound, t->max_height);
            }
            finest = std::max(finest, level);
            out.spacing = std::min(out.spacing, tile_span(level) / (n - 1));
            bool nodata = false;
            const double h =
                tile_interpolate(id, n, lat, lon, [&](std::uint32_t i, std::uint32_t j) {
                    const std::size_t k = static_cast<std::size_t>(j) * n + i;
                    nodata = nodata || t->q.data()[k] == elevation_nodata;
                    return t->height(k);
                });
            if (usable && !nodata)
            {
                out.height = h;
                found = true;
            }
            if (nodata || level == l.max_level ||
                !t->header.has_child(tile_child_index(tile_at(lat, lon, level + 1))))
            {
                break;
            }
        }
    }
    out.region = tile_bounds(tile_at(lat, lon, std::min(finest, max_tile_level)));
    return out;
}

// The distance from a position to the nearest edge of a region around it, in metres: a lower
// bound for any path that leaves the region, at any height above the deepest terrain.
double distance_to_edge(const ellipsoid& e, const degree_region& r, double lat, double lon) noexcept
{
    double d_lon = lon - r.west;
    if (d_lon < -180.0)
    {
        d_lon += 360.0;
    }
    const double ns = std::max(std::min(lat - r.south, r.north - lat), 0.0);
    const double ew = std::max(std::min(d_lon, r.east - r.west - d_lon), 0.0);
    // The angle to the nearest meridian edge, along a great circle.
    const double cos_lat = std::cos(radians(std::clamp(lat, -90.0, 90.0)));
    const double ew_angle =
        std::asin(std::min(std::sin(radians(std::min(ew, 90.0))) * cos_lat, 1.0));
    const double angle = std::min(radians(ns), ew_angle);
    // A chord between two points at least this far from the centre.
    const double radius = e.semi_minor_axis() + lowest_surface_height - shell_margin;
    return 2.0 * radius * std::sin(0.5 * std::min(angle, pi));
}

} // namespace

extern "C" VGL_API vgl_result vgl_context_ray_query(vgl_context* context,
                                                    const vgl_dvec3* origin,
                                                    const vgl_dvec3* direction,
                                                    vgl_ray_hit* out_hit)
{
    if (context == nullptr || origin == nullptr || direction == nullptr || out_hit == nullptr ||
        out_hit->struct_size < 8)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const dvec3 o = dvec3::from_c(*origin);
    const dvec3 raw = dvec3::from_c(*direction);
    const double norm = length(raw);
    if (!std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.z) || !std::isfinite(norm) ||
        norm == 0.0)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_context& ctx = *context;
    const ellipsoid& e = ctx.shape;
    const dvec3 d = raw * (1.0 / norm);

    // The part of the ray between the shells that hold every height the terrain can have.
    double begin = 0.0;
    double end = 0.0;
    if (!meet_shell(e, o, d, highest_surface_height + shell_margin, begin, end) || end < 0.0)
    {
        return VGL_RESULT_NOT_FOUND;
    }
    begin = std::max(begin, 0.0);
    double inner_near = 0.0;
    double inner_far = 0.0;
    if (meet_shell(e, o, d, lowest_surface_height - shell_margin, inner_near, inner_far) &&
        inner_far > begin)
    {
        // Under the deepest terrain, the ray is under the terrain: it ends there at the latest.
        end = std::min(end, std::max(inner_near, begin));
    }

    const auto below = [&](double t, probe& p) {
        const geodetic g = e.to_geodetic(o + d * t);
        p = probe_at(ctx, degrees(g.latitude), degrees(g.longitude));
        return g.height <= p.height;
    };

    // March to the first position under the terrain.
    double above = begin; // The last position known to be above the terrain.
    double t = begin;
    bool hit = false;
    probe p;
    for (int step = 0; step < max_steps; ++step)
    {
        const geodetic g = e.to_geodetic(o + d * t);
        const double lat = degrees(g.latitude);
        const double lon = degrees(g.longitude);
        p = probe_at(ctx, lat, lon);
        if (g.height <= p.height)
        {
            hit = true;
            break;
        }
        if (t >= end)
        {
            break;
        }
        above = t;
        // Fine steps of half the sample spacing, or a skip over the region while the ray is
        // above everything in it.
        const double fine = std::clamp(
            0.5 * radians(p.spacing) * e.semi_minor_axis(), hit_tolerance, max_fine_step);
        double next = fine;
        const double clearance = g.height - p.bound;
        if (clearance > fine)
        {
            next = std::max(std::min(clearance, distance_to_edge(e, p.region, lat, lon)), fine);
        }
        t = std::min(t + next, end);
    }
    if (!hit)
    {
        return VGL_RESULT_NOT_FOUND;
    }

    // Bisect between the last position above and the first under.
    double under = t;
    if (under > begin)
    {
        while (under - above > hit_tolerance)
        {
            const double mid = 0.5 * (above + under);
            probe q;
            (below(mid, q) ? under : above) = mid;
        }
    }
    else
    {
        under = begin; // The ray starts under the terrain.
    }

    vgl_ray_hit h{};
    h.distance = under;
    const dvec3 at = o + d * under;
    h.position = at.to_c();
    geodetic g = e.to_geodetic(at);
    // On the terrain, at the height a height query gives there.
    const probe q = probe_at(ctx, degrees(g.latitude), degrees(g.longitude));
    g.height = q.height;
    h.geodetic = g.to_c();

    // As a height query does, start the finer data's reads, and report whether they are done.
    double height = 0.0;
    vgl_result r = vgl_context_height_query(
        context, g.latitude, g.longitude, VGL_HEIGHT_REFERENCE_ELLIPSOID, &height);
    if (r == VGL_RESULT_NOT_FOUND)
    {
        // No pack has data there: the ellipsoid was hit. Packs still loading their manifests
        // may yet have some.
        r = VGL_RESULT_SUCCESS;
        for (const vgl_pack* candidate : ctx.packs)
        {
            r = candidate->state == VGL_PACK_STATE_LOADING ? VGL_RESULT_NOT_READY : r;
        }
    }

    // The fields that fit, and the size written (docs/c-api.md, section 8.4).
    const auto size =
        static_cast<std::uint32_t>(std::min<std::size_t>(out_hit->struct_size, sizeof(h)));
    h.struct_size = size;
    std::memcpy(out_hit, &h, size);
    return r;
}
