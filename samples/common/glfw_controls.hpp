// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The controls of the samples that run in a GLFW window (samples/gl/main.cpp lists them): the
// mouse and keys turned into the shared navigation's calls, the debug views' keys, the places on
// 1 to 7, and the keys held for moving, polled each frame.

#ifndef VGL_SAMPLES_GLFW_CONTROLS_HPP
#define VGL_SAMPLES_GLFW_CONTROLS_HPP

#include "navigation.hpp"
#include "overlay.hpp"
#include "sample_camera.hpp"

struct GLFWwindow;

namespace sample
{

struct glfw_controls
{
    sample::camera* camera = nullptr;
    debug_settings* debug = nullptr;
    overlay* ui = nullptr;
    globe_navigation* nav = nullptr;
    double last_click = -1.0; // When the left button was last pressed, for double-clicks.
    double click_x = 0.0;
    double click_y = 0.0;
};

// Points the window's user pointer at the controls and installs its callbacks. Install them
// before Dear ImGui's GLFW backend, which chains to them.
void controls_install(GLFWwindow* window, glfw_controls* controls);

// The movement keys held now, unless the overlay has the keyboard.
navigation_keys controls_keys(GLFWwindow* window, const glfw_controls& controls);

} // namespace sample

#endif // VGL_SAMPLES_GLFW_CONTROLS_HPP
