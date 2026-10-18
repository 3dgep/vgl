// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The Ogre backend (vgl/ogre.hpp): a consumer of the render interface (docs/render-interface.md)
// that turns each frame's resource lists into Ogre buffers, textures and materials, and its draw
// list into renderables in Ogre's render queue (design document, section 7.6).

#include "atmosphere_shader.hpp"
#include "backends/common/atmosphere_view.hpp"
#include "backends/common/stars.hpp"
#include "common/host_memory.hpp"
#include "common/pointer_map.hpp"
#include "shaders.hpp"

#include <vgl/ogre.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <string>

using vgl::common::memory;

namespace
{

constexpr std::uint32_t max_layers = 4;
constexpr std::size_t message_size = 1024;

// The custom parameters of a tile's renderable, as the programs read them (shaders.hpp).
enum custom : std::uint8_t
{
    custom_geoid = 0,
    custom_centre = 1,
    custom_item = 2,
    custom_samples = 3,
    custom_layer_uv = 4, // To 7.
    custom_layer_opacity = 8,
    custom_light = 9,
    custom_frame = 10,
    custom_to_planet = 11, // To 13.
    custom_camera = 14,
    custom_sun_planet = 15,
    custom_sky = 16,
    custom_celestial = 17, // To 19.
};

enum class resource_kind : std::uint8_t
{
    index_list,
    mesh,
    texture,
};

// The Ogre objects of one render resource.
struct ogre_resource
{
    resource_kind kind = resource_kind::index_list;
    // Index lists.
    Ogre::IndexData* index_data = nullptr;
    // Meshes: the vertices, the samples' texture and the mesh's own material.
    Ogre::VertexData* vertex_data = nullptr;
    Ogre::MaterialPtr material;
    std::uint32_t grid = 0;
    std::uint32_t sample_grid = 0;
    float min_height = 0.0f;
    std::array<Ogre::TexturePtr, max_layers> bound_layers; // The material's layers now.
    // Textures, and meshes' samples.
    Ogre::TexturePtr texture;
};

// A tile in Ogre's render queue: its mesh, its material, and the draw item's model_view as its
// world transform with the view set to identity, so that Ogre multiplies only the projection.
class tile_renderable final : public Ogre::Renderable
{
public:
    tile_renderable() { setUseIdentityView(true); }

    Ogre::RenderOperation operation;
    Ogre::MaterialPtr material;
    Ogre::Matrix4 world = Ogre::Matrix4::IDENTITY;

    [[nodiscard]] const Ogre::MaterialPtr& getMaterial() const override { return material; }
    void getRenderOperation(Ogre::RenderOperation& op) override { op = operation; }
    void getWorldTransforms(Ogre::Matrix4* xform) const override { *xform = world; }
    [[nodiscard]] Ogre::Real getSquaredViewDepth(const Ogre::Camera* /*camera*/) const override
    {
        return 0;
    }
    [[nodiscard]] const Ogre::LightList& getLights() const override
    {
        static const Ogre::LightList none;
        return none;
    }
};

// The sky: one triangle over the viewport, whose vertex shader places it itself, so that it
// takes Ogre's projection only for the fragment shader to find the view rays.
class sky_renderable final : public Ogre::Renderable
{
public:
    Ogre::RenderOperation operation;
    Ogre::MaterialPtr material;

    [[nodiscard]] const Ogre::MaterialPtr& getMaterial() const override { return material; }
    void getRenderOperation(Ogre::RenderOperation& op) override { op = operation; }
    void getWorldTransforms(Ogre::Matrix4* xform) const override
    {
        *xform = Ogre::Matrix4::IDENTITY;
    }
    [[nodiscard]] Ogre::Real getSquaredViewDepth(const Ogre::Camera* /*camera*/) const override
    {
        return 0;
    }
    [[nodiscard]] const Ogre::LightList& getLights() const override
    {
        static const Ogre::LightList none;
        return none;
    }
};

} // namespace

struct vgl::ogre::globe::state
{
    explicit state(const memory& m) noexcept : mem(m), resources(m) {}

    memory mem;
    vgl_context* context = nullptr;
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_LOGARITHMIC;
    Ogre::String group;
    Ogre::String name;
    globe_settings settings;

    Ogre::HighLevelGpuProgramPtr vertex_program;
    Ogre::HighLevelGpuProgramPtr fragment_program;
    Ogre::MaterialPtr material; // The one every mesh's is cloned from.
    Ogre::TexturePtr blank;     // In texture units with nothing to sample.
    Ogre::HighLevelGpuProgramPtr sky_vertex_program;
    Ogre::HighLevelGpuProgramPtr sky_fragment_program;
    Ogre::MaterialPtr sky_material;
    Ogre::VertexData* sky_vertices = nullptr;
    sky_renderable sky;
    bool sky_shown = false; // The last frame drew the atmosphere.
    // The stars: the host's, as quads (backends/common/stars.hpp), kept on the CPU to be
    // uploaded again after resources_reset; the buffer is made when they are first drawn.
    Ogre::HighLevelGpuProgramPtr stars_vertex_program;
    Ogre::HighLevelGpuProgramPtr stars_fragment_program;
    Ogre::MaterialPtr stars_material;
    Ogre::VertexData* stars_vertices = nullptr;
    sky_renderable stars;
    vgl::backends::star_vertex* star_vertices = nullptr;
    std::uint32_t star_count = 0;
    bool stars_shown = false; // The last frame drew them.
    vgl::common::pointer_map<ogre_resource> resources;
    std::uint64_t next_name = 0;

    // The renderables of the last frame's draw list, and how many it used.
    tile_renderable** renderables = nullptr;
    std::size_t capacity = 0;
    std::size_t used = 0;
    unsigned long last_frame = ~0ul;
};

namespace
{

using state = vgl::ogre::globe::state;
using vgl::ogre::globe_settings;

void log(const state& s, vgl_log_level level, const std::string& text) noexcept
{
    static_cast<void>(vgl_context_log(s.context, level, text.c_str()));
}

// Data for an Ogre pixel box, which takes it writable, though blitting from one only reads it.
void* readable(const void* data)
{
    return const_cast<void*>(data); // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

Ogre::String unique_name(state& s, const char* what)
{
    return s.name + "/" + what + std::to_string(s.next_name++);
}

// The shading language the render system compiles, of those the shaders are written in.
const char* shading_language()
{
    const Ogre::HighLevelGpuProgramManager& m = Ogre::HighLevelGpuProgramManager::getSingleton();
    for (const char* language : {"hlsl", "glsl", "glsles", "glslang", "metal"})
    {
        if (m.isLanguageSupported(language))
        {
            return language;
        }
    }
    return nullptr;
}

// A program from a source of shaders.hpp, with the atmosphere's functions where it asks for them.
Ogre::HighLevelGpuProgramPtr compile_program(
    state& s, const char* language, Ogre::GpuProgramType type, const char* name, const char* source)
{
    const bool vertex = type == Ogre::GPT_VERTEX_PROGRAM;
    const Ogre::HighLevelGpuProgramPtr p =
        Ogre::HighLevelGpuProgramManager::getSingleton().createProgram(
            s.name + "/" + name, s.group, language, type);
    // GLSL needs a version first, which the source cannot choose for itself: Ogre takes 1.10
    // otherwise, without integer textures or bit operations. Ogre replaces this directive with
    // the render system's own version, for OpenGL and OpenGL ES alike.
    const std::string lang(language);
    const bool glsl = lang == "glsl" || lang == "glsles" || lang == "glslang";
    std::string text = source;
    const std::string marker = "//@vgl_atmosphere\n";
    if (const std::size_t at = text.find(marker); at != std::string::npos)
    {
        text.replace(at,
                     marker.size(),
                     std::string("#define VGL_CONST STATIC const\n") +
                         vgl::backends::atmosphere_shader);
    }
    p->setSource(std::string(glsl ? "OGRE_NATIVE_GLSL_VERSION_DIRECTIVE\n" : "") + text);
    if (std::string(language) == "hlsl")
    {
        p->setParameter("target", vertex ? "vs_4_0" : "ps_4_0");
        p->setParameter("entry_point", "main");
    }
    if (s.depth == VGL_DEPTH_CONVENTION_LOGARITHMIC)
    {
        p->setParameter("preprocessor_defines", "VGL_LOGARITHMIC_DEPTH=1");
    }
    p->load();
    if (p->hasCompileError())
    {
        throw std::runtime_error(std::string("the program ") + name +
                                 " failed to compile; Ogre's log has the error");
    }
    p->getDefaultParameters()->setIgnoreMissingParams(true);
    return p;
}

// The atmosphere's parameters, which the terrain's and the sky's fragment programs share.
void atmosphere_constants(const Ogre::GpuProgramParametersSharedPtr& params)
{
    for (std::size_t k = 0; k < 3; ++k)
    {
        params->setNamedAutoConstant("u_to_planet" + std::to_string(k),
                                     Ogre::GpuProgramParameters::ACT_CUSTOM,
                                     static_cast<std::uint32_t>(custom_to_planet + k));
    }
    params->setNamedAutoConstant("u_camera", Ogre::GpuProgramParameters::ACT_CUSTOM, custom_camera);
    params->setNamedAutoConstant(
        "u_sun_planet", Ogre::GpuProgramParameters::ACT_CUSTOM, custom_sun_planet);
}

Ogre::HighLevelGpuProgramPtr
make_program(state& s, const char* language, Ogre::GpuProgramType type, const char* source)
{
    const bool vertex = type == Ogre::GPT_VERTEX_PROGRAM;
    const Ogre::HighLevelGpuProgramPtr p =
        compile_program(s, language, type, vertex ? "terrain_vs" : "terrain_fs", source);
    Ogre::GpuProgramParametersSharedPtr params = p->getDefaultParameters();
    params->setNamedAutoConstant("u_world_view", Ogre::GpuProgramParameters::ACT_WORLDVIEW_MATRIX);
    const auto custom_constant = [&](const char* name, std::size_t index) {
        params->setNamedAutoConstant(
            name, Ogre::GpuProgramParameters::ACT_CUSTOM, static_cast<std::uint32_t>(index));
    };
    custom_constant("u_geoid", custom_geoid);
    custom_constant("u_frame", custom_frame);
    if (vertex)
    {
        params->setNamedAutoConstant("u_world_view_proj",
                                     Ogre::GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX);
        custom_constant("u_centre", custom_centre);
        return p;
    }
    custom_constant("u_light", custom_light);
    custom_constant("u_item", custom_item);
    custom_constant("u_samples_info", custom_samples);
    for (std::size_t k = 0; k < max_layers; ++k)
    {
        custom_constant(("u_layer_uv" + std::to_string(k)).c_str(), custom_layer_uv + k);
    }
    custom_constant("u_layer_opacity", custom_layer_opacity);
    atmosphere_constants(params);
    // GLSL before 4.2 binds samplers to units by uniforms; HLSL by register.
    params->setNamedConstant("u_samples", 0);
    for (int k = 0; k < static_cast<int>(max_layers); ++k)
    {
        params->setNamedConstant("u_layer" + std::to_string(k), k + 1);
    }
    return p;
}

// The programs, the material every mesh's is cloned from, and the blank texture.
void build_pipeline(state& s)
{
    const char* language = shading_language();
    if (language == nullptr)
    {
        throw std::invalid_argument("the render system compiles none of the shading languages");
    }
    s.vertex_program = make_program(
        s, language, Ogre::GPT_VERTEX_PROGRAM, vgl::ogre::detail::terrain_vertex_shader);
    s.fragment_program = make_program(
        s, language, Ogre::GPT_FRAGMENT_PROGRAM, vgl::ogre::detail::terrain_fragment_shader);

    s.blank = Ogre::TextureManager::getSingleton().createManual(
        s.name + "/blank", s.group, Ogre::TEX_TYPE_2D, 1, 1, 0, Ogre::PF_BYTE_RGBA);
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    s.blank->getBuffer()->blitFromMemory(
        Ogre::PixelBox(1, 1, 1, Ogre::PF_BYTE_RGBA, readable(white.data())));

    s.material = Ogre::MaterialManager::getSingleton().create(s.name + "/terrain", s.group);
    Ogre::Pass* pass = s.material->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setDepthCheckEnabled(true);
    pass->setDepthWriteEnabled(true);
    // Counter-clockwise from outside the ellipsoid (docs/render-interface.md, section 6), which
    // is Ogre's front; Ogre turns this over for mirrored and flipped targets itself.
    pass->setCullingMode(Ogre::CULL_CLOCKWISE);
    pass->setVertexProgram(s.vertex_program->getName(), true);
    pass->setFragmentProgram(s.fragment_program->getName(), true);
    // Unit 0: the samples, read texel by texel. Units 1 to 4: the layers, trilinear and
    // anisotropic, clamped: tiles carry their neighbours' pixels in their border instead.
    Ogre::TextureUnitState* samples = pass->createTextureUnitState();
    samples->setTextureFiltering(Ogre::TFO_NONE);
    samples->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
    samples->setTexture(s.blank);
    for (std::uint32_t k = 0; k < max_layers; ++k)
    {
        Ogre::TextureUnitState* layer = pass->createTextureUnitState();
        layer->setTextureFiltering(Ogre::TFO_ANISOTROPIC);
        layer->setTextureAnisotropy(8);
        layer->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
        layer->setTexture(s.blank);
    }
    s.material->load();

    // The sky: its programs, its material, which draws at the far depth only, without writing
    // it, from both sides, over the render target by the transmittance in its alpha; and its
    // triangle over the viewport.
    s.sky_vertex_program = compile_program(
        s, language, Ogre::GPT_VERTEX_PROGRAM, "sky_vs", vgl::ogre::detail::sky_vertex_shader);
    s.sky_vertex_program->getDefaultParameters()->setNamedAutoConstant(
        "u_sky", Ogre::GpuProgramParameters::ACT_CUSTOM, custom_sky);
    s.sky_fragment_program = compile_program(
        s, language, Ogre::GPT_FRAGMENT_PROGRAM, "sky_fs", vgl::ogre::detail::sky_fragment_shader);
    const Ogre::GpuProgramParametersSharedPtr sky = s.sky_fragment_program->getDefaultParameters();
    sky->setNamedAutoConstant("u_inverse_projection",
                              Ogre::GpuProgramParameters::ACT_INVERSE_PROJECTION_MATRIX);
    atmosphere_constants(sky);
    s.sky_material = Ogre::MaterialManager::getSingleton().create(s.name + "/sky", s.group);
    Ogre::Pass* sky_pass = s.sky_material->getTechnique(0)->getPass(0);
    sky_pass->setLightingEnabled(false);
    sky_pass->setDepthCheckEnabled(true);
    sky_pass->setDepthWriteEnabled(false);
    // Ogre turns the comparison over itself for reversed-Z.
    sky_pass->setDepthFunction(Ogre::CMPF_LESS_EQUAL);
    sky_pass->setCullingMode(Ogre::CULL_NONE);
    sky_pass->setSeparateSceneBlending(
        Ogre::SBF_ONE, Ogre::SBF_SOURCE_ALPHA, Ogre::SBF_ZERO, Ogre::SBF_ONE);
    sky_pass->setVertexProgram(s.sky_vertex_program->getName(), true);
    sky_pass->setFragmentProgram(s.sky_fragment_program->getName(), true);
    s.sky_material->load();

    s.sky_vertices = OGRE_NEW Ogre::VertexData();
    s.sky_vertices->vertexStart = 0;
    s.sky_vertices->vertexCount = 3;
    s.sky_vertices->vertexDeclaration->addElement(0, 0, Ogre::VET_FLOAT2, Ogre::VES_POSITION);
    const Ogre::HardwareVertexBufferSharedPtr corners =
        Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            2 * sizeof(float), 3, Ogre::HBU_GPU_ONLY);
    const std::array<float, 6> ndc = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    corners->writeData(0, sizeof(ndc), ndc.data(), true);
    s.sky_vertices->vertexBufferBinding->setBinding(0, corners);
    // The stars: their programs, and a material that adds them to the render target at the far
    // depth only, without writing it.
    s.stars_vertex_program = compile_program(
        s, language, Ogre::GPT_VERTEX_PROGRAM, "stars_vs", vgl::ogre::detail::stars_vertex_shader);
    const Ogre::GpuProgramParametersSharedPtr stars_params =
        s.stars_vertex_program->getDefaultParameters();
    stars_params->setNamedAutoConstant("u_projection",
                                       Ogre::GpuProgramParameters::ACT_PROJECTION_MATRIX);
    stars_params->setNamedAutoConstant("u_viewport", Ogre::GpuProgramParameters::ACT_VIEWPORT_SIZE);
    for (std::size_t k = 0; k < 3; ++k)
    {
        stars_params->setNamedAutoConstant("u_celestial" + std::to_string(k),
                                           Ogre::GpuProgramParameters::ACT_CUSTOM,
                                           static_cast<std::uint32_t>(custom_celestial + k));
    }
    stars_params->setNamedAutoConstant("u_sky", Ogre::GpuProgramParameters::ACT_CUSTOM, custom_sky);
    atmosphere_constants(stars_params);
    s.stars_fragment_program = compile_program(s,
                                               language,
                                               Ogre::GPT_FRAGMENT_PROGRAM,
                                               "stars_fs",
                                               vgl::ogre::detail::stars_fragment_shader);
    s.stars_material = Ogre::MaterialManager::getSingleton().create(s.name + "/stars", s.group);
    Ogre::Pass* stars_pass = s.stars_material->getTechnique(0)->getPass(0);
    stars_pass->setLightingEnabled(false);
    stars_pass->setDepthCheckEnabled(true);
    stars_pass->setDepthWriteEnabled(false);
    stars_pass->setDepthFunction(Ogre::CMPF_LESS_EQUAL);
    stars_pass->setCullingMode(Ogre::CULL_NONE);
    stars_pass->setSeparateSceneBlending(
        Ogre::SBF_ONE, Ogre::SBF_ONE, Ogre::SBF_ZERO, Ogre::SBF_ONE);
    stars_pass->setVertexProgram(s.stars_vertex_program->getName(), true);
    stars_pass->setFragmentProgram(s.stars_fragment_program->getName(), true);
    s.stars_material->load();
    s.stars.material = s.stars_material;

    s.sky.operation.operationType = Ogre::RenderOperation::OT_TRIANGLE_LIST;
    s.sky.operation.vertexData = s.sky_vertices;
    s.sky.operation.useIndexes = false;
    s.sky.material = s.sky_material;
}

void release(ogre_resource& r) noexcept
{
    try
    {
        if (r.material)
        {
            Ogre::MaterialManager::getSingleton().remove(r.material);
        }
        if (r.texture)
        {
            Ogre::TextureManager::getSingleton().remove(r.texture);
        }
        OGRE_DELETE r.vertex_data;
        OGRE_DELETE r.index_data;
    }
    catch (...) // NOLINT(bugprone-empty-catch): nothing more to do while releasing.
    {
    }
    r.material.reset();
    r.texture.reset();
    r.vertex_data = nullptr;
    r.index_data = nullptr;
    for (Ogre::TexturePtr& t : r.bound_layers)
    {
        t.reset();
    }
}

// Frees the stars' buffer, which upload_stars makes again.
void free_star_buffer(state& s) noexcept
{
    OGRE_DELETE s.stars_vertices;
    s.stars_vertices = nullptr;
    s.stars.operation.vertexData = nullptr;
}

void forget_resources(state& s) noexcept
{
    free_star_buffer(s);
    s.resources.for_each([&](std::uint64_t, ogre_resource* r) {
        release(*r);
        s.mem.destroy(r);
    });
    s.resources.clear();
    s.used = 0;
}

// The Ogre objects of a resource the frame offers, as a record, or null: it is then reported
// failed.
ogre_resource* create_index_list(state& s, const vgl_index_list_data& d)
{
    auto* r = s.mem.create<ogre_resource>();
    if (r == nullptr)
    {
        return nullptr;
    }
    r->kind = resource_kind::index_list;
    const Ogre::HardwareIndexBuffer::IndexType type = d.index_size == 2
                                                          ? Ogre::HardwareIndexBuffer::IT_16BIT
                                                          : Ogre::HardwareIndexBuffer::IT_32BIT;
    const Ogre::HardwareIndexBufferSharedPtr buffer =
        Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            type, d.index_count, Ogre::HBU_GPU_ONLY);
    buffer->writeData(0, std::size_t{d.index_count} * d.index_size, d.indices, true);
    r->index_data = OGRE_NEW Ogre::IndexData();
    r->index_data->indexBuffer = buffer;
    r->index_data->indexStart = 0;
    r->index_data->indexCount = d.index_count;
    return r;
}

// The render interface's samples (docs/render-interface.md, section 6) as two floats each: the
// height, and the normal's two signed normalized 16-bit components quantized to 12 bits each and
// packed as a whole number below 2^24, which a float holds exactly.
void convert_samples(const unsigned char* samples, std::size_t count, float* out) noexcept
{
    for (std::size_t k = 0; k < count; ++k)
    {
        const unsigned char* sample = samples + k * VGL_TERRAIN_SAMPLE_SIZE;
        float height = 0.0f;
        std::array<std::int16_t, 2> normal{};
        std::memcpy(&height, sample + VGL_TERRAIN_SAMPLE_HEIGHT_OFFSET, sizeof(height));
        std::memcpy(normal.data(), sample + VGL_TERRAIN_SAMPLE_NORMAL_OFFSET, sizeof(normal));
        std::array<float, 2> q{};
        for (std::size_t c = 0; c < 2; ++c)
        {
            const float v = std::max(static_cast<float>(normal[c]) / 32767.0f, -1.0f);
            q[c] = std::round((v * 0.5f + 0.5f) * 4095.0f);
        }
        out[2 * k] = height;
        out[2 * k + 1] = q[0] + q[1] * 4096.0f;
    }
}

ogre_resource* create_mesh(state& s, const vgl_mesh_data& d)
{
    if (d.vertex_size != VGL_TERRAIN_VERTEX_SIZE || d.sample_size != VGL_TERRAIN_SAMPLE_SIZE ||
        d.samples == nullptr || d.sample_grid < 2)
    {
        return nullptr;
    }
    auto* r = s.mem.create<ogre_resource>();
    if (r == nullptr)
    {
        return nullptr;
    }
    r->kind = resource_kind::mesh;
    // A grid of g x g vertices and its skirts' 4 (g - 1) (docs/render-interface.md, section 6).
    const auto grid = static_cast<std::uint32_t>(
        std::lround(std::sqrt(8.0 + static_cast<double>(d.vertex_count)) - 2.0));
    if (grid > 1 && std::size_t{grid} * grid + 4 * (std::size_t{grid} - 1) == d.vertex_count)
    {
        r->grid = grid;
    }
    r->min_height = static_cast<float>(d.min_height);
    r->sample_grid = d.sample_grid;
    try
    {
        // The vertex layout of docs/render-interface.md, section 6; the normal is not read.
        r->vertex_data = OGRE_NEW Ogre::VertexData();
        r->vertex_data->vertexCount = d.vertex_count;
        Ogre::VertexDeclaration* decl = r->vertex_data->vertexDeclaration;
        decl->addElement(
            0, VGL_TERRAIN_VERTEX_POSITION_OFFSET, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        decl->addElement(0,
                         VGL_TERRAIN_VERTEX_UV_OFFSET,
                         Ogre::VET_USHORT2_NORM,
                         Ogre::VES_TEXTURE_COORDINATES,
                         0);
        decl->addElement(0,
                         VGL_TERRAIN_VERTEX_HEIGHT_OFFSET,
                         Ogre::VET_FLOAT1,
                         Ogre::VES_TEXTURE_COORDINATES,
                         1);
        const Ogre::HardwareVertexBufferSharedPtr buffer =
            Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
                VGL_TERRAIN_VERTEX_SIZE, d.vertex_count, Ogre::HBU_GPU_ONLY);
        buffer->writeData(0, std::size_t{d.vertex_count} * d.vertex_size, d.vertices, true);
        r->vertex_data->vertexBufferBinding->setBinding(0, buffer);

        // The samples, as two floats per texel (shaders.hpp): Ogre's Direct3D 11 render system
        // has no integer textures for the render interface's layout.
        r->texture = Ogre::TextureManager::getSingleton().createManual(unique_name(s, "samples"),
                                                                       s.group,
                                                                       Ogre::TEX_TYPE_2D,
                                                                       d.sample_grid,
                                                                       d.sample_grid,
                                                                       0,
                                                                       Ogre::PF_FLOAT32_GR);
        const std::size_t count = std::size_t{d.sample_grid} * d.sample_grid;
        auto* converted =
            static_cast<float*>(s.mem.allocate(count * 2 * sizeof(float), alignof(float)));
        if (converted == nullptr)
        {
            throw std::bad_alloc();
        }
        convert_samples(static_cast<const unsigned char*>(d.samples), count, converted);
        try
        {
            r->texture->getBuffer()->blitFromMemory(
                Ogre::PixelBox(d.sample_grid, d.sample_grid, 1, Ogre::PF_FLOAT32_GR, converted));
        }
        catch (...)
        {
            s.mem.deallocate(converted, count * 2 * sizeof(float), alignof(float));
            throw;
        }
        s.mem.deallocate(converted, count * 2 * sizeof(float), alignof(float));

        r->material = s.material->clone(unique_name(s, "mesh"));
        r->material->getTechnique(0)->getPass(0)->getTextureUnitState(0)->setTexture(r->texture);
        r->material->load();
    }
    catch (const std::exception& e)
    {
        log(s,
            VGL_LOG_LEVEL_WARNING,
            std::string("vgl::ogre::globe: creating a mesh failed: ") + e.what());
        release(*r);
        s.mem.destroy(r);
        return nullptr;
    }
    return r;
}

ogre_resource* create_texture(state& s, const vgl_texture_data& d)
{
    if ((d.format != VGL_TEXTURE_FORMAT_RGBA8 && d.format != VGL_TEXTURE_FORMAT_RGBA8_SRGB) ||
        d.mip_count == 0)
    {
        return nullptr;
    }
    auto* r = s.mem.create<ogre_resource>();
    if (r == nullptr)
    {
        return nullptr;
    }
    r->kind = resource_kind::texture;
    try
    {
        r->texture =
            Ogre::TextureManager::getSingleton().createManual(unique_name(s, "imagery"),
                                                              s.group,
                                                              Ogre::TEX_TYPE_2D,
                                                              d.width,
                                                              d.height,
                                                              static_cast<int>(d.mip_count) - 1,
                                                              Ogre::PF_BYTE_RGBA);
        std::uint32_t w = d.width;
        std::uint32_t h = d.height;
        for (std::uint32_t level = 0; level < d.mip_count; ++level)
        {
            r->texture->getBuffer(0, level)->blitFromMemory(
                Ogre::PixelBox(w, h, 1, Ogre::PF_BYTE_RGBA, readable(d.mip_data[level])));
            w = std::max<std::uint32_t>(1, w / 2);
            h = std::max<std::uint32_t>(1, h / 2);
        }
    }
    catch (const std::exception& e)
    {
        log(s,
            VGL_LOG_LEVEL_WARNING,
            std::string("vgl::ogre::globe: creating a texture failed: ") + e.what());
        release(*r);
        s.mem.destroy(r);
        return nullptr;
    }
    return r;
}

// Records a created resource and sets its handle, or reports it failed.
void install(state& s, vgl_frame* frame, std::uint64_t id, ogre_resource* r)
{
    if (r == nullptr || !s.resources.insert(id, r))
    {
        if (r != nullptr)
        {
            release(*r);
            s.mem.destroy(r);
        }
        static_cast<void>(vgl_frame_resource_failed(frame, id, VGL_RESULT_BACKEND_ERROR));
        return;
    }
    static_cast<void>(
        vgl_frame_resource_handle_set(frame, id, reinterpret_cast<std::uintptr_t>(r)));
}

// Applies the frame's resource lists in their order (docs/render-interface.md, section 4.2).
void apply_resources(state& s, vgl_frame* frame)
{
    const std::uint64_t* destroys = nullptr;
    std::uint32_t count = 0;
    static_cast<void>(vgl_frame_destroys_get(frame, &destroys, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (ogre_resource* r = s.resources.find(destroys[i]); r != nullptr)
        {
            release(*r);
            s.resources.erase(destroys[i]);
            s.mem.destroy(r);
        }
    }
    const vgl_index_list_data* lists = nullptr;
    static_cast<void>(vgl_frame_index_lists_get(frame, &lists, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        ogre_resource* r = nullptr;
        try
        {
            r = create_index_list(s, lists[i]);
        }
        catch (const std::exception& e)
        {
            log(s,
                VGL_LOG_LEVEL_WARNING,
                std::string("vgl::ogre::globe: creating an index list failed: ") + e.what());
        }
        install(s, frame, lists[i].resource, r);
    }
    const vgl_mesh_data* meshes = nullptr;
    static_cast<void>(vgl_frame_meshes_get(frame, &meshes, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const ogre_resource* list = s.resources.find(meshes[i].index_list);
        install(s,
                frame,
                meshes[i].resource,
                list != nullptr && list->kind == resource_kind::index_list
                    ? create_mesh(s, meshes[i])
                    : nullptr);
    }
    const vgl_texture_data* textures = nullptr;
    static_cast<void>(vgl_frame_textures_get(frame, &textures, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        install(s, frame, textures[i].resource, create_texture(s, textures[i]));
    }
}

// The record a handle names: its address, as install sets it; else the record of the id.
ogre_resource* resource_of(const state& s, std::uint64_t id, std::uint64_t handle)
{
    if (handle != 0)
    {
        const auto address = static_cast<std::uintptr_t>(handle);
        return reinterpret_cast<ogre_resource*>(address); // NOLINT(performance-no-int-to-ptr)
    }
    return s.resources.find(id);
}

tile_renderable* renderable_at(state& s, std::size_t i)
{
    if (i >= s.capacity)
    {
        const std::size_t capacity = std::max<std::size_t>(64, s.capacity * 2);
        auto** grown = static_cast<tile_renderable**>(
            s.mem.allocate(capacity * sizeof(tile_renderable*), alignof(tile_renderable*)));
        if (grown == nullptr)
        {
            return nullptr;
        }
        std::fill_n(grown, capacity, nullptr);
        std::copy_n(s.renderables, s.capacity, grown);
        // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion): raw memory
        s.mem.deallocate(static_cast<void*>(s.renderables),
                         s.capacity * sizeof(tile_renderable*),
                         alignof(tile_renderable*));
        s.renderables = grown;
        s.capacity = capacity;
    }
    if (s.renderables[i] == nullptr)
    {
        s.renderables[i] = s.mem.create<tile_renderable>();
    }
    return s.renderables[i];
}

Ogre::Vector4 vec4(const float* v)
{
    return {v[0], v[1], v[2], v[3]};
}

// Makes the stars' vertex buffer, once after each set or reset; false if it fails.
bool upload_stars(state& s)
{
    if (s.stars_vertices != nullptr)
    {
        return true;
    }
    try
    {
        const std::size_t count = std::size_t{s.star_count} * vgl::backends::star_vertex_count;
        auto* data = OGRE_NEW Ogre::VertexData();
        data->vertexStart = 0;
        data->vertexCount = static_cast<Ogre::uint32>(count);
        Ogre::VertexDeclaration* decl = data->vertexDeclaration;
        decl->addElement(
            0, vgl::backends::star_direction_offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        decl->addElement(0,
                         vgl::backends::star_corner_offset,
                         Ogre::VET_FLOAT2,
                         Ogre::VES_TEXTURE_COORDINATES,
                         0);
        decl->addElement(0,
                         vgl::backends::star_light_offset,
                         Ogre::VET_FLOAT2,
                         Ogre::VES_TEXTURE_COORDINATES,
                         1);
        decl->addElement(0,
                         vgl::backends::star_color_offset,
                         Ogre::VET_FLOAT3,
                         Ogre::VES_TEXTURE_COORDINATES,
                         2);
        const Ogre::HardwareVertexBufferSharedPtr buffer =
            Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
                sizeof(vgl::backends::star_vertex), count, Ogre::HBU_GPU_ONLY);
        buffer->writeData(0, count * sizeof(vgl::backends::star_vertex), s.star_vertices, true);
        data->vertexBufferBinding->setBinding(0, buffer);
        s.stars_vertices = data;
        s.stars.operation.operationType = Ogre::RenderOperation::OT_TRIANGLE_LIST;
        s.stars.operation.vertexData = data;
        s.stars.operation.useIndexes = false;
        return true;
    }
    catch (const std::exception& e)
    {
        log(s, VGL_LOG_LEVEL_ERROR, std::string("vgl::ogre::globe: the stars failed: ") + e.what());
        return false;
    }
}

// Frees the CPU copy of the stars, and their buffer.
void free_stars(state& s) noexcept
{
    free_star_buffer(s);
    if (s.star_vertices != nullptr)
    {
        s.mem.deallocate(s.star_vertices,
                         std::size_t{s.star_count} * vgl::backends::star_vertex_count *
                             sizeof(vgl::backends::star_vertex),
                         alignof(vgl::backends::star_vertex));
    }
    s.star_vertices = nullptr;
    s.star_count = 0;
}

// Fills the renderables from the draw list.
void fill_renderables(state& s, const vgl_frame* frame, const vgl_dmat4* view)
{
    const globe_settings& g = s.settings;
    // The atmosphere needs the camera and a sun: without one, it is above the camera.
    const bool atmosphere = g.atmosphere && view != nullptr;
    vgl_dvec3 sun = g.sun_direction;
    std::array<Ogre::Vector4, 3> to_planet{};
    Ogre::Vector4 camera(0.0f, 0.0f, 0.0f, 0.0f);
    Ogre::Vector4 sun_planet(0.0f, 0.0f, 0.0f, 0.0f);
    if (atmosphere)
    {
        const vgl::backends::atmosphere_view air = vgl::backends::atmosphere_for(*view, sun);
        sun = vgl::backends::atmosphere_sun(*view, sun);
        for (std::size_t i = 0; i < 3; ++i)
        {
            to_planet[i] =
                Ogre::Vector4(air.to_planet[i], air.to_planet[3 + i], air.to_planet[6 + i], 0.0f);
        }
        camera =
            Ogre::Vector4(air.camera_km[0], air.camera_km[1], air.camera_km[2], air.camera_height);
        sun_planet = Ogre::Vector4(air.sun_planet[0], air.sun_planet[1], air.sun_planet[2], 1.0f);
    }
    s.sky_shown = atmosphere;
    s.stars_shown = g.stars && view != nullptr && s.star_count != 0 && upload_stars(s);
    if (s.stars_shown)
    {
        const std::array<float, 9> c = vgl::backends::celestial_to_camera(*view, g.sidereal_angle);
        for (std::size_t i = 0; i < 3; ++i)
        {
            s.stars.setCustomParameter(custom_celestial + i,
                                       Ogre::Vector4(c[i], c[3 + i], c[6 + i], 0.0f));
            s.stars.setCustomParameter(custom_to_planet + i, to_planet[i]);
        }
        s.stars.setCustomParameter(custom_camera, camera);
        s.stars.setCustomParameter(custom_sun_planet, sun_planet);
        s.stars.setCustomParameter(
            custom_sky,
            Ogre::Vector4(
                s.depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f));
    }
    for (std::size_t i = 0; i < 3; ++i)
    {
        s.sky.setCustomParameter(custom_to_planet + i, to_planet[i]);
    }
    s.sky.setCustomParameter(custom_camera, camera);
    s.sky.setCustomParameter(custom_sun_planet, sun_planet);
    // The far plane's depth: 0 for reversed-Z, else 1, the logarithmic convention's too.
    s.sky.setCustomParameter(
        custom_sky,
        Ogre::Vector4(s.depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f));
    // The sun in camera space: only the view's rotation applies to a direction.
    Ogre::Vector4 light(0.0f, 0.0f, 0.0f, static_cast<float>(g.debug_flags));
    if (view != nullptr && (sun.x != 0.0 || sun.y != 0.0 || sun.z != 0.0))
    {
        const double* m = &view->m[0];
        const std::array<double, 3> l = {m[0] * sun.x + m[4] * sun.y + m[8] * sun.z,
                                         m[1] * sun.x + m[5] * sun.y + m[9] * sun.z,
                                         m[2] * sun.x + m[6] * sun.y + m[10] * sun.z};
        const double length = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        light.x = static_cast<float>(l[0] / length);
        light.y = static_cast<float>(l[1] / length);
        light.z = static_cast<float>(l[2] / length);
    }
    const double far_distance =
        g.logarithmic_far > 0.0 ? g.logarithmic_far : VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT;
    const Ogre::Vector4 frame_values(
        g.sea_surface ? 1.0f : 0.0f,
        static_cast<float>(2.0 / std::log2(far_distance + 1.0)),
        static_cast<float>(g.contour_interval > 0.0 ? g.contour_interval : 500.0),
        0.0f);

    const vgl_draw_layer* layers = nullptr;
    std::uint32_t layer_total = 0;
    static_cast<void>(vgl_frame_draw_layers_get(frame, &layers, &layer_total));
    const vgl_draw_item* items = nullptr;
    std::uint32_t count = 0;
    static_cast<void>(vgl_frame_draw_list_get(frame, &items, &count));
    s.used = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const vgl_draw_item& item = items[i];
        ogre_resource* mesh = resource_of(s, item.mesh, item.mesh_handle);
        const ogre_resource* list = resource_of(s, item.index_list, item.index_list_handle);
        tile_renderable* r =
            mesh != nullptr && list != nullptr ? renderable_at(s, s.used) : nullptr;
        if (r == nullptr)
        {
            continue; // Not created (docs/render-interface.md, section 4.2), or out of memory.
        }
        ++s.used;
        r->operation.operationType = Ogre::RenderOperation::OT_TRIANGLE_LIST;
        r->operation.vertexData = mesh->vertex_data;
        r->operation.indexData = list->index_data;
        r->operation.useIndexes = true;
        r->material = mesh->material;
        const float* mv = &item.model_view[0]; // Column-major; Ogre's matrices are row-major.
        r->world = Ogre::Matrix4(mv[0],
                                 mv[4],
                                 mv[8],
                                 mv[12],
                                 mv[1],
                                 mv[5],
                                 mv[9],
                                 mv[13],
                                 mv[2],
                                 mv[6],
                                 mv[10],
                                 mv[14],
                                 mv[3],
                                 mv[7],
                                 mv[11],
                                 mv[15]);

        // The top layers, if there are more than a draw blends, in the mesh's material.
        Ogre::Pass* pass = mesh->material->getTechnique(0)->getPass(0);
        const std::uint32_t used = std::min(item.layer_count, max_layers);
        const std::uint32_t first = item.first_layer + item.layer_count - used;
        std::uint32_t drawn = 0;
        std::array<float, 4> opacity{};
        float top_scale = 1.0f;
        for (std::uint32_t k = 0; k < used && first + k < layer_total; ++k)
        {
            const vgl_draw_layer& layer = layers[first + k];
            const ogre_resource* t = resource_of(s, layer.texture, layer.texture_handle);
            if (t == nullptr || !t->texture)
            {
                continue;
            }
            if (mesh->bound_layers[drawn] != t->texture)
            {
                pass->getTextureUnitState(1 + drawn)->setTexture(t->texture);
                mesh->bound_layers[drawn] = t->texture;
            }
            r->setCustomParameter(custom_layer_uv + drawn, vec4(&layer.uv_scale_offset[0]));
            opacity[drawn] = layer.opacity;
            top_scale = layer.uv_scale_offset[0];
            ++drawn;
        }
        // The top layer's scale is a little below 1 for the tile's own imagery (its border
        // aside), and halves with each level of an ancestor.
        const float coarser = std::max(std::round(-std::log2(std::max(top_scale, 1e-6f))), 0.0f);
        r->setCustomParameter(custom_layer_opacity, vec4(opacity.data()));
        r->setCustomParameter(custom_geoid, vec4(&item.geoid_heights[0]));
        r->setCustomParameter(custom_centre,
                              Ogre::Vector4(static_cast<float>(item.centre.x),
                                            static_cast<float>(item.centre.y),
                                            static_cast<float>(item.centre.z),
                                            mesh->min_height));
        r->setCustomParameter(
            custom_item,
            Ogre::Vector4(static_cast<float>(drawn),
                          static_cast<float>(item.tile.level),
                          coarser,
                          static_cast<float>(mesh->grid > 1 ? mesh->grid - 1 : 0)));
        r->setCustomParameter(
            custom_samples,
            Ogre::Vector4(static_cast<float>(mesh->sample_grid - 1), 0.0f, 0.0f, 0.0f));
        r->setCustomParameter(custom_light, light);
        r->setCustomParameter(custom_frame, frame_values);
        for (std::size_t k = 0; k < 3; ++k)
        {
            r->setCustomParameter(custom_to_planet + k, to_planet[k]);
        }
        r->setCustomParameter(custom_camera, camera);
        r->setCustomParameter(custom_sun_planet, sun_planet);
    }
}

// One frame of the render interface: resources applied and renderables filled, before Ogre
// draws them.
void run_frame(state& s)
{
    vgl_view view{};
    view.struct_size = sizeof(view);
    const bool has_view = vgl_context_view_get(s.context, &view) == VGL_RESULT_SUCCESS;
    const vgl_dmat4* view_matrix = has_view ? &view.view_matrix : nullptr;
    if (s.settings.has_view_matrix)
    {
        view_matrix = &s.settings.view_matrix;
    }
    vgl_frame_begin_info info{};
    info.struct_size = sizeof(info);
    info.upload_budget = s.settings.upload_budget;
    info.view_matrix = s.settings.has_view_matrix ? &s.settings.view_matrix : nullptr;
    vgl_frame* frame = nullptr;
    s.sky_shown = false;
    if (vgl_context_frame_begin(s.context, &info, &frame) != VGL_RESULT_SUCCESS)
    {
        s.used = 0;
        return;
    }
    try
    {
        apply_resources(s, frame);
        if (view_matrix != nullptr)
        {
            fill_renderables(s, frame, view_matrix);
        }
        else
        {
            s.used = 0;
        }
    }
    catch (const std::exception& e)
    {
        log(s, VGL_LOG_LEVEL_ERROR, std::string("vgl::ogre::globe: a frame failed: ") + e.what());
        s.used = 0;
    }
    static_cast<void>(vgl_frame_end(frame));
}

} // namespace

namespace vgl::ogre
{

vgl::result globe::create(vgl_context* context, const globe_create_info& info, globe*& out) noexcept
{
    out = nullptr;
    if (context == nullptr || info.depth_convention > VGL_DEPTH_CONVENTION_CONVENTIONAL)
    {
        return vgl::result::invalid_argument;
    }
    vgl_allocator_interface allocator{};
    static_cast<void>(vgl_context_allocator_get(context, &allocator));
    const memory mem(allocator);
    auto* s = mem.create<state>(mem);
    void* place = mem.allocate(sizeof(globe), alignof(globe));
    if (s == nullptr || place == nullptr)
    {
        mem.destroy(s);
        mem.deallocate(place, sizeof(globe), alignof(globe));
        return vgl::result::out_of_memory;
    }
    s->context = context;
    s->depth = info.depth_convention;
    globe* g = nullptr;
    try
    {
        s->group = info.resource_group;
        s->name = info.name;
        Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
        if (rs == nullptr)
        {
            throw std::invalid_argument(
                "Ogre has no render system yet; create the globe after Root::initialise");
        }
        if (s->depth == VGL_DEPTH_CONVENTION_REVERSED_Z && !rs->isReverseDepthBufferEnabled())
        {
            log(*s,
                VGL_LOG_LEVEL_ERROR,
                "vgl::ogre::globe::create: the reversed-Z convention needs the render system's "
                "\"Reversed Z-Buffer\" option");
            s->~state();
            mem.deallocate(s, sizeof(state), alignof(state));
            mem.deallocate(place, sizeof(globe), alignof(globe));
            return vgl::result::not_supported;
        }
        build_pipeline(*s);
        g = ::new (place) globe(s->name, s);
    }
    catch (const std::exception& e)
    {
        log(*s, VGL_LOG_LEVEL_ERROR, std::string("vgl::ogre::globe::create: ") + e.what());
        try
        {
            if (s->material)
            {
                Ogre::MaterialManager::getSingleton().remove(s->material);
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch): failing already.
        {
        }
        mem.destroy(s);
        mem.deallocate(place, sizeof(globe), alignof(globe));
        return vgl::result::backend_error;
    }
    const vgl_texture_format format = VGL_TEXTURE_FORMAT_RGBA8;
    static_cast<void>(vgl_context_texture_formats_set(context, &format, 1));
    out = g;
    return vgl::result::success;
}

void globe::destroy(globe* g) noexcept
{
    if (g == nullptr)
    {
        return;
    }
    const memory mem = g->state_->mem;
    g->~globe();
    mem.deallocate(g, sizeof(globe), alignof(globe));
}

globe::globe(const Ogre::String& name, state* s) noexcept : Ogre::MovableObject(name), state_(s)
{
    // The core culls the tiles; Ogre must not cull the globe.
    setCastShadows(false);
}

globe::~globe()
{
    state& s = *state_;
    forget_resources(s);
    for (std::size_t i = 0; i < s.capacity; ++i)
    {
        s.mem.destroy(s.renderables[i]);
    }
    // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion): raw memory
    s.mem.deallocate(static_cast<void*>(s.renderables),
                     s.capacity * sizeof(tile_renderable*),
                     alignof(tile_renderable*));
    try
    {
        Ogre::MaterialManager::getSingleton().remove(s.material);
        Ogre::MaterialManager::getSingleton().remove(s.sky_material);
        Ogre::TextureManager::getSingleton().remove(s.blank);
        Ogre::HighLevelGpuProgramManager::getSingleton().remove(s.vertex_program);
        Ogre::HighLevelGpuProgramManager::getSingleton().remove(s.fragment_program);
        Ogre::HighLevelGpuProgramManager::getSingleton().remove(s.sky_vertex_program);
        Ogre::HighLevelGpuProgramManager::getSingleton().remove(s.sky_fragment_program);
        free_stars(s);
        Ogre::MaterialManager::getSingleton().remove(s.stars_material);
        Ogre::HighLevelGpuProgramManager::getSingleton().remove(s.stars_vertex_program);
        Ogre::HighLevelGpuProgramManager::getSingleton().remove(s.stars_fragment_program);
        OGRE_DELETE s.sky_vertices;
        s.sky_vertices = nullptr;
    }
    catch (...) // NOLINT(bugprone-empty-catch): nothing more to do while destroying.
    {
    }
    // A later consumer of the context must receive every resource again.
    static_cast<void>(vgl_context_resources_reset(s.context));
    s.mem.destroy(state_);
}

void globe::settings_set(const globe_settings& settings) noexcept
{
    state_->settings = settings;
}

vgl::result globe::stars_set(const vgl_star* stars, std::uint32_t count) noexcept
{
    state& s = *state_;
    if ((stars == nullptr && count != 0) || count > vgl::backends::max_stars)
    {
        return vgl::result::invalid_argument;
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (!vgl::backends::star_is_valid(stars[i]))
        {
            log(s,
                VGL_LOG_LEVEL_ERROR,
                "vgl::ogre::globe::stars_set: star " + std::to_string(i) +
                    " is invalid: check that its direction is of unit length, that its values "
                    "are finite, its colour from 0 to 1, and reserved 0");
            return vgl::result::invalid_argument;
        }
    }
    const std::size_t vertices = std::size_t{count} * vgl::backends::star_vertex_count;
    vgl::backends::star_vertex* copy = nullptr;
    if (vertices != 0)
    {
        copy = static_cast<vgl::backends::star_vertex*>(s.mem.allocate(
            vertices * sizeof(vgl::backends::star_vertex), alignof(vgl::backends::star_vertex)));
        if (copy == nullptr)
        {
            return vgl::result::out_of_memory;
        }
        for (std::uint32_t i = 0; i < count; ++i)
        {
            vgl::backends::star_quad(stars[i],
                                     copy + std::size_t{i} * vgl::backends::star_vertex_count);
        }
    }
    free_stars(s);
    s.star_vertices = copy;
    s.star_count = count;
    return vgl::result::success;
}

void globe::resources_reset() noexcept
{
    forget_resources(*state_);
    static_cast<void>(vgl_context_resources_reset(state_->context));
}

const Ogre::String& globe::getMovableType() const
{
    static const Ogre::String type = "vgl_globe";
    return type;
}

const Ogre::AxisAlignedBox& globe::getBoundingBox() const
{
    return Ogre::AxisAlignedBox::BOX_INFINITE;
}

Ogre::Real globe::getBoundingRadius() const
{
    return std::numeric_limits<Ogre::Real>::infinity();
}

void globe::_updateRenderQueue(Ogre::RenderQueue* queue)
{
    state& s = *state_;
    // Once per Ogre frame; further viewports draw the same renderables.
    const unsigned long frame = Ogre::Root::getSingleton().getNextFrameNumber();
    if (frame != s.last_frame)
    {
        s.last_frame = frame;
        run_frame(s);
    }
    for (std::size_t i = 0; i < s.used; ++i)
    {
        queue->addRenderable(s.renderables[i], mRenderQueueID, mRenderQueuePriority);
    }
    if (s.stars_shown)
    {
        queue->addRenderable(&s.stars, Ogre::RENDER_QUEUE_SKIES_LATE - 1, mRenderQueuePriority);
    }
    if (s.sky_shown)
    {
        queue->addRenderable(&s.sky, Ogre::RENDER_QUEUE_SKIES_LATE, mRenderQueuePriority);
    }
}

void globe::visitRenderables(Ogre::Renderable::Visitor* visitor, bool /*debug_renderables*/)
{
    for (std::size_t i = 0; i < state_->used; ++i)
    {
        visitor->visit(state_->renderables[i], 0, false);
    }
}

} // namespace vgl::ogre
