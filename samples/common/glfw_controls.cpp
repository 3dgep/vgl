// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#define GLFW_INCLUDE_NONE
#include "glfw_controls.hpp"

#include "sample_check.hpp"

#include <GLFW/glfw3.h>

#if defined(__EMSCRIPTEN__)
#    include <emscripten/html5.h>
#endif

#include <cmath>
#include <cstddef>

namespace sample
{

namespace
{

glfw_controls& controls_of(GLFWwindow* window)
{
    return *static_cast<glfw_controls*>(glfwGetWindowUserPointer(window));
}

void on_mouse_button(GLFWwindow* window, int button, int action, int mods)
{
    glfw_controls& in = controls_of(window);
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (action == GLFW_RELEASE)
    {
        in.nav->release();
        return;
    }
    if (action != GLFW_PRESS || in.ui->wants_mouse())
    {
        return;
    }
    using drag = globe_navigation::drag;
    const camera& c = *in.camera;
    if (button == GLFW_MOUSE_BUTTON_LEFT)
    {
        // A second press soon after the first, close to it, is a double-click.
        const double now = glfwGetTime();
        if (now - in.last_click < 0.3 && std::abs(x - in.click_x) < 4.0 &&
            std::abs(y - in.click_y) < 4.0)
        {
            in.nav->double_click(x, y, c);
            in.last_click = -1.0;
            return;
        }
        in.last_click = now;
        in.click_x = x;
        in.click_y = y;
        const drag mode = (mods & GLFW_MOD_SHIFT) != 0     ? drag::orbit
                          : (mods & GLFW_MOD_CONTROL) != 0 ? drag::look
                                                           : drag::pan;
        in.nav->press(mode, x, y, c);
    }
    else if (button == GLFW_MOUSE_BUTTON_MIDDLE)
    {
        in.nav->press(drag::orbit, x, y, c);
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT)
    {
        in.nav->press(drag::zoom, x, y, c);
    }
}

void on_cursor(GLFWwindow* window, double x, double y)
{
    glfw_controls& in = controls_of(window);
    in.nav->move(x, y, *in.camera);
}

void on_scroll(GLFWwindow* window, double /*dx*/, double dy)
{
    glfw_controls& in = controls_of(window);
    if (in.ui->wants_mouse())
    {
        return;
    }
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    in.nav->scroll(dy, x, y, *in.camera);
}

void on_key(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/)
{
    if (action != GLFW_PRESS)
    {
        return;
    }
    glfw_controls& in = controls_of(window);
    if (in.ui->wants_keyboard())
    {
        return;
    }
    debug_settings& d = *in.debug;
    switch (key)
    {
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        break;
    case GLFW_KEY_L:
        d.sunlight = !d.sunlight;
        break;
    case GLFW_KEY_F1:
        d.panel = !d.panel;
        break;
    case GLFW_KEY_F2:
        d.wireframe = !d.wireframe;
        break;
    case GLFW_KEY_F3:
        d.level_colors = !d.level_colors;
        break;
    case GLFW_KEY_F4:
        d.imagery_source = !d.imagery_source;
        break;
    case GLFW_KEY_F5:
        d.freeze = !d.freeze;
        break;
    case GLFW_KEY_F6:
        d.height_colors = !d.height_colors;
        break;
    case GLFW_KEY_F7:
        d.contours = !d.contours;
        break;
    case GLFW_KEY_F8:
        d.sea = !d.sea;
        break;
    case GLFW_KEY_F9:
        d.atmosphere = !d.atmosphere;
        break;
    case GLFW_KEY_N:
    case GLFW_KEY_R:
    {
        camera to = *in.camera;
        to.heading = 0.0; // North up.
        if (key == GLFW_KEY_R)
        {
            to.pitch = 90.0; // And looking straight down.
        }
        in.nav->fly_to(*in.camera, to);
        break;
    }
    case GLFW_KEY_U:
    {
        camera to = *in.camera;
        to.pitch = 90.0;
        in.nav->fly_to(*in.camera, to);
        break;
    }
    default:
        if (key >= GLFW_KEY_1 && key < GLFW_KEY_1 + static_cast<int>(places.size()))
        {
            in.nav->fly_to(*in.camera, places[static_cast<std::size_t>(key - GLFW_KEY_1)].camera);
        }
        break;
    }
}

#if defined(__EMSCRIPTEN__)
// Fingers on a touch screen. The GLFW port turns the first finger into the mouse, which pans,
// double-taps and works the panel, and ignores the others; the controls follow every finger,
// and a second one turns the gesture into the navigation's two-finger one, which the mouse
// cannot disturb (move ignores it, and a release ends it as lifting a finger does). The page
// stops the browser's own gestures on the canvas (touch-action: none).
bool on_touch(int type, const EmscriptenTouchEvent* event, void* user)
{
    auto* window = static_cast<GLFWwindow*>(user);
    glfw_controls& in = controls_of(window);
    // Where a finger still on the screen is, in cursor coordinates, as the port puts the mouse.
    int wx = 0;
    int wy = 0;
    glfwGetWindowPos(window, &wx, &wy);
    const bool lifting = type == EMSCRIPTEN_EVENT_TOUCHEND || type == EMSCRIPTEN_EVENT_TOUCHCANCEL;
    const auto down = [&](const EmscriptenTouchPoint& t) { return !(lifting && t.isChanged); };
    const auto find = [&](long id, double& x, double& y) {
        for (int i = 0; i < event->numTouches; ++i)
        {
            const EmscriptenTouchPoint& t = event->touches[i];
            if (t.identifier == id && down(t))
            {
                x = static_cast<double>(t.clientX - wx);
                y = static_cast<double>(t.clientY - wy);
                return true;
            }
        }
        return false;
    };
    const camera& c = *in.camera;
    if (in.two_fingers)
    {
        double x0 = 0.0;
        double y0 = 0.0;
        double x1 = 0.0;
        double y1 = 0.0;
        if (!find(in.finger_ids[0], x0, y0) || !find(in.finger_ids[1], x1, y1))
        {
            in.two_fingers = false; // A finger lifted: the one left does nothing until it does.
            in.nav->release();
        }
        else if (type == EMSCRIPTEN_EVENT_TOUCHMOVE)
        {
            in.nav->touch_move(x0, y0, x1, y1, *in.camera);
        }
        return false;
    }
    // A second finger down, unless the first is working the panel, starts the gesture.
    if (type != EMSCRIPTEN_EVENT_TOUCHSTART || in.ui->wants_mouse())
    {
        return false;
    }
    std::size_t count = 0;
    for (int i = 0; i < event->numTouches && count < in.finger_ids.size(); ++i)
    {
        if (down(event->touches[i]))
        {
            in.finger_ids[count++] = event->touches[i].identifier;
        }
    }
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    if (count == 2 && find(in.finger_ids[0], x0, y0) && find(in.finger_ids[1], x1, y1))
    {
        in.two_fingers = true;
        in.last_click = -1.0; // Lifting the fingers is no click.
        in.nav->touch_press(x0, y0, x1, y1, c);
    }
    return false;
}
#endif

} // namespace

void controls_install(GLFWwindow* window, glfw_controls* controls)
{
    glfwSetWindowUserPointer(window, controls);
    glfwSetMouseButtonCallback(window, on_mouse_button);
    glfwSetCursorPosCallback(window, on_cursor);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetKeyCallback(window, on_key);
#if defined(__EMSCRIPTEN__)
    // On the document, as the port follows fingers, so that they are followed off the canvas.
    const char* document = EMSCRIPTEN_EVENT_TARGET_DOCUMENT;
    emscripten_set_touchstart_callback(document, window, false, on_touch);
    emscripten_set_touchmove_callback(document, window, false, on_touch);
    emscripten_set_touchend_callback(document, window, false, on_touch);
    emscripten_set_touchcancel_callback(document, window, false, on_touch);
#endif
}

navigation_keys controls_keys(GLFWwindow* window, const glfw_controls& controls)
{
    navigation_keys keys;
    if (controls.ui->wants_keyboard())
    {
        return keys;
    }
    const auto held = [&](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    keys.left = held(GLFW_KEY_LEFT) || held(GLFW_KEY_A);
    keys.right = held(GLFW_KEY_RIGHT) || held(GLFW_KEY_D);
    keys.up = held(GLFW_KEY_UP) || held(GLFW_KEY_W);
    keys.down = held(GLFW_KEY_DOWN) || held(GLFW_KEY_S);
    keys.zoom_in = held(GLFW_KEY_PAGE_UP) || held(GLFW_KEY_EQUAL) || held(GLFW_KEY_KP_ADD);
    keys.zoom_out = held(GLFW_KEY_PAGE_DOWN) || held(GLFW_KEY_MINUS) || held(GLFW_KEY_KP_SUBTRACT);
    keys.shift = held(GLFW_KEY_LEFT_SHIFT) || held(GLFW_KEY_RIGHT_SHIFT);
    return keys;
}

} // namespace sample
