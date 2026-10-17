// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Globe navigation in the style of Google Earth, shared by the interactive samples. It knows
// nothing of windows or graphics APIs: each sample turns its window's events into the calls
// below, which do the following:
//
//   left drag            grab the globe: the point under the cursor stays under it, and
//                        releasing while moving throws it; over space, turn the globe like a
//                        disk facing the screen, about its centre, following the cursor
//                        around it; the drag switches between the two wherever the cursor
//                        crosses the globe's edge, and a spin released while moving throws
//                        it too
//   scroll, double-click zoom towards the point under the cursor
//   shift + left drag,   orbit around the point under the cursor, which stays there: sideways
//   middle drag          turns, up and down tilts; pressed over space, nothing
//   ctrl + left drag     look around from where the camera is
//   right drag           zoom
//   arrows, WASD         move; with shift, turn and tilt around the point at the centre
//   page up, page down   zoom
//
// Points under the cursor are found on the ellipsoid raised to the terrain's height under the
// camera, which is close enough to grab at the sample's resolution. That height is taken when a
// drag, throw, smooth zoom or key movement begins and held until it ends: the height under the
// camera changes as it moves, and the point being held would otherwise slide with it.

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
        spin,  // Turn the globe like a disk; pressing to pan over space starts it.
        zoom,  // Zoom by dragging up and down.
    };

    // The window in cursor coordinates, and the vertical field of view in degrees.
    void viewport(double width, double height, double field_of_view);

    // The terrain's height above the ellipsoid under the camera, for finding points. It takes
    // effect when no drag, throw, smooth zoom or key movement is under way.
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

    // The point that the mouse orbits or zooms around, for a reticle: where it appears in the
    // window, and how strongly to show it, 1 while the movement lasts, fading to 0 over half a
    // second after it ends. False when there is none to show.
    [[nodiscard]] bool focus(const camera& c, double& x, double& y, double& strength) const;

    // The point of the (raised) ellipsoid under a cursor position, if the ray meets it.
    [[nodiscard]] bool pick(const camera& c, double x, double y, vgl_dvec3& out) const;

private:
    [[nodiscard]] vgl_dvec3 anchor(const camera& c, double x, double y) const;
    // The direction of the ray through a cursor position.
    [[nodiscard]] vgl_dvec3 ray(const camera& c, double x, double y) const;
    // Starts a pan that holds the point p, now under the cursor.
    void begin_pan(const camera& c, const vgl_dvec3& p);
    // Starts a spin that holds the point p on the line from the globe's centre to the cursor.
    void begin_spin(const camera& c, const vgl_dvec3& p);
    // Where the ray through (x, y) meets the disk: the plane through the globe's centre square to
    // the view. False when the centre is not in front of the camera.
    [[nodiscard]] bool disk_point(const camera& c, double x, double y, vgl_dvec3& out) const;
    // Turns the globe about the axis through its centre along the view, as a disk facing the
    // screen, keeping the held point on the line from the centre to (x, y) on the screen.
    void spin_to(camera& c, double x, double y);
    // Takes the latest ground height, unless a movement is holding the one it began with.
    void settle();
    // Records where the camera is during a pan.
    void record();
    // Starts a throw if the pan was moving when it was released.
    void start_throw(const camera& c);
    // Moves the camera over the ground, forward and to the right by distances in metres, keeping
    // its height.
    void walk(camera& c, double forward, double right) const;
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
    camera start_; // The camera when a pan or spin began.
    vgl_dvec3 grabbed_{};
    bool holding_ = false;    // The drag has grabbed a point, which it holds while spinning.
    vgl_dvec3 spin_point_{};  // The point that a spin holds on the line to the cursor.
    double spin_angle_ = 0.0; // How far the spin has turned the globe, in radians.
    vgl_dvec3 pivot_{};
    double pivot_x_ = 0.0; // Where an orbit's pivot stays on the screen.
    double pivot_y_ = 0.0;

    // Where the camera was during a pan, or how far a spin had turned, at each update, to
    // measure how fast the globe turned when it was released: a ring of the latest positions, on
    // a clock of the updates' times.
    struct pan_sample
    {
        double time = 0.0;
        vgl_dvec3 eye{};
        double angle = 0.0;
    };
    std::array<pan_sample, 64> samples_{};
    std::size_t sample_count_ = 0;
    double clock_ = 0.0;
    vgl_dvec3 pan_eye_{};   // The camera's position after the latest move of a pan.
    bool moved_ = false;    // A pan or spin moved since the last update.
    bool released_ = false; // A pan or spin ended; the next update decides whether it throws.
    bool spun_ = false;     // What ended was a spin.
    vgl_dvec3 spin_axis_{}; // The axis a spin turns the globe about: the view at its start.

    // A throw: the globe keeps turning about an axis through its centre, slowing down.
    bool throwing_ = false;
    vgl_dvec3 throw_axis_{};
    double throw_rate_ = 0.0; // Radians per second.
    double throw_stop_ = 0.0; // The rate at which it stops.

    bool keys_moving_ = false; // Keys moved the camera at the last update.
    // The speed of key movement forward and to the right, in view heights per second, and how
    // fast each changes.
    double walk_forward_ = 0.0;
    double walk_forward_rate_ = 0.0;
    double walk_right_ = 0.0;
    double walk_right_rate_ = 0.0;

    // A smooth zoom: the natural logarithm of the factor still to apply, towards a point.
    double zoom_left_ = 0.0;
    vgl_dvec3 zoom_point_{};

    // The point the mouse last orbited or zoomed around, and seconds since it stopped.
    vgl_dvec3 focus_{};
    bool has_focus_ = false;
    double focus_since_ = 1.0e9;
};

} // namespace sample

#endif // VGL_SAMPLES_NAVIGATION_HPP
