// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// vgl_context_attribution_get (FR-08): the attribution lines of the packs whose data is in
// view. A pack is in view when the view selects one of its tiles, or the last frame drew one;
// the two differ only while a region's new tiles upload.

#include "context/context_impl.hpp"

#include <algorithm>
#include <cstring>

using namespace vgl::internal;

namespace
{

bool in_view(const vgl_context& ctx, const vgl_pack& p) noexcept
{
    if (p.manifest.layer().type == vgl::internal::pack::layer_type::imagery)
    {
        // Selected tiles wanted its imagery in the last update, or the last frame drew it.
        return (ctx.has_view && p.imagery_wanted != 0 && p.imagery_wanted + 1 == ctx.frame) ||
               (p.imagery_drawn != 0 && p.imagery_drawn == ctx.render_frame);
    }
    if (ctx.has_view)
    {
        for (const selected_tile& s : ctx.selected)
        {
            if (s.pack_id == p.id)
            {
                return true;
            }
        }
    }
    return std::ranges::any_of(ctx.drawn, [&](const drawn_tile& d) {
        const gpu_resource* r = ctx.resources.find(d.mesh);
        return r != nullptr && r->pack_id == p.id;
    });
}

} // namespace

extern "C" VGL_API vgl_result vgl_context_attribution_get(const vgl_context* context,
                                                          uint32_t* inout_count,
                                                          const char** out_strings)
{
    if (context == nullptr || inout_count == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_context& ctx = *context;
    const std::uint32_t capacity = out_strings != nullptr ? *inout_count : 0;
    std::uint32_t found = 0;   // Distinct lines.
    std::uint32_t written = 0; // Of which written.

    for (const vgl_pack* p : ctx.packs)
    {
        if (p->state != VGL_PACK_STATE_READY || p->credit_count == 0 || !in_view(ctx, *p))
        {
            continue;
        }
        const char* line = p->credits.data();
        for (std::uint32_t i = 0; i < p->credit_count; ++i, line += std::strlen(line) + 1)
        {
            // Each distinct line once, even when several packs carry it. The lists are short,
            // so a search through the lines before it is cheap, and needs no memory.
            bool seen = false;
            for (const vgl_pack* q : ctx.packs)
            {
                if (q == p)
                {
                    break;
                }
                if (q->state != VGL_PACK_STATE_READY || q->credit_count == 0 || !in_view(ctx, *q))
                {
                    continue;
                }
                const char* other = q->credits.data();
                for (std::uint32_t k = 0; k < q->credit_count && !seen;
                     ++k, other += std::strlen(other) + 1)
                {
                    seen = std::strcmp(other, line) == 0;
                }
                if (seen)
                {
                    break;
                }
            }
            const char* earlier = p->credits.data();
            for (std::uint32_t k = 0; k < i && !seen; ++k, earlier += std::strlen(earlier) + 1)
            {
                seen = std::strcmp(earlier, line) == 0;
            }
            if (seen)
            {
                continue;
            }
            if (out_strings != nullptr && written < capacity)
            {
                out_strings[written++] = line;
            }
            ++found;
        }
    }

    if (out_strings == nullptr)
    {
        *inout_count = found;
        return VGL_RESULT_SUCCESS;
    }
    *inout_count = written;
    return written < found ? VGL_RESULT_INCOMPLETE : VGL_RESULT_SUCCESS;
}
