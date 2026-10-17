// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "navigation.hpp"

#include <algorithm>
#include <cmath>

namespace sample
{

namespace
{

constexpr double semi_major = 6378137.0;
constexpr double semi_minor = 6356752.314245;
constexpr double min_clearance = 20.0; // Metres above the terrain.
constexpr double max_altitude = 5.0e7; // Metres above the ellipsoid.
constexpr double max_tilt = 85.0;      // Degrees from looking straight down at the pivot.
constexpr double throw_window = 0.1;   // Seconds of a pan that set the speed of its throw.
constexpr double throw_decay = 3.0;    // How fast a throw slows down, per second.
constexpr double focus_fade = 0.5;     // Seconds over which the focus fades after a movement.
// Keys move the camera over the ground at up to four view heights per second. The speed eases in
// and out, as a critically damped spring: starting, it reaches 83% in a second and 99% in two;
// stopping or reversing, it falls below 5% in 0.15 seconds.
constexpr double walk_top_speed = 4.0;
constexpr double walk_start = 3.2; // Per second.
constexpr double walk_stop = 36.0;

vgl_dvec3 add(const vgl_dvec3& a, const vgl_dvec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

vgl_dvec3 scale(const vgl_dvec3& a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}

double length(const vgl_dvec3& a)
{
    return std::sqrt(dot(a, a));
}

// Rotates v about a unit axis by an angle in radians (Rodrigues' formula).
vgl_dvec3 rotate(const vgl_dvec3& v, const vgl_dvec3& axis, double angle)
{
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const vgl_dvec3 k_cross_v = cross(axis, v);
    const double k_dot_v = dot(axis, v);
    return {v.x * c + k_cross_v.x * s + axis.x * k_dot_v * (1.0 - c),
            v.y * c + k_cross_v.y * s + axis.y * k_dot_v * (1.0 - c),
            v.z * c + k_cross_v.z * s + axis.z * k_dot_v * (1.0 - c)};
}

// A camera as a position, a direction and the image's up, which rotations act on.
struct pose
{
    vgl_dvec3 eye;
    vgl_dvec3 forward;
    vgl_dvec3 up;
};

pose pose_of(const camera& c)
{
    const vgl_dmat4 v = camera_view_matrix(c);
    return {camera_eye(c), {-v.m[2], -v.m[6], -v.m[10]}, {v.m[1], v.m[5], v.m[9]}};
}

pose rotated(const pose& p, const vgl_dvec3& axis, double angle)
{
    return {rotate(p.eye, axis, angle), rotate(p.forward, axis, angle), rotate(p.up, axis, angle)};
}

// One direction of a key movement over dt: the speed approaches the target as a critically
// damped spring, solved exactly, so that it goes the same however the time is split into frames.
// Returns the distance covered, in the speed's units times seconds.
double approach(double& speed, double& rate, double target, double dt)
{
    const bool stopping = target == 0.0 || target * speed < 0.0;
    const double omega = stopping ? walk_stop : walk_start;
    const double e0 = speed - target;
    const double c = rate + omega * e0;
    const double decay = std::exp(-omega * dt);
    const double covered = target * dt + e0 * (1.0 - decay) / omega +
                           c * (1.0 - decay * (1.0 + omega * dt)) / (omega * omega);
    speed = target + (e0 + c * dt) * decay;
    rate = (rate - omega * c * dt) * decay;
    if (target == 0.0 && std::abs(speed) < 1e-5 && std::abs(rate) < 1e-3)
    {
        speed = rate = 0.0; // At rest.
    }
    return covered;
}

// The surface normal of the ellipsoid at a geodetic position, in degrees.
vgl_dvec3 up_at(double latitude, double longitude)
{
    const double lat = vgl::degrees_to_radians(latitude);
    const double lon = vgl::degrees_to_radians(longitude);
    return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

vgl_dvec3 up_at(const vgl_dvec3& ecef)
{
    const vgl::expected<vgl_geodetic> g =
        vgl::ellipsoid_ecef_to_geodetic(vgl::ellipsoid_wgs84_get(), ecef);
    return g ? up_at(vgl::radians_to_degrees(g->latitude), vgl::radians_to_degrees(g->longitude))
             : normalize(ecef);
}

// The camera with a pose. Looking straight down or up, the heading is where the image's up
// points; otherwise where the view does. The camera has no roll, so the horizon stays level.
// With keep_down, a camera that looked straight down before still does: turning the globe
// would otherwise tilt it slightly, by the difference between the ellipsoid's normal and the
// direction of its centre.
camera camera_of(const pose& p, const camera& previous, bool keep_down = false)
{
    const vgl::expected<vgl_geodetic> g =
        vgl::ellipsoid_ecef_to_geodetic(vgl::ellipsoid_wgs84_get(), p.eye);
    if (!g)
    {
        return previous;
    }
    camera c;
    c.latitude = vgl::radians_to_degrees(g->latitude);
    c.longitude = vgl::radians_to_degrees(g->longitude);
    c.altitude = g->height;
    const double lat = g->latitude;
    const double lon = g->longitude;
    const vgl_dvec3 east{-std::sin(lon), std::cos(lon), 0.0};
    const vgl_dvec3 north{
        -std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat)};
    const vgl_dvec3 up = up_at(c.latitude, c.longitude);
    const vgl_dvec3 f = normalize(p.forward);
    if (keep_down && previous.pitch >= 89.999)
    {
        c.pitch = 90.0;
        const double e = dot(p.up, east);
        const double n = dot(p.up, north);
        c.heading = std::abs(e) + std::abs(n) < 1e-12 ? previous.heading
                                                      : vgl::radians_to_degrees(std::atan2(e, n));
        return c;
    }
    const double down = std::clamp(-dot(f, up), -1.0, 1.0);
    c.pitch = vgl::radians_to_degrees(std::asin(down));
    // The view's own heading whenever it has a horizontal part, so that the camera rebuilds the
    // same view; only straight down or up does the image's up decide.
    double e = dot(f, east);
    double n = dot(f, north);
    if (std::abs(e) + std::abs(n) < 1e-9)
    {
        e = dot(p.up, east);
        n = dot(p.up, north);
    }
    c.heading = std::abs(e) + std::abs(n) < 1e-12 ? previous.heading
                                                  : vgl::radians_to_degrees(std::atan2(e, n));
    return c;
}

} // namespace

void globe_navigation::viewport(double width, double height, double field_of_view)
{
    width_ = std::max(width, 1.0);
    height_ = std::max(height, 1.0);
    field_of_view_ = field_of_view;
}

void globe_navigation::ground(double height)
{
    latest_ground_ = height;
    settle();
}

void globe_navigation::settle()
{
    // Between movements, points are found at the latest height; during one, at the height it
    // began with.
    if (mode_ == drag::none && !released_ && !throwing_ && zoom_left_ == 0.0 && !keys_moving_)
    {
        ground_ = latest_ground_;
    }
}

vgl_dvec3 globe_navigation::ray(const camera& c, double x, double y) const
{
    // From the camera's axes (the rows of its view matrix).
    const vgl_dmat4 v = camera_view_matrix(c);
    const double t = std::tan(vgl::degrees_to_radians(field_of_view_) / 2.0);
    const double sx = (2.0 * x / width_ - 1.0) * t * (width_ / height_);
    const double sy = (1.0 - 2.0 * y / height_) * t;
    return normalize({v.m[0] * sx + v.m[1] * sy - v.m[2],
                      v.m[4] * sx + v.m[5] * sy - v.m[6],
                      v.m[8] * sx + v.m[9] * sy - v.m[10]});
}

bool globe_navigation::pick(const camera& c, double x, double y, vgl_dvec3& out) const
{
    const vgl_dvec3 dir = ray(c, x, y);
    // The ellipsoid raised to the terrain, but never above the camera, scaled to a unit sphere.
    const double h = std::min(ground_, c.altitude - 1.0);
    const double a = semi_major + h;
    const double b = semi_minor + h;
    const vgl_dvec3 eye = camera_eye(c);
    const vgl_dvec3 o{eye.x / a, eye.y / a, eye.z / b};
    const vgl_dvec3 d{dir.x / a, dir.y / a, dir.z / b};
    const double qa = dot(d, d);
    const double qb = 2.0 * dot(o, d);
    const double qc = dot(o, o) - 1.0;
    const double disc = qb * qb - 4.0 * qa * qc;
    if (disc < 0.0)
    {
        return false;
    }
    const double distance = (-qb - std::sqrt(disc)) / (2.0 * qa);
    if (distance <= 0.0)
    {
        return false;
    }
    out = add(eye, scale(dir, distance));
    return true;
}

vgl_dvec3 globe_navigation::anchor(const camera& c, double x, double y) const
{
    // The point under the cursor, or else the one at the centre, or else the one below.
    vgl_dvec3 p{};
    if (pick(c, x, y, p) || pick(c, width_ / 2.0, height_ / 2.0, p))
    {
        return p;
    }
    return *vgl::ellipsoid_geodetic_to_ecef(
        vgl::ellipsoid_wgs84_get(),
        {vgl::degrees_to_radians(c.latitude), vgl::degrees_to_radians(c.longitude), ground_});
}

void globe_navigation::bring(camera& c, const vgl_dvec3& p, double x, double y) const
{
    // Turn the globe about its centre so that the point under (x, y) moves to p, which then
    // appears there. A camera looking straight down keeps doing so, and a few rounds absorb what
    // that changes; any other camera is aimed at the end, to put p under (x, y) exactly.
    const bool down = c.pitch >= 89.999;
    for (int round = 0; round < (down ? 3 : 1); ++round)
    {
        vgl_dvec3 now{};
        if (!pick(c, x, y, now))
        {
            return;
        }
        const vgl_dvec3 a = normalize(now);
        const vgl_dvec3 b = normalize(p);
        const vgl_dvec3 axis = cross(a, b);
        const double sine = length(axis);
        if (sine < 1e-15)
        {
            return;
        }
        c = camera_of(
            rotated(pose_of(c), scale(axis, 1.0 / sine), std::atan2(sine, dot(a, b))), c, true);
    }
    if (!down)
    {
        aim(c, p, x, y);
    }
}

void globe_navigation::walk(camera& c, double forward, double right) const
{
    // Turn the globe about its centre, carrying the camera over the ground in the direction it
    // faces (or, looking straight down, the image's up), at the same height.
    const double lat = vgl::degrees_to_radians(c.latitude);
    const double lon = vgl::degrees_to_radians(c.longitude);
    const double heading = vgl::degrees_to_radians(c.heading);
    const vgl_dvec3 east{-std::sin(lon), std::cos(lon), 0.0};
    const vgl_dvec3 north{
        -std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat)};
    const vgl_dvec3 ahead = add(scale(north, std::cos(heading)), scale(east, std::sin(heading)));
    const vgl_dvec3 side = add(scale(east, std::cos(heading)), scale(north, -std::sin(heading)));
    const vgl_dvec3 towards = add(scale(ahead, forward), scale(side, right));
    const double along = length(towards);
    if (along < 1e-12)
    {
        return;
    }
    // Turning the eye about eye x towards moves it towards, through the same angle in every
    // direction, since the axis is square to the eye.
    const vgl_dvec3 axis = normalize(cross(camera_eye(c), towards));
    const double altitude = c.altitude;
    c = camera_of(rotated(pose_of(c), axis, along / semi_major), c, true);
    c.altitude = altitude;
}

void globe_navigation::orbit_by(camera& c, const vgl_dvec3& pivot, double yaw, double tilt) const
{
    const vgl_dvec3 n = up_at(pivot);
    pose p = pose_of(c);
    // Turn about the vertical through the pivot.
    if (yaw != 0.0)
    {
        p.eye = add(pivot, rotate(add(p.eye, scale(pivot, -1.0)), n, yaw));
        p.forward = rotate(p.forward, n, yaw);
        p.up = rotate(p.up, n, yaw);
    }
    // Tilt about the camera's own horizontal axis through the pivot, within limits: well defined
    // even looking straight down.
    bool down = false;
    if (tilt != 0.0)
    {
        const vgl_dvec3 across = cross(p.forward, p.up); // The camera's right.
        if (length(across) > 1e-9)
        {
            const vgl_dvec3 k = normalize(across);
            const vgl_dvec3 offset = add(p.eye, scale(pivot, -1.0));
            // Past straight down a camera without roll would turn the view over: tilting stops
            // where the camera looks straight down, its image's up level with its own horizon.
            // For a pivot off the centre of the view, that comes before the eye is above it.
            const auto above_horizon = [&](double angle) {
                return dot(rotate(p.up, k, angle), up_at(add(pivot, rotate(offset, k, angle))));
            };
            if (above_horizon(tilt) < 0.0)
            {
                double lo = 0.0;
                double hi = tilt;
                for (int i = 0; i < 40; ++i)
                {
                    const double mid = 0.5 * (lo + hi);
                    (above_horizon(mid) >= 0.0 ? lo : hi) = mid;
                }
                tilt = lo;
                down = true;
            }
            const vgl_dvec3 eye = add(pivot, rotate(offset, k, tilt));
            const double from_vertical = vgl::radians_to_degrees(
                std::acos(std::clamp(dot(n, normalize(add(eye, scale(pivot, -1.0)))), -1.0, 1.0)));
            if (from_vertical <= max_tilt)
            {
                p.eye = eye;
                p.forward = rotate(p.forward, k, tilt);
                p.up = rotate(p.up, k, tilt);
            }
        }
    }
    // Stopped at straight down, the heading comes from the image's up: the view's own direction
    // has no horizontal part left to give it. So it does for a camera that looked straight down
    // and only turned, whose view's horizontal part is rounding, which would turn it anywhere.
    camera straight_down = c;
    straight_down.pitch = 90.0;
    const camera next = down ? camera_of(p, straight_down, true)
                             : camera_of(p, c, c.pitch >= 89.999 && tilt == 0.0);
    if (next.altitude >= ground_ + min_clearance)
    {
        c = next;
    }
}

void globe_navigation::zoom_by(camera& c, const vgl_dvec3& towards, double factor) const
{
    double x = 0.0;
    double y = 0.0;
    const bool visible = project(c, towards, x, y);
    // Looking (nearly) straight down, a level horizon cannot follow a point off the centre: the
    // camera keeps looking down, changes its height, and the globe turns to bring the point back
    // under its place on the screen.
    if (c.pitch > 80.0)
    {
        camera next = c;
        next.altitude = ground_ + (c.altitude - ground_) * factor;
        if (next.altitude < ground_ + min_clearance || next.altitude > max_altitude)
        {
            return;
        }
        if (visible)
        {
            // Turning about the centre of an ellipsoid changes the height above it a little, by
            // how far the turn goes: keep the height, so that a zoom reaches the same one however
            // it is split into steps.
            const double altitude = next.altitude;
            bring(next, towards, x, y);
            next.altitude = altitude;
        }
        c = next;
        return;
    }
    // Otherwise, move along the line to the point, keeping the direction of view and so the
    // point where it is on the screen.
    pose p = pose_of(c);
    const vgl_dvec3 offset = add(p.eye, scale(towards, -1.0));
    p.eye = add(towards, scale(offset, factor));
    camera next = camera_of(p, c);
    if (next.altitude >= ground_ + min_clearance && next.altitude <= max_altitude)
    {
        if (visible)
        {
            aim(next, towards, x, y);
        }
        c = next;
    }
}

bool globe_navigation::project(const camera& c, const vgl_dvec3& p, double& x, double& y) const
{
    const vgl_dmat4 v = camera_view_matrix(c);
    const double cx = v.m[0] * p.x + v.m[4] * p.y + v.m[8] * p.z + v.m[12];
    const double cy = v.m[1] * p.x + v.m[5] * p.y + v.m[9] * p.z + v.m[13];
    const double cz = v.m[2] * p.x + v.m[6] * p.y + v.m[10] * p.z + v.m[14];
    if (cz >= 0.0)
    {
        return false;
    }
    const double t = std::tan(vgl::degrees_to_radians(field_of_view_) / 2.0);
    x = (cx / -cz / (t * width_ / height_) * 0.5 + 0.5) * width_;
    y = (0.5 - cy / -cz / t * 0.5) * height_;
    return true;
}

void globe_navigation::aim(camera& c, const vgl_dvec3& p, double x, double y) const
{
    // Newton's method on the heading and pitch, which a level horizon leaves free, until the
    // point appears at (x, y); a few steps reach well under a pixel, a few more after a large
    // turn.
    for (int step = 0; step < 12; ++step)
    {
        double px = 0.0;
        double py = 0.0;
        if (!project(c, p, px, py))
        {
            return;
        }
        const double ex = px - x;
        const double ey = py - y;
        if (std::abs(ex) + std::abs(ey) < 1e-3)
        {
            return;
        }
        constexpr double d = 1e-4; // Degrees.
        camera ch = c;
        camera cp = c;
        ch.heading += d;
        cp.pitch += d;
        double hx = 0.0;
        double hy = 0.0;
        double qx = 0.0;
        double qy = 0.0;
        if (!project(ch, p, hx, hy) || !project(cp, p, qx, qy))
        {
            return;
        }
        // The Jacobian of the screen position by heading and pitch.
        const double a = (hx - px) / d;
        const double b = (qx - px) / d;
        const double cc = (hy - py) / d;
        const double dd = (qy - py) / d;
        const double det = a * dd - b * cc;
        if (std::abs(det) < 1e-12)
        {
            return;
        }
        c.heading -= (dd * ex - b * ey) / det;
        c.pitch = std::clamp(c.pitch - (-cc * ex + a * ey) / det, -60.0, 90.0);
    }
}

void globe_navigation::begin_pan(const camera& c, const vgl_dvec3& p)
{
    mode_ = drag::pan;
    start_ = c;
    grabbed_ = p;
    holding_ = true;
    pan_eye_ = camera_eye(c);
    moved_ = false;
    sample_count_ = 0;
    record();
}

void globe_navigation::begin_spin(const camera& c, const vgl_dvec3& p)
{
    mode_ = drag::spin;
    start_ = c;
    spin_point_ = p;
    spin_angle_ = 0.0;
    spin_axis_ = pose_of(c).forward;
    pan_eye_ = camera_eye(c);
    moved_ = false;
    sample_count_ = 0;
    record();
}

bool globe_navigation::disk_point(const camera& c, double x, double y, vgl_dvec3& out) const
{
    const pose p = pose_of(c);
    const vgl_dvec3 axis = normalize(p.forward);
    const double ahead = -dot(p.eye, axis); // How far in front of the camera the centre is.
    const vgl_dvec3 d = ray(c, x, y);
    const double along = dot(d, axis);
    if (ahead <= 0.0 || along <= 1e-9)
    {
        return false;
    }
    out = add(p.eye, scale(d, ahead / along));
    return true;
}

void globe_navigation::spin_to(camera& c, double x, double y)
{
    // The globe turns about the axis through its centre along the view, as a disk facing the
    // screen, by the angle that keeps the held point on the line from the centre to the cursor
    // on the screen: a grabbed point then comes back under the cursor when it returns to the
    // globe. Turning about that axis, the point keeps its depth, so its image circles the
    // axis's image at that depth: a + b cos(angle) + e sin(angle), relative to the centre's
    // image. From the camera at the start, so that the globe follows the cursor exactly however
    // the drag goes.
    const vgl_dmat4 v = camera_view_matrix(start_);
    const vgl_dvec3 right{v.m[0], v.m[4], v.m[8]};
    const vgl_dvec3 up{v.m[1], v.m[5], v.m[9]};
    const vgl_dvec3 axis{-v.m[2], -v.m[6], -v.m[10]};
    const vgl_dvec3 eye = camera_eye(start_);
    const double ahead = -dot(eye, axis); // How far in front of the camera the centre is.
    const vgl_dvec3 along = scale(axis, dot(spin_point_, axis));
    const vgl_dvec3 across = add(spin_point_, scale(along, -1.0));
    const vgl_dvec3 rel = add(along, scale(eye, -1.0));
    const double depth = dot(rel, axis);
    if (ahead <= 0.0 || depth <= 1e-9)
    {
        return; // The centre or the point is behind the camera.
    }
    const double cx = -dot(eye, right) / ahead;
    const double cy = -dot(eye, up) / ahead;
    // The cursor on the image plane at unit depth, as the rays are built.
    const double t = std::tan(vgl::degrees_to_radians(field_of_view_) / 2.0);
    const double dx = (2.0 * x / width_ - 1.0) * t * (width_ / height_) - cx;
    const double dy = (1.0 - 2.0 * y / height_) * t - cy;
    if (std::hypot(dx, dy) < 1e-12)
    {
        return; // At the centre's image, where the line has no direction.
    }
    const vgl_dvec3 turned = cross(axis, across);
    const double ax = dot(rel, right) / depth - cx;
    const double ay = dot(rel, up) / depth - cy;
    const double bx = dot(across, right) / depth;
    const double by = dot(across, up) / depth;
    const double ex = dot(turned, right) / depth;
    const double ey = dot(turned, up) / depth;
    // On the line: square to (-dy, dx) it has no part, A + B cos + C sin = 0.
    const double a = -dy * ax + dx * ay;
    const double b = -dy * bx + dx * by;
    const double e = -dy * ex + dx * ey;
    const double r = std::hypot(b, e);
    if (r < 1e-15)
    {
        return; // On the axis, where turning does not move it.
    }
    // Of the two angles, one on the cursor's side of the centre, the nearer to the last;
    // where the line misses the point's circle, the angle that brings it closest.
    const double phi = std::atan2(e, b);
    const double delta = std::acos(std::clamp(-a / r, -1.0, 1.0));
    double best = spin_angle_;
    double best_score = 1e300;
    for (const double candidate : {phi + delta, phi - delta})
    {
        const double angle = spin_angle_ + std::remainder(candidate - spin_angle_, 2.0 * vgl::pi);
        const double px = ax + bx * std::cos(angle) + ex * std::sin(angle);
        const double py = ay + by * std::cos(angle) + ey * std::sin(angle);
        const double score = std::abs(angle - spin_angle_) + (px * dx + py * dy < 0.0 ? 10.0 : 0.0);
        if (score < best_score)
        {
            best_score = score;
            best = angle;
        }
    }
    spin_angle_ = best;
    // Turning the camera back about the axis by the angle turns the globe forward by it: the
    // camera moves over the globe, still looking the same way. On a sphere, its horizon would
    // stay level; on the ellipsoid it is levelled, and keeps its height.
    camera next = camera_of(rotated(pose_of(start_), axis, -spin_angle_), start_, true);
    next.altitude = start_.altitude;
    c = next;
    pan_eye_ = camera_eye(c);
    moved_ = true;
}

void globe_navigation::press(drag mode, double x, double y, const camera& c)
{
    stop();
    ground_ = latest_ground_;
    mode_ = mode;
    x_ = x;
    y_ = y;
    start_ = c;
    holding_ = false;
    vgl_dvec3 p{};
    if (mode == drag::pan && !pick(c, x, y, p))
    {
        mode_ = drag::spin; // Grabbing space turns the globe like a disk instead.
    }
    if (mode_ == drag::pan)
    {
        begin_pan(c, p);
    }
    if (mode_ == drag::spin)
    {
        // Until the drag grabs a point, it holds the one where the line from the cursor's place
        // on the disk to the centre meets the globe: the disk turns under the cursor.
        vgl_dvec3 on_disk{};
        begin_spin(c, disk_point(c, x, y, on_disk) ? on_disk : vgl_dvec3{});
    }
    if (mode == drag::orbit)
    {
        // Around the point under the cursor, which stays there; pressed over space, nothing.
        if (!pick(c, x, y, pivot_))
        {
            mode_ = drag::none;
        }
        pivot_x_ = x;
        pivot_y_ = y;
        focus_ = pivot_;
        has_focus_ = mode_ == drag::orbit;
        focus_since_ = 0.0;
    }
    if (mode == drag::zoom)
    {
        pivot_ = anchor(c, x, y);
        focus_ = pivot_;
        has_focus_ = true;
        focus_since_ = 0.0;
    }
}

void globe_navigation::move(double x, double y, camera& c)
{
    const double dx = x - x_;
    const double dy = y - y_;
    x_ = x;
    y_ = y;
    switch (mode_)
    {
    case drag::none:
        return;
    case drag::spin:
    {
        // Once the cursor reaches the globe, it grabs the point it held, which the spin kept on
        // the line to the cursor, or else the point there.
        spin_to(c, x, y);
        vgl_dvec3 p{};
        if (pick(c, x, y, p))
        {
            begin_pan(c, holding_ ? grabbed_ : p);
            bring(c, grabbed_, x, y);
            pan_eye_ = camera_eye(c);
        }
        return;
    }
    case drag::pan:
    {
        vgl_dvec3 under{};
        // Once the cursor leaves the globe, it spins.
        if (!pick(c, x, y, under))
        {
            begin_spin(c, grabbed_);
            spin_to(c, x, y);
            return;
        }
        // From the camera at the start, so that the grabbed point stays exactly under the
        // cursor however the drag goes; if the globe has turned the cursor's place on it out
        // of that camera's view, from the camera now.
        if (!pick(start_, x, y, under))
        {
            start_ = c;
        }
        if (pick(start_, x, y, under))
        {
            camera moved = start_;
            bring(moved, grabbed_, x, y);
            c = moved;
            pan_eye_ = camera_eye(c);
            moved_ = true;
        }
        return;
    }
    case drag::orbit:
        orbit_by(
            c, pivot_, vgl::degrees_to_radians(-dx * 0.25), vgl::degrees_to_radians(-dy * 0.25));
        // Turning about a point off the centre would roll the camera, which has no roll: aim it
        // to keep the point under the cursor. Looking straight down, the heading and pitch
        // cannot both move the point (aiming would tilt the camera anywhere): turn the globe to
        // bring it back instead, at the same height, as zooming does.
        if (c.pitch >= 89.999)
        {
            const double altitude = c.altitude;
            bring(c, pivot_, pivot_x_, pivot_y_);
            c.altitude = altitude;
        }
        else
        {
            aim(c, pivot_, pivot_x_, pivot_y_);
        }
        return;
    case drag::look:
        c.heading = std::remainder(c.heading + dx * 0.15, 360.0);
        c.pitch = std::clamp(c.pitch + dy * 0.15, -60.0, 90.0);
        return;
    case drag::zoom:
        zoom_by(c, pivot_, std::exp(dy * 0.01));
        return;
    }
}

void globe_navigation::release()
{
    // A pan or spin released while moving carries on, slowing down: the next update, which
    // knows when the last moves happened, decides.
    released_ = mode_ == drag::pan || mode_ == drag::spin;
    spun_ = mode_ == drag::spin;
    mode_ = drag::none;
    settle();
}

void globe_navigation::record()
{
    samples_[sample_count_ % samples_.size()] = {clock_, pan_eye_, spin_angle_};
    ++sample_count_;
}

void globe_navigation::start_throw(const camera& c)
{
    // How fast the globe turned over the last moments of the pan, measured from the oldest
    // position in that window to the last: steady however the moves and frames fell.
    const std::size_t kept = std::min(sample_count_, samples_.size());
    if (kept < 2)
    {
        return;
    }
    const pan_sample& last = samples_[(sample_count_ - 1) % samples_.size()];
    const pan_sample* first = &last;
    for (std::size_t i = 2; i <= kept; ++i)
    {
        const pan_sample& s = samples_[(sample_count_ - i) % samples_.size()];
        if (last.time - s.time > throw_window)
        {
            break;
        }
        first = &s;
    }
    const double elapsed = last.time - first->time;
    if (elapsed <= 0.0)
    {
        return;
    }
    if (spun_)
    {
        // A spin turned the camera back about its axis by the angle: the throw carries on.
        const double turned = last.angle - first->angle;
        if (std::abs(turned) < 1e-15)
        {
            return;
        }
        throw_axis_ = scale(normalize(spin_axis_), turned > 0.0 ? -1.0 : 1.0);
        throw_rate_ = std::abs(turned) / elapsed;
    }
    else
    {
        const vgl_dvec3 a = normalize(first->eye);
        const vgl_dvec3 b = normalize(last.eye);
        const vgl_dvec3 axis = cross(a, b);
        const double sine = length(axis);
        if (sine < 1e-15)
        {
            return;
        }
        throw_axis_ = scale(axis, 1.0 / sine);
        throw_rate_ = std::atan2(sine, dot(a, b)) / elapsed;
    }
    // Measured against how much of the globe the view spans: a throw needs a twentieth of the
    // view each second to start, and stops below a hundred-and-fiftieth. A spin's rate is
    // measured the same way, at the rim of the globe.
    const double span = std::max(c.altitude - ground_, 1.0) * 2.0 *
                        std::tan(vgl::degrees_to_radians(field_of_view_) / 2.0) / semi_major;
    throw_stop_ = span / 150.0;
    throwing_ = throw_rate_ > span / 20.0;
}

void globe_navigation::scroll(double steps, double x, double y, const camera& c)
{
    settle();
    zoom_point_ = anchor(c, x, y);
    focus_ = zoom_point_;
    has_focus_ = true;
    focus_since_ = 0.0;
    zoom_left_ += -steps * 0.3;
}

void globe_navigation::double_click(double x, double y, const camera& c)
{
    settle();
    vgl_dvec3 p{};
    if (pick(c, x, y, p))
    {
        zoom_point_ = p;
        focus_ = p;
        has_focus_ = true;
        focus_since_ = 0.0;
        zoom_left_ = std::log(0.5);
    }
}

void globe_navigation::stop()
{
    released_ = false;
    throwing_ = false;
    throw_rate_ = 0.0;
    walk_forward_ = walk_forward_rate_ = 0.0;
    walk_right_ = walk_right_rate_ = 0.0;
    zoom_left_ = 0.0;
    settle();
}

void globe_navigation::update(double dt, const navigation_keys& keys, camera& c)
{
    if (dt <= 0.0)
    {
        return;
    }
    // The moves since the last update happened during the dt that has just passed. During a pan,
    // every update counts, moved or not, so that holding still slows it; after its release, only
    // moves that came before the release.
    clock_ += dt;
    if (mode_ == drag::pan || mode_ == drag::spin || (released_ && moved_))
    {
        record();
    }
    moved_ = false;
    if (released_)
    {
        released_ = false;
        start_throw(c);
    }
    if (throwing_)
    {
        // The rate decays exponentially, and the angle is its integral over dt: the throw covers
        // the same ground in the same time however the time is split into frames.
        const double decay = std::exp(-dt * throw_decay);
        const double angle = throw_rate_ * (1.0 - decay) / throw_decay;
        // At the same height: turning about the centre of an ellipsoid would change it a little.
        const double altitude = c.altitude;
        c = camera_of(rotated(pose_of(c), throw_axis_, angle), c, true);
        c.altitude = altitude;
        throw_rate_ *= decay;
        throwing_ = throw_rate_ > throw_stop_;
        settle();
    }

    // Smooth zoom: most of what is left, each frame.
    if (zoom_left_ != 0.0)
    {
        const double step =
            std::abs(zoom_left_) < 1e-4 ? zoom_left_ : zoom_left_ * (1.0 - std::exp(-dt * 10.0));
        zoom_by(c, zoom_point_, std::exp(step));
        zoom_left_ -= step;
        settle();
    }

    // Keys: move over the ground, in view heights (at the camera's height above the ground) per
    // second, easing in and stopping quickly, the same in every direction; with shift, turn at
    // 45 and tilt at 30 degrees per second. While keys move the camera, the ground height they
    // began with holds.
    const double cx = width_ / 2.0;
    const double cy = height_ / 2.0;
    const double horizontal = (keys.right ? 1.0 : 0.0) - (keys.left ? 1.0 : 0.0);
    const double vertical = (keys.up ? 1.0 : 0.0) - (keys.down ? 1.0 : 0.0);
    if (keys.shift && (horizontal != 0.0 || vertical != 0.0))
    {
        orbit_by(c,
                 anchor(c, cx, cy),
                 vgl::degrees_to_radians(horizontal * 45.0 * dt),
                 vgl::degrees_to_radians(-vertical * 30.0 * dt));
    }
    // Diagonally as fast as straight on.
    const double top =
        horizontal != 0.0 && vertical != 0.0 ? walk_top_speed / std::sqrt(2.0) : walk_top_speed;
    const double forward =
        approach(walk_forward_, walk_forward_rate_, keys.shift ? 0.0 : vertical * top, dt);
    const double right =
        approach(walk_right_, walk_right_rate_, keys.shift ? 0.0 : horizontal * top, dt);
    if (forward != 0.0 || right != 0.0)
    {
        const double view = std::max(c.altitude - ground_, 1.0) * 2.0 *
                            std::tan(vgl::degrees_to_radians(field_of_view_) / 2.0);
        walk(c, forward * view, right * view);
    }
    keys_moving_ = horizontal != 0.0 || vertical != 0.0 || keys.zoom_in != keys.zoom_out ||
                   walk_forward_ != 0.0 || walk_right_ != 0.0;
    if (keys.zoom_in != keys.zoom_out)
    {
        zoom_by(c, anchor(c, cx, cy), std::exp((keys.zoom_in ? -1.2 : 1.2) * dt));
    }
    settle();

    // The focus shows while the mouse orbits or zooms around it, then fades.
    const bool focused = mode_ == drag::orbit || mode_ == drag::zoom || zoom_left_ != 0.0;
    focus_since_ = focused ? 0.0 : focus_since_ + dt;
}

bool globe_navigation::focus(const camera& c, double& x, double& y, double& strength) const
{
    strength = has_focus_ ? 1.0 - focus_since_ / focus_fade : 0.0;
    return strength > 0.0 && project(c, focus_, x, y);
}

} // namespace sample
