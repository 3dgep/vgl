// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// vgl Ogre backend: draws a context's globe in a classic Ogre 14 scene (design document,
// section 7.6). It is a consumer of the render interface (vgl/render.h), linked as vgl::ogre, a
// static library that uses vgl only through the C API. Ogre's interface is C++, so this one is
// too; no exception crosses it.
//
// The globe is an Ogre MovableObject. The host attaches it to a scene node at the origin, which
// is ECEF's, and keeps that node unrotated and unscaled:
//
//     vgl::ogre::globe_create_info info;
//     info.depth_convention = VGL_DEPTH_CONVENTION_REVERSED_Z;
//     vgl::ogre::globe* globe = nullptr;
//     vgl::ogre::globe::create(context, info, globe);
//     scene_manager->getRootSceneNode()->attachObject(globe);
//
//     // Each frame, after vgl_context_update, before Ogre renders:
//     globe->settings_set(settings);  // Optional: the camera in double precision, debug views.
//     root->renderOneFrame();
//
//     scene_manager->getRootSceneNode()->detachObject(globe);
//     vgl::ogre::globe::destroy(globe);  // Before the context and Ogre's render system.
//
// Ogre draws the tiles with its camera's projection only: each tile's model_view, composed by
// the core in double precision from the context's view or from the view in the settings,
// replaces Ogre's view, whose single precision is too coarse at the Earth's radius. The host
// therefore keeps Ogre's camera's field of view, aspect and clip distances matching the
// projection it gives the context. For the reversed-Z convention, the render system's "Reversed
// Z-Buffer" option must be on, and Ogre's far clip distance is best 0 (infinite). Ogre then gives
// render textures a 32-bit floating-point depth buffer, but its render windows keep a 24-bit
// integer one, which is too coarse at planetary scale: draw the globe into a render texture and
// show that, as the Ogre sample does, or use the logarithmic convention.
//
// The globe needs Ogre's Media/Main among the resource locations of its resource group, for
// OgreUnifiedShader.h, as every Ogre application with the shader system has.

#ifndef VGL_OGRE_HPP
#define VGL_OGRE_HPP

#include <vgl/base.hpp>
#include <vgl/context.h>
#include <vgl/render.h>

#include <Ogre.h>

#include <cstdint>

namespace vgl::ogre
{

// Creation parameters of a globe.
struct globe_create_info
{
    // How depth is written and compared (vgl/render.h).
    vgl_depth_convention depth_convention = VGL_DEPTH_CONVENTION_LOGARITHMIC;
    // The resource group of the globe's programs, materials, buffers and textures. It must see
    // Ogre's Media/Main.
    Ogre::String resource_group = Ogre::RGN_DEFAULT;
    // Ogre's name of the globe, unique among its movable objects.
    Ogre::String name = "vgl/globe";
};

// Debug views of globe_settings.debug_flags, the same as the other backends'
// (vgl_gl_debug_flag in vgl/gl.h describes them).
enum debug_flag : std::uint32_t
{
    debug_wireframe = 0x1,
    debug_level_colors = 0x2,
    debug_imagery_source = 0x4,
    debug_height_colors = 0x8,
    debug_contours = 0x10,
};

// How the globe draws, from the next frame on.
struct globe_settings
{
    // The camera to draw from, in double precision, when it differs from the context's view
    // (vgl_frame_begin_info.view_matrix). has_view_matrix false uses the context's view.
    bool has_view_matrix = false;
    vgl_dmat4 view_matrix{};
    // Direction towards the sun, along the ECEF axes; it need not be normalized. Zero lights the
    // terrain from the camera.
    vgl_dvec3 sun_direction{};
    // The logarithmic convention's far distance, in metres. 0 selects
    // VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT.
    double logarithmic_far = 0.0;
    // Bytes of resources to upload per frame. 0 selects 16 MiB.
    std::uint64_t upload_budget = 0;
    // A combination of debug_flag values.
    std::uint32_t debug_flags = 0;
    // Height between contour lines, in metres. 0 selects 500.
    double contour_interval = 0.0;
    // Terrain below mean sea level raised to it and shaded as water (vgl_gl_draw_info).
    bool sea_surface = false;
};

class globe final : public Ogre::MovableObject
{
public:
    // Creates a globe for a context on Ogre's current render system: creates its programs and
    // material, and tells the context which texture formats it samples. The context must outlive
    // the globe. Fails with VGL_RESULT_NOT_SUPPORTED if the render system lacks a shading
    // language the globe has, or the reversed-Z convention without the render system's option,
    // and with VGL_RESULT_BACKEND_ERROR if Ogre fails, as the log says.
    [[nodiscard]] static vgl::result
    create(vgl_context* context, const globe_create_info& info, globe*& out) noexcept;

    // Detach it from its scene node first. Null does nothing.
    static void destroy(globe* g) noexcept;

    void settings_set(const globe_settings& settings) noexcept;

    // Forgets the globe's Ogre objects and the context's render resources, which are uploaded
    // again over the next frames: after Ogre's render system lost its device, and the like.
    void resources_reset() noexcept;

    // Ogre::MovableObject
    [[nodiscard]] const Ogre::String& getMovableType() const override;
    [[nodiscard]] const Ogre::AxisAlignedBox& getBoundingBox() const override;
    [[nodiscard]] Ogre::Real getBoundingRadius() const override;
    void _updateRenderQueue(Ogre::RenderQueue* queue) override;
    void visitRenderables(Ogre::Renderable::Visitor* visitor, bool debug_renderables) override;

    globe(const globe&) = delete;
    globe& operator=(const globe&) = delete;
    globe(globe&&) = delete;
    globe& operator=(globe&&) = delete;

    struct state;

private:
    explicit globe(const Ogre::String& name, state* s) noexcept;
    ~globe() override;

    state* state_;
};

} // namespace vgl::ogre

#endif // VGL_OGRE_HPP
