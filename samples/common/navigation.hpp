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
//   arrows, WASD         move; with shift, turn and tilt
//   page up, page down   zoom
//
// Points under the cursor are found on the ellipsoid raised to the terrain's height under the
// camera, which is close enough to grab at the sample's resolution. That height is taken when a
// drag, throw or smooth zoom begins and held until it ends: the height under the camera changes
// as it moves, and the point being held would otherwise slide with it.

#ifndef VGL_SAMPLES_NAVIGATION_HPP
#define VGL_SAMPLES_NAVIGATION_HPP

#include "sample_camera.hpp"

#include <array>

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

    // The terrain's height above the ellipsoid under the camera, for finding points. It takes
    // effect when no drag, throw or smooth zoom is under way.
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
    // Takes the latest ground height, unless a movement is holding the one it began with.
    void settle();
    // Records where the camera is during a pan.
    void record();
    // Starts a throw if the pan was moving when it was released.
    void start_throw(const camera& c);
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
    double ground_ = 0.0;        // The height that points are found at.
    double latest_ground_ = 0.0; // The height last reported, for when a movement ends.

    drag mode_ = drag::none;
    double x_ = 0.0; // Cursor at the last move.
    double y_ = 0.0;
    camera start_; // The camera when a pan began.
    vgl_dvec3 grabbed_{};
    vgl_dvec3 pivot_{};

    // Where the camera was during a pan, at each update, to measure how fast the globe turned
    // when it was released: a ring of the latest positions, on a clock of the updates' times.
    struct pan_sample
    {
        double time = 0.0;
        vgl_dvec3 eye{};
    };
    std::array<pan_sample, 64> samples_{};
    std::size_t sample_count_ = 0;
    double clock_ = 0.0;
    vgl_dvec3 pan_eye_{};   // The camera's position after the latest move of a pan.
    bool moved_ = false;    // A pan moved since the last update.
    bool released_ = false; // A pan ended; the next update decides whether it throws.

    // A throw: the globe keeps turning about an axis through its centre, slowing down.
    bool throwing_ = false;
    vgl_dvec3 throw_axis_{};
    double throw_rate_ = 0.0; // Radians per second.
    double throw_stop_ = 0.0; // The rate at which it stops.

    // A smooth zoom: the natural logarithm of the factor still to apply, towards a point.
    double zoom_left_ = 0.0;
    vgl_dvec3 zoom_point_{};
};

} // namespace sample

#endif // VGL_SAMPLES_NAVIGATION_HPP
