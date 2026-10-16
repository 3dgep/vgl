// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/context.h (docs/c-api.md, section 16).

#ifndef VGL_CONTEXT_HPP
#define VGL_CONTEXT_HPP

#include <vgl/base.hpp>
#include <vgl/context.h>
#include <vgl/foundation.hpp>
#include <vgl/render.h>

#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace vgl
{

enum class pack_state : std::int32_t
{
    loading = VGL_PACK_STATE_LOADING,
    ready = VGL_PACK_STATE_READY,
    failed = VGL_PACK_STATE_FAILED,
};

enum class height_reference : std::int32_t
{
    ellipsoid = VGL_HEIGHT_REFERENCE_ELLIPSOID,
    mean_sea_level = VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL,
};

// A height from context::height_query. status is result::success when the height is final,
// or result::not_ready while finer data loads; height is NaN until any data has loaded.
struct height_sample
{
    double height = std::numeric_limits<double>::quiet_NaN();
    result status = result::not_ready;
};

struct pack_status
{
    pack_state state = pack_state::loading;
    result failure = result::success;
};

// Owns a vgl_context.
class context
{
public:
    context() noexcept = default;
    explicit context(vgl_context* handle) noexcept : handle_(handle) {}
    context(const context&) = delete;
    context& operator=(const context&) = delete;
    context(context&& other) noexcept : handle_(other.release()) {}
    context& operator=(context&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~context() { reset(); }

    // Creates a context. info may be null for all defaults.
    [[nodiscard]] static expected<context>
    create(const vgl_context_create_info* info = nullptr) noexcept
    {
        vgl_context* handle = nullptr;
        const auto r = static_cast<result>(vgl_context_create(info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return context(handle);
    }

    [[nodiscard]] vgl_context* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] vgl_context* release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(vgl_context* handle = nullptr) noexcept
    {
        static_cast<void>(vgl_context_destroy(std::exchange(handle_, handle)));
    }

    // Sets the view that update selects tiles for; null stops selection.
    [[nodiscard]] result view_set(const vgl_view* view) noexcept
    {
        return static_cast<result>(vgl_context_view_set(handle_, view));
    }

    // The texture formats the consumer can sample, most preferred first (vgl/render.h).
    [[nodiscard]] result texture_formats_set(std::span<const vgl_texture_format> formats) noexcept
    {
        return static_cast<result>(vgl_context_texture_formats_set(
            handle_, formats.data(), static_cast<std::uint32_t>(formats.size())));
    }

    // Forgets every render resource, after the consumer lost them (vgl/render.h).
    [[nodiscard]] result resources_reset() noexcept
    {
        return static_cast<result>(vgl_context_resources_reset(handle_));
    }

    // Takes completed work, decodes within the time budget, and selects tiles for the view.
    // info may be null.
    [[nodiscard]] result update(const vgl_update_info* info = nullptr) noexcept
    {
        return static_cast<result>(vgl_context_update(handle_, info));
    }

    // Latitude and longitude in radians.
    [[nodiscard]] expected<height_sample>
    height_query(double latitude,
                 double longitude,
                 height_reference reference = height_reference::ellipsoid) noexcept
    {
        height_sample sample;
        sample.status = static_cast<result>(
            vgl_context_height_query(handle_,
                                     latitude,
                                     longitude,
                                     static_cast<vgl_height_reference>(reference),
                                     &sample.height));
        if (failed(sample.status))
        {
            return unexpected(sample.status);
        }
        return sample;
    }

private:
    vgl_context* handle_ = nullptr;
};

// Owns a vgl_pack. Destroy packs before their context.
class pack
{
public:
    pack() noexcept = default;
    explicit pack(vgl_pack* handle) noexcept : handle_(handle) {}
    pack(const pack&) = delete;
    pack& operator=(const pack&) = delete;
    pack(pack&& other) noexcept : handle_(other.release()) {}
    pack& operator=(pack&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~pack() { reset(); }

    // Adds a pack to a context and starts loading it.
    [[nodiscard]] static expected<pack> create(context& owner,
                                               const vgl_pack_create_info& info) noexcept
    {
        vgl_pack* handle = nullptr;
        const auto r = static_cast<result>(vgl_pack_create(owner.get(), &info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return pack(handle);
    }

    // Adds the pack at uri with a priority.
    [[nodiscard]] static expected<pack>
    create(context& owner, const char* uri, std::int32_t priority = 0) noexcept
    {
        auto info = init<vgl_pack_create_info>();
        info.uri = uri;
        info.priority = priority;
        return create(owner, info);
    }

    [[nodiscard]] vgl_pack* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] vgl_pack* release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(vgl_pack* handle = nullptr) noexcept
    {
        static_cast<void>(vgl_pack_destroy(std::exchange(handle_, handle)));
    }

    [[nodiscard]] expected<pack_status> state_get() const noexcept
    {
        vgl_pack_state state{};
        vgl_result failure = VGL_RESULT_SUCCESS;
        const auto r = static_cast<result>(vgl_pack_state_get(handle_, &state, &failure));
        if (failed(r))
        {
            return unexpected(r);
        }
        return pack_status{static_cast<pack_state>(state), static_cast<result>(failure)};
    }

private:
    vgl_pack* handle_ = nullptr;
};

} // namespace vgl

#endif // VGL_CONTEXT_HPP
