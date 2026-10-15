// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Tests of the selection geometry (src/tiling/selection.hpp): views, frustum and horizon
// culling, and screen-space error.

#include "geodesy/ellipsoid.hpp"
#include "test_views.hpp"
#include "tiling/selection.hpp"
#include "tiling/tile.hpp"
#include "vgl_test.hpp"

#include <cmath>
#include <limits>

namespace
{

using namespace vgl::internal;
using namespace vgl::test;

constexpr double earth_radius = 6378137.0;

// A small box around a point on the ellipsoid.
oriented_box
box_at(const ellipsoid& e, double lat_degrees, double lon_degrees, double size = 1000.0)
{
    const double lat = radians(lat_degrees);
    const double lon = radians(lon_degrees);
    const double half = radians(size / earth_radius) / 2.0;
    return bound_region(e, {lon - half, lat - half, lon + half, lat + half}, 0.0, 100.0);
}

// A camera over (0, 0) at an altitude, looking straight down.
vgl_view looking_down(double altitude, bool d3d = false)
{
    const dvec3 eye{earth_radius + altitude, 0.0, 0.0};
    const dmat4 v = look_at(eye, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, d3d);
    const dmat4 p = d3d ? perspective_d3d_reversed_infinite(radians(60.0), 16.0 / 9.0, 1.0)
                        : perspective_gl(radians(60.0), 16.0 / 9.0, 1.0, 1e8);
    return make_view(v, p);
}

} // namespace

TEST_CASE(views_give_the_eye_and_reject_invalid_matrices)
{
    view_state s;
    const vgl_view v = looking_down(1000.0);
    REQUIRE(make_view_state(v, s));
    CHECK_NEAR(s.eye.x, earth_radius + 1000.0, 1e-6);
    CHECK_NEAR(s.eye.y, 0.0, 1e-6);
    CHECK_NEAR(s.eye.z, 0.0, 1e-6);
    CHECK(!s.orthographic);
    // 1080 pixels over 60 degrees.
    CHECK_NEAR(s.pixel_scale, 1080.0 / 2.0 / std::tan(radians(30.0)), 1e-9);
    CHECK_EQ(s.max_error, default_max_screen_space_error);

    vgl_view bad = v;
    bad.viewport_height = 0;
    CHECK(!make_view_state(bad, s));
    bad = v;
    bad.view_matrix.m[3] = 1.0; // Not affine.
    CHECK(!make_view_state(bad, s));
    bad = v;
    for (int i = 0; i < 3; ++i)
    {
        bad.view_matrix.m[i] = 0.0; // Singular.
    }
    CHECK(!make_view_state(bad, s));
    bad = v;
    bad.projection_matrix.m[5] = std::numeric_limits<double>::quiet_NaN();
    CHECK(!make_view_state(bad, s));
    bad = v;
    bad.max_screen_space_error = -1.0;
    CHECK(!make_view_state(bad, s));

    // Direct3D conventions give the same eye.
    REQUIRE(make_view_state(looking_down(1000.0, true), s));
    CHECK_NEAR(s.eye.x, earth_radius + 1000.0, 1e-6);
}

TEST_CASE(frustum_culling_works_for_both_conventions)
{
    const ellipsoid e;
    for (const bool d3d : {false, true})
    {
        view_state s;
        REQUIRE(make_view_state(looking_down(10000.0, d3d), s));
        CHECK(box_in_frustum(s, box_at(e, 0.0, 0.0)));
        // Off to the side: 10 km up, a 60 degree view reaches about 6 km sideways.
        CHECK(!box_in_frustum(s, box_at(e, 0.0, 1.0)));
        CHECK(!box_in_frustum(s, box_at(e, 1.0, 0.0)));
        // Behind the camera.
        oriented_box behind = box_at(e, 0.0, 0.0);
        behind.center = behind.center * 1.01;
        CHECK(!box_in_frustum(s, behind));
        // Far beyond the near and far planes of the OpenGL projection, still in view.
        oriented_box deep = box_at(e, 0.0, 0.0);
        deep.center = deep.center * -10.0;
        CHECK(box_in_frustum(s, deep));
    }
}

TEST_CASE(horizon_culling)
{
    const ellipsoid e;
    // From 1000 km up over (0, 0), the horizon lies about 31 degrees away.
    const dvec3 eye{earth_radius + 1e6, 0.0, 0.0};
    const horizon_occluder o = make_horizon_occluder(e, lowest_surface_height, eye);
    REQUIRE(o.active);
    CHECK(!box_below_horizon(o, box_at(e, 0.0, 0.0)));
    CHECK(!box_below_horizon(o, box_at(e, 0.0, 25.0)));
    CHECK(box_below_horizon(o, box_at(e, 0.0, 45.0)));
    CHECK(box_below_horizon(o, box_at(e, 0.0, 180.0)));
    CHECK(box_below_horizon(o, box_at(e, -60.0, 0.0)));

    // A box that reaches over the horizon is kept, though its corners near the ground are not.
    CHECK(!box_below_horizon(
        o, bound_region(e, {radians(20.0), 0.0, radians(60.0), radians(1.0)}, 0.0, 100.0)));

    // Terrain behind the horizon shows if it is tall enough. At 34.5 degrees, the ground is
    // hidden (beyond about 33.8 degrees), but a 9 km peak is not (up to about 34.9 degrees).
    CHECK(box_below_horizon(o, box_at(e, 0.0, 34.5)));
    const double half = radians(0.01);
    const geographic_region peak{radians(34.5) - half, -half, radians(34.5) + half, half};
    CHECK(!box_below_horizon(o, bound_region(e, peak, 0.0, 9000.0)));

    // For a large region beyond the horizon, the box test fails, since the box's corners stand
    // far above the surface at the region's edges; the horizon point does not. (For regions of
    // 90 degrees and more, the point itself stands too high to help.)
    const geographic_region far_side{radians(50.0), radians(-30.0), radians(110.0), radians(30.0)};
    const oriented_box far_box = bound_region(e, far_side, 0.0, 9000.0);
    CHECK(!box_below_horizon(o, far_box));
    dvec3 point;
    REQUIRE(horizon_point(e, lowest_surface_height, far_side, 9000.0, point));
    CHECK(scaled_point_below_horizon(o, point));
    // A large region that reaches into view keeps its point visible.
    const geographic_region near_side{radians(20.0), radians(-30.0), radians(80.0), radians(30.0)};
    REQUIRE(horizon_point(e, lowest_surface_height, near_side, 9000.0, point));
    CHECK(!scaled_point_below_horizon(o, point));
    // A hemisphere has no horizon point.
    CHECK(!horizon_point(e, lowest_surface_height, {0.0, -pi / 2.0, pi, pi / 2.0}, 0.0, point));

    // Inside the lowered ellipsoid nothing is hidden.
    CHECK(!make_horizon_occluder(e, lowest_surface_height, dvec3{1000.0, 0.0, 0.0}).active);
}

TEST_CASE(screen_space_error)
{
    const ellipsoid e;
    // Level 0, 129 samples over 180 degrees: 156 km.
    CHECK_NEAR(tile_geometric_error(e, 0, 129), earth_radius * pi / 128.0, 1e-6);
    CHECK_NEAR(tile_geometric_error(e, 12, 129), earth_radius * pi / 128.0 / 4096.0, 1e-9);

    view_state s;
    REQUIRE(make_view_state(looking_down(1000.0), s));
    CHECK_NEAR(screen_space_error(s, 10.0, 1000.0), 10.0 * s.pixel_scale / 1000.0, 1e-12);
    CHECK(std::isinf(screen_space_error(s, 10.0, 0.0)));

    const oriented_box b = box_at(e, 0.0, 0.0);
    CHECK_EQ(distance_to_box(b.center, b), 0.0);
    CHECK_NEAR(distance_to_box(b.center + b.axes[2] * (b.half_extents.z + 50.0), b), 50.0, 1e-6);

    // Orthographic: the error does not depend on distance.
    dmat4 ortho;
    ortho(0, 0) = 1e-3;
    ortho(1, 1) = 1e-3;
    ortho(2, 2) = -1e-7;
    vgl_view v = looking_down(1000.0);
    v.projection_matrix = ortho.to_c();
    REQUIRE(make_view_state(v, s));
    CHECK(s.orthographic);
    CHECK_NEAR(screen_space_error(s, 10.0, 1.0), screen_space_error(s, 10.0, 1e6), 1e-12);
    CHECK_NEAR(screen_space_error(s, 10.0, 1.0), 10.0 * 1e-3 * 540.0, 1e-9);
}
