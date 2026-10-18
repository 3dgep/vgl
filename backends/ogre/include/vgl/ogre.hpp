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
//     vgl::expected<vgl::ogre::globe*> globe = vgl::ogre::globe::create(context, info);
//     scene_manager->getRootSceneNode()->attachObject(*globe);
//
//     // Each frame, after vgl_context_update, before Ogre renders:
//     (*globe)->settings_set(settings);  // Optional: the camera in double precision, features.
//     root->renderOneFrame();
//
//     scene_manager->getRootSceneNode()->detachObject(*globe);
//     vgl::ogre::globe::destroy(*globe);  // Before the context and Ogre's render system.
//
// The globe's functions are called on Ogre's render thread, as Ogre's own are, and one at a
// time with the calls on its context (docs/c-api.md, section 11).
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
#include <vgl/context.hpp>
#include <vgl/render.h>

#include <Ogre.h>

#include <cstdint>
#include <span>

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
    // Bytes of resources to upload per frame. 0 selects VGL_UPLOAD_BUDGET_DEFAULT.
    std::uint64_t upload_budget = 0;
    // What to draw besides the terrain and its imagery: vgl_draw_feature values (vgl/render.h).
    // The atmosphere draws in the render queue group RENDER_QUEUE_SKIES_LATE, and the stars in
    // the group before it.
    vgl_draw_features features = 0;
    // Views for inspecting level of detail and loading: vgl_debug_view values (vgl/render.h).
    vgl_debug_views debug_views = 0;
    // The Earth rotation angle in radians (vgl_earth_rotation_angle_get), for the stars.
    double earth_rotation_angle = 0.0;
    // Height between contour lines, in metres. 0 selects VGL_CONTOUR_INTERVAL_DEFAULT.
    double contour_interval = 0.0;
    // The cloud shell's height above the ellipsoid, in metres. 0 selects
    // VGL_CLOUD_HEIGHT_DEFAULT.
    double cloud_height = 0.0;
};

class globe final : public Ogre::MovableObject
{
public:
    // Creates a globe for a context on Ogre's current render system: creates its programs and
    // material, and tells the context which texture formats it samples. The context must outlive
    // the globe. Fails with invalid_argument for an unknown depth convention, not_supported if
    // the render system lacks a shading language the globe has, or the reversed-Z convention
    // without the render system's option, out_of_memory, and backend_error if Ogre fails; the
    // context's log says which.
    [[nodiscard]] static vgl::expected<globe*> create(vgl::context& context,
                                                      const globe_create_info& info) noexcept;
    [[nodiscard]] static vgl::expected<globe*> create(vgl_context* context,
                                                      const globe_create_info& info) noexcept;

    // Detach it from its scene node first. Null does nothing.
    static void destroy(globe* g) noexcept;

    // Sets how the globe draws from the next frame on. Fails with invalid_argument, keeping the
    // settings before, for a matrix or direction that is not finite, unknown feature or debug
    // view bits, or a distance, interval, height or angle that is not finite or, but for the
    // angle, negative; the context's log says which.
    [[nodiscard]] vgl::result settings_set(const globe_settings& settings) noexcept;

    // Sets the stars the globe draws with VGL_DRAW_FEATURE_STARS, replacing those set before, as
    // vgl_gl_backend_stars_set does: copied, drawn as small quads. vgl ships no stars. Fails with
    // invalid_argument for an invalid star or more than VGL_STAR_COUNT_MAX, and out_of_memory.
    [[nodiscard]] vgl::result stars_set(std::span<const vgl_star> stars) noexcept;

    // Sets the clouds the globe draws with VGL_DRAW_FEATURE_CLOUDS, as
    // vgl_gl_backend_clouds_set does: a coverage image of the whole globe, copied with its mip
    // levels; null with width and height 0 removes them. Fails with invalid_argument for a size
    // outside the limits of vgl/render.h (VGL_CLOUD_IMAGE_*), and out_of_memory.
    [[nodiscard]] vgl::result
    clouds_set(const std::uint8_t* coverage, std::uint32_t width, std::uint32_t height) noexcept;

    // Forgets the globe's Ogre objects and the context's render resources, which are uploaded
    // again over the next frames: after Ogre's render system lost its device, and the like, as
    // the C backends' device_reset does.
    void device_reset() noexcept;

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
