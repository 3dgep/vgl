// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Direct3D 11 sample (design document, section 11): the globe drawn by the Direct3D 11 backend
// in a plain Win32 window. The host owns the window, the device, the camera and the render
// targets; vgl draws into them. It does what the OpenGL sample does, with the same controls
// (samples/gl/main.cpp lists them) and the same overlay.
//
//   vgl-d3d11 [--data <directory> | --pack <uri>...] [--depth logarithmic|reversed|conventional]
//             [--warp] [--check] [--save <directory>]
//
// It draws the sample set (samples/data/README.md) from the directory given with --data, or else
// from the one in the environment variable VGL_SAMPLE_DATA, or else from the source tree's
// samples/data. Instead, --pack, repeated, draws the packs named. --warp draws with WARP, the
// software rasterizer, which the sample also falls back to without a hardware device.
//
// With --check, the sample runs without a window: it draws a few views offscreen with each depth
// convention, and checks what it draws, that the conventions agree, that the backend restores
// the host's state, and that it draws again after a reset. It exits with 1 on a failure, so
// that it doubles as a test of the backend. --save writes the images it checks as PPM files.

#include "navigation.hpp"
#include "overlay.hpp"
#include "sample_camera.hpp"
#include "sample_check.hpp"
#include "sample_data.hpp"
#include "stars.hpp"
#include <dxgi1_2.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <vgl/d3d11.hpp>
#include <vgl/defaults.hpp>
#include <vgl/vgl.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>
#include <windowsx.h>

#ifndef VGL_SAMPLE_DATA_DIR
#    define VGL_SAMPLE_DATA_DIR "."
#endif

// Dear ImGui's Win32 message handler, which its header declares only for C++ code that asks.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window,
                                                             UINT message,
                                                             WPARAM w,
                                                             LPARAM l);

namespace
{

using sample::clear_color;
using sample::depth_name;
using sample::expect;
using sample::field_of_view;
using sample::image;
using sample::places;

// A COM pointer, released when it goes.
template <typename T>
class com
{
public:
    com() = default;
    com(const com&) = delete;
    com& operator=(const com&) = delete;
    com(com&& other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
    com& operator=(com&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            p_ = std::exchange(other.p_, nullptr);
        }
        return *this;
    }
    ~com() { reset(); }

    void reset()
    {
        if (p_ != nullptr)
        {
            p_->Release();
            p_ = nullptr;
        }
    }
    [[nodiscard]] T* get() const { return p_; }
    T** put()
    {
        reset();
        return &p_;
    }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// The device, and the host's render targets: color and a 32-bit floating-point depth buffer.
// ---------------------------------------------------------------------------------------------

struct gpu
{
    com<ID3D11Device> device;
    com<ID3D11DeviceContext> dc;
    std::string adapter;
};

bool gpu_create(gpu& g, bool warp)
{
    const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = E_FAIL;
    if (!warp)
    {
        hr = D3D11CreateDevice(nullptr,
                               D3D_DRIVER_TYPE_HARDWARE,
                               nullptr,
                               0,
                               &wanted,
                               1,
                               D3D11_SDK_VERSION,
                               g.device.put(),
                               nullptr,
                               g.dc.put());
    }
    if (FAILED(hr))
    {
        hr = D3D11CreateDevice(nullptr,
                               D3D_DRIVER_TYPE_WARP,
                               nullptr,
                               0,
                               &wanted,
                               1,
                               D3D11_SDK_VERSION,
                               g.device.put(),
                               nullptr,
                               g.dc.put());
    }
    if (FAILED(hr))
    {
        return false;
    }
    com<IDXGIDevice> dxgi;
    com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(g.device->QueryInterface(__uuidof(IDXGIDevice),
                                           reinterpret_cast<void**>(dxgi.put()))) &&
        SUCCEEDED(dxgi->GetAdapter(adapter.put())) && SUCCEEDED(adapter->GetDesc(&desc)))
    {
        std::array<char, 256> name{};
        WideCharToMultiByte(CP_UTF8,
                            0,
                            desc.Description,
                            -1,
                            name.data(),
                            static_cast<int>(name.size()),
                            nullptr,
                            nullptr);
        g.adapter = name.data();
    }
    return true;
}

struct target
{
    int width = 0;
    int height = 0;
    com<ID3D11Texture2D> color; // Offscreen targets only; a swap chain owns its own.
    com<ID3D11RenderTargetView> color_view;
    com<ID3D11Texture2D> depth;
    com<ID3D11DepthStencilView> depth_view;
};

bool depth_create(const gpu& g, target& t)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(t.width);
    d.Height = static_cast<UINT>(t.height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    return SUCCEEDED(g.device->CreateTexture2D(&d, nullptr, t.depth.put())) &&
           SUCCEEDED(g.device->CreateDepthStencilView(t.depth.get(), nullptr, t.depth_view.put()));
}

// An offscreen target, for the check.
bool offscreen_create(const gpu& g, target& t, int width, int height)
{
    t.width = width;
    t.height = height;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(g.device->CreateTexture2D(&d, nullptr, t.color.put())) &&
           SUCCEEDED(
               g.device->CreateRenderTargetView(t.color.get(), nullptr, t.color_view.put())) &&
           depth_create(g, t);
}

// The target's pixels, rows from the top, through a staging copy.
image read_image(const gpu& g, const target& t)
{
    image i;
    i.width = t.width;
    i.height = t.height;
    D3D11_TEXTURE2D_DESC d{};
    t.color->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    com<ID3D11Texture2D> staging;
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, staging.put())))
    {
        return i;
    }
    g.dc->CopyResource(staging.get(), t.color.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g.dc->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        return i;
    }
    const std::size_t row = static_cast<std::size_t>(t.width) * 4;
    i.pixels.resize(row * static_cast<std::size_t>(t.height));
    for (int y = 0; y < t.height; ++y)
    {
        std::memcpy(i.pixels.data() + static_cast<std::size_t>(y) * row,
                    static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch,
                    row);
    }
    g.dc->Unmap(staging.get(), 0);
    return i;
}

// ---------------------------------------------------------------------------------------------
// Drawing a frame
// ---------------------------------------------------------------------------------------------

struct scene
{
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_REVERSED_Z;
    sample::camera camera;
    sample::camera frozen; // Where tiles are selected for while the selection is frozen.
    sample::debug_settings debug;
};

// A right-handed perspective with depth from 0 to 1, as Direct3D maps it.
vgl_dmat4 perspective_zero_to_one(double fovy, double aspect, double near_plane, double far_plane)
{
    const double f = 1.0 / std::tan(fovy / 2.0);
    vgl_dmat4 m{};
    m.m[0] = f / aspect;
    m.m[5] = f;
    m.m[10] = far_plane / (near_plane - far_plane);
    m.m[11] = -1.0;
    m.m[14] = near_plane * far_plane / (near_plane - far_plane);
    return m;
}

vgl_dmat4 projection_for(const scene& s, const sample::camera& c, double aspect)
{
    const double fovy = vgl::degrees_to_radians(field_of_view);
    switch (s.depth)
    {
    case VGL_DEPTH_CONVENTION_REVERSED_Z:
        // Already from 1 at the near plane to 0 at infinity.
        return sample::perspective_reversed(fovy, aspect, 1.0);
    case VGL_DEPTH_CONVENTION_CONVENTIONAL:
        // A near plane that grows with the altitude, which is all a conventional depth buffer
        // can do at this scale.
        return perspective_zero_to_one(
            fovy, aspect, std::max(1.0, c.altitude * 0.05), c.altitude + 2.0e7);
    default:
        // The logarithmic convention writes its own depth; the planes do not matter.
        return perspective_zero_to_one(fovy, aspect, 1.0, 1.0e9);
    }
}

// Sets the view that the next update selects tiles for: the camera's, or the frozen one's.
void view_set(vgl::context& ctx, const scene& s, int width, int height)
{
    const sample::camera& c = s.debug.freeze ? s.frozen : s.camera;
    auto view = vgl::init<vgl_view>();
    view.view_matrix = sample::camera_view_matrix(c);
    view.projection_matrix = projection_for(s, c, static_cast<double>(width) / height);
    view.viewport_width = static_cast<std::uint32_t>(width);
    view.viewport_height = static_cast<std::uint32_t>(height);
    view.max_screen_space_error = s.debug.max_error;
    static_cast<void>(ctx.view_set(&view));
}

// Clears the target for the depth convention, binds it and sets the viewport.
void clear(const gpu& g, const scene& s, const target& t)
{
    const std::array<float, 4> color{clear_color[0], clear_color[1], clear_color[2], 1.0f};
    g.dc->ClearRenderTargetView(t.color_view.get(), color.data());
    g.dc->ClearDepthStencilView(t.depth_view.get(),
                                D3D11_CLEAR_DEPTH,
                                s.depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? 0.0f : 1.0f,
                                0);
    ID3D11RenderTargetView* rtv = t.color_view.get();
    g.dc->OMSetRenderTargets(1, &rtv, t.depth_view.get());
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(t.width);
    viewport.Height = static_cast<float>(t.height);
    viewport.MaxDepth = 1.0f;
    g.dc->RSSetViewports(1, &viewport);
}

// Has the backend draw the globe into the bound target, from the live camera, which differs
// from the view the tiles were selected for while the selection is frozen.
vgl::result draw(vgl::d3d11_backend& backend, const scene& s, int width, int height)
{
    const vgl_dmat4 view = sample::camera_view_matrix(s.camera);
    const vgl_dmat4 projection = projection_for(s, s.camera, static_cast<double>(width) / height);
    auto info = vgl::init<vgl_d3d11_draw_info>();
    info.view_matrix = &view;
    info.projection_matrix = &projection;
    if (s.debug.sunlight)
    {
        info.sun_direction = sample::sun_direction(s.debug);
    }
    info.debug_flags = sample::debug_flags(s.debug);
    info.contour_interval = s.debug.contour_interval;
    info.sea_surface = s.debug.sea ? VGL_TRUE : VGL_FALSE;
    info.atmosphere = s.debug.atmosphere ? VGL_TRUE : VGL_FALSE;
    info.stars = s.debug.stars ? VGL_TRUE : VGL_FALSE;
    info.sidereal_angle =
        sample::sidereal_angle(s.debug.time_of_day, s.debug.day_of_year, s.debug.year);
    return backend.draw(&info);
}

vgl::expected<vgl::d3d11_backend>
backend_create(vgl::context& ctx, const gpu& g, vgl_depth_convention depth)
{
    auto info = vgl::init<vgl_d3d11_backend_create_info>();
    info.depth_convention = depth;
    info.device = g.device.get();
    info.device_context = g.dc.get();
    vgl::expected<vgl::d3d11_backend> backend = vgl::d3d11_backend::create(ctx, info);
    if (backend)
    {
        expect(backend->stars_set(sample::sky_stars()) == vgl::result::success,
               "the stars are set");
    }
    return backend;
}

// ---------------------------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------------------------

// Host state set before each draw in the check, which the backend must leave as it was.
struct sentinel
{
    com<ID3D11RasterizerState> rasterizer;
    com<ID3D11DepthStencilState> depth_stencil;
    com<ID3D11BlendState> blend;
};

bool sentinel_create(const gpu& g, sentinel& s)
{
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(g.device->CreateRasterizerState(&r, s.rasterizer.put())) &&
           SUCCEEDED(g.device->CreateDepthStencilState(&ds, s.depth_stencil.put())) &&
           SUCCEEDED(g.device->CreateBlendState(&b, s.blend.put()));
}

void sentinel_set(const gpu& g, const sentinel& s)
{
    g.dc->RSSetState(s.rasterizer.get());
    g.dc->OMSetDepthStencilState(s.depth_stencil.get(), 7);
    const std::array<float, 4> factor{0.25f, 0.5f, 0.75f, 1.0f};
    g.dc->OMSetBlendState(s.blend.get(), factor.data(), 0xF0F0F0F0);
    g.dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    g.dc->IASetInputLayout(nullptr);
    g.dc->VSSetShader(nullptr, nullptr, 0);
    g.dc->PSSetShader(nullptr, nullptr, 0);
}

void sentinel_check(const gpu& g, const sentinel& s, const std::string& where)
{
    com<ID3D11RasterizerState> r;
    g.dc->RSGetState(r.put());
    expect(r.get() == s.rasterizer.get(), "the rasterizer state is restored: " + where);
    com<ID3D11DepthStencilState> ds;
    UINT ref = 0;
    g.dc->OMGetDepthStencilState(ds.put(), &ref);
    expect(ds.get() == s.depth_stencil.get() && ref == 7,
           "the depth-stencil state is restored: " + where);
    com<ID3D11BlendState> b;
    std::array<float, 4> factor{};
    UINT mask = 0;
    g.dc->OMGetBlendState(b.put(), factor.data(), &mask);
    expect(b.get() == s.blend.get() && factor[1] == 0.5f && mask == 0xF0F0F0F0,
           "the blend state is restored: " + where);
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    g.dc->IAGetPrimitiveTopology(&topology);
    expect(topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "the topology is restored: " + where);
    com<ID3D11InputLayout> layout;
    g.dc->IAGetInputLayout(layout.put());
    com<ID3D11VertexShader> vs;
    g.dc->VSGetShader(vs.put(), nullptr, nullptr);
    com<ID3D11PixelShader> ps;
    g.dc->PSGetShader(ps.put(), nullptr, nullptr);
    expect(!layout && !vs && !ps, "the input layout and shaders are restored: " + where);
    std::array<ID3D11ShaderResourceView*, 5> views{};
    g.dc->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    bool none = true;
    for (ID3D11ShaderResourceView* v : views)
    {
        none = none && v == nullptr;
        if (v != nullptr)
        {
            v->Release();
        }
    }
    expect(none, "the pixel shader's resources are restored: " + where);
}

struct options
{
    std::string data;               // The sample set's directory, from --data.
    std::vector<std::string> packs; // From --pack.
    std::string stars;              // A star catalogue, from --stars.
    bool warp = false;
    bool check = false;
    std::string save;
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_REVERSED_Z;
};

// Updates and draws a place until loading settles; returns the frames it took.
int settle(vgl::context& ctx,
           vgl::d3d11_backend& backend,
           const gpu& g,
           const sentinel& host,
           scene& s,
           const target& t,
           bool& settled)
{
    int frames = 0;
    int quiet = 0;
    const std::string where = std::string(depth_name(s.depth));
    while (frames < 3000 && quiet < 5)
    {
        view_set(ctx, s, t.width, t.height);
        expect(ctx.update() == vgl::result::success, "vgl_context_update succeeds");
        clear(g, s, t);
        sentinel_set(g, host);
        expect(draw(backend, s, t.width, t.height) == vgl::result::success,
               "vgl_d3d11_backend_draw succeeds: " + where);
        sentinel_check(g, host, where);
        quiet = sample::idle(ctx.statistics_get()) ? quiet + 1 : 0;
        ++frames;
        if (quiet == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    settled = quiet >= 5;
    return frames;
}

int run_check(const options& o, const gpu& g, vgl::context& ctx)
{
    constexpr int width = 640;
    constexpr int height = 360;
    sentinel host;
    expect(sentinel_create(g, host), "the host's states are created");
    target t;
    expect(offscreen_create(g, t, width, height), "the render target is created");

    const std::vector<vgl_depth_convention> conventions{VGL_DEPTH_CONVENTION_REVERSED_Z,
                                                        VGL_DEPTH_CONVENTION_LOGARITHMIC,
                                                        VGL_DEPTH_CONVENTION_CONVENTIONAL};
    std::vector<std::vector<image>> images(conventions.size());
    std::vector<std::vector<image>> atmosphere_images(conventions.size());
    for (std::size_t c = 0; c < conventions.size(); ++c)
    {
        scene s;
        s.depth = conventions[c];
        vgl::expected<vgl::d3d11_backend> backend = backend_create(ctx, g, s.depth);
        expect(backend.has_value(),
               std::string("the backend is created for ") + depth_name(s.depth));
        if (!backend)
        {
            continue;
        }
        for (const sample::place& p : places)
        {
            s.camera = p.camera;
            bool settled = false;
            const int frames = settle(ctx, *backend, g, host, s, t, settled);
            image pixels = read_image(g, t);
            sample::check_place(s.depth, p, frames, settled, ctx.statistics_get(), pixels);
            if (!o.save.empty())
            {
                sample::save_image(pixels, sample::image_path(o.save, s.depth, p.name));
            }
            images[c].push_back(std::move(pixels));

            // With the atmosphere, the same with every convention, and the host's state as it
            // was.
            scene air = s;
            air.debug.atmosphere = true;
            air.debug.stars = true;
            clear(g, air, t);
            sentinel_set(g, host);
            expect(draw(*backend, air, width, height) == vgl::result::success,
                   "vgl_d3d11_backend_draw succeeds with the atmosphere");
            sentinel_check(g, host, std::string(depth_name(s.depth)) + " with the atmosphere");
            image sky = read_image(g, t);
            if (!o.save.empty())
            {
                sample::save_image(
                    sky,
                    sample::image_path(
                        o.save, s.depth, (std::string(p.name) + "-atmosphere").c_str()));
            }
            atmosphere_images[c].push_back(std::move(sky));
        }

        // The debug views draw and leave the host's state as it was.
        s.debug.wireframe = s.debug.level_colors = s.debug.imagery_source = true;
        s.debug.height_colors = s.debug.contours = s.debug.sea = true;
        clear(g, s, t);
        sentinel_set(g, host);
        expect(draw(*backend, s, width, height) == vgl::result::success,
               "vgl_d3d11_backend_draw succeeds with the debug views");
        sentinel_check(g, host, std::string(depth_name(s.depth)) + " with the debug views");
        if (!o.save.empty())
        {
            sample::save_image(read_image(g, t), sample::image_path(o.save, s.depth, "debug"));
        }
        s.debug = {};

        // After a reset, as after a removed device, the backend draws the same view again once
        // its resources are uploaded anew.
        expect(backend->device_lost(g.device.get(), g.dc.get()) == vgl::result::success,
               std::string("vgl_d3d11_backend_reset succeeds: ") + depth_name(s.depth));
        bool settled = false;
        static_cast<void>(settle(ctx, *backend, g, host, s, t, settled));
        const image again = read_image(g, t);
        expect(settled && !images[c].empty() && sample::difference(again, images[c].back()) < 0.01,
               std::string("the backend draws the same after a reset: ") + depth_name(s.depth));
        backend->reset();
    }
    sample::compare_conventions(conventions, images);
    sample::compare_conventions(conventions, atmosphere_images);
    std::printf("\n%s\n", sample::failures() == 0 ? "all checks passed" : "checks failed");
    return sample::failures() == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// Interactive
// ---------------------------------------------------------------------------------------------

struct window_state
{
    scene* s = nullptr;
    sample::overlay* ui = nullptr;
    sample::globe_navigation* nav = nullptr;
    bool resized = false;
    double last_click = -1.0; // When the left button was last pressed, for double-clicks.
    double click_x = 0.0;
    double click_y = 0.0;
};

double seconds_now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void on_key(HWND window, window_state& in, WPARAM key)
{
    sample::debug_settings& d = in.s->debug;
    switch (key)
    {
    case VK_ESCAPE:
        DestroyWindow(window);
        break;
    case 'L':
        d.sunlight = !d.sunlight;
        break;
    case VK_F1:
        d.panel = !d.panel;
        break;
    case VK_F2:
        d.wireframe = !d.wireframe;
        break;
    case VK_F3:
        d.level_colors = !d.level_colors;
        break;
    case VK_F4:
        d.imagery_source = !d.imagery_source;
        break;
    case VK_F5:
        d.freeze = !d.freeze;
        break;
    case VK_F6:
        d.height_colors = !d.height_colors;
        break;
    case VK_F7:
        d.contours = !d.contours;
        break;
    case VK_F8:
        d.sea = !d.sea;
        break;
    case VK_F9:
        d.atmosphere = !d.atmosphere;
        break;
    case 'N':
    case 'R':
        in.nav->stop();
        in.s->camera.heading = 0.0; // North up.
        if (key == 'R')
        {
            in.s->camera.pitch = 90.0; // And looking straight down.
        }
        break;
    case 'U':
        in.nav->stop();
        in.s->camera.pitch = 90.0;
        break;
    default:
        if (key >= '1' && key < '1' + places.size())
        {
            in.nav->stop();
            in.s->camera = places[static_cast<std::size_t>(key - '1')].camera;
        }
        break;
    }
}

void on_button(HWND window, window_state& in, UINT message, WPARAM w, LPARAM l)
{
    const double x = GET_X_LPARAM(l);
    const double y = GET_Y_LPARAM(l);
    using drag = sample::globe_navigation::drag;
    const sample::camera& c = in.s->camera;
    SetCapture(window);
    if (message == WM_LBUTTONDOWN)
    {
        // A second press soon after the first, close to it, is a double-click.
        const double now = seconds_now();
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
        const drag mode = (w & MK_SHIFT) != 0     ? drag::orbit
                          : (w & MK_CONTROL) != 0 ? drag::look
                                                  : drag::pan;
        in.nav->press(mode, x, y, c);
    }
    else if (message == WM_MBUTTONDOWN)
    {
        in.nav->press(drag::orbit, x, y, c);
    }
    else
    {
        in.nav->press(drag::zoom, x, y, c);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, w, l) != 0)
    {
        return 1;
    }
    auto* in = reinterpret_cast<window_state*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (in == nullptr)
    {
        return DefWindowProcW(window, message, w, l);
    }
    switch (message)
    {
    case WM_SIZE:
        in->resized = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (!in->ui->wants_mouse())
        {
            on_button(window, *in, message, w, l);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        ReleaseCapture();
        in->nav->release();
        return 0;
    case WM_MOUSEMOVE:
        in->nav->move(GET_X_LPARAM(l), GET_Y_LPARAM(l), in->s->camera);
        return 0;
    case WM_MOUSEWHEEL:
        if (!in->ui->wants_mouse())
        {
            // The wheel's position is on the screen; the navigation's in the window.
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(window, &p);
            in->nav->scroll(GET_WHEEL_DELTA_WPARAM(w) / static_cast<double>(WHEEL_DELTA),
                            p.x,
                            p.y,
                            in->s->camera);
        }
        return 0;
    case WM_KEYDOWN:
        if (!in->ui->wants_keyboard() && (l & (1 << 30)) == 0) // Not repeats.
        {
            on_key(window, *in, w);
        }
        return 0;
    default:
        return DefWindowProcW(window, message, w, l);
    }
}

struct swap_target
{
    com<IDXGISwapChain1> chain;
    target t;
};

bool swap_target_resize(const gpu& g, swap_target& st, int width, int height)
{
    st.t.color_view.reset();
    st.t.depth_view.reset();
    st.t.depth.reset();
    st.t.width = width;
    st.t.height = height;
    if (FAILED(st.chain->ResizeBuffers(
            0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0)))
    {
        return false;
    }
    com<ID3D11Texture2D> back;
    return SUCCEEDED(st.chain->GetBuffer(
               0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.put()))) &&
           SUCCEEDED(
               g.device->CreateRenderTargetView(back.get(), nullptr, st.t.color_view.put())) &&
           depth_create(g, st.t);
}

bool swap_target_create(const gpu& g, HWND window, swap_target& st)
{
    com<IDXGIDevice> dxgi;
    com<IDXGIAdapter> adapter;
    com<IDXGIFactory2> factory;
    if (FAILED(g.device->QueryInterface(__uuidof(IDXGIDevice),
                                        reinterpret_cast<void**>(dxgi.put()))) ||
        FAILED(dxgi->GetAdapter(adapter.put())) ||
        FAILED(
            adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put()))))
    {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(
            g.device.get(), window, &d, nullptr, nullptr, st.chain.put())))
    {
        return false;
    }
    RECT r{};
    GetClientRect(window, &r);
    return swap_target_resize(
        g, st, std::max<int>(1, r.right - r.left), std::max<int>(1, r.bottom - r.top));
}

int run_interactive(const options& o,
                    const gpu& g,
                    vgl::context& ctx,
                    const std::vector<sample::layer_control>& layers)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = L"vgl-d3d11";
    RegisterClassExW(&wc);
    RECT rect{0, 0, 1280, 720};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0,
                                  wc.lpszClassName,
                                  L"vgl",
                                  WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  rect.right - rect.left,
                                  rect.bottom - rect.top,
                                  nullptr,
                                  nullptr,
                                  instance,
                                  nullptr);
    if (window == nullptr)
    {
        return 1;
    }

    scene s;
    s.debug.atmosphere = true;
    s.debug.stars = true;
    s.depth = o.depth;
    s.camera = places[0].camera;
    vgl::expected<vgl::d3d11_backend> backend = backend_create(ctx, g, s.depth);
    if (!backend)
    {
        std::fprintf(
            stderr, "could not create the backend: %s\n", vgl::result_name_get(backend.error()));
        return 1;
    }
    swap_target st;
    if (!swap_target_create(g, window, st))
    {
        std::fprintf(stderr, "could not create the swap chain\n");
        return 1;
    }

    sample::overlay ui;
    sample::globe_navigation nav;
    nav.terrain(ctx.get()); // Grab the terrain under the cursor.
    window_state in;
    in.s = &s;
    in.ui = &ui;
    in.nav = &nav;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&in));
    struct imgui_host
    {
        HWND window;
        const gpu* g;
    } host{window, &g};
    sample::overlay::backend imgui;
    imgui.user_data = &host;
    imgui.init = [](void* user) {
        const auto* h = static_cast<const imgui_host*>(user);
        return ImGui_ImplWin32_Init(h->window) &&
               ImGui_ImplDX11_Init(h->g->device.get(), h->g->dc.get());
    };
    imgui.new_frame = [](void*) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
    };
    imgui.render = [](void*) { ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); };
    imgui.shutdown = [](void*) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
    };
    if (!ui.init(imgui))
    {
        std::fprintf(stderr, "could not set up the overlay\n");
        return 1;
    }
    ShowWindow(window, SW_SHOWDEFAULT);

    sample::panel_info panel;
    panel.layers = layers;
    panel.api = "Direct3D 11";
    panel.device = g.adapter.c_str();
    panel.depth = depth_name(s.depth);
    bool was_frozen = false;
    auto last_frame = std::chrono::steady_clock::now();
    auto last_navigated = last_frame;
    auto last_title = last_frame;
    int frames = 0;
    bool running = true;
    while (running)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0)
        {
            if (message.message == WM_QUIT)
            {
                running = false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running)
        {
            break;
        }
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width == 0 || height == 0)
        {
            WaitMessage(); // Minimized.
            continue;
        }
        if (in.resized || width != st.t.width || height != st.t.height)
        {
            g.dc->OMSetRenderTargets(0, nullptr, nullptr);
            swap_target_resize(g, st, width, height);
            in.resized = false;
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
        nav.viewport(width, height, field_of_view);
        sample::navigation_keys keys;
        if (!ui.wants_keyboard() && GetForegroundWindow() == window)
        {
            const auto held = [](int key) { return (GetKeyState(key) & 0x8000) != 0; };
            keys.left = held(VK_LEFT) || held('A');
            keys.right = held(VK_RIGHT) || held('D');
            keys.up = held(VK_UP) || held('W');
            keys.down = held(VK_DOWN) || held('S');
            keys.zoom_in = held(VK_PRIOR) || held(VK_OEM_PLUS) || held(VK_ADD);
            keys.zoom_out = held(VK_NEXT) || held(VK_OEM_MINUS) || held(VK_SUBTRACT);
            keys.shift = held(VK_SHIFT);
        }
        // From one update to the next, so that the time spent updating and drawing counts too.
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
        clear(g, s, st.t);
        static_cast<void>(draw(*backend, s, width, height));

        const auto frame_end = std::chrono::steady_clock::now();
        const double frame_seconds = std::chrono::duration<double>(frame_end - last_frame).count();
        last_frame = frame_end;
        const double aspect = static_cast<double>(width) / height;
        panel.live = s.camera;
        panel.frozen = s.frozen;
        panel.focus_strength = 0.0;
        double focus_strength = 0.0;
        if (nav.focus(s.camera, panel.focus_x, panel.focus_y, focus_strength))
        {
            panel.focus_strength = focus_strength;
        }
        panel.credits = ctx.attribution_get();
        // The frozen frustum reaches to about the frozen camera's horizon.
        const double reach = std::sqrt(s.frozen.altitude * (2.0 * 6371000.0 + s.frozen.altitude));
        ui.frame(ctx.statistics_get(),
                 panel,
                 frame_seconds,
                 sample::camera_view_matrix(s.camera),
                 projection_for(s, s.camera, aspect),
                 sample::frustum_of(s.frozen, field_of_view, aspect, reach),
                 s.debug);
        ui.render();
        st.chain->Present(1, 0);

        ++frames;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - last_title).count();
        if (seconds >= 0.5)
        {
            const vgl_context_statistics stats = ctx.statistics_get();
            std::array<char, 256> title{};
            std::snprintf(
                title.data(),
                title.size(),
                "vgl - Direct3D 11 - %s depth - %.0f fps - %u tiles - %.0f MiB GPU - %.0f km",
                depth_name(s.depth),
                frames / seconds,
                stats.tiles_drawn,
                static_cast<double>(stats.gpu_used) / (1024.0 * 1024.0),
                s.camera.altitude / 1000.0);
            SetWindowTextA(window, title.data());
            frames = 0;
            last_title = now;
        }
    }
    // The credits of what was in view last, as a host would show them.
    std::printf("credits:\n");
    for (const char* line : ctx.attribution_get())
    {
        std::printf("  %s\n", line);
    }
    g.dc->ClearState();
    return 0;
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
        else if (std::strcmp(a, "--warp") == 0)
        {
            o.warp = true;
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
    // The check expects the sample set, and --pack replaces it.
    return o.packs.empty() || (o.data.empty() && !o.check);
}

} // namespace

int main(int argc, char** argv)
{
    options o;
    if (!parse(argc, argv, o))
    {
        std::fprintf(stderr,
                     "usage: vgl-d3d11 [--data <directory> | --pack <uri>...] "
                     "[--depth logarithmic|reversed|conventional] [--warp] [--check] "
                     "[--save <directory>] [--stars <catalog>]\n"
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
    SetProcessDPIAware();
    gpu g;
    if (!gpu_create(g, o.warp))
    {
        std::fprintf(stderr, "could not create a Direct3D 11 device\n");
        return 1;
    }
    std::printf("Direct3D 11\n%s\n\n", g.adapter.c_str());

    // The defaults: a thread pool for decoding and reading, and a file reader on it.
    vgl::expected<vgl::thread_pool> pool = vgl::thread_pool::create();
    if (!pool)
    {
        std::fprintf(stderr, "could not create the thread pool\n");
        return 1;
    }
    vgl::expected<vgl::file_reader> reader = vgl::file_reader::create(*pool);
    if (!reader)
    {
        std::fprintf(stderr, "could not create the file reader\n");
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

    const int code = o.check ? run_check(o, g, *ctx)
                             : run_interactive(o, g, *ctx, sample::layer_controls(sources, packs));

    // Leave in order: the backend went with its run; then packs and context, the reader and
    // the pool, and last the device.
    packs.clear();
    ctx->reset();
    reader->reset();
    pool->reset();
    return code;
}
