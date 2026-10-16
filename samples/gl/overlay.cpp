// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "overlay.hpp"

#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_glfw.h>

namespace sample
{

namespace
{

double mib(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// A row of a two-column table: a label, and a value formatted with printf rules.
void row(const char* label, const char* format, ...) // NOLINT(cert-dcl50-cpp): printf-style
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    std::va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
}

bool begin_table(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    return true;
}

// Memory used against its budget, as a bar.
void budget_bar(const char* label, std::uint64_t used, std::uint64_t budget)
{
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "%.1f of %.0f MiB", mib(used), mib(budget));
    ImGui::TextUnformatted(label);
    ImGui::SameLine(70.0f);
    ImGui::ProgressBar(
        budget != 0 ? static_cast<float>(static_cast<double>(used) / static_cast<double>(budget))
                    : 0.0f,
        ImVec2(-1.0f, 0.0f),
        text.data());
}

void camera_rows(const camera& c)
{
    row("Latitude", "%.4f", c.latitude);
    row("Longitude", "%.4f", c.longitude);
    row("Altitude", "%.1f km", c.altitude / 1000.0);
    row("Heading, pitch", "%.0f, %.0f", c.heading, c.pitch);
}

vgl_dvec3 transform(const vgl_dmat4& m, const vgl_dvec3& p, double& w)
{
    w = m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15];
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
            m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

} // namespace

overlay::~overlay()
{
    if (initialized_)
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
}

bool overlay::init(GLFWwindow* window, bool es)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // No imgui.ini next to the executable.
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Alpha = 0.92f;
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) ||
        !ImGui_ImplOpenGL3_Init(es ? "#version 300 es" : "#version 330 core"))
    {
        ImGui::DestroyContext();
        return false;
    }
    initialized_ = true;
    return true;
}

bool overlay::wants_mouse() const
{
    return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool overlay::wants_keyboard() const
{
    return initialized_ && ImGui::GetIO().WantCaptureKeyboard;
}

void overlay::frame(const vgl_context_statistics& s,
                    const panel_info& info,
                    double frame_seconds,
                    const vgl_dmat4& view,
                    const vgl_dmat4& projection,
                    const std::array<vgl_dvec3, 5>& frustum,
                    debug_settings& settings)
{
    frame_ms_[next_] = static_cast<float>(frame_seconds * 1000.0);
    next_ = (next_ + 1) % frame_ms_.size();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (settings.panel)
    {
        panel(s, info, settings);
    }
    if (settings.freeze)
    {
        frustum_lines(view, projection, frustum);
    }
    ImGui::Render();
}

void overlay::render()
{
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void overlay::panel(const vgl_context_statistics& s, const panel_info& info, debug_settings& d)
{
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("vgl (F1)", &d.panel))
    {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", info.gl_version);
    ImGui::TextWrapped("%s", info.gl_renderer);
    ImGui::Text("Depth: %s", info.depth);

    // Frame times, the newest on the right.
    float sum = 0.0f;
    float worst = 0.0f;
    for (const float ms : frame_ms_)
    {
        sum += ms;
        worst = std::max(worst, ms);
    }
    const float mean = sum / static_cast<float>(frame_ms_.size());
    std::array<char, 64> label{};
    std::snprintf(
        label.data(), label.size(), "%.2f ms, %.0f fps", mean, mean > 0.0f ? 1000.0f / mean : 0.0f);
    ImGui::PlotLines("##frame",
                     frame_ms_.data(),
                     static_cast<int>(frame_ms_.size()),
                     static_cast<int>(next_),
                     label.data(),
                     0.0f,
                     std::max(worst, 20.0f),
                     ImVec2(-1.0f, 50.0f));

    if (ImGui::CollapsingHeader("Debug views", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::BeginDisabled(!info.wireframe_available);
        ImGui::Checkbox("Wireframe (F2)", &d.wireframe);
        ImGui::EndDisabled();
        if (!info.wireframe_available)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(not on OpenGL ES)");
        }
        ImGui::Checkbox("Colour by tile level (F3)", &d.level_colors);
        ImGui::Checkbox("Imagery source (F4)", &d.imagery_source);
        if (d.imagery_source)
        {
            ImGui::TextDisabled("orange: coarser imagery, magenta: none");
        }
        ImGui::Checkbox("Freeze selection (F5)", &d.freeze);
        ImGui::Checkbox("Heightmap (F6)", &d.height_colors);
        ImGui::Checkbox("Contour lines (F7)", &d.contours);
        ImGui::Checkbox("Sea surface (F8)", &d.sea);
        if (d.contours)
        {
            ImGui::Checkbox("Interval from altitude", &d.contour_auto);
            ImGui::BeginDisabled(d.contour_auto);
            ImGui::SliderFloat("Interval",
                               &d.contour_interval,
                               1.0f,
                               5000.0f,
                               "%.0f m",
                               ImGuiSliderFlags_Logarithmic);
            ImGui::EndDisabled();
            ImGui::TextDisabled("heavier every 5th; heights above the ellipsoid");
        }
        ImGui::Checkbox("Sunlight (L)", &d.sunlight);
        ImGui::SliderFloat(
            "Max error", &d.max_error, 0.5f, 16.0f, "%.1f px", ImGuiSliderFlags_Logarithmic);
    }

    if (ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen))
    {
        budget_bar("CPU", s.cpu_used, s.cpu_budget);
        budget_bar("GPU", s.gpu_used, s.gpu_budget);
    }

    if (ImGui::CollapsingHeader("Tiles and resources", ImGuiTreeNodeFlags_DefaultOpen) &&
        begin_table("work"))
    {
        row("Selected, drawn", "%u, %u", s.tiles_selected, s.tiles_drawn);
        row("Packs loading, failed", "%u, %u", s.packs_loading, s.packs_failed);
        row("Reads outstanding", "%u", s.reads_outstanding);
        row("Tiles decoding, cached", "%u, %u", s.tiles_decoding, s.tiles_decoded);
        row("Meshes building, waiting", "%u, %u", s.meshes_building, s.meshes_waiting);
        row("Textures decoding, waiting", "%u, %u", s.textures_decoding, s.textures_waiting);
        row("Resources resident", "%u", s.resources_resident);
        row("Last frame created", "%u (%.2f MiB)", s.resources_created, mib(s.bytes_created));
        row("Last frame destroyed", "%u", s.resources_destroyed);
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Times and totals") && begin_table("totals"))
    {
        row("Update", "%.3f ms", s.update_time * 1000.0);
        row("Frame", "%.3f ms", s.frame_time * 1000.0);
        row("Mean tile load", "%.1f ms", s.tile_load_time_mean * 1000.0);
        row("Reads", "%llu", static_cast<unsigned long long>(s.reads_total));
        row("Read", "%.1f MiB", mib(s.bytes_read_total));
        row("Tiles loaded", "%llu", static_cast<unsigned long long>(s.tiles_loaded_total));
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Camera") && begin_table("camera"))
    {
        camera_rows(info.live);
        if (d.freeze)
        {
            ImGui::EndTable();
            ImGui::TextDisabled("Frozen camera");
            if (begin_table("frozen"))
            {
                camera_rows(info.frozen);
                ImGui::EndTable();
            }
        }
        else
        {
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Controls") && begin_table("controls"))
    {
        row("Left drag", "grab the globe");
        row("Scroll, double-click", "zoom to the cursor");
        row("Shift or middle drag", "orbit and tilt");
        row("Ctrl drag", "look around");
        row("Right drag", "zoom");
        row("Arrows, WASD", "move");
        row("Shift + arrows, WASD", "orbit and tilt");
        row("Page up, down", "zoom");
        row("N, U, R", "north up, top down, both");
        row("1 to 7", "places");
        row("L", "sunlight");
        row("F1 to F8", "panel and views");
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Credits", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (const char* line : info.credits)
        {
            ImGui::TextWrapped("%s", line);
        }
    }
    ImGui::End();
}

void overlay::frustum_lines(const vgl_dmat4& view,
                            const vgl_dmat4& projection,
                            const std::array<vgl_dvec3, 5>& frustum)
{
    // Camera-space points, clipped against a near plane, then projected to the screen.
    constexpr double near_plane = 1.0;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const auto to_screen = [&](const vgl_dvec3& c) {
        double w = 0.0;
        const vgl_dvec3 clip = transform(projection, c, w);
        return ImVec2(static_cast<float>((clip.x / w * 0.5 + 0.5) * size.x),
                      static_cast<float>((0.5 - clip.y / w * 0.5) * size.y));
    };
    const auto line = [&](const vgl_dvec3& a_ecef, const vgl_dvec3& b_ecef, ImU32 colour) {
        double w = 0.0;
        vgl_dvec3 a = transform(view, a_ecef, w);
        vgl_dvec3 b = transform(view, b_ecef, w);
        if (a.z > -near_plane && b.z > -near_plane)
        {
            return; // Behind the camera.
        }
        const auto clip = [&](vgl_dvec3& p, const vgl_dvec3& q) {
            const double t = (-near_plane - p.z) / (q.z - p.z);
            p = {p.x + t * (q.x - p.x), p.y + t * (q.y - p.y), -near_plane};
        };
        if (a.z > -near_plane)
        {
            clip(a, b);
        }
        else if (b.z > -near_plane)
        {
            clip(b, a);
        }
        list->AddLine(to_screen(a), to_screen(b), colour, 2.0f);
    };
    const ImU32 edges = IM_COL32(255, 220, 60, 230);
    for (std::size_t k = 1; k < frustum.size(); ++k)
    {
        line(frustum[0], frustum[k], edges);
        line(frustum[k], frustum[k % 4 + 1], edges);
    }
}

std::array<vgl_dvec3, 5>
frustum_of(const camera& c, double field_of_view, double aspect, double distance)
{
    const vgl_dmat4 v = camera_view_matrix(c);
    const vgl_dvec3 eye = camera_eye(c);
    // The rows of the view matrix are the camera's axes: right, up, and backwards.
    const vgl_dvec3 right{v.m[0], v.m[4], v.m[8]};
    const vgl_dvec3 up{v.m[1], v.m[5], v.m[9]};
    const vgl_dvec3 forward{-v.m[2], -v.m[6], -v.m[10]};
    const double h = std::tan(vgl::degrees_to_radians(field_of_view) / 2.0);
    std::array<vgl_dvec3, 5> f{};
    f[0] = eye;
    const std::array<std::array<double, 2>, 4> corners = {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (std::size_t k = 0; k < 4; ++k)
    {
        const double sx = corners[k][0] * h * aspect;
        const double sy = corners[k][1] * h;
        f[k + 1] = {eye.x + distance * (forward.x + sx * right.x + sy * up.x),
                    eye.y + distance * (forward.y + sx * right.y + sy * up.y),
                    eye.z + distance * (forward.z + sx * right.z + sy * up.z)};
    }
    return f;
}

} // namespace sample
