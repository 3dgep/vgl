// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Ogre sample (design document, section 11): the globe in a classic Ogre 14 scene, drawn by the
// Ogre backend on Ogre's OpenGL 3+ or Direct3D 11 render system. GLFW owns the window and its
// input, as in the OpenGL sample, whose controls this one has (samples/gl/main.cpp lists them);
// Ogre draws into it. The panel is the samples' shared overlay, drawn by Ogre's Dear ImGui
// overlay.
//
//   vgl-ogre [--data <directory> | --pack <uri>...] [--cache <directory> [--cache-budget <MiB>]]
//            [--rs gl|d3d11] [--depth logarithmic|reversed|conventional] [--check]
//            [--save <directory>]
//
// It draws the sample set (samples/data/README.md) from the directory given with --data, or else
// from the one in the environment variable VGL_SAMPLE_DATA, or else from the source tree's
// samples/data. Instead, --pack, repeated, draws the packs named.
//
// With --check, the sample runs hidden: it draws a few views offscreen with each depth
// convention, a new Ogre root for each, since reversed-Z is the render system's option, and
// checks what it draws and that the conventions agree. It exits with 1 on a failure, so that it
// doubles as a test of the backend. --save writes the images it checks as PPM files.

#define GLFW_INCLUDE_NONE
#include "clouds.hpp"
#include "glfw_controls.hpp"
#include "navigation.hpp"
#include "overlay.hpp"
#include "pack_reader.hpp"
#include "sample_camera.hpp"
#include "sample_check.hpp"
#include "sample_data.hpp"
#include "stars.hpp"

#include <vgl/defaults.hpp>
#include <vgl/ogre.hpp>
#include <vgl/vgl.hpp>

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#    define GLFW_EXPOSE_NATIVE_WIN32
#else
#    define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>
#include <Ogre.h>
#include <OgreImGuiOverlay.h>
#include <OgreOverlayManager.h>
#include <OgreOverlaySystem.h>
#include <OgreRTShaderSystem.h>
#include <OgreSGTechniqueResolverListener.h>
#include <OgreStaticPluginLoader.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef VGL_SAMPLE_DATA_DIR
#    define VGL_SAMPLE_DATA_DIR "."
#endif
#ifndef VGL_OGRE_MEDIA_DIR
#    define VGL_OGRE_MEDIA_DIR "."
#endif

namespace
{

using sample::clear_color;
using sample::depth_name;
using sample::expect;
using sample::field_of_view;
using sample::image;
using sample::places;

struct options
{
    std::string data;               // The sample set's directory, from --data.
    std::vector<std::string> packs; // From --pack.
    std::string cache;              // From --cache.
    std::uint64_t cache_budget = 0; // From --cache-budget, in bytes; 0 for the default.
    std::string stars;              // A star catalogue, from --stars.
    std::string rs = "gl";          // Ogre's render system: gl or d3d11.
    bool check = false;
    std::string save;
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_REVERSED_Z;
};

const char* render_system_name(const std::string& rs)
{
    return rs == "d3d11" ? "Direct3D11 Rendering Subsystem" : "OpenGL 3+ Rendering Subsystem";
}

// ---------------------------------------------------------------------------------------------
// Ogre: a root on a render system, drawing into a GLFW window, with the shader system and the
// overlay.
// ---------------------------------------------------------------------------------------------

struct ogre_app
{
    std::unique_ptr<Ogre::Root> root;
    OgreBites::StaticPluginLoader plugins;
    Ogre::OverlaySystem* overlay_system = nullptr;
    std::unique_ptr<OgreBites::SGTechniqueResolverListener> resolver;
    Ogre::RenderWindow* window = nullptr;
    Ogre::SceneManager* scene = nullptr;
    Ogre::Camera* camera = nullptr;
    Ogre::Viewport* viewport = nullptr;

    ogre_app() = default;
    ogre_app(const ogre_app&) = delete;
    ogre_app& operator=(const ogre_app&) = delete;
    ogre_app(ogre_app&&) = delete;
    ogre_app& operator=(ogre_app&&) = delete;

    ~ogre_app()
    {
        if (!root)
        {
            return;
        }
        if (resolver)
        {
            Ogre::MaterialManager::getSingleton().removeListener(resolver.get());
        }
        if (Ogre::RTShader::ShaderGenerator::getSingletonPtr() != nullptr)
        {
            Ogre::RTShader::ShaderGenerator::destroy();
        }
        delete overlay_system; // NOLINT(cppcoreguidelines-owning-memory): Ogre's convention.
        root.reset();
        plugins.unload();
    }
};

// Starts Ogre on the window: false, with a message, on failure.
bool ogre_start(ogre_app& a, const options& o, vgl_depth_convention depth, GLFWwindow* window)
{
    try
    {
        a.root = std::make_unique<Ogre::Root>("", "", "vgl-ogre.log");
        Ogre::LogManager::getSingleton().getDefaultLog()->setDebugOutputEnabled(false);
        a.plugins.load();
        a.overlay_system = new Ogre::OverlaySystem(); // NOLINT(cppcoreguidelines-owning-memory)
        Ogre::RenderSystem* rs = a.root->getRenderSystemByName(render_system_name(o.rs));
        if (rs == nullptr)
        {
            std::fprintf(stderr, "Ogre has no %s render system here\n", o.rs.c_str());
            return false;
        }
        a.root->setRenderSystem(rs);
        rs->setConfigOption("Reversed Z-Buffer",
                            depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? "Yes" : "No");
        a.root->initialise(false);

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        Ogre::NameValuePairList params;
#if defined(_WIN32)
        params["externalWindowHandle"] =
            std::to_string(reinterpret_cast<std::uintptr_t>(glfwGetWin32Window(window)));
#else
        // A child window of Ogre's own, with the visual its context needs; GLFW keeps the input,
        // which X passes up from the child.
        params["parentWindowHandle"] = std::to_string(glfwGetX11Window(window));
#endif
        a.window = a.root->createRenderWindow(
            "vgl", static_cast<unsigned>(width), static_cast<unsigned>(height), false, &params);

        // Ogre's media: Main for the unified shaders, and the shader system's library, which
        // generates the shaders of Ogre's own materials, such as the overlay's.
        Ogre::ResourceGroupManager& groups = Ogre::ResourceGroupManager::getSingleton();
        const std::string media = VGL_OGRE_MEDIA_DIR;
        groups.addResourceLocation(media + "/Main", "FileSystem", Ogre::RGN_INTERNAL);
        groups.addResourceLocation(media + "/RTShaderLib", "FileSystem", Ogre::RGN_INTERNAL);
        if (!Ogre::RTShader::ShaderGenerator::initialize())
        {
            std::fprintf(stderr, "Ogre's shader system did not start\n");
            return false;
        }
        Ogre::RTShader::ShaderGenerator* generator =
            Ogre::RTShader::ShaderGenerator::getSingletonPtr();
        a.resolver = std::make_unique<OgreBites::SGTechniqueResolverListener>(generator);
        Ogre::MaterialManager::getSingleton().addListener(a.resolver.get());
        groups.initialiseAllResourceGroups();

        a.scene = a.root->createSceneManager();
        generator->addSceneManager(a.scene);
        a.scene->addRenderQueueListener(a.overlay_system);
        a.camera = a.scene->createCamera("camera");
        a.scene->getRootSceneNode()->createChildSceneNode()->attachObject(a.camera);
        a.camera->setFOVy(Ogre::Degree(static_cast<Ogre::Real>(field_of_view)));
        a.viewport = a.window->addViewport(a.camera);
        a.viewport->setBackgroundColour(
            Ogre::ColourValue(clear_color[0], clear_color[1], clear_color[2]));
        return true;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "Ogre failed to start: %s\n", e.what());
        return false;
    }
}

// Ogre's camera's projection, which Ogre multiplies the tiles' model_view with: the same field
// of view and aspect as the context's, and clip distances for the depth convention.
void camera_set(Ogre::Camera* camera, vgl_depth_convention depth, double altitude, double aspect)
{
    camera->setAspectRatio(static_cast<Ogre::Real>(aspect));
    switch (depth)
    {
    case VGL_DEPTH_CONVENTION_REVERSED_Z:
        camera->setNearClipDistance(1.0f);
        camera->setFarClipDistance(0.0f); // Infinite.
        break;
    case VGL_DEPTH_CONVENTION_CONVENTIONAL:
        // A near plane that grows with the altitude, which is all a conventional depth buffer
        // can do at this scale.
        camera->setNearClipDistance(static_cast<Ogre::Real>(std::max(1.0, altitude * 0.05)));
        camera->setFarClipDistance(static_cast<Ogre::Real>(altitude + 2.0e7));
        break;
    default:
        // The logarithmic convention writes its own depth; the planes do not matter.
        camera->setNearClipDistance(1.0f);
        camera->setFarClipDistance(1.0e9f);
        break;
    }
}

struct scene
{
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_REVERSED_Z;
    sample::camera camera;
    sample::camera frozen; // Where tiles are selected for while the selection is frozen.
    sample::debug_settings debug;
};

// Sets the view that the next update selects tiles for: the camera's, or the frozen one's.
// Selection ignores the near and far planes, so any projection with the field of view will do.
void view_set(vgl::context& ctx, const scene& s, int width, int height)
{
    const sample::camera& c = s.debug.freeze ? s.frozen : s.camera;
    auto view = vgl::init<vgl_view>();
    view.view_matrix = sample::camera_view_matrix(c);
    view.projection_matrix = sample::perspective(
        vgl::degrees_to_radians(field_of_view), static_cast<double>(width) / height, 1.0, 1.0e9);
    view.viewport_width = static_cast<std::uint32_t>(width);
    view.viewport_height = static_cast<std::uint32_t>(height);
    view.max_screen_space_error = s.debug.max_error;
    static_cast<void>(ctx.view_set(&view));
}

// The globe's settings for drawing from the live camera, which differs from the view the tiles
// were selected for while the selection is frozen.
vgl::ogre::globe_settings settings_for(const scene& s)
{
    vgl::ogre::globe_settings g;
    g.has_view_matrix = true;
    g.view_matrix = sample::camera_view_matrix(s.camera);
    if (s.debug.sunlight)
    {
        g.sun_direction = sample::sun_direction(s.debug);
    }
    g.debug_views = sample::debug_views(s.debug);
    g.features = sample::draw_features(s.debug);
    g.contour_interval = s.debug.contour_interval;
    g.earth_rotation_angle =
        sample::earth_rotation_angle(s.debug.time_of_day, s.debug.day_of_year, s.debug.year);
    return g;
}

vgl::result
globe_create(vgl::context& ctx, ogre_app& a, vgl_depth_convention depth, vgl::ogre::globe*& globe)
{
    vgl::ogre::globe_create_info info;
    info.depth_convention = depth;
    info.resource_group = Ogre::RGN_INTERNAL;
    vgl::expected<vgl::ogre::globe*> made = vgl::ogre::globe::create(ctx, info);
    if (!made)
    {
        globe = nullptr;
        return made.error();
    }
    globe = *made;
    a.scene->getRootSceneNode()->attachObject(globe);
    const std::vector<vgl_star>& stars = sample::sky_stars();
    expect(globe->stars_set(stars) == vgl::result::success, "the stars are set");
    const sample::cloud_image& clouds = sample::sky_clouds();
    expect(clouds.width == 0 ||
               globe->clouds_set(clouds.coverage.data(), clouds.width, clouds.height) ==
                   vgl::result::success,
           "the clouds are set");
    return vgl::result::success;
}

void globe_destroy(ogre_app& a, vgl::ogre::globe* globe)
{
    if (globe != nullptr)
    {
        a.scene->getRootSceneNode()->detachObject(globe);
        vgl::ogre::globe::destroy(globe);
    }
}

GLFWwindow* window_create(int width, int height, bool visible)
{
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Ogre makes the context.
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    return glfwCreateWindow(width, height, "vgl", nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------------------------

image read_image(Ogre::RenderTarget* target)
{
    image i;
    i.width = static_cast<int>(target->getWidth());
    i.height = static_cast<int>(target->getHeight());
    i.pixels.resize(static_cast<std::size_t>(i.width) * static_cast<std::size_t>(i.height) * 4);
    const Ogre::PixelBox box(
        target->getWidth(), target->getHeight(), 1, Ogre::PF_BYTE_RGBA, i.pixels.data());
    target->copyContentsToMemory(box, box);
    return i;
}

int run_check(const options& o, vgl::context& ctx)
{
    constexpr int width = 640;
    constexpr int height = 360;
    const std::vector<vgl_depth_convention> conventions{VGL_DEPTH_CONVENTION_REVERSED_Z,
                                                        VGL_DEPTH_CONVENTION_LOGARITHMIC,
                                                        VGL_DEPTH_CONVENTION_CONVENTIONAL};
    std::vector<std::vector<image>> images(conventions.size());
    std::vector<std::vector<image>> atmosphere_images(conventions.size());
    for (std::size_t c = 0; c < conventions.size(); ++c)
    {
        GLFWwindow* window = window_create(width, height, false);
        expect(window != nullptr, "the window is created");
        if (window == nullptr)
        {
            continue;
        }
        {
            ogre_app a;
            expect(ogre_start(a, o, conventions[c], window), "Ogre starts");
            if (!a.root)
            {
                glfwDestroyWindow(window);
                continue;
            }
            // Offscreen, for reading back.
            Ogre::TexturePtr texture =
                Ogre::TextureManager::getSingleton().createManual("vgl/check",
                                                                  Ogre::RGN_INTERNAL,
                                                                  Ogre::TEX_TYPE_2D,
                                                                  width,
                                                                  height,
                                                                  0,
                                                                  Ogre::PF_BYTE_RGBA,
                                                                  Ogre::TU_RENDERTARGET);
            Ogre::RenderTarget* target = texture->getBuffer()->getRenderTarget();
            Ogre::Viewport* viewport = target->addViewport(a.camera);
            viewport->setBackgroundColour(
                Ogre::ColourValue(clear_color[0], clear_color[1], clear_color[2]));
            viewport->setOverlaysEnabled(false);
            a.window->removeAllViewports();

            scene s;
            s.depth = conventions[c];
            vgl::ogre::globe* globe = nullptr;
            const vgl::result created = globe_create(ctx, a, s.depth, globe);
            expect(created == vgl::result::success,
                   std::string("the globe is created for ") + depth_name(s.depth));
            for (const sample::place& p : places)
            {
                if (created != vgl::result::success)
                {
                    break;
                }
                s.camera = p.camera;
                int frames = 0;
                int quiet = 0;
                while (frames < 3000 && quiet < 5)
                {
                    view_set(ctx, s, width, height);
                    expect(ctx.update() == vgl::result::success, "vgl_context_update succeeds");
                    camera_set(
                        a.camera, s.depth, s.camera.altitude, static_cast<double>(width) / height);
                    expect(globe->settings_set(settings_for(s)) == vgl::result::success,
                           "the settings are valid");
                    a.root->renderOneFrame();
                    quiet = sample::idle(ctx.statistics_get()) ? quiet + 1 : 0;
                    ++frames;
                    if (quiet == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                image pixels = read_image(target);
                sample::check_place(s.depth, p, frames, quiet >= 5, ctx.statistics_get(), pixels);
                if (!o.save.empty())
                {
                    sample::save_image(pixels, sample::image_path(o.save, s.depth, p.name));
                }
                images[c].push_back(std::move(pixels));

                // With the atmosphere, the same with every convention.
                scene air = s;
                air.debug.atmosphere = true;
                air.debug.stars = true;
                air.debug.clouds = true;
                expect(globe->settings_set(settings_for(air)) == vgl::result::success,
                       "the settings are valid");
                a.root->renderOneFrame();
                image sky = read_image(target);
                if (!o.save.empty())
                {
                    sample::save_image(
                        sky,
                        sample::image_path(
                            o.save, s.depth, (std::string(p.name) + "-atmosphere").c_str()));
                }
                atmosphere_images[c].push_back(std::move(sky));
            }
            if (globe != nullptr)
            {
                // The debug views draw.
                s.debug.wireframe = s.debug.level_colors = s.debug.imagery_source = true;
                s.debug.height_colors = s.debug.contours = s.debug.sea = true;
                expect(globe->settings_set(settings_for(s)) == vgl::result::success,
                       "the settings are valid");
                a.root->renderOneFrame();
                if (!o.save.empty())
                {
                    sample::save_image(read_image(target),
                                       sample::image_path(o.save, s.depth, "debug"));
                }
            }
            globe_destroy(a, globe);
            target->removeAllViewports();
            Ogre::TextureManager::getSingleton().remove(texture);
        }
        glfwDestroyWindow(window);
    }
    sample::compare_conventions(conventions, images);
    sample::compare_conventions(conventions, atmosphere_images);
    std::printf("\n%s\n", sample::failures() == 0 ? "all checks passed" : "checks failed");
    return sample::failures() == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// Interactive
// ---------------------------------------------------------------------------------------------

// The globe is drawn into a render texture the size of the window, which the window then shows:
// Ogre gives render textures a 32-bit floating-point depth buffer for the reversed-Z convention,
// but its windows keep a 24-bit integer one, too coarse at planetary scale. The OpenGL sample
// draws into a framebuffer of its own for the same reason.
struct presenter
{
    Ogre::SceneManager* scene = nullptr; // Holds the rectangle that shows the texture.
    Ogre::Camera* camera = nullptr;
    Ogre::Rectangle2D* quad = nullptr;
    Ogre::MaterialPtr material;
    Ogre::TexturePtr texture;
    int textures = 0; // Created so far, for unique names.
};

void presenter_start(ogre_app& a, presenter& p)
{
    p.scene = a.root->createSceneManager();
    Ogre::RTShader::ShaderGenerator::getSingleton().addSceneManager(p.scene);
    p.scene->addRenderQueueListener(a.overlay_system); // The panel draws in the window.
    p.camera = p.scene->createCamera("present");
    p.scene->getRootSceneNode()->createChildSceneNode()->attachObject(p.camera);
    p.material = Ogre::MaterialManager::getSingleton().create("vgl/present", Ogre::RGN_INTERNAL);
    Ogre::Pass* pass = p.material->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setDepthCheckEnabled(false);
    pass->setDepthWriteEnabled(false);
    pass->setCullingMode(Ogre::CULL_NONE);
    pass->createTextureUnitState()->setTextureFiltering(Ogre::TFO_NONE);
    p.quad = p.scene->createScreenSpaceRect(true);
    p.quad->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);
    p.quad->setBoundingBox(Ogre::AxisAlignedBox::BOX_INFINITE);
    p.quad->setMaterial(p.material);
    p.scene->getRootSceneNode()->attachObject(p.quad);
    a.window->removeAllViewports();
    Ogre::Viewport* viewport = a.window->addViewport(p.camera);
    viewport->setBackgroundColour(
        Ogre::ColourValue(clear_color[0], clear_color[1], clear_color[2]));
}

// A render texture of the window's size for the globe's camera, replacing the last.
void presenter_resize(ogre_app& a, presenter& p, int width, int height)
{
    if (p.texture)
    {
        p.texture->getBuffer()->getRenderTarget()->removeAllViewports();
        Ogre::TextureManager::getSingleton().remove(p.texture);
    }
    p.texture = Ogre::TextureManager::getSingleton().createManual("vgl/scene" +
                                                                      std::to_string(p.textures++),
                                                                  Ogre::RGN_INTERNAL,
                                                                  Ogre::TEX_TYPE_2D,
                                                                  static_cast<unsigned>(width),
                                                                  static_cast<unsigned>(height),
                                                                  0,
                                                                  Ogre::PF_BYTE_RGBA,
                                                                  Ogre::TU_RENDERTARGET);
    Ogre::RenderTarget* target = p.texture->getBuffer()->getRenderTarget();
    Ogre::Viewport* viewport = target->addViewport(a.camera);
    viewport->setBackgroundColour(
        Ogre::ColourValue(clear_color[0], clear_color[1], clear_color[2]));
    viewport->setOverlaysEnabled(false);
    // Ogre renders into OpenGL's textures upside down itself, so that they read the right way up.
    // Every technique shows it: the shader system adds one of its own, with a copy of the
    // texture unit, which would otherwise keep showing the last texture.
    for (Ogre::Technique* technique : p.material->getTechniques())
    {
        for (Ogre::Pass* pass : technique->getPasses())
        {
            if (pass->getNumTextureUnitStates() > 0)
            {
                pass->getTextureUnitState(0)->setTexture(p.texture);
            }
        }
    }
}

int run_interactive(const options& o,
                    vgl::context& ctx,
                    const std::vector<sample::layer_control>& layers,
                    const sample::pack_reader& reader)
{
    GLFWwindow* window = window_create(1280, 720, true);
    if (window == nullptr)
    {
        return 1;
    }
    int code = 0;
    {
        ogre_app a;
        if (!ogre_start(a, o, o.depth, window))
        {
            glfwDestroyWindow(window);
            return 1;
        }
        scene s;
        s.debug.atmosphere = true;
        s.debug.stars = true;
        s.debug.clouds = true;
        s.depth = o.depth;
        s.camera = places[0].camera;
        vgl::ogre::globe* globe = nullptr;
        const vgl::result created = globe_create(ctx, a, s.depth, globe);
        if (created != vgl::result::success)
        {
            std::fprintf(stderr, "could not create the globe: %s\n", vgl::result_name_get(created));
            glfwDestroyWindow(window);
            return 1;
        }

        presenter present;
        presenter_start(a, present);

        // Ogre's Dear ImGui overlay draws the panel; GLFW gives it its input.
        auto* imgui =
            new Ogre::ImGuiOverlay(); // NOLINT(cppcoreguidelines-owning-memory): Ogre owns it.
        imgui->setZOrder(300);
        imgui->show();
        Ogre::OverlayManager::getSingleton().addOverlay(imgui);
        sample::overlay ui;
        sample::globe_navigation nav;
        nav.terrain(ctx.get()); // Grab the terrain under the cursor.
        sample::glfw_controls controls;
        controls.camera = &s.camera;
        controls.debug = &s.debug;
        controls.ui = &ui;
        controls.nav = &nav;
        sample::controls_install(window, &controls);
        sample::overlay::backend backend;
        backend.external_frames = true;
        backend.user_data = window;
        backend.init = [](void* w) {
            return ImGui_ImplGlfw_InitForOther(static_cast<GLFWwindow*>(w), true);
        };
        backend.new_frame = [](void*) {
            ImGui_ImplGlfw_NewFrame();
            Ogre::ImGuiOverlay::NewFrame();
        };
        backend.render = [](void*) {}; // Ogre draws the overlay with the frame.
        backend.shutdown = [](void*) { ImGui_ImplGlfw_Shutdown(); };
        if (!ui.init(backend))
        {
            std::fprintf(stderr, "could not set up the overlay\n");
            code = 1;
        }

        sample::panel_info panel;
        panel.layers = layers;
        const std::string api =
            "Ogre " OGRE_VERSION_NAME ", " + a.root->getRenderSystem()->getName();
        panel.api = api.c_str();
        panel.device = "";
        panel.depth = depth_name(s.depth);
        bool was_frozen = false;
        auto last_frame = std::chrono::steady_clock::now();
        auto last_navigated = last_frame;
        auto last_title = last_frame;
        int frames = 0;
        int last_width = 0;
        int last_height = 0;
        while (code == 0 && glfwWindowShouldClose(window) == 0)
        {
            glfwPollEvents();
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            if (width == 0 || height == 0)
            {
                glfwWaitEvents(); // Minimized.
                continue;
            }
            if (width != last_width || height != last_height)
            {
                a.window->resize(static_cast<unsigned>(width), static_cast<unsigned>(height));
                a.window->windowMovedOrResized();
                presenter_resize(a, present, width, height);
                last_width = width;
                last_height = height;
            }

            // Navigation: throws, smooth zooms and keys, over the terrain under the camera.
            const vgl::expected<vgl::height_sample> ground =
                ctx.height_query(vgl::degrees_to_radians(s.camera.latitude),
                                 vgl::degrees_to_radians(s.camera.longitude));
            if (ground && ground->status == vgl::result::success)
            {
                nav.ground(ground->height);
                s.camera.altitude = std::max(s.camera.altitude, ground->height + 20.0);
            }
            int window_width = 0;
            int window_height = 0;
            glfwGetWindowSize(window, &window_width, &window_height);
            nav.viewport(window_width, window_height, field_of_view);
            const sample::navigation_keys keys = sample::controls_keys(window, controls);
            const auto navigated = std::chrono::steady_clock::now();
            nav.update(
                std::chrono::duration<double>(navigated - last_navigated).count(), keys, s.camera);
            last_navigated = navigated;
            if (s.debug.contour_auto)
            {
                s.debug.contour_interval = sample::contour_interval_for(s.camera.altitude);
            }
            if (s.debug.freeze && !was_frozen)
            {
                s.frozen = s.camera;
            }
            was_frozen = s.debug.freeze;

            view_set(ctx, s, width, height);
            static_cast<void>(ctx.update());
            const double aspect = static_cast<double>(width) / height;
            camera_set(a.camera, s.depth, s.camera.altitude, aspect);
            expect(globe->settings_set(settings_for(s)) == vgl::result::success,
                   "the settings are valid");

            const auto frame_end = std::chrono::steady_clock::now();
            const double frame_seconds =
                std::chrono::duration<double>(frame_end - last_frame).count();
            last_frame = frame_end;
            panel.live = s.camera;
            panel.frozen = s.frozen;
            panel.focus_strength = 0.0;
            double focus_strength = 0.0;
            if (nav.focus(s.camera, panel.focus_x, panel.focus_y, focus_strength))
            {
                panel.focus_strength = focus_strength;
            }
            panel.credits = ctx.attribution_get();
            panel.cache = reader.cache_info();
            const double reach =
                std::sqrt(s.frozen.altitude * (2.0 * 6371000.0 + s.frozen.altitude));
            ui.frame(
                ctx.statistics_get(),
                panel,
                frame_seconds,
                sample::camera_view_matrix(s.camera),
                sample::perspective(vgl::degrees_to_radians(field_of_view), aspect, 1.0, 1.0e9),
                sample::frustum_of(s.frozen, field_of_view, aspect, reach),
                s.debug);
            a.root->renderOneFrame();

            ++frames;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - last_title).count();
            if (seconds >= 0.5)
            {
                const vgl_context_statistics st = ctx.statistics_get();
                std::array<char, 256> title{};
                std::snprintf(
                    title.data(),
                    title.size(),
                    "vgl - Ogre %s - %s depth - %.0f fps - %u tiles - %.0f MiB GPU - %.0f km",
                    o.rs.c_str(),
                    depth_name(s.depth),
                    frames / seconds,
                    st.tiles_drawn,
                    static_cast<double>(st.gpu_used) / (1024.0 * 1024.0),
                    s.camera.altitude / 1000.0);
                glfwSetWindowTitle(window, title.data());
                frames = 0;
                last_title = now;
            }
        }
        std::printf("credits:\n");
        for (const char* line : ctx.attribution_get())
        {
            std::printf("  %s\n", line);
        }
        globe_destroy(a, globe);
    }
    glfwDestroyWindow(window);
    return code;
}

bool parse(int argc, char** argv, options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (std::strcmp(a, "--check") == 0)
        {
            o.check = true;
        }
        else if (std::strcmp(a, "--rs") == 0 && i + 1 < argc)
        {
            o.rs = argv[++i];
            if (o.rs != "gl" && o.rs != "d3d11")
            {
                return false;
            }
        }
        else if (std::strcmp(a, "--data") == 0 && i + 1 < argc)
        {
            o.data = argv[++i];
        }
        else if (std::strcmp(a, "--stars") == 0 && i + 1 < argc)
        {
            o.stars = argv[++i];
        }
        else if (std::strcmp(a, "--pack") == 0 && i + 1 < argc)
        {
            o.packs.emplace_back(argv[++i]);
        }
        else if (std::strcmp(a, "--cache") == 0 && i + 1 < argc)
        {
            o.cache = argv[++i];
        }
        else if (std::strcmp(a, "--cache-budget") == 0 && i + 1 < argc)
        {
            if (!sample::parse_cache_budget(argv[++i], o.cache_budget))
            {
                return false;
            }
        }
        else if (std::strcmp(a, "--save") == 0 && i + 1 < argc)
        {
            o.save = argv[++i];
        }
        else if (std::strcmp(a, "--depth") == 0 && i + 1 < argc)
        {
            const char* d = argv[++i];
            if (std::strcmp(d, "logarithmic") == 0)
            {
                o.depth = VGL_DEPTH_CONVENTION_LOGARITHMIC;
            }
            else if (std::strcmp(d, "reversed") == 0)
            {
                o.depth = VGL_DEPTH_CONVENTION_REVERSED_Z;
            }
            else if (std::strcmp(d, "conventional") == 0)
            {
                o.depth = VGL_DEPTH_CONVENTION_CONVENTIONAL;
            }
            else
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    return o.packs.empty() || (o.data.empty() && !o.check);
}

} // namespace

int main(int argc, char** argv)
{
    options o;
    if (!parse(argc, argv, o))
    {
        std::fprintf(stderr,
                     "usage: vgl-ogre [--data <directory> | --pack <uri>...] [--cache <directory> "
                     "[--cache-budget <MiB>]] "
                     "[--rs gl|d3d11] "
                     "[--depth logarithmic|reversed|conventional] [--check] [--save <directory>] "
                     "[--stars <catalog>]\n"
                     "--cache keeps what comes from web servers in a directory, for next time "
                     "and offline, within --cache-budget mebibytes (1024 by default). "
                     "--pack, repeated, replaces the sample set, and cannot be combined with "
                     "--data or --check. Without either, the sample set is read from "
                     "$%s, or else from %s. --stars draws the stars of the Yale Bright Star "
                     "Catalogue's file \"catalog\" (CDS V/50, decompressed) instead of "
                     "generated ones.\n",
                     sample::data_variable,
                     VGL_SAMPLE_DATA_DIR);
        return 2;
    }
    if (!o.stars.empty())
    {
        std::string error;
        if (!sample::stars_load(o.stars, error))
        {
            std::fprintf(stderr, "--stars: %s\n", error.c_str());
            return 1;
        }
    }
    {
        // The clouds of the sample set, if it has them.
        std::string error;
        if (!sample::clouds_load(sample::data_directory(o.data, VGL_SAMPLE_DATA_DIR), error))
        {
            std::fprintf(stderr, "no clouds: %s\n", error.c_str());
        }
    }
#if !defined(_WIN32)
    // Ogre's OpenGL draws into X11 windows.
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    glfwSetErrorCallback(
        [](int, const char* message) { std::fprintf(stderr, "glfw: %s\n", message); });
    if (glfwInit() == 0)
    {
        return 1;
    }

    vgl::expected<vgl::thread_pool> pool = vgl::thread_pool::create();
    vgl::expected<sample::pack_reader> reader =
        pool ? sample::pack_reader::create(*pool, o.cache, o.cache_budget)
             : vgl::expected<sample::pack_reader>(vgl::unexpected(pool.error()));
    if (!pool || !reader)
    {
        std::fprintf(stderr, "could not create the thread pool or the file reader\n");
        return 1;
    }
    const vgl_task_interface tasks = pool->task_interface_get();
    const vgl_io_interface io = reader->io_interface_get();
    auto info = vgl::init<vgl_context_create_info>();
    info.tasks = &tasks;
    info.io = &io;
    info.log_level = VGL_LOG_LEVEL_INFO;
    info.log = [](void*, vgl_log_level, const char* message) {
        std::fprintf(stderr, "vgl: %s\n", message);
    };
    vgl::expected<vgl::context> ctx = vgl::context::create(&info);
    if (!ctx)
    {
        std::fprintf(
            stderr, "could not create the context: %s\n", vgl::result_name_get(ctx.error()));
        return 1;
    }
    const std::vector<sample::pack_source> sources =
        o.packs.empty() ? sample::sample_packs(sample::data_directory(o.data, VGL_SAMPLE_DATA_DIR))
                        : sample::named_packs(o.packs);
    reader->warn_unreadable(sources);
    std::vector<vgl::pack> packs;
    for (const sample::pack_source& s : sources)
    {
        std::printf("pack %s\n", s.uri.c_str());
        vgl::expected<vgl::pack> p = vgl::pack::create(*ctx, s.uri.c_str(), s.priority);
        if (!p)
        {
            std::fprintf(stderr,
                         "could not add the pack %s: %s\n",
                         s.uri.c_str(),
                         vgl::result_name_get(p.error()));
            return 1;
        }
        packs.push_back(std::move(*p));
    }
    std::printf("\n");

    const int code =
        o.check ? run_check(o, *ctx)
                : run_interactive(o, *ctx, sample::layer_controls(sources, packs), *reader);

    packs.clear();
    ctx->reset();
    reader->reset();
    pool->reset();
    glfwTerminate();
    return code;
}
