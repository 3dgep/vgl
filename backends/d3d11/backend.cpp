// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The Direct3D 11 backend (vgl/d3d11.h): a consumer of the render interface
// (docs/render-interface.md) that turns each frame's resource lists into buffers and textures,
// and its draw list into one draw call per tile, with up to four imagery layers. The shaders
// (terrain.hlsl) are compiled at build time.

#include "backends/common/atmosphere_view.hpp"
#include "backends/common/clouds.hpp"
#include "backends/common/stars.hpp"
#include "common/host_memory.hpp"
#include "common/pointer_map.hpp"

#include <vgl/d3d11.h>

// The compiled shaders, from terrain.hlsl (CMakeLists.txt).
#include "sky_ps.h"
#include "sky_vs.h"
#include "stars_ps.h"
#include "stars_vs.h"
#include "terrain_ps.h"
#include "terrain_ps_log.h"
#include "terrain_vs.h"
#include "terrain_vs_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

using vgl::common::memory;

namespace
{

enum class resource_kind : std::uint8_t
{
    index_list,
    mesh,
    texture,
};

// Imagery layers a draw blends; a tile with more draws the top ones.
constexpr std::uint32_t max_layers = 4;

// Slots 0 to 3 hold the layers, the next one the mesh's samples, and the last the clouds.
constexpr std::uint32_t samples_slot = max_layers;
constexpr std::uint32_t clouds_slot = max_layers + 1;
constexpr std::uint32_t texture_slots = max_layers + 2;

// The Direct3D objects of one render resource. Its address is the handle the context echoes in
// draw items (vgl_frame_resource_handle_set).
struct d3d_resource
{
    resource_kind kind = resource_kind::index_list;
    ID3D11Buffer* buffer = nullptr;                  // Indices or vertices.
    ID3D11Texture2D* texture = nullptr;              // Textures, and meshes' samples.
    ID3D11ShaderResourceView* view = nullptr;        // Of the texture.
    DXGI_FORMAT index_format = DXGI_FORMAT_R16_UINT; // Index lists.
    std::uint32_t grid = 0;        // Meshes: vertices along a side of the grid, or 0 if unknown.
    std::uint32_t sample_grid = 0; // Meshes: samples along a side.
    float min_height = 0.0f;       // Meshes: the lowest terrain height; the skirts hang below it.
};

// The constant buffers of terrain.hlsl, laid out as HLSL packs them.
struct frame_constants
{
    std::array<float, 16> projection = {};
    std::array<float, 3> light = {};
    std::int32_t debug = 0;
    float log_depth = 0.0f;
    std::int32_t sea = 0;
    float contour_interval = 0.0f;
    float pad = 0.0f;
    std::array<float, 12> to_planet = {}; // Three rows of four.
    std::array<float, 3> camera_km = {};
    float camera_height = 0.0f;
    std::array<float, 3> sun_planet = {};
    std::int32_t atmosphere = 0;
    std::array<float, 16> inverse_projection = {};
    float far_depth = 0.0f;
    std::array<float, 3> sky_pad = {};
    std::array<float, 12> celestial = {}; // Three rows of four.
    std::array<float, 2> viewport = {};
    std::array<float, 2> stars_pad = {};
    std::array<float, 4> cloud = {}; // Drawn, height and texel size in kilometres, unused.
};
static_assert(sizeof(frame_constants) == 336, "frame_constants must match terrain.hlsl");

struct item_constants
{
    std::array<float, 16> model_view = {};
    std::array<float, 4> geoid = {};
    std::array<float, 3> centre = {};
    float min_height = 0.0f;
    std::array<float, std::size_t{4} * max_layers> layer_uv = {};
    std::array<float, max_layers> layer_opacity = {};
    std::int32_t layer_count = 0;
    float level = 0.0f;
    float imagery_coarser = 0.0f;
    float cells = 0.0f;
    float sample_cells = 0.0f;
    std::array<float, 3> pad = {};
};
static_assert(sizeof(item_constants) == 208, "item_constants must match terrain.hlsl");

// The host's state that drawing changes (NFR-08; vgl/d3d11.h lists it). Each pointer holds a
// reference until restored.
struct saved_state
{
    ID3D11InputLayout* input_layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vertex_buffer = nullptr;
    UINT vertex_stride = 0;
    UINT vertex_offset = 0;
    ID3D11Buffer* index_buffer = nullptr;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    UINT index_offset = 0;
    ID3D11VertexShader* vertex_shader = nullptr;
    std::array<ID3D11Buffer*, 2> vs_constants = {};
    ID3D11PixelShader* pixel_shader = nullptr;
    std::array<ID3D11Buffer*, 2> ps_constants = {};
    std::array<ID3D11ShaderResourceView*, texture_slots> ps_resources = {};
    std::array<ID3D11SamplerState*, 2> ps_samplers = {};
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11DepthStencilState* depth_stencil = nullptr;
    UINT stencil_ref = 0;
    ID3D11BlendState* blend = nullptr;
    std::array<FLOAT, 4> blend_factor = {};
    UINT sample_mask = 0;
};

constexpr std::size_t message_size = 1024;

template <typename T>
void release(T*& p) noexcept
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

} // namespace

struct vgl_d3d11_backend
{
    explicit vgl_d3d11_backend(const memory& m) noexcept : mem(m), resources(m) {}

    memory mem;
    vgl_context* context = nullptr;
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_LOGARITHMIC;

    // Valid while ready: after a successful create or reset.
    bool ready = false;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* dc = nullptr;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11InputLayout* input_layout = nullptr;
    ID3D11Buffer* frame_buffer = nullptr; // frame_constants
    ID3D11Buffer* item_buffer = nullptr;  // item_constants
    ID3D11SamplerState* sampler = nullptr;
    std::array<ID3D11RasterizerState*, 2> rasterizers = {}; // As is, and mirrored.
    ID3D11DepthStencilState* depth_state = nullptr;
    // The sky: its shaders, and drawing at the far depth without writing it, from both sides,
    // blended over the render target.
    ID3D11VertexShader* sky_vertex_shader = nullptr;
    ID3D11PixelShader* sky_pixel_shader = nullptr;
    ID3D11DepthStencilState* sky_depth_state = nullptr;
    ID3D11RasterizerState* sky_rasterizer = nullptr;
    ID3D11BlendState* sky_blend = nullptr;
    // The stars: the host's, as quads (backends/common/stars.hpp), kept on the CPU to be
    // uploaded again after a reset; the buffer is made when they are first drawn.
    ID3D11VertexShader* stars_vertex_shader = nullptr;
    ID3D11PixelShader* stars_pixel_shader = nullptr;
    ID3D11InputLayout* stars_layout = nullptr;
    ID3D11BlendState* stars_blend = nullptr;
    ID3D11Buffer* stars_buffer = nullptr;
    vgl::backends::star_vertex* star_vertices = nullptr;
    std::uint32_t star_count = 0;
    // The clouds: the host's coverage with its mip levels (backends/common/clouds.hpp), kept on
    // the CPU to be uploaded again after a reset, as a texture made when they are first drawn.
    std::uint8_t* cloud_data = nullptr;
    vgl::backends::cloud_levels cloud_levels;
    ID3D11Texture2D* cloud_texture = nullptr;
    ID3D11ShaderResourceView* cloud_view = nullptr;
    ID3D11SamplerState* cloud_sampler = nullptr;

    vgl::common::pointer_map<d3d_resource> resources;
};

namespace
{

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
// A C-style variadic function, as the core's logger is (src/foundation/log.cpp), to format with
// printf rules. On some platforms va_list is an array type, which also triggers the array decay
// check.
// NOLINTBEGIN(cert-dcl50-cpp,cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
void log(const vgl_d3d11_backend& b, vgl_log_level level, const char* format, ...) noexcept
{
    std::array<char, message_size> text{};
    va_list args;
    va_start(args, format);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized): args is initialized by va_start.
    const int written = std::vsnprintf(text.data(), text.size(), format, args);
    va_end(args);
    if (written >= 0)
    {
        static_cast<void>(vgl_context_log(b.context, level, text.data()));
    }
}
// NOLINTEND(cert-dcl50-cpp,cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-array-to-pointer-decay)

// Logs a failed Direct3D call; true if it failed.
bool failed(const vgl_d3d11_backend& b, HRESULT hr, const char* function, const char* what)
{
    if (SUCCEEDED(hr))
    {
        return false;
    }
    log(b,
        VGL_LOG_LEVEL_ERROR,
        "%s: creating %s failed (HRESULT 0x%08lx)",
        function,
        what,
        static_cast<unsigned long>(hr));
    return true;
}

// Creates the shaders and states on the device.
vgl_result build_pipeline(vgl_d3d11_backend& b, const char* function)
{
    ID3D11Device* d = b.device;
    const bool logarithmic = b.depth == VGL_DEPTH_CONVENTION_LOGARITHMIC;
    const BYTE* vs = logarithmic ? g_terrain_vs_log : g_terrain_vs;
    const SIZE_T vs_size = logarithmic ? sizeof(g_terrain_vs_log) : sizeof(g_terrain_vs);
    const BYTE* ps = logarithmic ? g_terrain_ps_log : g_terrain_ps;
    const SIZE_T ps_size = logarithmic ? sizeof(g_terrain_ps_log) : sizeof(g_terrain_ps);
    if (failed(b,
               d->CreateVertexShader(vs, vs_size, nullptr, &b.vertex_shader),
               function,
               "the vertex shader") ||
        failed(b,
               d->CreatePixelShader(ps, ps_size, nullptr, &b.pixel_shader),
               function,
               "the pixel shader") ||
        failed(b,
               d->CreateVertexShader(g_sky_vs, sizeof(g_sky_vs), nullptr, &b.sky_vertex_shader),
               function,
               "the sky's vertex shader") ||
        failed(b,
               d->CreatePixelShader(g_sky_ps, sizeof(g_sky_ps), nullptr, &b.sky_pixel_shader),
               function,
               "the sky's pixel shader") ||
        failed(
            b,
            d->CreateVertexShader(g_stars_vs, sizeof(g_stars_vs), nullptr, &b.stars_vertex_shader),
            function,
            "the stars' vertex shader") ||
        failed(b,
               d->CreatePixelShader(g_stars_ps, sizeof(g_stars_ps), nullptr, &b.stars_pixel_shader),
               function,
               "the stars' pixel shader"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }
    const std::array<D3D11_INPUT_ELEMENT_DESC, 4> star_layout = {{
        {"TEXCOORD",
         0,
         DXGI_FORMAT_R32G32B32_FLOAT,
         0,
         vgl::backends::star_direction_offset,
         D3D11_INPUT_PER_VERTEX_DATA,
         0},
        {"TEXCOORD",
         1,
         DXGI_FORMAT_R32G32_FLOAT,
         0,
         vgl::backends::star_corner_offset,
         D3D11_INPUT_PER_VERTEX_DATA,
         0},
        {"TEXCOORD",
         2,
         DXGI_FORMAT_R32G32_FLOAT,
         0,
         vgl::backends::star_light_offset,
         D3D11_INPUT_PER_VERTEX_DATA,
         0},
        {"TEXCOORD",
         3,
         DXGI_FORMAT_R32G32B32_FLOAT,
         0,
         vgl::backends::star_color_offset,
         D3D11_INPUT_PER_VERTEX_DATA,
         0},
    }};
    if (failed(b,
               d->CreateInputLayout(star_layout.data(),
                                    static_cast<UINT>(star_layout.size()),
                                    g_stars_vs,
                                    sizeof(g_stars_vs),
                                    &b.stars_layout),
               function,
               "the stars' input layout"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }

    // The vertex layout of docs/render-interface.md, section 6. The normal is not read: the
    // pixel shader takes it from the samples.
    const std::array<D3D11_INPUT_ELEMENT_DESC, 3> layout = {{
        {"POSITION",
         0,
         DXGI_FORMAT_R32G32B32_FLOAT,
         0,
         VGL_TERRAIN_VERTEX_POSITION_OFFSET,
         D3D11_INPUT_PER_VERTEX_DATA,
         0},
        {"HEIGHT",
         0,
         DXGI_FORMAT_R32_FLOAT,
         0,
         VGL_TERRAIN_VERTEX_HEIGHT_OFFSET,
         D3D11_INPUT_PER_VERTEX_DATA,
         0},
        {"TEXCOORD",
         0,
         DXGI_FORMAT_R16G16_UNORM,
         0,
         VGL_TERRAIN_VERTEX_UV_OFFSET,
         D3D11_INPUT_PER_VERTEX_DATA,
         0},
    }};
    if (failed(b,
               d->CreateInputLayout(
                   layout.data(), static_cast<UINT>(layout.size()), vs, vs_size, &b.input_layout),
               function,
               "the input layout"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }

    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = sizeof(frame_constants);
    if (failed(b, d->CreateBuffer(&cb, nullptr, &b.frame_buffer), function, "a constant buffer"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }
    cb.ByteWidth = sizeof(item_constants);
    if (failed(b, d->CreateBuffer(&cb, nullptr, &b.item_buffer), function, "a constant buffer"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }

    // Trilinear and anisotropic filtering, with edges clamped: tiles carry their neighbours'
    // pixels in their border instead.
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_ANISOTROPIC;
    s.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxAnisotropy = 8;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (failed(b, d->CreateSamplerState(&s, &b.sampler), function, "the sampler"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }

    // Triangles are counter-clockwise from outside the ellipsoid in right-handed camera space,
    // and stay so in clip space: Direct3D tells front from back there, before the viewport
    // turns y down, so counter-clockwise is the front, as in OpenGL. A mirroring projection
    // turns them over.
    for (std::size_t k = 0; k < b.rasterizers.size(); ++k)
    {
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_BACK;
        r.FrontCounterClockwise = k == 0 ? TRUE : FALSE;
        r.DepthClipEnable = TRUE;
        if (failed(
                b, d->CreateRasterizerState(&r, &b.rasterizers[k]), function, "a rasterizer state"))
        {
            return VGL_RESULT_BACKEND_ERROR;
        }
    }

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? D3D11_COMPARISON_GREATER
                                                              : D3D11_COMPARISON_LESS;
    ds.StencilEnable = FALSE;
    if (failed(b,
               d->CreateDepthStencilState(&ds, &b.depth_state),
               function,
               "the depth-stencil state"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }

    // The sky passes the depth test only at the far depth, writes none, and is blended over the
    // render target by the atmosphere's transmittance, which its alpha holds.
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? D3D11_COMPARISON_GREATER_EQUAL
                                                              : D3D11_COMPARISON_LESS_EQUAL;
    D3D11_RASTERIZER_DESC sky_r{};
    sky_r.FillMode = D3D11_FILL_SOLID;
    sky_r.CullMode = D3D11_CULL_NONE;
    sky_r.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (failed(b,
               d->CreateDepthStencilState(&ds, &b.sky_depth_state),
               function,
               "the sky's depth-stencil state") ||
        failed(b,
               d->CreateRasterizerState(&sky_r, &b.sky_rasterizer),
               function,
               "the sky's rasterizer state") ||
        failed(b, d->CreateBlendState(&blend, &b.sky_blend), function, "the sky's blend state"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }
    // The stars are added to the render target, keeping its alpha.
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    if (failed(b, d->CreateBlendState(&blend, &b.stars_blend), function, "the stars' blend state"))
    {
        return VGL_RESULT_BACKEND_ERROR;
    }
    return VGL_RESULT_SUCCESS;
}

// Frees the records of every resource, releasing their Direct3D objects.
void forget_resources(vgl_d3d11_backend& b)
{
    b.resources.for_each([&](std::uint64_t, d3d_resource* r) {
        release(r->view);
        release(r->texture);
        release(r->buffer);
        b.mem.destroy(r);
    });
    b.resources.clear();
}

// Frees the CPU copy of the clouds, and their texture.
void free_clouds(vgl_d3d11_backend& b)
{
    release(b.cloud_sampler);
    release(b.cloud_view);
    release(b.cloud_texture);
    if (b.cloud_data != nullptr)
    {
        b.mem.deallocate(b.cloud_data, b.cloud_levels.bytes, 1);
    }
    b.cloud_data = nullptr;
    b.cloud_levels = {};
}

// Frees the CPU copy of the stars, and their buffer.
void free_stars(vgl_d3d11_backend& b)
{
    release(b.stars_buffer);
    if (b.star_vertices != nullptr)
    {
        b.mem.deallocate(b.star_vertices,
                         std::size_t{b.star_count} * vgl::backends::star_vertex_count *
                             sizeof(vgl::backends::star_vertex),
                         alignof(vgl::backends::star_vertex));
    }
    b.star_vertices = nullptr;
    b.star_count = 0;
}

// Releases everything the backend holds of its device, and the device itself.
void stop(vgl_d3d11_backend& b)
{
    forget_resources(b);
    release(b.cloud_sampler);
    release(b.cloud_view);
    release(b.cloud_texture);
    release(b.stars_buffer);
    release(b.stars_blend);
    release(b.stars_layout);
    release(b.stars_pixel_shader);
    release(b.stars_vertex_shader);
    release(b.sky_blend);
    release(b.sky_rasterizer);
    release(b.sky_depth_state);
    release(b.sky_pixel_shader);
    release(b.sky_vertex_shader);
    release(b.depth_state);
    for (ID3D11RasterizerState*& r : b.rasterizers)
    {
        release(r);
    }
    release(b.sampler);
    release(b.item_buffer);
    release(b.frame_buffer);
    release(b.input_layout);
    release(b.pixel_shader);
    release(b.vertex_shader);
    release(b.dc);
    release(b.device);
    b.ready = false;
}

// Takes the device and builds the pipeline on it.
vgl_result
start(vgl_d3d11_backend& b, ID3D11Device* device, ID3D11DeviceContext* dc, const char* function)
{
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "%s: the device's feature level is below 11_0, which the backend needs",
            function);
        return VGL_RESULT_NOT_SUPPORTED;
    }
    device->AddRef();
    dc->AddRef();
    b.device = device;
    b.dc = dc;
    const vgl_result r = build_pipeline(b, function);
    if (r != VGL_RESULT_SUCCESS)
    {
        stop(b);
        return r;
    }
    b.ready = true;
    log(b, VGL_LOG_LEVEL_INFO, "%s: drawing with Direct3D 11", function);
    return VGL_RESULT_SUCCESS;
}

saved_state save_state(ID3D11DeviceContext* dc)
{
    saved_state s;
    dc->IAGetInputLayout(&s.input_layout);
    dc->IAGetPrimitiveTopology(&s.topology);
    dc->IAGetVertexBuffers(0, 1, &s.vertex_buffer, &s.vertex_stride, &s.vertex_offset);
    dc->IAGetIndexBuffer(&s.index_buffer, &s.index_format, &s.index_offset);
    dc->VSGetShader(&s.vertex_shader, nullptr, nullptr);
    dc->VSGetConstantBuffers(0, static_cast<UINT>(s.vs_constants.size()), s.vs_constants.data());
    dc->PSGetShader(&s.pixel_shader, nullptr, nullptr);
    dc->PSGetConstantBuffers(0, static_cast<UINT>(s.ps_constants.size()), s.ps_constants.data());
    dc->PSGetShaderResources(0, texture_slots, s.ps_resources.data());
    dc->PSGetSamplers(0, static_cast<UINT>(s.ps_samplers.size()), s.ps_samplers.data());
    dc->RSGetState(&s.rasterizer);
    dc->OMGetDepthStencilState(&s.depth_stencil, &s.stencil_ref);
    dc->OMGetBlendState(&s.blend, s.blend_factor.data(), &s.sample_mask);
    return s;
}

// Restores the state and drops the references the save took.
void restore_state(ID3D11DeviceContext* dc, saved_state& s)
{
    dc->IASetInputLayout(s.input_layout);
    dc->IASetPrimitiveTopology(s.topology);
    dc->IASetVertexBuffers(0, 1, &s.vertex_buffer, &s.vertex_stride, &s.vertex_offset);
    dc->IASetIndexBuffer(s.index_buffer, s.index_format, s.index_offset);
    dc->VSSetShader(s.vertex_shader, nullptr, 0);
    dc->VSSetConstantBuffers(0, static_cast<UINT>(s.vs_constants.size()), s.vs_constants.data());
    dc->PSSetShader(s.pixel_shader, nullptr, 0);
    dc->PSSetConstantBuffers(0, static_cast<UINT>(s.ps_constants.size()), s.ps_constants.data());
    dc->PSSetShaderResources(0, texture_slots, s.ps_resources.data());
    dc->PSSetSamplers(0, static_cast<UINT>(s.ps_samplers.size()), s.ps_samplers.data());
    dc->RSSetState(s.rasterizer);
    dc->OMSetDepthStencilState(s.depth_stencil, s.stencil_ref);
    dc->OMSetBlendState(s.blend, s.blend_factor.data(), s.sample_mask);

    release(s.input_layout);
    release(s.vertex_buffer);
    release(s.index_buffer);
    release(s.vertex_shader);
    for (ID3D11Buffer*& c : s.vs_constants)
    {
        release(c);
    }
    release(s.pixel_shader);
    for (ID3D11Buffer*& c : s.ps_constants)
    {
        release(c);
    }
    for (ID3D11ShaderResourceView*& v : s.ps_resources)
    {
        release(v);
    }
    for (ID3D11SamplerState*& sampler : s.ps_samplers)
    {
        release(sampler);
    }
    release(s.rasterizer);
    release(s.depth_stencil);
    release(s.blend);
}

d3d_resource* new_record(vgl_d3d11_backend& b, vgl_frame* frame, std::uint64_t id)
{
    auto* r = b.mem.create<d3d_resource>();
    if (r == nullptr || !b.resources.insert(id, r))
    {
        b.mem.destroy(r);
        static_cast<void>(vgl_frame_resource_failed(frame, id, VGL_RESULT_OUT_OF_MEMORY));
        return nullptr;
    }
    return r;
}

// Forgets a record whose creation failed, after logging it.
void drop_record(vgl_d3d11_backend& b, vgl_frame* frame, std::uint64_t id, d3d_resource* r)
{
    release(r->view);
    release(r->texture);
    release(r->buffer);
    b.resources.erase(id);
    b.mem.destroy(r);
    static_cast<void>(vgl_frame_resource_failed(frame, id, VGL_RESULT_BACKEND_ERROR));
}

void create_index_list(vgl_d3d11_backend& b, vgl_frame* frame, const vgl_index_list_data& d)
{
    d3d_resource* r = new_record(b, frame, d.resource);
    if (r == nullptr)
    {
        return;
    }
    r->kind = resource_kind::index_list;
    r->index_format = d.index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(std::size_t{d.index_count} * d.index_size);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = d.indices;
    if (failed(b,
               b.device->CreateBuffer(&desc, &data, &r->buffer),
               "vgl_d3d11_backend_draw",
               "an index buffer"))
    {
        drop_record(b, frame, d.resource, r);
        return;
    }
    static_cast<void>(
        vgl_frame_resource_handle_set(frame, d.resource, reinterpret_cast<std::uintptr_t>(r)));
}

void create_mesh(vgl_d3d11_backend& b, vgl_frame* frame, const vgl_mesh_data& d)
{
    const d3d_resource* indices = b.resources.find(d.index_list);
    if (indices == nullptr || indices->kind != resource_kind::index_list)
    {
        // Its index list failed (docs/render-interface.md, section 4.2).
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_NOT_FOUND));
        return;
    }
    if (d.samples == nullptr || d.sample_grid < 2 || d.sample_size != VGL_TERRAIN_SAMPLE_SIZE ||
        d.vertex_size != VGL_TERRAIN_VERTEX_SIZE)
    {
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_NOT_SUPPORTED));
        return;
    }
    d3d_resource* r = new_record(b, frame, d.resource);
    if (r == nullptr)
    {
        return;
    }
    r->kind = resource_kind::mesh;
    // A grid of g x g vertices and its skirts' 4 (g - 1) (docs/render-interface.md, section 6).
    const auto grid = static_cast<std::uint32_t>(
        std::lround(std::sqrt(8.0 + static_cast<double>(d.vertex_count)) - 2.0));
    if (grid > 1 && std::size_t{grid} * grid + 4 * (std::size_t{grid} - 1) == d.vertex_count)
    {
        r->grid = grid;
    }
    // Vertex heights are floats, so a skirt's is below the lowest terrain height as a float.
    r->min_height = static_cast<float>(d.min_height);
    r->sample_grid = d.sample_grid;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(std::size_t{d.vertex_count} * d.vertex_size);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = d.vertices;
    if (failed(b,
               b.device->CreateBuffer(&desc, &data, &r->buffer),
               "vgl_d3d11_backend_draw",
               "a vertex buffer"))
    {
        drop_record(b, frame, d.resource, r);
        return;
    }

    // The samples, as two 32-bit unsigned integers per texel: the height's bits and the
    // normal's two halves, read back and interpolated in the shader.
    D3D11_TEXTURE2D_DESC t{};
    t.Width = d.sample_grid;
    t.Height = d.sample_grid;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R32G32_UINT;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_IMMUTABLE;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA samples{};
    samples.pSysMem = d.samples;
    samples.SysMemPitch = d.sample_grid * VGL_TERRAIN_SAMPLE_SIZE;
    if (failed(b,
               b.device->CreateTexture2D(&t, &samples, &r->texture),
               "vgl_d3d11_backend_draw",
               "a samples texture") ||
        failed(b,
               b.device->CreateShaderResourceView(r->texture, nullptr, &r->view),
               "vgl_d3d11_backend_draw",
               "a shader resource view"))
    {
        drop_record(b, frame, d.resource, r);
        return;
    }
    static_cast<void>(
        vgl_frame_resource_handle_set(frame, d.resource, reinterpret_cast<std::uintptr_t>(r)));
}

void create_texture(vgl_d3d11_backend& b, vgl_frame* frame, const vgl_texture_data& d)
{
    if ((d.format != VGL_TEXTURE_FORMAT_RGBA8 && d.format != VGL_TEXTURE_FORMAT_RGBA8_SRGB) ||
        d.mip_count == 0 || d.mip_count > D3D11_REQ_MIP_LEVELS)
    {
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_NOT_SUPPORTED));
        return;
    }
    d3d_resource* r = new_record(b, frame, d.resource);
    if (r == nullptr)
    {
        return;
    }
    r->kind = resource_kind::texture;
    D3D11_TEXTURE2D_DESC t{};
    t.Width = d.width;
    t.Height = d.height;
    t.MipLevels = d.mip_count;
    t.ArraySize = 1;
    t.Format = d.format == VGL_TEXTURE_FORMAT_RGBA8_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                         : DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_IMMUTABLE;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::array<D3D11_SUBRESOURCE_DATA, D3D11_REQ_MIP_LEVELS> levels{};
    std::uint32_t w = d.width;
    for (std::uint32_t level = 0; level < d.mip_count; ++level)
    {
        levels[level].pSysMem = d.mip_data[level];
        levels[level].SysMemPitch = w * 4;
        w = std::max<std::uint32_t>(1, w / 2);
    }
    if (failed(b,
               b.device->CreateTexture2D(&t, levels.data(), &r->texture),
               "vgl_d3d11_backend_draw",
               "a texture") ||
        failed(b,
               b.device->CreateShaderResourceView(r->texture, nullptr, &r->view),
               "vgl_d3d11_backend_draw",
               "a shader resource view"))
    {
        drop_record(b, frame, d.resource, r);
        return;
    }
    static_cast<void>(
        vgl_frame_resource_handle_set(frame, d.resource, reinterpret_cast<std::uintptr_t>(r)));
}

void destroy_resource(vgl_d3d11_backend& b, std::uint64_t id)
{
    d3d_resource* r = b.resources.find(id);
    if (r == nullptr)
    {
        return; // It failed to create.
    }
    release(r->view);
    release(r->texture);
    release(r->buffer);
    b.resources.erase(id);
    b.mem.destroy(r);
}

// Applies the frame's resource lists in their order (docs/render-interface.md, section 4.2).
void apply_resources(vgl_d3d11_backend& b, vgl_frame* frame)
{
    const std::uint64_t* destroys = nullptr;
    std::uint32_t count = 0;
    static_cast<void>(vgl_frame_destroys_get(frame, &destroys, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        destroy_resource(b, destroys[i]);
    }
    const vgl_index_list_data* index_lists = nullptr;
    static_cast<void>(vgl_frame_index_lists_get(frame, &index_lists, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        create_index_list(b, frame, index_lists[i]);
    }
    const vgl_mesh_data* meshes = nullptr;
    static_cast<void>(vgl_frame_meshes_get(frame, &meshes, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        create_mesh(b, frame, meshes[i]);
    }
    const vgl_texture_data* textures = nullptr;
    static_cast<void>(vgl_frame_textures_get(frame, &textures, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        create_texture(b, frame, textures[i]);
    }
}

std::array<float, 16> to_float(const vgl_dmat4& m)
{
    std::array<float, 16> f{};
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        f[i] = static_cast<float>(m.m[i]);
    }
    return f;
}

// The record a handle names: its address, as the create functions set it.
const d3d_resource* from_handle(std::uint64_t handle)
{
    const auto address = static_cast<std::uintptr_t>(handle);
    return reinterpret_cast<const d3d_resource*>(address); // NOLINT(performance-no-int-to-ptr)
}

const d3d_resource* resource_of(const vgl_d3d11_backend& b, std::uint64_t id, std::uint64_t handle)
{
    return handle != 0 ? from_handle(handle) : b.resources.find(id);
}

template <typename T>
bool write_constants(ID3D11DeviceContext* dc, ID3D11Buffer* buffer, const T& values)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(dc->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        return false;
    }
    std::memcpy(mapped.pData, &values, sizeof(values));
    dc->Unmap(buffer, 0);
    return true;
}

// Creates the clouds' texture with its mip levels and its sampler, once after each set or
// reset; false if it fails.
bool upload_clouds(vgl_d3d11_backend& b)
{
    if (b.cloud_view != nullptr)
    {
        return true;
    }
    const vgl::backends::cloud_levels& l = b.cloud_levels;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = l.width[0];
    desc.Height = l.height[0];
    desc.MipLevels = l.count;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::array<D3D11_SUBRESOURCE_DATA, 16> data{};
    for (std::uint32_t k = 0; k < l.count; ++k)
    {
        data[k].pSysMem = b.cloud_data + l.offset[k];
        data[k].SysMemPitch = l.width[k];
    }
    // Trilinear, round the globe in longitude, and clamped at the poles.
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    s.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    const char* function = "vgl_d3d11_backend_draw";
    if (failed(b,
               b.device->CreateTexture2D(&desc, data.data(), &b.cloud_texture),
               function,
               "the clouds' texture") ||
        failed(b,
               b.device->CreateShaderResourceView(b.cloud_texture, nullptr, &b.cloud_view),
               function,
               "the clouds' view") ||
        failed(
            b, b.device->CreateSamplerState(&s, &b.cloud_sampler), function, "the clouds' sampler"))
    {
        release(b.cloud_sampler);
        release(b.cloud_view);
        release(b.cloud_texture);
        return false;
    }
    return true;
}

// Creates the stars' vertex buffer, once after each set or reset; false if there is none.
bool upload_stars(vgl_d3d11_backend& b)
{
    if (b.stars_buffer != nullptr)
    {
        return true;
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth =
        static_cast<UINT>(std::size_t{b.star_count} * vgl::backends::star_vertex_count *
                          sizeof(vgl::backends::star_vertex));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = b.star_vertices;
    return !failed(b,
                   b.device->CreateBuffer(&desc, &data, &b.stars_buffer),
                   "vgl_d3d11_backend_draw",
                   "the stars' vertex buffer");
}

void draw_items(vgl_d3d11_backend& b,
                const vgl_frame* frame,
                const vgl_d3d11_draw_info& info,
                const vgl_dmat4& projection,
                const vgl_dmat4* view)
{
    ID3D11DeviceContext* dc = b.dc;
    frame_constants fc;
    fc.projection = to_float(projection);
    fc.debug = static_cast<std::int32_t>(info.debug_flags);
    fc.sea = info.sea_surface != VGL_FALSE ? 1 : 0;
    fc.contour_interval =
        static_cast<float>(info.contour_interval > 0.0 ? info.contour_interval : 500.0);
    // The atmosphere needs the camera and a sun: without one, it is above the camera.
    const bool atmosphere = info.atmosphere != VGL_FALSE && view != nullptr;
    vgl_dmat4 inverse{};
    const bool sky = atmosphere && vgl::backends::invert(projection, inverse);
    vgl_dvec3 sun = info.sun_direction;
    if (atmosphere)
    {
        const vgl::backends::atmosphere_view air =
            vgl::backends::atmosphere_for(*view, info.sun_direction);
        sun = vgl::backends::atmosphere_sun(*view, sun);
        for (std::size_t i = 0; i < 3; ++i)
        {
            for (std::size_t j = 0; j < 3; ++j)
            {
                fc.to_planet[i * 4 + j] = air.to_planet[j * 3 + i];
            }
        }
        fc.camera_km = air.camera_km;
        fc.camera_height = air.camera_height;
        fc.sun_planet = air.sun_planet;
        fc.atmosphere = 1;
        fc.inverse_projection = to_float(inverse);
    }
    // The far plane's depth: 0 for reversed-Z, else 1, the logarithmic convention's too.
    fc.far_depth = b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? 0.0f : 1.0f;
    // The clouds, with the atmosphere, on their own slot and sampler.
    const bool clouds =
        atmosphere && info.clouds != VGL_FALSE && b.cloud_data != nullptr && upload_clouds(b);
    if (clouds)
    {
        const double height =
            info.cloud_height > 0.0 ? info.cloud_height : vgl::backends::default_cloud_height;
        fc.cloud = {1.0f,
                    static_cast<float>(height * 1e-3),
                    static_cast<float>(vgl::backends::cloud_texel_km(b.cloud_levels.width[0])),
                    0.0f};
    }
    const bool stars = info.stars != VGL_FALSE && view != nullptr && b.star_count != 0;
    if (stars)
    {
        const std::array<float, 9> c =
            vgl::backends::celestial_to_camera(*view, info.sidereal_angle);
        for (std::size_t i = 0; i < 3; ++i)
        {
            for (std::size_t j = 0; j < 3; ++j)
            {
                fc.celestial[i * 4 + j] = c[j * 3 + i];
            }
        }
        UINT viewports = 1;
        D3D11_VIEWPORT vp{};
        dc->RSGetViewports(&viewports, &vp);
        fc.viewport = {std::max(vp.Width, 1.0f), std::max(vp.Height, 1.0f)};
    }
    // The sun in camera space: only the view's rotation applies to a direction.
    if (view != nullptr && (sun.x != 0.0 || sun.y != 0.0 || sun.z != 0.0))
    {
        const double* m = view->m;
        std::array<double, 3> light = {m[0] * sun.x + m[4] * sun.y + m[8] * sun.z,
                                       m[1] * sun.x + m[5] * sun.y + m[9] * sun.z,
                                       m[2] * sun.x + m[6] * sun.y + m[10] * sun.z};
        const double length =
            std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
        for (std::size_t k = 0; k < 3; ++k)
        {
            fc.light[k] = static_cast<float>(light[k] / length);
        }
    }
    if (b.depth == VGL_DEPTH_CONVENTION_LOGARITHMIC)
    {
        const double far_distance =
            info.logarithmic_far > 0.0 ? info.logarithmic_far : VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT;
        fc.log_depth = static_cast<float>(2.0 / std::log2(far_distance + 1.0));
    }
    if (!write_constants(dc, b.frame_buffer, fc))
    {
        log(b, VGL_LOG_LEVEL_ERROR, "vgl_d3d11_backend_draw: mapping a constant buffer failed");
        return;
    }

    dc->IASetInputLayout(b.input_layout);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(b.vertex_shader, nullptr, 0);
    dc->PSSetShader(b.pixel_shader, nullptr, 0);
    const std::array<ID3D11Buffer*, 2> constants = {b.frame_buffer, b.item_buffer};
    dc->VSSetConstantBuffers(0, 2, constants.data());
    dc->PSSetConstantBuffers(0, 2, constants.data());
    const std::array<ID3D11SamplerState*, 2> samplers = {b.sampler, b.cloud_sampler};
    dc->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    dc->RSSetState(b.rasterizers[info.mirrored != VGL_FALSE ? 1 : 0]);
    dc->OMSetDepthStencilState(b.depth_state, 0);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    const vgl_draw_layer* layers = nullptr;
    std::uint32_t layer_total = 0;
    static_cast<void>(vgl_frame_draw_layers_get(frame, &layers, &layer_total));
    const vgl_draw_item* items = nullptr;
    std::uint32_t count = 0;
    static_cast<void>(vgl_frame_draw_list_get(frame, &items, &count));
    std::array<ID3D11ShaderResourceView*, texture_slots> bound = {};
    bound[clouds_slot] = clouds ? b.cloud_view : nullptr;
    dc->PSSetShaderResources(0, texture_slots, bound.data());
    const ID3D11Buffer* bound_indices = nullptr;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const vgl_draw_item& item = items[i];
        const d3d_resource* mesh = resource_of(b, item.mesh, item.mesh_handle);
        const d3d_resource* list = resource_of(b, item.index_list, item.index_list_handle);
        if (mesh == nullptr || list == nullptr)
        {
            continue; // It could not be created (docs/render-interface.md, section 4.2).
        }
        item_constants ic;
        std::copy(std::begin(item.model_view), std::end(item.model_view), ic.model_view.begin());
        std::copy(std::begin(item.geoid_heights), std::end(item.geoid_heights), ic.geoid.begin());
        ic.centre = {static_cast<float>(item.centre.x),
                     static_cast<float>(item.centre.y),
                     static_cast<float>(item.centre.z)};
        ic.min_height = mesh->min_height;
        ic.sample_cells = static_cast<float>(mesh->sample_grid - 1);
        ic.cells = static_cast<float>(mesh->grid > 1 ? mesh->grid - 1 : 0);
        ic.level = static_cast<float>(item.tile.level);

        // The top layers, if there are more than a draw can blend.
        std::array<ID3D11ShaderResourceView*, texture_slots> views = {};
        views[clouds_slot] = bound[clouds_slot];
        const std::uint32_t used = std::min(item.layer_count, max_layers);
        const std::uint32_t first = item.first_layer + item.layer_count - used;
        std::uint32_t drawn_layers = 0;
        for (std::uint32_t k = 0; k < used && first + k < layer_total; ++k)
        {
            const vgl_draw_layer& layer = layers[first + k];
            const d3d_resource* texture = resource_of(b, layer.texture, layer.texture_handle);
            if (texture == nullptr || texture->view == nullptr)
            {
                continue;
            }
            const std::uint32_t slot = drawn_layers++;
            views[slot] = texture->view;
            std::copy(std::begin(layer.uv_scale_offset),
                      std::end(layer.uv_scale_offset),
                      ic.layer_uv.begin() + static_cast<std::ptrdiff_t>(std::size_t{4} * slot));
            ic.layer_opacity[slot] = layer.opacity;
        }
        ic.layer_count = static_cast<std::int32_t>(drawn_layers);
        // The top layer's scale is a little below 1 for the tile's own imagery (its border
        // aside), and halves with each level of an ancestor.
        const float scale =
            drawn_layers != 0 ? ic.layer_uv[4 * std::size_t{drawn_layers - 1}] : 1.0f;
        ic.imagery_coarser =
            static_cast<float>(std::max(std::round(-std::log2(std::max(scale, 1e-6f))), 0.0f));
        views[samples_slot] = mesh->view;
        if (views != bound)
        {
            dc->PSSetShaderResources(0, texture_slots, views.data());
            bound = views;
        }
        if (!write_constants(dc, b.item_buffer, ic))
        {
            continue;
        }
        if (bound_indices != list->buffer)
        {
            dc->IASetIndexBuffer(list->buffer, list->index_format, 0);
            bound_indices = list->buffer;
        }
        const UINT stride = VGL_TERRAIN_VERTEX_SIZE;
        const UINT offset = 0;
        ID3D11Buffer* vertices = mesh->buffer;
        dc->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
        dc->DrawIndexed(item.index_count, 0, 0);
    }
    if (stars && upload_stars(b))
    {
        // A quad per star, where nothing was drawn, added to the render target.
        const UINT stride = sizeof(vgl::backends::star_vertex);
        const UINT offset = 0;
        dc->IASetInputLayout(b.stars_layout);
        dc->IASetVertexBuffers(0, 1, &b.stars_buffer, &stride, &offset);
        dc->VSSetShader(b.stars_vertex_shader, nullptr, 0);
        dc->PSSetShader(b.stars_pixel_shader, nullptr, 0);
        dc->RSSetState(b.sky_rasterizer);
        dc->OMSetDepthStencilState(b.sky_depth_state, 0);
        dc->OMSetBlendState(b.stars_blend, nullptr, 0xFFFFFFFF);
        dc->Draw(b.star_count * vgl::backends::star_vertex_count, 0);
    }
    if (sky)
    {
        // One triangle, without vertex buffers, where nothing was drawn.
        dc->IASetInputLayout(nullptr);
        dc->VSSetShader(b.sky_vertex_shader, nullptr, 0);
        dc->PSSetShader(b.sky_pixel_shader, nullptr, 0);
        dc->RSSetState(b.sky_rasterizer);
        dc->OMSetDepthStencilState(b.sky_depth_state, 0);
        dc->OMSetBlendState(b.sky_blend, nullptr, 0xFFFFFFFF);
        dc->Draw(3, 0);
    }
}

bool finite(const vgl_dmat4* m)
{
    return m == nullptr || std::ranges::all_of(m->m, [](double v) { return std::isfinite(v); });
}

} // namespace

extern "C" VGL_D3D11_API vgl_result
vgl_d3d11_backend_create(vgl_context* context,
                         const vgl_d3d11_backend_create_info* info_in,
                         vgl_d3d11_backend** out_backend)
{
    if (out_backend == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_backend = nullptr;
    if (context == nullptr || info_in == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_d3d11_backend_create_info info{};
    vgl_result r =
        vgl::common::read_versioned(info_in, sizeof(vgl_d3d11_backend_create_info), info);
    if (r != VGL_RESULT_SUCCESS || info.device == nullptr || info.device_context == nullptr ||
        info.depth_convention > VGL_DEPTH_CONVENTION_CONVENTIONAL)
    {
        const char* problem = r == VGL_RESULT_NOT_SUPPORTED
                                  ? "info has fields of a newer vgl; update the library"
                              : r != VGL_RESULT_SUCCESS ? "info->struct_size is too small"
                              : info.device == nullptr || info.device_context == nullptr
                                  ? "info->device or info->device_context is null"
                                  : "info->depth_convention is unknown";
        std::array<char, message_size> text{};
        static_cast<void>(
            std::snprintf(text.data(), text.size(), "vgl_d3d11_backend_create: %s", problem));
        static_cast<void>(vgl_context_log(context, VGL_LOG_LEVEL_ERROR, text.data()));
        return r == VGL_RESULT_NOT_SUPPORTED ? r : VGL_RESULT_INVALID_ARGUMENT;
    }

    vgl_allocator_interface allocator{};
    static_cast<void>(vgl_context_allocator_get(context, &allocator));
    const memory mem(allocator);
    auto* b = mem.create<vgl_d3d11_backend>(mem);
    if (b == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    b->context = context;
    b->depth = info.depth_convention;
    r = start(*b, info.device, info.device_context, "vgl_d3d11_backend_create");
    if (r != VGL_RESULT_SUCCESS)
    {
        mem.destroy(b);
        return r;
    }
    const vgl_texture_format format = VGL_TEXTURE_FORMAT_RGBA8;
    static_cast<void>(vgl_context_texture_formats_set(context, &format, 1));
    *out_backend = b;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_D3D11_API vgl_result vgl_d3d11_backend_destroy(vgl_d3d11_backend* backend)
{
    if (backend == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    stop(*backend);
    free_stars(*backend);
    free_clouds(*backend);
    // A later consumer of the context must receive every resource again.
    static_cast<void>(vgl_context_resources_reset(backend->context));
    const memory mem = backend->mem;
    mem.destroy(backend);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_D3D11_API vgl_result vgl_d3d11_backend_stars_set(vgl_d3d11_backend* backend,
                                                                const vgl_star* stars,
                                                                std::uint32_t star_count)
{
    if (backend == nullptr || (stars == nullptr && star_count != 0) ||
        star_count > vgl::backends::max_stars)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_d3d11_backend& b = *backend;
    for (std::uint32_t i = 0; i < star_count; ++i)
    {
        if (!vgl::backends::star_is_valid(stars[i]))
        {
            log(b,
                VGL_LOG_LEVEL_ERROR,
                "vgl_d3d11_backend_stars_set: star %u is invalid: check that its direction is of "
                "unit length, that its values are finite, its colour from 0 to 1, and reserved 0",
                static_cast<unsigned>(i));
            return VGL_RESULT_INVALID_ARGUMENT;
        }
    }
    const std::size_t count = std::size_t{star_count} * vgl::backends::star_vertex_count;
    vgl::backends::star_vertex* vertices = nullptr;
    if (count != 0)
    {
        vertices = static_cast<vgl::backends::star_vertex*>(b.mem.allocate(
            count * sizeof(vgl::backends::star_vertex), alignof(vgl::backends::star_vertex)));
        if (vertices == nullptr)
        {
            return VGL_RESULT_OUT_OF_MEMORY;
        }
        for (std::uint32_t i = 0; i < star_count; ++i)
        {
            vgl::backends::star_quad(stars[i],
                                     vertices + std::size_t{i} * vgl::backends::star_vertex_count);
        }
    }
    free_stars(b);
    b.star_vertices = vertices;
    b.star_count = star_count;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_D3D11_API vgl_result vgl_d3d11_backend_clouds_set(vgl_d3d11_backend* backend,
                                                                 const std::uint8_t* coverage,
                                                                 std::uint32_t width,
                                                                 std::uint32_t height)
{
    if (backend == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_d3d11_backend& b = *backend;
    const bool removal = coverage == nullptr && width == 0 && height == 0;
    if (!removal && (coverage == nullptr || !vgl::backends::cloud_size_is_valid(width, height)))
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_d3d11_backend_clouds_set: the coverage is null, or %ux%u is outside 2x2 to "
            "16384x8192",
            static_cast<unsigned>(width),
            static_cast<unsigned>(height));
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    std::uint8_t* data = nullptr;
    vgl::backends::cloud_levels levels;
    if (!removal)
    {
        levels = vgl::backends::cloud_levels_of(width, height);
        data = static_cast<std::uint8_t*>(b.mem.allocate(levels.bytes, 1));
        if (data == nullptr)
        {
            return VGL_RESULT_OUT_OF_MEMORY;
        }
        vgl::backends::cloud_levels_build(coverage, levels, data);
    }
    free_clouds(b);
    b.cloud_data = data;
    b.cloud_levels = levels;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_D3D11_API vgl_result vgl_d3d11_backend_reset(vgl_d3d11_backend* backend,
                                                            ID3D11Device* device,
                                                            ID3D11DeviceContext* device_context)
{
    if (backend == nullptr || device == nullptr || device_context == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    // The old device's objects are released, never used again: releasing is all a removed
    // device allows, and frees their memory.
    stop(*backend);
    static_cast<void>(vgl_context_resources_reset(backend->context));
    return start(*backend, device, device_context, "vgl_d3d11_backend_reset");
}

extern "C" VGL_D3D11_API vgl_result vgl_d3d11_backend_draw(vgl_d3d11_backend* backend,
                                                           const vgl_d3d11_draw_info* info_in)
{
    if (backend == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_d3d11_backend& b = *backend;
    vgl_d3d11_draw_info info{};
    vgl_result r = vgl::common::read_versioned(info_in, sizeof(vgl_d3d11_draw_info), info);
    if (r != VGL_RESULT_SUCCESS)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_d3d11_backend_draw: %s",
            r == VGL_RESULT_NOT_SUPPORTED ? "info has fields of a newer vgl; update the library"
                                          : "info->struct_size is too small");
        return r == VGL_RESULT_NOT_SUPPORTED ? r : VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_dvec3 sun = info.sun_direction;
    if (!finite(info.view_matrix) || !finite(info.projection_matrix) || !std::isfinite(sun.x) ||
        !std::isfinite(sun.y) || !std::isfinite(sun.z) || !std::isfinite(info.logarithmic_far) ||
        info.logarithmic_far < 0.0)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_d3d11_backend_draw: info has a matrix or sun direction that is not finite, or a "
            "negative far distance");
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    constexpr std::uint32_t known_flags =
        VGL_D3D11_DEBUG_FLAG_WIREFRAME | VGL_D3D11_DEBUG_FLAG_LEVEL_COLORS |
        VGL_D3D11_DEBUG_FLAG_IMAGERY_SOURCE | VGL_D3D11_DEBUG_FLAG_HEIGHT_COLORS |
        VGL_D3D11_DEBUG_FLAG_CONTOURS;
    if ((info.debug_flags & ~known_flags) != 0 || info.reserved != 0 || info.reserved2 != 0 ||
        info.reserved3 != 0 || info.reserved4 != 0 || !std::isfinite(info.sidereal_angle) ||
        info.reserved5 != 0 || !std::isfinite(info.cloud_height) || info.cloud_height < 0.0 ||
        !std::isfinite(info.contour_interval) || info.contour_interval < 0.0)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_d3d11_backend_draw: info->debug_flags has unknown flags, info->reserved is not "
            "zero, or info->contour_interval is negative or not finite");
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (!b.ready)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_d3d11_backend_draw: the backend has no device since a failed reset; call "
            "vgl_d3d11_backend_reset again");
        return VGL_RESULT_INVALID_OPERATION;
    }

    vgl_view view{};
    view.struct_size = sizeof(view);
    const bool has_view = vgl_context_view_get(b.context, &view) == VGL_RESULT_SUCCESS;
    // The host's matrices, or else the context's view, or else none: nothing is selected then.
    const vgl_dmat4* projection = has_view ? &view.projection_matrix : nullptr;
    const vgl_dmat4* view_matrix = has_view ? &view.view_matrix : nullptr;
    if (info.projection_matrix != nullptr)
    {
        projection = info.projection_matrix;
    }
    if (info.view_matrix != nullptr)
    {
        view_matrix = info.view_matrix;
    }

    vgl_frame_begin_info frame_info{};
    frame_info.struct_size = sizeof(frame_info);
    frame_info.upload_budget = info.upload_budget;
    frame_info.view_matrix = info.view_matrix;
    vgl_frame* frame = nullptr;
    r = vgl_context_frame_begin(b.context, &frame_info, &frame);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    saved_state saved = save_state(b.dc);
    apply_resources(b, frame);
    if (projection != nullptr)
    {
        draw_items(b, frame, info, *projection, view_matrix);
    }
    restore_state(b.dc, saved);
    return vgl_frame_end(frame);
}
