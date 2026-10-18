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

// How depth is written and compared (vgl_depth_convention).
enum class depth_convention : std::int32_t
{
    logarithmic = VGL_DEPTH_CONVENTION_LOGARITHMIC,
    reversed_z = VGL_DEPTH_CONVENTION_REVERSED_Z,
    conventional = VGL_DEPTH_CONVENTION_CONVENTIONAL,
};

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

    // Begins a frame. The lists are read as spans of this header's structures, so the strides
    // in info (draw_item_size and the others) must be 0 or this header's sizes; other strides
    // return result::invalid_argument here, and are read through the C API instead.
    [[nodiscard]] static expected<frame> begin(context& owner,
                                               const vgl_frame_begin_info* info = nullptr) noexcept
    {
        if (info != nullptr && !own_strides(*info))
        {
            return unexpected(result::invalid_argument);
        }
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
    [[nodiscard]] std::span<const std::uint64_t> destroys_get() const noexcept
    {
        return list(&vgl_frame_destroys_get);
    }
    [[nodiscard]] std::span<const vgl_index_list_data> index_lists_get() const noexcept
    {
        return list(&vgl_frame_index_lists_get);
    }
    [[nodiscard]] std::span<const vgl_mesh_data> meshes_get() const noexcept
    {
        return list(&vgl_frame_meshes_get);
    }
    [[nodiscard]] std::span<const vgl_texture_data> textures_get() const noexcept
    {
        return list(&vgl_frame_textures_get);
    }
    [[nodiscard]] std::span<const vgl_draw_item> draw_list_get() const noexcept
    {
        return list(&vgl_frame_draw_list_get);
    }
    [[nodiscard]] std::span<const vgl_draw_layer> draw_layers_get() const noexcept
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
    [[nodiscard]] static constexpr bool own_stride(std::uint32_t stride) noexcept
    {
        return stride == 0 || stride == sizeof(T);
    }

    [[nodiscard]] static constexpr bool own_strides(const vgl_frame_begin_info& info) noexcept
    {
        // The C API rejects a smaller structure; its strides are not read here.
        if (info.struct_size < sizeof(vgl_frame_begin_info))
        {
            return true;
        }
        return own_stride<vgl_draw_item>(info.draw_item_size) &&
               own_stride<vgl_draw_layer>(info.draw_layer_size) &&
               own_stride<vgl_index_list_data>(info.index_list_data_size) &&
               own_stride<vgl_mesh_data>(info.mesh_data_size) &&
               own_stride<vgl_texture_data>(info.texture_data_size);
    }

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
