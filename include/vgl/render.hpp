// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/render.h (docs/c-api.md, section 16; docs/render-interface.md).

#ifndef VGL_RENDER_HPP
#define VGL_RENDER_HPP

#include <vgl/base.hpp>
#include <vgl/context.hpp>
#include <vgl/render.h>

#include <cstdint>
#include <span>
#include <utility>

namespace vgl
{

// An open frame of a context. It ends when destroyed, so a frame cannot be left open by an
// early return. Its lists are valid until it ends.
class frame
{
public:
    frame() noexcept = default;
    explicit frame(vgl_frame* handle) noexcept : handle_(handle) {}
    frame(const frame&) = delete;
    frame& operator=(const frame&) = delete;
    frame(frame&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    frame& operator=(frame&& other) noexcept
    {
        if (this != &other)
        {
            end();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~frame() { end(); }

    // Begins a frame. The draw items are those of this header; set info->draw_item_size only
    // to read them at another size through the C API.
    [[nodiscard]] static expected<frame> begin(context& owner,
                                               const vgl_frame_begin_info* info = nullptr) noexcept
    {
        vgl_frame* handle = nullptr;
        const auto r = static_cast<result>(vgl_context_frame_begin(owner.get(), info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return frame(handle);
    }

    [[nodiscard]] vgl_frame* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    // The lists, in the order to apply them. The frame must be open.
    [[nodiscard]] std::span<const std::uint64_t> destroys() const noexcept
    {
        return list(&vgl_frame_destroys_get);
    }
    [[nodiscard]] std::span<const vgl_index_list_data> index_lists() const noexcept
    {
        return list(&vgl_frame_index_lists_get);
    }
    [[nodiscard]] std::span<const vgl_mesh_data> meshes() const noexcept
    {
        return list(&vgl_frame_meshes_get);
    }
    [[nodiscard]] std::span<const vgl_texture_data> textures() const noexcept
    {
        return list(&vgl_frame_textures_get);
    }
    [[nodiscard]] std::span<const vgl_draw_item> draw_list() const noexcept
    {
        return list(&vgl_frame_draw_list_get);
    }
    [[nodiscard]] std::span<const vgl_draw_layer> draw_layers() const noexcept
    {
        return list(&vgl_frame_draw_layers_get);
    }

    [[nodiscard]] result resource_handle_set(std::uint64_t resource, std::uint64_t handle) noexcept
    {
        return static_cast<result>(vgl_frame_resource_handle_set(handle_, resource, handle));
    }

    [[nodiscard]] result resource_failed(std::uint64_t resource, result reason) noexcept
    {
        return static_cast<result>(
            vgl_frame_resource_failed(handle_, resource, static_cast<vgl_result>(reason)));
    }

    // Ends the frame early; the destructor does nothing then.
    void end() noexcept { static_cast<void>(vgl_frame_end(std::exchange(handle_, nullptr))); }

private:
    template <typename T>
    using getter = vgl_result (*)(const vgl_frame*, const T**, std::uint32_t*);

    template <typename T>
    [[nodiscard]] std::span<const T> list(getter<T> fetch) const noexcept
    {
        const T* data = nullptr;
        std::uint32_t count = 0;
        if (handle_ == nullptr || fetch(handle_, &data, &count) != VGL_RESULT_SUCCESS)
        {
            return {};
        }
        return {data, count};
    }

    vgl_frame* handle_ = nullptr;
};

} // namespace vgl

#endif // VGL_RENDER_HPP
