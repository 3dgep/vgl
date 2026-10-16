// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Globe navigation in the style of Google Earth, shared by the interactive samples. It knows
// nothing of windows or graphics APIs: each sample turns its window's events into the calls
// below, which do the following:
//
//   left drag            grab the globe: the point under the cursor stays under it, and
//                        releasing while moving throws it
//   scroll, double-click zoom towards the point under the cursor
//   shift + left drag,   orbit around the point at the centre of the view: sideways turns,
//   middle drag          up and down tilts
//   ctrl + left drag     look around from where the camera is
//   right drag           zoom
//   arrows               move; with shift, turn and tilt
//   page up, page down   zoom
//
// Points under the cursor are found on the ellipsoid raised to the terrain's height under the
// camera, which is close enough to grab at the sample's resolution.

#ifndef VGL_SAMPLES_NAVIGATION_HPP
#define VGL_SAMPLES_NAVIGATION_HPP

#include "sample_camera.hpp"

namespace sample
{

// Keys held down, polled each frame.
struct navigation_keys
{
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool zoom_in = false;
    bool zoom_out = false;
    bool shift = false;
};

class globe_navigation
{
public:
    enum class drag
    {
        none,
        pan,   // Grab the globe.
        orbit, // Turn and tilt around the point at the centre.
        look,  // Turn the camera in place.
        zoom,  // Zoom by dragging up and down.
    };

    // The window in cursor coordinates, and the vertical field of view in degrees.
    void viewport(double width, double height, double field_of_view);

    // The terrain's height above the ellipsoid under the camera, for finding points.
    void ground(double height);

    void press(drag mode, double x, double y, const camera& c);
    void move(double x, double y, camera& c);
    void release();
    void scroll(double steps, double x, double y, const camera& c);
    void double_click(double x, double y, const camera& c);

    // Advances throws, smooth zooms and keys by dt seconds.
    void update(double dt, const navigation_keys& keys, camera& c);

    // Stops any movement, as when jumping to another place.
    void stop();

    [[nodiscard]] bool dragging() const { return mode_ != drag::none; }

    // The point of the (raised) ellipsoid under a cursor position, if the ray meets it.
    [[nodiscard]] bool pick(const camera& c, double x, double y, vgl_dvec3& out) const;

private:
    [[nodiscard]] vgl_dvec3 anchor(const camera& c, double x, double y) const;
    void pan_by(camera& c, double x, double y, double dx, double dy) const;
    // Turns the globe about its centre until the point p appears at (x, y).
    void bring(camera& c, const vgl_dvec3& p, double x, double y) const;
    void orbit_by(camera& c, const vgl_dvec3& pivot, double yaw, double tilt) const;
    void zoom_by(camera& c, const vgl_dvec3& towards, double factor) const;
    // Where a point appears in the window; false behind the camera.
    [[nodiscard]] bool project(const camera& c, const vgl_dvec3& p, double& x, double& y) const;
    // Turns the camera, keeping the horizon level, until the point appears at (x, y).
    void aim(camera& c, const vgl_dvec3& p, double x, double y) const;

    double width_ = 1.0;
    double height_ = 1.0;
    double field_of_view_ = 60.0;
    double ground_ = 0.0;

    drag mode_ = drag::none;
    double x_ = 0.0; // Cursor at the last move.
    double y_ = 0.0;
    camera start_; // The camera when a pan began.
    vgl_dvec3 grabbed_{};
    vgl_dvec3 pivot_{};

    // A throw: the cursor's velocity in pixels per second when a pan was released.
    double velocity_x_ = 0.0;
    double velocity_y_ = 0.0;
    double since_move_ = 0.0; // Seconds since the cursor last moved during a pan.
    double frame_dx_ = 0.0;   // Cursor movement since the last update.
    double frame_dy_ = 0.0;
    bool throwing_ = false;

    // A smooth zoom: the natural logarithm of the factor still to apply, towards a point.
    double zoom_left_ = 0.0;
    vgl_dvec3 zoom_point_{};
};

} // namespace sample

#endif // VGL_SAMPLES_NAVIGATION_HPP
