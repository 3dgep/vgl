/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jeremiah van Oosten
 *
 * vgl geodesy: ellipsoids and conversions between geodetic, Earth-centred Earth-fixed (ECEF)
 * and local East-North-Up (ENU) coordinates (design document, section 7.2; FR-09).
 *
 * Angles are in radians and lengths in metres (docs/c-api.md, section 4.3). ECEF is
 * right-handed, with +Z through the north pole and +X through latitude 0, longitude 0.
 */
#ifndef VGL_GEODESY_H
#define VGL_GEODESY_H

#include <vgl/base.h>

#define VGL_PI 3.14159265358979323846

#define VGL_DEGREES_TO_RADIANS(degrees) ((degrees) * (VGL_PI / 180.0))
#define VGL_RADIANS_TO_DEGREES(radians) ((radians) * (180.0 / VGL_PI))

/* WGS84 defining parameters. */
#define VGL_WGS84_SEMI_MAJOR_AXIS 6378137.0
#define VGL_WGS84_FLATTENING      (1.0 / 298.257223563)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reference ellipsoid of revolution. Valid ellipsoids have a finite semi_major_axis greater
 * than zero and a flattening in [0, 0.5). A flattening of 0 is a sphere.
 */
typedef struct vgl_ellipsoid
{
    double semi_major_axis; /* Equatorial radius in metres. */
    double flattening;      /* (a - b) / a. */
} vgl_ellipsoid;

/* Geodetic position. */
typedef struct vgl_geodetic
{
    double latitude;  /* Geodetic latitude in radians, in [-pi/2, pi/2]. */
    double longitude; /* Longitude in radians. Any finite value; outputs are in [-pi, pi]. */
    double height;    /* Height above the ellipsoid in metres. */
} vgl_geodetic;

/**
 * Gets the WGS84 ellipsoid.
 *
 * @param out_ellipsoid  Receives the ellipsoid.
 *
 * @retval VGL_RESULT_SUCCESS           The ellipsoid was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  out_ellipsoid is null.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_ellipsoid_wgs84_get(vgl_ellipsoid* out_ellipsoid);

/**
 * Converts a geodetic position to ECEF coordinates.
 *
 * A latitude up to 1e-12 radians beyond a pole is accepted and treated as the pole, so
 * that converted angles such as VGL_DEGREES_TO_RADIANS(90.0) are always valid.
 *
 * @param ellipsoid  The reference ellipsoid.
 * @param geodetic   The position to convert.
 * @param out_ecef   Receives the ECEF position in metres.
 *
 * @retval VGL_RESULT_SUCCESS           The position was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null, the ellipsoid is invalid, a value is
 *                                      not finite, or the latitude is out of range.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_ellipsoid_geodetic_to_ecef(const vgl_ellipsoid* ellipsoid,
                                                  const vgl_geodetic* geodetic,
                                                  vgl_dvec3* out_ecef);

/**
 * Converts ECEF coordinates to a geodetic position.
 *
 * The conversion is closed-form and accurate to a few nanometres for any position, including
 * positions near the centre of the ellipsoid. On the polar axis the longitude is 0, and at
 * the centre the result is the north pole at a height of minus the semi-minor axis.
 *
 * @param ellipsoid     The reference ellipsoid.
 * @param ecef          The ECEF position in metres.
 * @param out_geodetic  Receives the geodetic position.
 *
 * @retval VGL_RESULT_SUCCESS           The position was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  A pointer is null, the ellipsoid is invalid, or a
 *                                      coordinate is not finite.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_ellipsoid_ecef_to_geodetic(const vgl_ellipsoid* ellipsoid,
                                                  const vgl_dvec3* ecef,
                                                  vgl_geodetic* out_geodetic);

/**
 * Gets the local East-North-Up frame at a geodetic position.
 *
 * In ENU coordinates, +X points east, +Y north and +Z up along the ellipsoid normal, with the
 * origin at the given position. The matrices are rigid transforms in the layout of section
 * 4.2 of docs/c-api.md: columns 0 to 2 of out_enu_to_ecef are the east, north and up axes in
 * ECEF, and column 3 is the origin.
 *
 * @param ellipsoid           The reference ellipsoid.
 * @param origin              Origin of the frame.
 * @param out_enu_to_ecef     Optional. Receives the transform from ENU to ECEF coordinates.
 * @param out_ecef_to_enu     Optional. Receives the transform from ECEF to ENU coordinates.
 *
 * @retval VGL_RESULT_SUCCESS           The matrices were written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  ellipsoid or origin is null, both outputs are null, the
 *                                      ellipsoid is invalid, or origin is out of range.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_ellipsoid_enu_frame_get(const vgl_ellipsoid* ellipsoid,
                                               const vgl_geodetic* origin,
                                               vgl_dmat4* out_enu_to_ecef,
                                               vgl_dmat4* out_ecef_to_enu);

/**
 * Gets the Earth rotation angle at a time: the angle by which the Earth has turned from the
 * celestial frame (the equator and equinox of J2000), so that a direction d in that frame is
 * (d.x cos(a) + d.y sin(a), -d.x sin(a) + d.y cos(a), d.z) in ECEF. It is the IAU 2000
 * definition, which leaves out precession and nutation, a fraction of a degree over decades:
 * enough to place stars behind the globe.
 *
 * @param julian_date  The time as a Julian date in UT1, which UTC is within a second of:
 *                     2451545.0 is 1 January 2000 at 12:00.
 * @param out_angle    Receives the angle in radians, in [0, 2 pi).
 *
 * @retval VGL_RESULT_SUCCESS           The angle was written.
 * @retval VGL_RESULT_INVALID_ARGUMENT  out_angle is null, or julian_date is not finite.
 *
 * @par Thread safety
 * Free-threaded.
 */
VGL_API vgl_result vgl_earth_rotation_angle_get(double julian_date, double* out_angle);

#ifdef __cplusplus
}
#endif

#endif /* VGL_GEODESY_H */
