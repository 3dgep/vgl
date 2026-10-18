// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The context and pack objects behind vgl/context.h.
//
// Everything here is touched only on the thread that calls into the context, except decode
// jobs while a task runs them; those carry copies of what they need, so that a pack can be
// destroyed while its tiles decode.

#ifndef VGL_CONTEXT_CONTEXT_IMPL_HPP
#define VGL_CONTEXT_CONTEXT_IMPL_HPP

#include "decoders/image_decoder.hpp"
#include "decoders/zstd_decompressor.hpp"
#include "foundation/allocator.hpp"
#include "foundation/async.hpp"
#include "foundation/buffer.hpp"
#include "foundation/containers.hpp"
#include "foundation/log.hpp"
#include "geodesy/ellipsoid.hpp"
#include "mesh/tile_mesh.hpp"
#include "providers/pack_reader.hpp"
#include "tiling/selection.hpp"
#include "tiling/tile.hpp"

#include <vgl/context.h>
#include <vgl/render.h>

#include <chrono>
#include <cstdint>

namespace vgl::internal
{

// Key of the base shard in a pack's shard table; level-S shards use morton(x, y).
inline constexpr std::uint64_t base_shard_key = ~0ull;

struct shard_state
{
    enum class state : std::uint8_t
    {
        loading,           // Reading the header (and usually the directory with it).
        loading_directory, // Reading the rest of the directory.
        ready,
        failed,
    };

    state current = state::loading;
    vgl_result failure = VGL_RESULT_SUCCESS; // Why it failed.
    pack::shard_place place;
    pack::shard_header header;
    pack::shard_directory directory;
    buffer<char> path; // Relative to the pack, null-terminated.
};

struct tile_state
{
    enum class state : std::uint8_t
    {
        loading,
        decoding,
        ready,
        failed,
    };

    state current = state::loading;
    vgl_result failure = VGL_RESULT_SUCCESS; // Why it failed.
    pack::tile_header header;
    buffer<std::int16_t> q;    // Heights: the quantized samples once ready.
    buffer<std::byte> encoded; // Imagery: the tile as read, header included, once ready.
    std::uint64_t last_used = 0;
    std::chrono::steady_clock::time_point requested; // When its read started.
    // Range of the decoded heights, without nodata. min_height > max_height if every sample
    // is nodata.
    double min_height = 0.0;
    double max_height = 0.0;

    [[nodiscard]] double height(std::size_t k) const noexcept
    {
        return header.offset + header.scale * q.data()[k];
    }

    // Bytes of CPU memory the tile holds.
    [[nodiscard]] std::uint64_t cpu_bytes() const noexcept
    {
        return q.size() * sizeof(std::int16_t) + encoded.size();
    }
};

// Decoding of one elevation or geoid tile, on a task thread or in vgl_context_update.
struct decode_job : async_item
{
    decode_job() noexcept : async_item(kind::task) {}

    async_core* core = nullptr;
    std::uint32_t pack_id = 0;
    std::uint64_t tile_key = 0;
    pack::layer_type type = pack::layer_type::elevation;
    std::uint32_t tile_size = 0;
    double step = 0.0;
    buffer<std::byte> bytes;
    pack::tile_header header;
    buffer<std::int16_t> q;
    buffer<std::byte> scratch;
    vgl_result result = VGL_RESULT_SUCCESS;
    double min_height = 0.0;
    double max_height = 0.0;

    // Decodes bytes into q and finds the height range, then frees the bytes and scratch.
    void run() noexcept;
};

// Building of one tile mesh, on a task thread or in vgl_context_update. Everything it needs
// is copied or allocated before it runs, so that it neither allocates nor touches the context.
struct mesh_job : async_item
{
    mesh_job() noexcept : async_item(kind::mesh) {}

    async_core* core = nullptr;
    tile_id id;
    tile_id source;
    std::uint32_t pack_id = 0;
    std::uint64_t generation = 0; // The context's terrain generation it was built in.
    std::uint32_t grid = 0;
    ellipsoid shape;
    double skirt_height = 0.0;
    double tolerance = 0.0;   // Of a coarser grid, in metres; 0 keeps every sample.
    bool placeholder = false; // A small stand-in, built at once, until the tile's mesh is ready.
    buffer<float> heights;
    buffer<dvec3> scratch;
    buffer<tile_vertex> vertices;
    buffer<tile_sample> samples; // At the grid's full resolution.
    std::uint32_t sample_grid = 0;
    dvec3 centre;
    double min_height = 0.0;
    double max_height = 0.0;

    // Coarsens the grid within the tolerance, builds the vertices, then frees the heights and
    // scratch.
    void run() noexcept;
};

// Decoding of one imagery tile into a texture with its mip levels, on a task thread or in
// vgl_context_update. It holds a copy of the encoded tile, so that the tile may be evicted
// meanwhile.
struct texture_job : async_item
{
    texture_job() noexcept : async_item(kind::texture) {}

    async_core* core = nullptr;
    std::uint32_t pack_id = 0;
    std::uint64_t tile_key = 0;
    std::uint32_t side = 0; // Pixels per side, the border included.
    buffer<std::byte> file; // The encoded image.
    mip_chain image;
    vgl_result result = VGL_RESULT_SUCCESS;

    // Decodes file into image, then frees the file.
    void run() noexcept;
};

// The texture of one imagery tile of a pack, by tile key in the pack.
struct texture_state
{
    std::uint64_t current = 0;     // Resource, or 0.
    bool decoding = false;         // A texture job is running for it.
    std::uint64_t last_wanted = 0; // Update in which a selected tile last wanted it.
};

// A resource of the render interface (docs/render-interface.md, section 4): a tile mesh, a
// shared index list or an imagery texture. It keeps its CPU copy until the consumer has
// created it.
struct gpu_resource
{
    enum class kind : std::uint8_t
    {
        mesh,
        index_list,
        texture,
    };

    enum class state : std::uint8_t
    {
        pending,  // Not offered yet, or offered and failed.
        offered,  // In the open frame's create lists.
        resident, // Created by the consumer.
    };

    kind resource_kind = kind::mesh;
    state current = state::pending;
    bool garbage = false;     // To destroy in the next frame.
    bool failed = false;      // Reported failed in the open frame.
    std::uint64_t bytes = 0;  // Size on the GPU.
    std::uint64_t handle = 0; // As set by the consumer.
    std::uint64_t last_drawn = 0;
    std::uint64_t last_wanted = 0; // Textures: frame in which a selected tile wanted it.

    // Meshes and textures.
    tile_id tile;
    tile_id source;
    std::uint32_t pack_id = 0;
    std::uint64_t generation = 0; // Meshes: the terrain generation they were built in.
    std::uint32_t grid = 0;
    dvec3 centre;
    double min_height = 0.0;
    double max_height = 0.0;
    // The geoid's heights at the tile's corners (north-west, north-east, south-west,
    // south-east), once a geoid pack has a tile there.
    std::array<float, 4> geoid = {};
    bool has_geoid = false;
    bool placeholder = false;     // A small stand-in until the tile's own mesh is ready.
    buffer<tile_vertex> vertices; // Until resident.
    buffer<tile_sample> samples;  // Until resident.
    std::uint32_t sample_grid = 0;

    // Index lists.
    buffer<std::byte> indices; // Until resident.
    std::uint32_t index_count = 0;
    std::uint32_t index_size = 0;

    // Textures: the image and its mip levels until resident, with the pointers and sizes that
    // the frame's texture list points at.
    mip_chain image;
    std::array<const void*, max_mip_levels> mip_data = {};
    std::array<std::size_t, max_mip_levels> mip_sizes = {};

    // Bytes of CPU memory the resource holds.
    [[nodiscard]] std::uint64_t cpu_bytes() const noexcept
    {
        return vertices.size() * sizeof(tile_vertex) + samples.size() * sizeof(tile_sample) +
               indices.size() + image.pixels.size();
    }

    // Frees the CPU copy, once the consumer has the resource or will never get it.
    void free_cpu_copy() noexcept
    {
        vertices = buffer<tile_vertex>();
        samples = buffer<tile_sample>();
        indices = buffer<std::byte>();
        image.pixels = buffer<std::uint8_t>();
    }
};

// The meshes of a selected tile, by tile key: the one drawn, which is resident, and a newer
// one waiting to be created. A tile keeps drawing its current mesh until the next is resident.
struct mesh_state
{
    std::uint64_t current = 0; // Resource, or 0.
    std::uint64_t next = 0;    // Resource, or 0.
    bool building = false;
    std::uint64_t last_used = 0;
};

// A tile drawn in the last frame, with the mesh it was drawn with.
struct drawn_tile
{
    tile_id id;
    std::uint64_t mesh = 0;
};

// A tile chosen for drawing (design document, section 7.3).
struct selected_tile
{
    tile_id id;
    tile_id source;            // The tile whose data draws it: itself, or an ancestor.
    std::uint32_t pack_id = 0; // Pack of the source; 0 for the bare ellipsoid.
    double min_height = 0.0;
    double max_height = 0.0;
};

// A tile that selection wants loaded.
struct load_request
{
    std::uint32_t pack_id = 0;
    tile_id id;
    double screen_space_error = 0.0;
};

// Bounding box of a tile for a height range, kept between frames.
struct cached_box
{
    oriented_box box;
    dvec3 horizon_point; // In the horizon occluder's scaled space, if has_horizon_point.
    bool has_horizon_point = false;
    double min_height = 0.0;
    double max_height = 0.0;
    std::uint64_t last_used = 0;
};

enum class availability : std::uint8_t
{
    absent,  // The pack has no such tile.
    wanted,  // It may exist, and no read for it has started; only when not starting reads.
    pending, // It is loading.
    ready,
};

// Finds a tile in a pack. With start, it starts the reads the tile needs; without, it reports
// availability::wanted instead. error receives VGL_RESULT_OUT_OF_MEMORY if a read could not
// start.
availability find_tile(vgl_context& ctx,
                       vgl_pack& p,
                       const tile_id& t,
                       tile_state*& out,
                       vgl_result& error,
                       bool start = true) noexcept;

// The geoid height at a position in degrees, from the deepest geoid tile loaded there; starts
// the reads for finer ones. False without a ready geoid pack, or before it has a tile there.
[[nodiscard]] bool sample_geoid(vgl_context& ctx, double lat, double lon, double& out) noexcept;

// Selects tiles for the context's view and starts the reads they need.
void select_tiles(vgl_context& ctx) noexcept;

// Starts building the meshes of selected tiles that need one, while time remains before
// deadline; at least one.
void build_meshes(vgl_context& ctx, std::chrono::steady_clock::time_point deadline) noexcept;

// Installs a built mesh, and frees the job.
void handle_mesh_built(vgl_context& ctx, mesh_job* job) noexcept;

// Frees a mesh job without installing it.
void free_mesh_job(const allocator& a, mesh_job* job) noexcept;

// Forgets the meshes drawn from a pack.
void drop_pack_meshes(vgl_context& ctx, std::uint32_t pack_id) noexcept;

// The newest mesh of a tile, resident or not, or null.
[[nodiscard]] const gpu_resource* latest_mesh(const vgl_context& ctx, const tile_id& t) noexcept;

// Gives up a resource: frees it at once if the consumer never received it, or else destroys it
// in the next frame.
void retire_resource(vgl_context& ctx, std::uint64_t id) noexcept;

// Frees the CPU copies of resources that tiles not used in this frame are waiting to upload,
// least recently used first, until the CPU budget is met.
void evict_pending_meshes(vgl_context& ctx) noexcept;

// Starts decoding the textures that selected tiles need, while time remains before deadline;
// at least one. Imagery tiles are kept encoded, and decoded when a texture is wanted.
void build_textures(vgl_context& ctx, std::chrono::steady_clock::time_point deadline) noexcept;

// Installs a decoded texture, and frees the job.
void handle_texture_built(vgl_context& ctx, texture_job* job) noexcept;

// Frees a texture job without installing it.
void free_texture_job(const allocator& a, texture_job* job) noexcept;

// Gives up the textures of a pack.
void drop_pack_textures(vgl_context& ctx, vgl_pack& p) noexcept;

// Like evict_pending_meshes, for textures that no selected tile wanted in this update.
void evict_pending_textures(vgl_context& ctx) noexcept;

// The resident texture that draws tile t from imagery pack p: its own, or the deepest
// ancestor's. Returns 0 if there is none, and sets level to the imagery tile's level.
[[nodiscard]] std::uint64_t texture_for(const vgl_context& ctx,
                                        const vgl_pack& p,
                                        const tile_id& t,
                                        std::uint32_t& level) noexcept;

} // namespace vgl::internal

struct vgl_pack
{
    explicit vgl_pack(const vgl::internal::allocator& a) noexcept : shards(a), tiles(a), textures(a)
    {
    }

    vgl_context* context = nullptr;
    std::uint32_t id = 0;
    std::int32_t priority = 0;
    std::uint64_t order = 0;
    float opacity = 1.0f;            // Imagery packs: 0 hides the layer.
    vgl::internal::buffer<char> uri; // Null-terminated.
    vgl_pack_state state = VGL_PACK_STATE_LOADING;
    vgl_result failure = VGL_RESULT_SUCCESS;
    vgl::internal::pack::manifest manifest;
    vgl::internal::u64_map<vgl::internal::shard_state> shards;
    vgl::internal::u64_map<vgl::internal::tile_state> tiles;
    vgl::internal::u64_map<vgl::internal::texture_state> textures; // Imagery, by tile key.
    // When its data was last in view, for attribution: the update in which selected tiles last
    // wanted its imagery, and the frame that last drew it.
    std::uint64_t imagery_wanted = 0;
    std::uint64_t imagery_drawn = 0;
    // The pack's attribution lines, each null-terminated, one after another.
    vgl::internal::buffer<char> credits;
    std::uint32_t credit_count = 0;
};

// The handle of a frame: the context, whose lists it shows.
struct vgl_frame
{
    vgl_context* context = nullptr;
};

// One per context, with fields grouped by purpose rather than packed.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct vgl_context
{
    explicit vgl_context(const vgl::internal::allocator& a) noexcept
        : memory(a), decompressor(a), outstanding(a), packs(a), pending(a), selected(a),
          prefetched(a), wanted(a), boxes(a), meshes(a), resources(a), index_lists(a), drawn(a),
          frame_destroys(a), frame_index_lists(a), frame_meshes(a), frame_textures(a),
          frame_draw(a), frame_layers(a), frame_index_lists_strided(a), frame_meshes_strided(a),
          frame_textures_strided(a), frame_layers_strided(a), frame_offered(a), frame_selected(a),
          frame_drawn(a), frame_tiles(a), frame_ready(a), frame_counts(a), frame_owner(a),
          frame_candidates(a)
    {
    }

    vgl::internal::allocator memory;
    vgl::internal::logger log;
    vgl::internal::ellipsoid shape;
    bool has_io = false;
    vgl_io_interface io{};
    bool has_tasks = false;
    vgl_task_interface tasks{};
    std::uint64_t cpu_budget = 0;
    std::uint64_t cpu_used = 0;
    vgl::internal::async_core* core = nullptr;
    vgl::internal::zstd_decompressor decompressor; // For manifests and directories.
    vgl::internal::u64_map<vgl_io_request*> outstanding;
    std::uint64_t next_request_id = 1;
    vgl::internal::vector<vgl_pack*> packs; // Highest priority first.
    vgl_pack* geoid = nullptr;
    std::uint32_t next_pack_id = 1;
    std::uint64_t next_order = 1;
    // Changes when the order of the elevation packs does, so that meshes built from the old
    // order, whose nodata samples another pack filled, are built again.
    std::uint64_t terrain_generation = 0;
    vgl::internal::vector<vgl::internal::decode_job*> pending; // Waiting for an update to decode.
    std::uint64_t frame = 1;

    bool has_view = false;
    vgl_view host_view{}; // As the host set it, for vgl_context_view_get.
    vgl::internal::view_state view;
    vgl::internal::vector<vgl::internal::selected_tile> selected; // For the view, this frame.
    // Coarser tiles whose meshes build, but which are not drawn yet: until they are ready, finer
    // tiles inside them stand in, where the last frame drew finer terrain.
    vgl::internal::vector<vgl::internal::selected_tile> prefetched;
    vgl::internal::vector<vgl::internal::load_request> wanted;
    vgl::internal::u64_map<vgl::internal::cached_box> boxes;  // By tile key.
    vgl::internal::u64_map<vgl::internal::mesh_state> meshes; // By tile key.

    // The render interface (docs/render-interface.md).
    std::uint64_t gpu_budget = 0;
    std::uint64_t gpu_used = 0; // Resources offered or resident.
    // The formats the consumer samples, most preferred first (vgl_context_texture_formats_set).
    std::array<vgl_texture_format, 8> texture_formats = {VGL_TEXTURE_FORMAT_RGBA8};
    std::uint32_t texture_format_count = 1;
    std::uint64_t next_resource_id = 1;
    vgl::internal::u64_map<vgl::internal::gpu_resource> resources; // By id.
    vgl::internal::u64_map<std::uint64_t> index_lists;             // Resource by grid size.
    vgl::internal::vector<vgl::internal::drawn_tile> drawn;        // In the last frame.
    std::uint64_t render_frame = 0;

    // Statistics (vgl_context_statistics).
    std::uint64_t reads_total = 0;
    std::uint64_t bytes_read_total = 0;
    std::uint64_t tiles_loaded_total = 0;
    double load_seconds_total = 0.0;
    double update_seconds = 0.0;
    double frame_seconds = 0.0;
    std::uint32_t last_created = 0;
    std::uint32_t last_destroyed = 0;
    std::uint64_t last_bytes_created = 0;
    bool frame_open = false;
    vgl_frame frame_handle; // The open frame's handle.

    // The open frame's lists. They keep their capacity, so frames stop allocating once they
    // have grown to the size of the view.
    vgl::internal::vector<std::uint64_t> frame_destroys;
    vgl::internal::vector<vgl_index_list_data> frame_index_lists;
    vgl::internal::vector<vgl_mesh_data> frame_meshes;
    vgl::internal::vector<vgl_texture_data> frame_textures;
    vgl::internal::vector<std::byte> frame_draw; // Draw items, frame_draw_stride bytes apart.
    std::uint32_t frame_draw_count = 0;
    std::uint32_t frame_draw_stride = 0;
    vgl::internal::vector<vgl_draw_layer> frame_layers;
    // The lists at the consumer's strides, where they differ from this header's sizes
    // (docs/render-interface.md, section 5); empty and unused where they do not.
    vgl::internal::vector<std::byte> frame_index_lists_strided;
    vgl::internal::vector<std::byte> frame_meshes_strided;
    vgl::internal::vector<std::byte> frame_textures_strided;
    vgl::internal::vector<std::byte> frame_layers_strided;
    std::uint32_t frame_index_list_stride = sizeof(vgl_index_list_data);
    std::uint32_t frame_mesh_stride = sizeof(vgl_mesh_data);
    std::uint32_t frame_texture_stride = sizeof(vgl_texture_data);
    std::uint32_t frame_layer_stride = sizeof(vgl_draw_layer);
    vgl::internal::vector<std::uint64_t> frame_offered;
    // Scratch for deciding what to draw.
    vgl::internal::u64_map<std::uint32_t> frame_selected; // Selected tile key to index.
    vgl::internal::u64_map<std::uint32_t> frame_drawn;    // Last frame's tile key to index.
    vgl::internal::vector<vgl::internal::drawn_tile> frame_tiles;
    vgl::internal::vector<std::uint8_t> frame_ready;       // Per selected tile.
    vgl::internal::vector<std::uint32_t> frame_counts;     // Per last-frame tile, 3 counters.
    vgl::internal::vector<std::uint32_t> frame_owner;      // Per selected tile.
    vgl::internal::vector<std::uint64_t> frame_candidates; // Resources to upload or evict.
};

#endif // VGL_CONTEXT_CONTEXT_IMPL_HPP
