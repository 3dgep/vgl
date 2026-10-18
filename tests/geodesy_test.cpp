// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Geodesy tests against GeographicLib reference values (design document, section 12.2), plus
// round trips, edge cases, the C API and bounding volumes.

#include "geodesy/bounds.hpp"
#include "geodesy/ellipsoid.hpp"
#include "vgl_test.hpp"

#include <vgl/geodesy.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <random>

namespace
{

#include "data/geographiclib_reference.inc"

using namespace vgl::internal;

// Positions are compared to 10 nm. Angles are compared in radians to 1.5e-15, about 10 nm on
// the Earth's surface.
constexpr double position_tolerance = 1e-8;
constexpr double angle_tolerance = 1.5e-15;

// The reference values are printed with 10 decimals, so far-away points get a tolerance
// relative to their distance from the centre.
double tolerance_at(double distance)
{
    return std::fmax(position_tolerance, distance * 2e-16);
}

const ellipsoid wgs84;

} // namespace

TEST_CASE(wgs84_parameters)
{
    CHECK_EQ(wgs84.semi_major_axis(), 6378137.0);
    CHECK_NEAR(wgs84.semi_minor_axis(), 6356752.314245179, 1e-9);
    CHECK_NEAR(wgs84.eccentricity_squared(), 0.00669437999014132, 1e-17);
    CHECK(ellipsoid::is_valid(6378137.0, 0.0));
    CHECK(!ellipsoid::is_valid(0.0, 0.0));
    CHECK(!ellipsoid::is_valid(1.0, -0.1));
    CHECK(!ellipsoid::is_valid(1.0, 0.5));
    CHECK(!ellipsoid::is_valid(INFINITY, 0.0));
}

TEST_CASE(geodetic_to_ecef_matches_geographiclib)
{
    double max_deviation = 0.0;
    for (const auto& row : geocentric_forward)
    {
        const dvec3 p = wgs84.to_ecef({radians(row[0]), radians(row[1]), row[2]});
        const double tolerance = tolerance_at(std::hypot(row[3], row[4], row[5]));
        CHECK_NEAR(p.x, row[3], tolerance);
        CHECK_NEAR(p.y, row[4], tolerance);
        CHECK_NEAR(p.z, row[5], tolerance);
        if (row[2] < 1e6)
        {
            max_deviation = std::max(max_deviation, length(p - dvec3{row[3], row[4], row[5]}));
        }
    }
    std::printf("geodetic to ECEF: largest deviation below 1000 km height %.3g m\n", max_deviation);
}

TEST_CASE(ecef_to_geodetic_matches_geographiclib)
{
    double max_height_deviation = 0.0;
    double max_latitude_deviation = 0.0;
    for (const auto& row : geocentric_forward)
    {
        const geodetic g = wgs84.to_geodetic({row[3], row[4], row[5]});
        const double distance = std::hypot(row[3], row[4], row[5]);
        if (row[2] < 1e6)
        {
            max_height_deviation = std::max(max_height_deviation, std::fabs(g.height - row[2]));
            max_latitude_deviation =
                std::max(max_latitude_deviation, std::fabs(g.latitude - radians(row[0])));
        }
        CHECK_NEAR(
            g.latitude, radians(row[0]), angle_tolerance + tolerance_at(distance) / distance);
        CHECK_NEAR(g.height, row[2], tolerance_at(distance));
        // Longitude is undefined at the poles. Near them, an error in x and y (from rounding
        // the reference values) becomes an angle divided by the distance from the axis.
        if (std::fabs(row[0]) != 90.0)
        {
            const double axis_distance = std::hypot(row[3], row[4]);
            const double expected = radians(row[1] == -180.0 ? 180.0 : row[1]);
            const double actual = g.longitude == -pi ? pi : g.longitude;
            CHECK_NEAR(actual, expected, angle_tolerance + tolerance_at(distance) / axis_distance);
        }
    }
    std::printf("ECEF to geodetic: largest deviation below 1000 km height %.3g m in height, %.3g "
                "rad in latitude\n",
                max_height_deviation,
                max_latitude_deviation);
}

TEST_CASE(ecef_to_geodetic_near_centre_matches_geographiclib)
{
    // Points inside the evolute, on the singular disc and on the polar axis exercise every
    // branch of the closed-form inverse.
    for (const auto& row : geocentric_reverse)
    {
        const geodetic g = wgs84.to_geodetic({row[0], row[1], row[2]});
        const double tolerance = tolerance_at(std::hypot(row[0], row[1], row[2]));
        CHECK_NEAR(g.latitude, radians(row[3]), 1e-14);
        CHECK_NEAR(g.longitude, radians(row[4]), 1e-14);
        CHECK_NEAR(g.height, row[5], tolerance);
    }
}

TEST_CASE(enu_matches_geographiclib)
{
    for (const auto& row : local_enu)
    {
        const geodetic origin{radians(row[0]), radians(row[1]), row[2]};
        const dmat4 to_enu = rigid_inverse(wgs84.enu_to_ecef(origin));
        const dvec3 local =
            transform_point(to_enu, wgs84.to_ecef({radians(row[3]), radians(row[4]), row[5]}));
        const double tolerance = tolerance_at(std::hypot(row[6], row[7], row[8]) + 6.4e6);
        CHECK_NEAR(local.x, row[6], tolerance);
        CHECK_NEAR(local.y, row[7], tolerance);
        CHECK_NEAR(local.z, row[8], tolerance);
    }
}

TEST_CASE(enu_frame_is_orthonormal)
{
    const dmat4 m = wgs84.enu_to_ecef({radians(52.1), radians(5.1), 10.0});
    const dvec3 east{m(0, 0), m(1, 0), m(2, 0)};
    const dvec3 north{m(0, 1), m(1, 1), m(2, 1)};
    const dvec3 up{m(0, 2), m(1, 2), m(2, 2)};
    CHECK_NEAR(length(east), 1.0, 1e-15);
    CHECK_NEAR(length(north), 1.0, 1e-15);
    CHECK_NEAR(length(up), 1.0, 1e-15);
    CHECK_NEAR(dot(east, north), 0.0, 1e-15);
    CHECK_NEAR(dot(east, up), 0.0, 1e-15);
    CHECK_NEAR(dot(north, up), 0.0, 1e-15);
    // Right-handed: east x north = up.
    const dvec3 c = cross(east, north);
    CHECK_NEAR(c.x, up.x, 1e-15);
    CHECK_NEAR(c.y, up.y, 1e-15);
    CHECK_NEAR(c.z, up.z, 1e-15);

    const dmat4 identity = rigid_inverse(m) * m;
    for (int r = 0; r < 4; ++r)
    {
        for (int col = 0; col < 4; ++col)
        {
            CHECK_NEAR(identity(r, col), r == col ? 1.0 : 0.0, r < 3 && col == 3 ? 1e-9 : 1e-15);
        }
    }
}

TEST_CASE(geodetic_round_trip)
{
    std::mt19937_64 rng(1);
    std::uniform_real_distribution<double> lat(-pi / 2.0, pi / 2.0);
    std::uniform_real_distribution<double> lon(-pi, pi);
    std::uniform_real_distribution<double> h(-11000.0, 1e7);
    for (int i = 0; i < 100000; ++i)
    {
        const geodetic g{lat(rng), lon(rng), h(rng)};
        const geodetic back = wgs84.to_geodetic(wgs84.to_ecef(g));
        CHECK_NEAR(back.latitude, g.latitude, 1e-15);
        CHECK_NEAR(back.height, g.height, tolerance_at(1.7e7));
        if (std::fabs(g.latitude) < pi / 2.0 - 1e-9)
        {
            CHECK_NEAR(back.longitude, g.longitude, 1e-15);
        }
    }
}

TEST_CASE(ecef_round_trip_inside_and_outside)
{
    // Every ECEF point, including those deep inside the ellipsoid, must convert back to
    // itself. Points inside the evolute have several surface normals, and the inverse picks
    // the nearest surface point.
    std::mt19937_64 rng(2);
    std::uniform_real_distribution<double> c(-8e6, 8e6);
    std::uniform_real_distribution<double> small(-6e4, 6e4);
    for (int i = 0; i < 100000; ++i)
    {
        const dvec3 p =
            i % 2 == 0 ? dvec3{c(rng), c(rng), c(rng)} : dvec3{small(rng), small(rng), small(rng)};
        const dvec3 back = wgs84.to_ecef(wgs84.to_geodetic(p));
        CHECK_NEAR(back.x, p.x, 1e-8);
        CHECK_NEAR(back.y, p.y, 1e-8);
        CHECK_NEAR(back.z, p.z, 1e-8);
    }
}

TEST_CASE(sphere_conversions)
{
    const ellipsoid sphere(1000.0, 0.0);
    const dvec3 p = sphere.to_ecef({radians(30.0), radians(60.0), 10.0});
    CHECK_NEAR(length(p), 1010.0, 1e-12);
    const geodetic g = sphere.to_geodetic(p);
    CHECK_NEAR(g.latitude, radians(30.0), 1e-15);
    CHECK_NEAR(g.longitude, radians(60.0), 1e-15);
    CHECK_NEAR(g.height, 10.0, 1e-12);

    const geodetic centre = sphere.to_geodetic({0.0, 0.0, 0.0});
    CHECK_EQ(centre.latitude, pi / 2.0);
    CHECK_EQ(centre.height, -1000.0);
}

TEST_CASE(c_api_conversions)
{
    vgl_ellipsoid e{};
    REQUIRE(vgl_ellipsoid_wgs84_get(&e) == VGL_RESULT_SUCCESS);
    CHECK_EQ(e.semi_major_axis, VGL_WGS84_SEMI_MAJOR_AXIS);
    CHECK_EQ(e.flattening, VGL_WGS84_FLATTENING);

    const vgl_geodetic g{VGL_DEGREES_TO_RADIANS(52.1), VGL_DEGREES_TO_RADIANS(5.1), 10.0};
    vgl_dvec3 p{};
    REQUIRE(vgl_ellipsoid_geodetic_to_ecef(&e, &g, &p) == VGL_RESULT_SUCCESS);
    CHECK_NEAR(p.x, 3910649.0343778431, 1e-8);
    CHECK_NEAR(p.y, 349016.1282246748, 1e-8);
    CHECK_NEAR(p.z, 5009653.9420088734, 1e-8);

    vgl_geodetic back{};
    REQUIRE(vgl_ellipsoid_ecef_to_geodetic(&e, &p, &back) == VGL_RESULT_SUCCESS);
    CHECK_NEAR(back.latitude, g.latitude, 1e-15);
    CHECK_NEAR(back.longitude, g.longitude, 1e-15);
    CHECK_NEAR(back.height, g.height, 1e-8);

    vgl_dmat4 to_ecef{};
    vgl_dmat4 to_enu{};
    REQUIRE(vgl_ellipsoid_enu_frame_get(&e, &g, &to_ecef, &to_enu) == VGL_RESULT_SUCCESS);
    CHECK_NEAR(to_ecef.m[12], p.x, 1e-8);
    CHECK_NEAR(to_ecef.m[13], p.y, 1e-8);
    CHECK_NEAR(to_ecef.m[14], p.z, 1e-8);
    REQUIRE(vgl_ellipsoid_enu_frame_get(&e, &g, nullptr, &to_enu) == VGL_RESULT_SUCCESS);
    REQUIRE(vgl_ellipsoid_enu_frame_get(&e, &g, &to_ecef, nullptr) == VGL_RESULT_SUCCESS);

    // The C++ wrapper forwards to the same functions.
    const auto wrapped = vgl::ellipsoid_geodetic_to_ecef(vgl::ellipsoid_wgs84_get(), g);
    REQUIRE(wrapped.has_value());
    CHECK_EQ(wrapped->x, p.x);
}

TEST_CASE(c_api_poles_accept_converted_degrees)
{
    vgl_ellipsoid e{};
    REQUIRE(vgl_ellipsoid_wgs84_get(&e) == VGL_RESULT_SUCCESS);
    vgl_dvec3 p{};
    const vgl_geodetic north{VGL_DEGREES_TO_RADIANS(90.0), 0.0, 0.0};
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(&e, &north, &p), VGL_RESULT_SUCCESS);
    CHECK_NEAR(p.z, 6356752.3142451793, 1e-8);
    const vgl_geodetic beyond{std::nextafter(VGL_PI / 2.0, 4.0), 0.0, 0.0};
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(&e, &beyond, &p), VGL_RESULT_SUCCESS);
    const vgl_geodetic too_far{VGL_PI / 2.0 + 1e-9, 0.0, 0.0};
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(&e, &too_far, &p), VGL_RESULT_INVALID_ARGUMENT);
}

TEST_CASE(c_api_rejects_invalid_arguments)
{
    vgl_ellipsoid e{};
    REQUIRE(vgl_ellipsoid_wgs84_get(&e) == VGL_RESULT_SUCCESS);
    const vgl_geodetic g{0.1, 0.2, 0.3};
    const vgl_dvec3 p{1e6, 2e6, 3e6};
    vgl_dvec3 out_p{};
    vgl_geodetic out_g{};
    vgl_dmat4 out_m{};

    CHECK_EQ(vgl_ellipsoid_wgs84_get(nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(nullptr, &g, &out_p), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(&e, nullptr, &out_p), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(&e, &g, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_ecef_to_geodetic(&e, nullptr, &out_g), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_ecef_to_geodetic(&e, &p, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_enu_frame_get(&e, &g, nullptr, nullptr), VGL_RESULT_INVALID_ARGUMENT);

    const vgl_ellipsoid bad_axis{-1.0, 0.0};
    const vgl_ellipsoid bad_flattening{1.0, 0.7};
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(&bad_axis, &g, &out_p), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(&bad_flattening, &g, &out_p),
             VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_enu_frame_get(&bad_axis, &g, &out_m, nullptr),
             VGL_RESULT_INVALID_ARGUMENT);

    const vgl_geodetic nan_height{0.1, 0.2, NAN};
    const vgl_dvec3 inf_point{INFINITY, 0.0, 0.0};
    CHECK_EQ(vgl_ellipsoid_geodetic_to_ecef(&e, &nan_height, &out_p), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_ellipsoid_ecef_to_geodetic(&e, &inf_point, &out_g), VGL_RESULT_INVALID_ARGUMENT);

    const auto wrapped = vgl::ellipsoid_geodetic_to_ecef(bad_axis, g);
    CHECK(!wrapped.has_value());
    CHECK(wrapped.error() == vgl::result::invalid_argument);
}

namespace
{

// Checks that every point of a region, sampled more densely than bound_region samples it
// and including random interior points, lies inside the box and the sphere.
void check_region_is_bounded(const ellipsoid& e,
                             const geographic_region& r,
                             double min_h,
                             double max_h)
{
    const oriented_box box = bound_region(e, r, min_h, max_h);
    const bounding_sphere sphere = bound_box(box);
    // Containment is tested with a tolerance for rounding in the test itself.
    const double tolerance = 1e-7 * (e.semi_major_axis() + std::fabs(max_h));
    constexpr int n = 64;
    bool all_inside = true;
    for (int i = 0; i <= n; ++i)
    {
        for (int j = 0; j <= n; ++j)
        {
            for (const double h : {min_h, (min_h + max_h) / 2.0, max_h})
            {
                const double lat = r.south + (r.north - r.south) * i / n;
                const double lon = r.west + (r.east - r.west) * j / n;
                const dvec3 p = e.to_ecef({lat, lon, h});
                all_inside =
                    all_inside && box.contains(p, tolerance) && sphere.contains(p, tolerance);
            }
        }
    }
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (int i = 0; i < 2000; ++i)
    {
        const double lat = r.south + (r.north - r.south) * u(rng);
        const double lon = r.west + (r.east - r.west) * u(rng);
        const double h = min_h + (max_h - min_h) * u(rng);
        const dvec3 p = e.to_ecef({lat, lon, h});
        all_inside = all_inside && box.contains(p, tolerance) && sphere.contains(p, tolerance);
    }
    CHECK(all_inside);
}

geographic_region degrees_region(double west, double south, double east, double north)
{
    return {radians(west), radians(south), radians(east), radians(north)};
}

} // namespace

TEST_CASE(bounding_volumes_contain_regions)
{
    // Hemispheres (level 0 tiles), polar caps, the equator, the antimeridian and small tiles.
    check_region_is_bounded(wgs84, degrees_region(-180, -90, 0, 90), -11000, 9000);
    check_region_is_bounded(wgs84, degrees_region(0, -90, 180, 90), -11000, 9000);
    check_region_is_bounded(wgs84, degrees_region(-180, 45, -90, 90), 0, 5000);
    check_region_is_bounded(wgs84, degrees_region(90, -90, 135, -45), -1000, 4000);
    check_region_is_bounded(wgs84, degrees_region(-11.25, -11.25, 0, 0), -6000, 100);
    check_region_is_bounded(wgs84, degrees_region(174.375, -45, 180, -39.375), -50, 3700);
    check_region_is_bounded(wgs84, degrees_region(5.0, 52.0, 5.0439453125, 52.0439453125), -5, 300);
    check_region_is_bounded(wgs84, degrees_region(0, 89.95, 0.0439453125, 90), 2800, 2900);
    check_region_is_bounded(wgs84, degrees_region(10, 60, 10.01, 60.01), 1000, 1000);

    // Other bodies, including a strongly flattened one and a sphere.
    check_region_is_bounded(
        ellipsoid(3396190.0, 1.0 / 169.894), degrees_region(-90, -45, 0, 45), -8000, 21000);
    check_region_is_bounded(ellipsoid(1737400.0, 0.0), degrees_region(0, 0, 90, 90), -9000, 10000);
    check_region_is_bounded(ellipsoid(1000.0, 0.3), degrees_region(-180, -90, 0, 90), -10, 10);
}

TEST_CASE(bounding_box_of_small_tile_is_tight)
{
    // A level-12 tile: the sampling margin must stay small compared with the tile size.
    const geographic_region r = degrees_region(5.0, 52.0, 5.0439453125, 52.0439453125);
    const oriented_box box = bound_region(wgs84, r, 0.0, 100.0);
    // About 3 km east-west and 4.9 km north-south at this latitude, and 100 m of height plus
    // the curvature of the surface over the tile (about 0.5 m).
    CHECK_NEAR(box.half_extents.x * 2.0, 3010.0, 20.0);
    CHECK_NEAR(box.half_extents.y * 2.0, 4892.0, 10.0);
    CHECK(box.half_extents.z * 2.0 < 102.0);
    CHECK(box.half_extents.z * 2.0 > 100.0);
}

TEST_CASE(earth_rotation_angle_follows_the_iers_definition)
{
    // At J2000.0, 2 pi times 0.7790572732640; a sidereal day later, the same; half a day later,
    // half a turn on.
    double a = 0.0;
    REQUIRE(vgl_earth_rotation_angle_get(2451545.0, &a) == VGL_RESULT_SUCCESS);
    CHECK_NEAR(a, 2.0 * pi * 0.7790572732640, 1e-12);
    const double sidereal_day = 1.0 / 1.00273781191135448;
    double b = 0.0;
    REQUIRE(vgl_earth_rotation_angle_get(2451545.0 + sidereal_day, &b) == VGL_RESULT_SUCCESS);
    CHECK_NEAR(b, a, 1e-8); // A Julian date as a double is exact to about 5e-10 days.
    REQUIRE(vgl_earth_rotation_angle_get(2451545.0 + 0.5 * sidereal_day, &b) == VGL_RESULT_SUCCESS);
    CHECK_NEAR(b, std::fmod(a + pi, 2.0 * pi), 1e-8);
    // Decades on, the angle stays in [0, 2 pi) and keeps its precision.
    REQUIRE(vgl_earth_rotation_angle_get(2461041.5 + 0.25, &b) == VGL_RESULT_SUCCESS);
    CHECK(b >= 0.0 && b < 2.0 * pi);
    CHECK_EQ(vgl_earth_rotation_angle_get(2451545.0, nullptr), VGL_RESULT_INVALID_ARGUMENT);
    CHECK_EQ(vgl_earth_rotation_angle_get(std::numeric_limits<double>::infinity(), &b),
             VGL_RESULT_INVALID_ARGUMENT);
}
