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
    ground_ = height;
}

bool globe_navigation::pick(const camera& c, double x, double y, vgl_dvec3& out) const
{
    // The ray through the cursor, from the camera's axes (the rows of its view matrix).
    const vgl_dmat4 v = camera_view_matrix(c);
    const double t = std::tan(vgl::degrees_to_radians(field_of_view_) / 2.0);
    const double sx = (2.0 * x / width_ - 1.0) * t * (width_ / height_);
    const double sy = (1.0 - 2.0 * y / height_) * t;
    const vgl_dvec3 dir = normalize({v.m[0] * sx + v.m[1] * sy - v.m[2],
                                     v.m[4] * sx + v.m[5] * sy - v.m[6],
                                     v.m[8] * sx + v.m[9] * sy - v.m[10]});
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

void globe_navigation::pan_by(camera& c, double x, double y, double dx, double dy) const
{
    // Turn the globe about its centre so that the point at (x, y) moves to (x + dx, y + dy):
    // the camera turns the other way.
    // The point under (x, y) moves to (x + dx, y + dy).
    vgl_dvec3 from{};
    if (pick(c, x, y, from))
    {
        bring(c, from, x + dx, y + dy);
    }
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
    if (tilt != 0.0)
    {
        const vgl_dvec3 across = cross(p.forward, p.up); // The camera's right.
        if (length(across) > 1e-9)
        {
            const vgl_dvec3 k = normalize(across);
            const vgl_dvec3 eye = add(pivot, rotate(add(p.eye, scale(pivot, -1.0)), k, tilt));
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
    const camera next = camera_of(p, c);
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
            bring(next, towards, x, y);
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
    // point appears at (x, y); a few steps reach well under a pixel.
    for (int step = 0; step < 6; ++step)
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

void globe_navigation::press(drag mode, double x, double y, const camera& c)
{
    stop();
    mode_ = mode;
    x_ = x;
    y_ = y;
    start_ = c;
    if (mode == drag::pan && !pick(c, x, y, grabbed_))
    {
        mode_ = drag::look; // Grabbing the sky turns the camera instead.
    }
    if (mode == drag::orbit)
    {
        pivot_ = anchor(c, width_ / 2.0, height_ / 2.0);
    }
    if (mode == drag::zoom)
    {
        pivot_ = anchor(c, x, y);
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
    case drag::pan:
    {
        // From the camera at the start, so that the grabbed point stays exactly under the
        // cursor however the drag goes.
        vgl_dvec3 under{};
        if (pick(start_, x, y, under))
        {
            camera moved = start_;
            bring(moved, grabbed_, x, y);
            c = moved;
        }
        frame_dx_ += dx;
        frame_dy_ += dy;
        since_move_ = 0.0;
        return;
    }
    case drag::orbit:
        orbit_by(
            c, pivot_, vgl::degrees_to_radians(-dx * 0.25), vgl::degrees_to_radians(-dy * 0.25));
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
    // A pan released while moving carries on, slowing down.
    throwing_ = mode_ == drag::pan && since_move_ < 0.08 &&
                (std::abs(velocity_x_) + std::abs(velocity_y_)) > 30.0;
    mode_ = drag::none;
}

void globe_navigation::scroll(double steps, double x, double y, const camera& c)
{
    zoom_point_ = anchor(c, x, y);
    zoom_left_ += -steps * 0.3;
}

void globe_navigation::double_click(double x, double y, const camera& c)
{
    vgl_dvec3 p{};
    if (pick(c, x, y, p))
    {
        zoom_point_ = p;
        zoom_left_ = std::log(0.5);
    }
}

void globe_navigation::stop()
{
    throwing_ = false;
    velocity_x_ = velocity_y_ = 0.0;
    frame_dx_ = frame_dy_ = 0.0;
    zoom_left_ = 0.0;
}

void globe_navigation::update(double dt, const navigation_keys& keys, camera& c)
{
    if (dt <= 0.0)
    {
        return;
    }
    // The cursor's velocity while panning, smoothed over the last few frames.
    if (mode_ == drag::pan)
    {
        const double blend = std::min(1.0, dt * 20.0);
        velocity_x_ += (frame_dx_ / dt - velocity_x_) * blend;
        velocity_y_ += (frame_dy_ / dt - velocity_y_) * blend;
        frame_dx_ = frame_dy_ = 0.0;
        since_move_ += dt;
    }
    if (throwing_)
    {
        pan_by(c, x_, y_, velocity_x_ * dt, velocity_y_ * dt);
        x_ += velocity_x_ * dt;
        y_ += velocity_y_ * dt;
        const double decay = std::exp(-dt * 3.0);
        velocity_x_ *= decay;
        velocity_y_ *= decay;
        throwing_ = std::abs(velocity_x_) + std::abs(velocity_y_) > 5.0;
    }

    // Smooth zoom: most of what is left, each frame.
    if (zoom_left_ != 0.0)
    {
        const double step =
            std::abs(zoom_left_) < 1e-4 ? zoom_left_ : zoom_left_ * (1.0 - std::exp(-dt * 10.0));
        zoom_by(c, zoom_point_, std::exp(step));
        zoom_left_ -= step;
    }

    // Keys: move at a third of the view per second, turn at 45 and tilt at 30 degrees per second.
    const double cx = width_ / 2.0;
    const double cy = height_ / 2.0;
    const double horizontal = (keys.right ? 1.0 : 0.0) - (keys.left ? 1.0 : 0.0);
    const double vertical = (keys.up ? 1.0 : 0.0) - (keys.down ? 1.0 : 0.0);
    if (horizontal != 0.0 || vertical != 0.0)
    {
        if (keys.shift)
        {
            orbit_by(c,
                     anchor(c, cx, cy),
                     vgl::degrees_to_radians(horizontal * 45.0 * dt),
                     vgl::degrees_to_radians(-vertical * 30.0 * dt));
        }
        else
        {
            const double speed = height_ / 3.0 * dt;
            pan_by(c, cx, cy, -horizontal * speed, vertical * speed);
        }
    }
    if (keys.zoom_in != keys.zoom_out)
    {
        zoom_by(c, anchor(c, cx, cy), std::exp((keys.zoom_in ? -1.2 : 1.2) * dt));
    }
}

} // namespace sample
