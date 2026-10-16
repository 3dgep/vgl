// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Context, packs, loading and height queries (vgl/context.h).
//
// Loading a tile takes up to four reads: the pack's manifest, the head of its shard (header
// and, usually, the whole directory), the rest of the directory if it did not fit, and the
// tile itself. Each read completes through vgl_io_request_complete; vgl_context_update then
// takes the next step. Tiles decode on a task thread, or in the update.

#include "context/context_impl.hpp"
#include "decoders/elevation_codec.hpp"
#include "foundation/math.hpp"
#include "tiling/tile.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace vgl::internal
{

namespace
{

namespace pk = pack;

constexpr std::uint64_t default_cpu_budget = 256ull * 1024 * 1024;
constexpr std::uint64_t default_gpu_budget = 512ull * 1024 * 1024;
constexpr double default_time_budget = 0.002;
// The first read of a shard; the header and a typical directory fit (pack format, section 1.1).
constexpr std::uint64_t shard_head_size = 64ull * 1024;
constexpr double pole_tolerance = 1e-12;

const char* result_name(vgl_result r) noexcept
{
    const char* name = nullptr;
    static_cast<void>(vgl_result_name_get(r, &name));
    return name;
}

// Copies a versioned input structure (docs/c-api.md, section 8.3). in may be null, which
// leaves out zero-initialized.
template <typename T>
vgl_result read_versioned(const T* in, std::size_t first_size, T& out) noexcept
{
    out = T{};
    if (in == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    const std::size_t size = in->struct_size;
    if (size < first_size)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (size > sizeof(T))
    {
        // Fields of a newer version: accepted only if they are all zero.
        const auto* bytes = reinterpret_cast<const unsigned char*>(in);
        for (std::size_t i = sizeof(T); i < size; ++i)
        {
            if (bytes[i] != 0)
            {
                return VGL_RESULT_NOT_SUPPORTED;
            }
        }
    }
    std::memcpy(&out, in, size < sizeof(T) ? size : sizeof(T));
    return VGL_RESULT_SUCCESS;
}

template <typename Interface>
vgl_result check_interface_version(const Interface& i, std::uint32_t current) noexcept
{
    return i.version == 0 || i.version > current ? VGL_RESULT_VERSION_MISMATCH : VGL_RESULT_SUCCESS;
}

bool copy_string(const allocator& a, const char* s, std::size_t length, buffer<char>& out) noexcept
{
    if (!out.allocate(a, length + 1))
    {
        return false;
    }
    std::memcpy(out.data(), s, length);
    out.data()[length] = '\0';
    return true;
}

// Inserts a pack into the context's list, highest priority first and, among equal priorities,
// latest first.
bool insert_pack(vgl_context& ctx, vgl_pack* p) noexcept
{
    if (!ctx.packs.push_back(p))
    {
        return false;
    }
    for (std::size_t i = ctx.packs.size() - 1; i > 0; --i)
    {
        vgl_pack* before = ctx.packs[i - 1];
        if (before->priority > p->priority ||
            (before->priority == p->priority && before->order > p->order))
        {
            break;
        }
        ctx.packs[i] = before;
        ctx.packs[i - 1] = p;
    }
    return true;
}

vgl_pack* find_pack(vgl_context& ctx, std::uint32_t id) noexcept
{
    for (vgl_pack* p : ctx.packs)
    {
        if (p->id == id)
        {
            return p;
        }
    }
    return nullptr;
}

void free_request(vgl_context& ctx, vgl_io_request* request) noexcept
{
    ctx.memory.destroy(request);
    ctx.core->release();
}

void free_job(const allocator& a, decode_job* job) noexcept
{
    async_core* core = job->core;
    a.destroy(job);
    core->release();
}

// Starts reading base + "/" + relative. Returns VGL_RESULT_OUT_OF_MEMORY if the request could
// not be made; the read is then not started.
vgl_result start_read(vgl_context& ctx,
                      const io_target& target,
                      const char* base,
                      const char* relative,
                      std::uint64_t offset,
                      std::uint64_t size) noexcept
{
    auto* request = ctx.memory.create<vgl_io_request>();
    if (request == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    const std::size_t base_length = std::strlen(base);
    const std::size_t relative_length = std::strlen(relative);
    const bool slash = base_length != 0 && base[base_length - 1] != '/';
    const std::size_t length = base_length + (slash ? 1 : 0) + relative_length;
    if (!request->uri.allocate(ctx.memory, length + 1))
    {
        ctx.memory.destroy(request);
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    char* uri = request->uri.data();
    std::memcpy(uri, base, base_length);
    if (slash)
    {
        uri[base_length] = '/';
    }
    std::memcpy(uri + base_length + (slash ? 1 : 0), relative, relative_length);
    uri[length] = '\0';

    request->core = ctx.core;
    request->id = ctx.next_request_id++;
    request->target = target;
    request->read = {uri, offset, size};
    if (ctx.outstanding.insert(request->id, request) == nullptr)
    {
        ctx.memory.destroy(request);
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    ctx.core->retain();
    // The host may complete the request inside this call; the completion is queued.
    ctx.io.read(ctx.io.user_data, request, &request->read);
    return VGL_RESULT_SUCCESS;
}

void fail_pack(vgl_context& ctx, vgl_pack& p, vgl_result why, const char* what) noexcept
{
    p.state = VGL_PACK_STATE_FAILED;
    p.failure = why;
    ctx.log.log(VGL_LOG_LEVEL_ERROR,
                "vgl_context_update: pack %s: %s (%s)",
                p.uri.data(),
                what,
                result_name(why));
}

// --- Completions ---------------------------------------------------------------------------

void handle_manifest(vgl_context& ctx, vgl_pack& p, vgl_io_request& r) noexcept
{
    if (r.result != VGL_RESULT_SUCCESS)
    {
        fail_pack(ctx, p, r.result, "reading pack.vglm failed; check the pack's URI");
        return;
    }
    const vgl_result parsed =
        pk::manifest::parse(r.data.span(), ctx.memory, ctx.decompressor, p.manifest);
    if (parsed != VGL_RESULT_SUCCESS)
    {
        fail_pack(ctx, p, parsed, "pack.vglm is invalid");
        return;
    }
    const pk::layer_description& l = p.manifest.layer();
    const double flattening = l.inverse_flattening == 0.0 ? 0.0 : 1.0 / l.inverse_flattening;
    if (std::fabs(l.semi_major_axis - ctx.shape.semi_major_axis()) > 1e-6 ||
        std::fabs(flattening - ctx.shape.flattening()) > 1e-12)
    {
        fail_pack(ctx,
                  p,
                  VGL_RESULT_NOT_SUPPORTED,
                  "the pack uses a different ellipsoid than the context");
        return;
    }
    if (l.type == pk::layer_type::geoid)
    {
        if (ctx.geoid != nullptr && ctx.geoid != &p)
        {
            fail_pack(ctx, p, VGL_RESULT_INVALID_OPERATION, "the context already has a geoid pack");
            return;
        }
        ctx.geoid = &p;
    }
    // Copy the attribution lines, null-terminated, for vgl_context_attribution_get.
    constexpr std::string_view attribution_key = "attribution";
    std::size_t size = 0;
    for (std::uint32_t i = 0; i < p.manifest.metadata_count(); ++i)
    {
        const pk::metadata_entry e = p.manifest.metadata(i);
        size += e.key == attribution_key ? e.value.size() + 1 : 0;
    }
    if (size != 0)
    {
        if (!p.credits.allocate(ctx.memory, size))
        {
            fail_pack(ctx, p, VGL_RESULT_OUT_OF_MEMORY, "its attribution could not be copied");
            if (ctx.geoid == &p)
            {
                ctx.geoid = nullptr;
            }
            return;
        }
        char* out = p.credits.data();
        for (std::uint32_t i = 0; i < p.manifest.metadata_count(); ++i)
        {
            const pk::metadata_entry e = p.manifest.metadata(i);
            if (e.key == attribution_key)
            {
                std::memcpy(out, e.value.data(), e.value.size());
                out += e.value.size();
                *out++ = '\0';
                ++p.credit_count;
            }
        }
    }
    p.state = VGL_PACK_STATE_READY;
}

void fail_shard(
    vgl_context& ctx, vgl_pack& p, shard_state& s, vgl_result why, const char* what) noexcept
{
    s.current = shard_state::state::failed;
    s.failure = why;
    // Running out of memory is not the shard's fault: it is read again when next needed.
    ctx.log.log(why == VGL_RESULT_OUT_OF_MEMORY ? VGL_LOG_LEVEL_WARNING : VGL_LOG_LEVEL_ERROR,
                "vgl_context_update: pack %s: shard %s: %s (%s)",
                p.uri.data(),
                s.path.data(),
                what,
                result_name(why));
}

void parse_directory(vgl_context& ctx,
                     vgl_pack& p,
                     shard_state& s,
                     std::span<const std::byte> stored) noexcept
{
    const vgl_result r = pk::shard_directory::parse(
        stored, s.header, p.manifest, ctx.memory, ctx.decompressor, s.directory);
    if (r != VGL_RESULT_SUCCESS)
    {
        fail_shard(ctx, p, s, r, "its directory is invalid");
        return;
    }
    s.current = shard_state::state::ready;
}

void handle_shard(vgl_context& ctx, vgl_pack& p, shard_state& s, vgl_io_request& r) noexcept
{
    if (r.result != VGL_RESULT_SUCCESS)
    {
        fail_shard(ctx, p, s, r.result, "reading it failed");
        return;
    }
    const std::span<const std::byte> data = r.data.span();
    if (r.target.target_kind == io_target::kind::shard_directory)
    {
        parse_directory(ctx, p, s, data);
        return;
    }
    const vgl_result header = pk::parse_shard_header(data, p.manifest, s.place, s.header);
    if (header != VGL_RESULT_SUCCESS)
    {
        fail_shard(ctx, p, s, header, "its header is invalid");
        return;
    }
    // The host delivers fewer bytes only at the end of the file.
    const std::uint64_t expected = std::min(shard_head_size, s.header.file_size);
    if (data.size() != expected)
    {
        fail_shard(ctx, p, s, VGL_RESULT_CORRUPT_DATA, "the file is shorter than its header says");
        return;
    }
    const pk::byte_range dr = pk::directory_range(s.header);
    if (dr.offset + dr.size <= data.size())
    {
        parse_directory(
            ctx,
            p,
            s,
            data.subspan(static_cast<std::size_t>(dr.offset), static_cast<std::size_t>(dr.size)));
        return;
    }
    io_target t = r.target;
    t.target_kind = io_target::kind::shard_directory;
    s.current = shard_state::state::loading_directory;
    if (start_read(ctx, t, p.uri.data(), s.path.data(), dr.offset, dr.size) != VGL_RESULT_SUCCESS)
    {
        fail_shard(ctx, p, s, VGL_RESULT_OUT_OF_MEMORY, "reading its directory could not start");
    }
}

void fail_tile(vgl_context& ctx,
               vgl_pack& p,
               tile_state& t,
               std::uint64_t key,
               vgl_result why,
               const char* what) noexcept
{
    t.current = tile_state::state::failed;
    t.failure = why;
    const tile_id id = tile_from_key(key);
    // As for shards, a tile that failed for lack of memory is read again when next needed.
    ctx.log.log(why == VGL_RESULT_OUT_OF_MEMORY ? VGL_LOG_LEVEL_WARNING : VGL_LOG_LEVEL_ERROR,
                "vgl_context_update: pack %s: tile %u/%u/%u: %s (%s)",
                p.uri.data(),
                id.level,
                id.x,
                id.y,
                what,
                result_name(why));
}

void decode_task(void* data) noexcept
{
    auto* job = static_cast<decode_job*>(data);
    async_core* core = job->core;
    if (!core->begin_task())
    {
        free_job(core->memory(), job);
        return;
    }
    job->run();
    if (!core->end_task(job))
    {
        free_job(core->memory(), job);
    }
}

void handle_tile(vgl_context& ctx, vgl_pack& p, tile_state& t, vgl_io_request& r) noexcept
{
    const std::uint64_t key = r.target.tile_key;
    if (r.result != VGL_RESULT_SUCCESS)
    {
        fail_tile(ctx, p, t, key, r.result, "reading it failed");
        return;
    }
    shard_state* s = p.shards.find(r.target.shard_key);
    pk::directory_entry e;
    if (s == nullptr || s->current != shard_state::state::ready ||
        !s->directory.find(tile_from_key(key), e))
    {
        fail_tile(ctx, p, t, key, VGL_RESULT_INTERNAL_ERROR, "its shard is gone");
        return;
    }
    vgl_result result = pk::check_tile(r.data.span(), e);
    if (result == VGL_RESULT_SUCCESS)
    {
        result = pk::parse_tile_header(r.data.span(), p.manifest, t.header);
    }
    if (result != VGL_RESULT_SUCCESS)
    {
        fail_tile(ctx, p, t, key, result, "it is invalid");
        return;
    }

    const pk::layer_description& layer = p.manifest.layer();
    if (layer.type == pk::layer_type::imagery)
    {
        // Imagery stays encoded until a texture is wanted (build_textures.cpp). stb_image
        // decodes JPEG and PNG; other encodings need a decoder vgl does not have yet.
        const auto encoding = static_cast<pk::imagery_encoding>(t.header.encoding);
        if (encoding != pk::imagery_encoding::jpeg && encoding != pk::imagery_encoding::png)
        {
            fail_tile(ctx,
                      p,
                      t,
                      key,
                      VGL_RESULT_NOT_SUPPORTED,
                      "its encoding has no decoder; bake the pack with JPEG or PNG tiles");
            return;
        }
        t.encoded = std::move(r.data);
        t.current = tile_state::state::ready;
        t.last_used = ctx.frame;
        ctx.cpu_used += t.cpu_bytes();
        ++ctx.tiles_loaded_total;
        ctx.load_seconds_total +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t.requested).count();
        return;
    }

    auto* job = ctx.memory.create<decode_job>();
    const pk::layer_description& l = p.manifest.layer();
    const std::size_t count = static_cast<std::size_t>(l.tile_size) * l.tile_size;
    if (job == nullptr || !job->q.allocate(ctx.memory, count) ||
        !job->scratch.allocate(ctx.memory, 2 * count))
    {
        if (job != nullptr)
        {
            ctx.memory.destroy(job);
        }
        fail_tile(ctx, p, t, key, VGL_RESULT_OUT_OF_MEMORY, "there is no memory to decode it");
        return;
    }
    job->core = ctx.core;
    ctx.core->retain();
    job->pack_id = p.id;
    job->tile_key = key;
    job->type = l.type;
    job->tile_size = l.tile_size;
    job->step = p.manifest.height_step(tile_from_key(key).level);
    job->bytes = std::move(r.data);
    job->header = t.header;
    t.current = tile_state::state::decoding;

    if (ctx.has_tasks &&
        ctx.tasks.submit(ctx.tasks.user_data, &decode_task, job) == VGL_RESULT_SUCCESS)
    {
        return;
    }
    if (!ctx.pending.push_back(job))
    {
        free_job(ctx.memory, job);
        fail_tile(
            ctx, p, t, key, VGL_RESULT_OUT_OF_MEMORY, "there is no memory to queue its decoding");
    }
}

void handle_request(vgl_context& ctx, vgl_io_request* r) noexcept
{
    static_cast<void>(ctx.outstanding.erase(r->id));
    ++ctx.reads_total;
    ctx.bytes_read_total += r->data.size();
    vgl_pack* p = find_pack(ctx, r->target.pack_id);
    if (p != nullptr)
    {
        switch (r->target.target_kind)
        {
        case io_target::kind::manifest:
            if (p->state == VGL_PACK_STATE_LOADING)
            {
                handle_manifest(ctx, *p, *r);
            }
            break;
        case io_target::kind::shard_head:
        case io_target::kind::shard_directory:
            if (shard_state* s = p->shards.find(r->target.shard_key); s != nullptr)
            {
                handle_shard(ctx, *p, *s, *r);
            }
            break;
        case io_target::kind::tile:
            if (tile_state* t = p->tiles.find(r->target.tile_key);
                t != nullptr && t->current == tile_state::state::loading)
            {
                handle_tile(ctx, *p, *t, *r);
            }
            break;
        }
    }
    free_request(ctx, r);
}

void handle_decoded(vgl_context& ctx, decode_job* job) noexcept
{
    vgl_pack* p = find_pack(ctx, job->pack_id);
    tile_state* t = p == nullptr ? nullptr : p->tiles.find(job->tile_key);
    if (t != nullptr && t->current == tile_state::state::decoding)
    {
        if (job->result == VGL_RESULT_SUCCESS)
        {
            t->q = std::move(job->q);
            t->min_height = job->min_height;
            t->max_height = job->max_height;
            t->current = tile_state::state::ready;
            t->last_used = ctx.frame;
            ctx.cpu_used += t->q.size() * sizeof(std::int16_t);
            ++ctx.tiles_loaded_total;
            ctx.load_seconds_total +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t->requested)
                    .count();
        }
        else
        {
            fail_tile(ctx, *p, *t, job->tile_key, job->result, "decoding it failed");
        }
    }
    free_job(ctx.memory, job);
}

void process_completions(vgl_context& ctx) noexcept
{
    async_item* item = ctx.core->take_all();
    while (item != nullptr)
    {
        async_item* next = item->next;
        if (item->item_kind == async_item::kind::io)
        {
            // item_kind gives the type.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            handle_request(ctx, static_cast<vgl_io_request*>(item));
        }
        else if (item->item_kind == async_item::kind::task)
        {
            // item_kind gives the type.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            handle_decoded(ctx, static_cast<decode_job*>(item));
        }
        else if (item->item_kind == async_item::kind::mesh)
        {
            // item_kind gives the type.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            handle_mesh_built(ctx, static_cast<mesh_job*>(item));
        }
        else
        {
            // item_kind gives the type.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            handle_texture_built(ctx, static_cast<texture_job*>(item));
        }
        item = next;
    }
}

// Frees decoded tiles, least recently used first, until the CPU budget is met. Tiles used in
// this frame, since the last update, are kept.
void evict(vgl_context& ctx) noexcept
{
    // Meshes and textures waiting for upload first: they are rebuilt from tiles, not reread.
    evict_pending_meshes(ctx);
    evict_pending_textures(ctx);
    if (ctx.cpu_used <= ctx.cpu_budget)
    {
        return;
    }
    struct candidate
    {
        std::uint64_t last_used;
        vgl_pack* pack;
        std::uint64_t key;
    };
    vector<candidate> candidates(ctx.memory);
    for (vgl_pack* p : ctx.packs)
    {
        p->tiles.for_each([&](std::uint64_t key, tile_state& t) {
            if (t.current == tile_state::state::ready && t.last_used < ctx.frame)
            {
                static_cast<void>(candidates.push_back({t.last_used, p, key}));
            }
        });
    }
    std::sort(candidates.begin(), candidates.end(), [](const candidate& a, const candidate& b) {
        return a.last_used < b.last_used;
    });
    for (const candidate& c : candidates)
    {
        if (ctx.cpu_used <= ctx.cpu_budget)
        {
            break;
        }
        tile_state* t = c.pack->tiles.find(c.key);
        ctx.cpu_used -= t->cpu_bytes();
        static_cast<void>(c.pack->tiles.erase(c.key));
    }
}

// --- Queries -------------------------------------------------------------------------------

} // namespace

availability find_tile(vgl_context& ctx,
                       vgl_pack& p,
                       const tile_id& t,
                       tile_state*& out,
                       vgl_result& error,
                       bool start) noexcept
{
    const pk::layer_description& l = p.manifest.layer();
    if (t.level < l.min_level || t.level > l.max_level)
    {
        return availability::absent;
    }
    const shard_location loc = tile_shard(t, l.shard_level);
    const std::uint64_t shard_key = loc.in_base ? base_shard_key : morton(loc.root.x, loc.root.y);
    shard_state* s = p.shards.find(shard_key);
    if (s != nullptr && s->current == shard_state::state::failed &&
        s->failure == VGL_RESULT_OUT_OF_MEMORY)
    {
        static_cast<void>(p.shards.erase(shard_key)); // Try again.
        s = nullptr;
    }
    if (s == nullptr)
    {
        if (!start)
        {
            // Whether the shard exists is in the manifest; whether the tile does, in the shard.
            pk::shard_entry e;
            const bool exists = loc.in_base ? p.manifest.has_base()
                                            : p.manifest.find_shard(loc.root.x, loc.root.y, e);
            return exists ? availability::wanted : availability::absent;
        }
        shard_state fresh;
        std::array<char, pk::max_path_size> name{};
        std::size_t length = 0;
        if (loc.in_base)
        {
            if (!p.manifest.has_base())
            {
                return availability::absent;
            }
            length = pk::base_shard_path(p.manifest.base_digest(), name);
            fresh.place = {true, 0, 0};
        }
        else
        {
            pk::shard_entry e;
            if (!p.manifest.find_shard(loc.root.x, loc.root.y, e))
            {
                return availability::absent;
            }
            length = pk::shard_path(e.x, e.y, e.digest, name);
            fresh.place = {false, e.x, e.y};
        }
        if (!copy_string(ctx.memory, name.data(), length, fresh.path))
        {
            error = VGL_RESULT_OUT_OF_MEMORY;
            return availability::pending;
        }
        s = p.shards.insert(shard_key, std::move(fresh));
        if (s == nullptr)
        {
            error = VGL_RESULT_OUT_OF_MEMORY;
            return availability::pending;
        }
        const io_target target{io_target::kind::shard_head, p.id, shard_key, 0};
        if (start_read(ctx, target, p.uri.data(), s->path.data(), 0, shard_head_size) !=
            VGL_RESULT_SUCCESS)
        {
            static_cast<void>(p.shards.erase(shard_key));
            error = VGL_RESULT_OUT_OF_MEMORY;
        }
        return availability::pending;
    }
    switch (s->current)
    {
    case shard_state::state::loading:
    case shard_state::state::loading_directory:
        return availability::pending;
    case shard_state::state::failed:
        return availability::absent;
    case shard_state::state::ready:
        break;
    }
    pk::directory_entry e;
    if (!s->directory.find(t, e))
    {
        return availability::absent;
    }
    const std::uint64_t key = tile_key(t);
    tile_state* tile = p.tiles.find(key);
    if (tile != nullptr && tile->current == tile_state::state::failed &&
        tile->failure == VGL_RESULT_OUT_OF_MEMORY)
    {
        static_cast<void>(p.tiles.erase(key)); // Try again.
        tile = nullptr;
    }
    if (tile == nullptr)
    {
        if (!start)
        {
            return availability::wanted;
        }
        tile = p.tiles.insert(key, tile_state{});
        if (tile == nullptr)
        {
            error = VGL_RESULT_OUT_OF_MEMORY;
            return availability::pending;
        }
        tile->requested = std::chrono::steady_clock::now();
        const pk::byte_range r = s->directory.tile_range(e);
        const io_target target{io_target::kind::tile, p.id, shard_key, key};
        if (start_read(ctx, target, p.uri.data(), s->path.data(), r.offset, r.size) !=
            VGL_RESULT_SUCCESS)
        {
            static_cast<void>(p.tiles.erase(key));
            error = VGL_RESULT_OUT_OF_MEMORY;
        }
        return availability::pending;
    }
    switch (tile->current)
    {
    case tile_state::state::loading:
    case tile_state::state::decoding:
        return availability::pending;
    case tile_state::state::failed:
        return availability::absent;
    case tile_state::state::ready:
        break;
    }
    tile->last_used = ctx.frame;
    out = tile;
    return availability::ready;
}

namespace
{

struct sample
{
    bool valid = false;   // value holds a height.
    bool pending = false; // Finer data is loading.
    double value = std::numeric_limits<double>::quiet_NaN();
};

// The height of a pack at a position in degrees, from the deepest level loaded so far. Starts
// the reads for the next level down.
sample
sample_pack(vgl_context& ctx, vgl_pack& p, double lat, double lon, vgl_result& error) noexcept
{
    sample result;
    const pk::layer_description& l = p.manifest.layer();
    if (lat < l.south || lat > l.north || (lon < l.west && lon + 360.0 > l.east) ||
        (lon > l.east && lon - 360.0 < l.west))
    {
        return result;
    }
    const std::uint32_t n = l.tile_size;
    for (std::uint32_t level = l.min_level; level <= l.max_level; ++level)
    {
        const tile_id id = tile_at(lat, lon, level);
        tile_state* t = nullptr;
        const availability a = find_tile(ctx, p, id, t, error);
        if (a == availability::pending)
        {
            result.pending = true;
            break;
        }
        if (a == availability::absent)
        {
            break;
        }
        bool nodata = false;
        const double h = tile_interpolate(id, n, lat, lon, [&](std::uint32_t i, std::uint32_t j) {
            const std::size_t k = static_cast<std::size_t>(j) * n + i;
            nodata = nodata || t->q.data()[k] == elevation_nodata;
            return t->height(k);
        });
        if (nodata)
        {
            break;
        }
        result.valid = true;
        result.value = h;
        if (level == l.max_level ||
            !t->header.has_child(tile_child_index(tile_at(lat, lon, level + 1))))
        {
            break;
        }
    }
    return result;
}

} // namespace

void decode_job::run() noexcept
{
    zstd_decompressor* d = core->acquire_decompressor();
    if (d == nullptr)
    {
        result = VGL_RESULT_OUT_OF_MEMORY;
    }
    else
    {
        result = pack::decode_height_samples(
            bytes.span(), header, type, tile_size, step, *d, q.span(), scratch.span());
        core->release_decompressor(d);
    }
    if (result == VGL_RESULT_SUCCESS)
    {
        std::int16_t lo = std::numeric_limits<std::int16_t>::max();
        std::int16_t hi = std::numeric_limits<std::int16_t>::min();
        for (const std::int16_t v : q.span())
        {
            if (v != elevation_nodata)
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        // With every sample nodata, lo > hi, and so is the range.
        min_height = header.offset + header.scale * lo;
        max_height = header.offset + header.scale * hi;
    }
    bytes = buffer<std::byte>();
    scratch = buffer<std::byte>();
}

} // namespace vgl::internal

using namespace vgl::internal;

extern "C" VGL_API vgl_result vgl_context_create(const vgl_context_create_info* info,
                                                 vgl_context** out_context)
{
    if (out_context == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_context = nullptr;
    vgl_context_create_info settings;
    vgl_result r = read_versioned(info, sizeof(vgl_context_create_info), settings);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    if (settings.log_level < VGL_LOG_LEVEL_DEFAULT || settings.log_level > VGL_LOG_LEVEL_DEBUG)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }

    allocator memory;
    if (settings.allocator != nullptr)
    {
        r = validate_allocator_interface(settings.allocator);
        if (r != VGL_RESULT_SUCCESS)
        {
            return r;
        }
        memory = allocator(*settings.allocator);
    }
    if (settings.io != nullptr)
    {
        r = check_interface_version(*settings.io, VGL_IO_INTERFACE_VERSION);
        if (r != VGL_RESULT_SUCCESS)
        {
            return r;
        }
        if (settings.io->read == nullptr)
        {
            return VGL_RESULT_INVALID_ARGUMENT;
        }
    }
    if (settings.tasks != nullptr)
    {
        r = check_interface_version(*settings.tasks, VGL_TASK_INTERFACE_VERSION);
        if (r != VGL_RESULT_SUCCESS)
        {
            return r;
        }
        if (settings.tasks->submit == nullptr)
        {
            return VGL_RESULT_INVALID_ARGUMENT;
        }
    }
    if (settings.ellipsoid != nullptr &&
        !ellipsoid::is_valid(settings.ellipsoid->semi_major_axis, settings.ellipsoid->flattening))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }

    auto* ctx = memory.create<vgl_context>(memory);
    if (ctx == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    ctx->core = async_core::create(memory);
    if (ctx->core == nullptr || !ctx->decompressor.valid())
    {
        if (ctx->core != nullptr)
        {
            ctx->core->release();
        }
        memory.destroy(ctx);
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    ctx->log = logger(settings.log, settings.log_user_data, settings.log_level);
    if (settings.ellipsoid != nullptr)
    {
        ctx->shape = ellipsoid::from_c(*settings.ellipsoid);
    }
    if (settings.io != nullptr)
    {
        ctx->has_io = true;
        ctx->io = *settings.io;
    }
    if (settings.tasks != nullptr)
    {
        ctx->has_tasks = true;
        ctx->tasks = *settings.tasks;
    }
    ctx->cpu_budget = settings.cpu_budget == 0 ? default_cpu_budget : settings.cpu_budget;
    ctx->gpu_budget = settings.gpu_budget == 0 ? default_gpu_budget : settings.gpu_budget;
    ctx->frame_handle.context = ctx;
    *out_context = ctx;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_pack_destroy(vgl_pack* pack)
{
    vgl_pack* const p = pack;
    if (p == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    vgl_context& ctx = *p->context;
    // Outstanding reads stay with the host; their completions find no pack and are dropped.
    if (ctx.io.cancel != nullptr)
    {
        ctx.outstanding.for_each([&](std::uint64_t, vgl_io_request* r) {
            if (r->target.pack_id == p->id)
            {
                ctx.io.cancel(ctx.io.user_data, r);
            }
        });
    }
    for (std::size_t i = ctx.pending.size(); i-- > 0;)
    {
        if (ctx.pending[i]->pack_id == p->id)
        {
            free_job(ctx.memory, ctx.pending[i]);
            ctx.pending.erase_unordered(i);
        }
    }
    p->tiles.for_each([&](std::uint64_t, tile_state& t) {
        if (t.current == tile_state::state::ready)
        {
            ctx.cpu_used -= t.cpu_bytes();
        }
    });
    drop_pack_meshes(ctx, p->id);
    drop_pack_textures(ctx, *p);
    for (std::size_t i = 0; i < ctx.packs.size(); ++i)
    {
        if (ctx.packs[i] == p)
        {
            // Keep the priority order: shift the rest up.
            for (std::size_t j = i + 1; j < ctx.packs.size(); ++j)
            {
                ctx.packs[j - 1] = ctx.packs[j];
            }
            ctx.packs.pop_back();
            break;
        }
    }
    if (ctx.geoid == p)
    {
        ctx.geoid = nullptr;
    }
    ctx.memory.destroy(p);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_context_destroy(vgl_context* context)
{
    vgl_context* const ctx = context;
    if (ctx == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    while (!ctx->packs.empty())
    {
        vgl_pack* p = ctx->packs[0];
        ctx->log.log(
            VGL_LOG_LEVEL_WARNING,
            "vgl_context_destroy: pack %s was not destroyed; destroy packs before their context",
            p->uri.data());
        static_cast<void>(vgl_pack_destroy(p));
    }
    // Stop taking completions, wait for running tasks, and free what was queued.
    async_item* item = ctx->core->shut_down();
    while (item != nullptr)
    {
        async_item* next = item->next;
        if (item->item_kind == async_item::kind::io)
        {
            // item_kind gives the type.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            auto* r = static_cast<vgl_io_request*>(item);
            static_cast<void>(ctx->outstanding.erase(r->id));
            free_request(*ctx, r);
        }
        else if (item->item_kind == async_item::kind::task)
        {
            // item_kind gives the type.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            free_job(ctx->memory, static_cast<decode_job*>(item));
        }
        else if (item->item_kind == async_item::kind::mesh)
        {
            // item_kind gives the type.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            free_mesh_job(ctx->memory, static_cast<mesh_job*>(item));
        }
        else
        {
            // item_kind gives the type.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            free_texture_job(ctx->memory, static_cast<texture_job*>(item));
        }
        item = next;
    }
    for (decode_job* job : ctx->pending)
    {
        free_job(ctx->memory, job);
    }
    ctx->pending.clear();
    // Reads still with the host free themselves when completed.
    if (ctx->io.cancel != nullptr)
    {
        vector<vgl_io_request*> left(ctx->memory);
        ctx->outstanding.for_each(
            [&](std::uint64_t, vgl_io_request* r) { static_cast<void>(left.push_back(r)); });
        ctx->outstanding.clear();
        for (vgl_io_request* r : left)
        {
            ctx->io.cancel(ctx->io.user_data, r);
        }
    }
    if (ctx->has_io && ctx->io.destroy != nullptr)
    {
        ctx->io.destroy(ctx->io.user_data);
    }
    if (ctx->has_tasks && ctx->tasks.destroy != nullptr)
    {
        ctx->tasks.destroy(ctx->tasks.user_data);
    }
    async_core* core = ctx->core;
    const allocator memory = ctx->memory;
    memory.destroy(ctx);
    core->release();
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_context_allocator_get(const vgl_context* context,
                                                        vgl_allocator_interface* out_allocator)
{
    if (context == nullptr || out_allocator == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_allocator = context->memory.host_interface();
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_context_log(const vgl_context* context,
                                              vgl_log_level level,
                                              const char* message)
{
    if (context == nullptr || message == nullptr || level < VGL_LOG_LEVEL_ERROR ||
        level > VGL_LOG_LEVEL_DEBUG)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    context->log.log(level, "%s", message);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_context_update(vgl_context* context, const vgl_update_info* info)
{
    vgl_context* const ctx = context;
    if (ctx == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_update_info settings;
    const vgl_result r = read_versioned(info, sizeof(vgl_update_info), settings);
    if (r != VGL_RESULT_SUCCESS || !std::isfinite(settings.time_budget) ||
        settings.time_budget < 0.0)
    {
        return r != VGL_RESULT_SUCCESS ? r : VGL_RESULT_INVALID_ARGUMENT;
    }
    if (ctx->frame_open)
    {
        ctx->log.log(VGL_LOG_LEVEL_ERROR,
                     "vgl_context_update: a frame is open; call vgl_frame_end before updating");
        return VGL_RESULT_INVALID_OPERATION;
    }
    const double budget = settings.time_budget == 0.0 ? default_time_budget : settings.time_budget;
    const auto start = std::chrono::steady_clock::now();

    process_completions(*ctx);

    // Without a task interface, decode here, oldest first, within the time budget.
    std::size_t done = 0;
    while (done < ctx->pending.size())
    {
        decode_job* job = ctx->pending[done++];
        job->run();
        handle_decoded(*ctx, job);
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
            budget)
        {
            break;
        }
    }
    if (done != 0)
    {
        for (std::size_t i = done; i < ctx->pending.size(); ++i)
        {
            ctx->pending[i - done] = ctx->pending[i];
        }
        for (std::size_t i = 0; i < done; ++i)
        {
            ctx->pending.pop_back();
        }
    }

    if (ctx->has_view)
    {
        select_tiles(*ctx);
        const auto deadline =
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(budget));
        build_meshes(*ctx, deadline);
        build_textures(*ctx, deadline);
    }

    // The frame ends here: what queries and selection used since the last update is kept.
    evict(*ctx);
    ++ctx->frame;
    ctx->update_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_pack_create(vgl_context* context,
                                              const vgl_pack_create_info* info,
                                              vgl_pack** out_pack)
{
    vgl_context* const ctx = context;
    if (out_pack == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_pack = nullptr;
    if (ctx == nullptr || info == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_pack_create_info settings;
    const vgl_result r = read_versioned(info, sizeof(vgl_pack_create_info), settings);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    if (settings.uri == nullptr)
    {
        ctx->log.log(VGL_LOG_LEVEL_ERROR,
                     "vgl_pack_create: info->uri is null; set it to the pack's directory");
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (!ctx->has_io)
    {
        ctx->log.log(VGL_LOG_LEVEL_ERROR,
                     "vgl_pack_create: the context has no I/O interface; pass one in "
                     "vgl_context_create_info::io");
        return VGL_RESULT_INVALID_OPERATION;
    }
    auto* p = ctx->memory.create<vgl_pack>(ctx->memory);
    if (p == nullptr || !copy_string(ctx->memory, settings.uri, std::strlen(settings.uri), p->uri))
    {
        ctx->memory.destroy(p);
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    p->context = ctx;
    p->id = ctx->next_pack_id++;
    p->priority = settings.priority;
    p->order = ctx->next_order++;
    if (!insert_pack(*ctx, p))
    {
        ctx->memory.destroy(p);
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    const io_target target{io_target::kind::manifest, p->id, 0, 0};
    std::array<char, 16> name{};
    std::memcpy(name.data(), pack::manifest_file_name.data(), pack::manifest_file_name.size());
    if (start_read(*ctx, target, p->uri.data(), name.data(), 0, VGL_IO_SIZE_TO_END) !=
        VGL_RESULT_SUCCESS)
    {
        static_cast<void>(vgl_pack_destroy(p));
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    *out_pack = p;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_pack_state_get(const vgl_pack* pack,
                                                 vgl_pack_state* out_state,
                                                 vgl_result* out_failure)
{
    const vgl_pack* const p = pack;
    if (p == nullptr || out_state == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_state = p->state;
    if (out_failure != nullptr)
    {
        *out_failure = p->failure;
    }
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_API vgl_result vgl_context_height_query(vgl_context* context,
                                                       double latitude,
                                                       double longitude,
                                                       vgl_height_reference reference,
                                                       double* out_height)
{
    vgl_context* const ctx = context;
    if (ctx == nullptr || out_height == nullptr || !std::isfinite(latitude) ||
        !std::isfinite(longitude) || std::fabs(latitude) > pi / 2.0 + pole_tolerance ||
        (reference != VGL_HEIGHT_REFERENCE_ELLIPSOID &&
         reference != VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL))
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_height = std::numeric_limits<double>::quiet_NaN();
    const bool msl = reference == VGL_HEIGHT_REFERENCE_MEAN_SEA_LEVEL;
    if (msl && (ctx->geoid == nullptr || ctx->geoid->state == VGL_PACK_STATE_FAILED))
    {
        // A geoid pack still loading its manifest is not ctx->geoid yet; look for one.
        bool loading = false;
        for (vgl_pack* p : ctx->packs)
        {
            loading = loading || p->state == VGL_PACK_STATE_LOADING;
        }
        if (!loading)
        {
            ctx->log.log(VGL_LOG_LEVEL_ERROR,
                         "vgl_context_height_query: heights above mean sea level need a geoid "
                         "pack; add one with "
                         "vgl_pack_create");
            return VGL_RESULT_INVALID_OPERATION;
        }
        return VGL_RESULT_NOT_READY;
    }

    const double lat = std::clamp(degrees(latitude), -90.0, 90.0);
    const double lon = degrees(longitude);
    vgl_result error = VGL_RESULT_SUCCESS;
    bool pending = false;
    bool found = false;
    double height = std::numeric_limits<double>::quiet_NaN();
    for (vgl_pack* p : ctx->packs)
    {
        if (p->state == VGL_PACK_STATE_LOADING)
        {
            pending = true;
            continue;
        }
        if (p->state != VGL_PACK_STATE_READY ||
            p->manifest.layer().type != pack::layer_type::elevation)
        {
            continue;
        }
        const sample s = sample_pack(*ctx, *p, lat, lon, error);
        pending = pending || s.pending;
        if (s.valid)
        {
            height = s.value;
            found = true;
            break;
        }
    }
    if (error != VGL_RESULT_SUCCESS)
    {
        return error;
    }
    if (!found)
    {
        return pending ? VGL_RESULT_NOT_READY : VGL_RESULT_NOT_FOUND;
    }
    if (msl)
    {
        const sample n = sample_pack(*ctx, *ctx->geoid, lat, lon, error);
        if (error != VGL_RESULT_SUCCESS)
        {
            return error;
        }
        if (!n.valid)
        {
            return VGL_RESULT_NOT_READY;
        }
        height -= n.value;
        pending = pending || n.pending;
    }
    *out_height = height;
    return pending ? VGL_RESULT_NOT_READY : VGL_RESULT_SUCCESS;
}
