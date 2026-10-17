// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#define GLFW_INCLUDE_NONE
#include "glfw_controls.hpp"

#include "sample_check.hpp"

#include <GLFW/glfw3.h>

#include <cmath>

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
        in.nav->stop();
        in.camera->heading = 0.0; // North up.
        if (key == GLFW_KEY_R)
        {
            in.camera->pitch = 90.0; // And looking straight down.
        }
        break;
    case GLFW_KEY_U:
        in.nav->stop();
        in.camera->pitch = 90.0;
        break;
    default:
        if (key >= GLFW_KEY_1 && key < GLFW_KEY_1 + static_cast<int>(places.size()))
        {
            in.nav->stop();
            *in.camera = places[static_cast<std::size_t>(key - GLFW_KEY_1)].camera;
        }
        break;
    }
}

} // namespace

void controls_install(GLFWwindow* window, glfw_controls* controls)
{
    glfwSetWindowUserPointer(window, controls);
    glfwSetMouseButtonCallback(window, on_mouse_button);
    glfwSetCursorPosCallback(window, on_cursor);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetKeyCallback(window, on_key);
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
