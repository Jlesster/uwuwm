#include "wallpaper.hpp"

#include "layershell.hpp"
#include "output.hpp"
#include "server.hpp"

extern "C" {
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
}

// stb_image is a public-domain single-header decoder (PNG/JPEG/BMP/GIF/
// TGA/PSD/...), vendored verbatim rather than pulled in as a system
// dependency -- meson.build's whole philosophy is a short, deliberate
// dependency list (see its own comments), and a single ~8k-line header
// with zero transitive deps fits that better than adding gdk-pixbuf or
// similar just to decode a wallpaper image. STBI_NO_STDIO is
// deliberately *not* defined -- stbi_load(path, ...) reads the file
// itself, which is exactly what loadWallpaperImage wants.
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace {

// Keyed on the literal path string uwu.wallpaper.set() was given (already
// ~-expanded by lua_config.cpp) -- see loadWallpaperImage/
// forgetWallpaperImage. shared_ptr, not a raw cache, so a WallpaperImage
// a live wlr_buffer is still reading from can outlive its own cache entry
// being evicted (forgetWallpaperImage only erases the map's reference,
// not necessarily the last one).
std::unordered_map<std::string, std::shared_ptr<WallpaperImage>> g_decode_cache;

// ---------------------------------------------------------------------------
// A minimal custom wlr_buffer wrapping a decoded WallpaperImage's pixel
// data -- the wallpaper equivalent of a client's SHM buffer, just backed
// by our own decode instead of a Wayland client's wl_shm_pool. `base`
// must stay the first member for wl_container_of() below to be valid.
// ---------------------------------------------------------------------------
struct WallpaperImageBuffer {
    wlr_buffer                      base;
    std::shared_ptr<WallpaperImage> image;
};

void wallpaperBufferDestroy(wlr_buffer* buffer) {
    WallpaperImageBuffer* self = wl_container_of(buffer, self, base);
    delete self;
}

bool wallpaperBufferBeginDataPtrAccess(wlr_buffer* buffer,
                                       uint32_t /*flags*/,
                                       void**    data,
                                       uint32_t* format,
                                       size_t*   stride) {
    WallpaperImageBuffer* self = wl_container_of(buffer, self, base);
    *data                      = self->image->pixels.data();
    // stb_image's forced-4-channel output is packed R,G,B,A per pixel in
    // memory with no row padding -- DRM_FORMAT_ABGR8888 is the fourcc
    // whose byte order (low-to-high: R,G,B,A) matches that exactly. Easy
    // to get backwards -- ARGB/ABGR name channels MSB-to-LSB as a 32-bit
    // word, which is the *reverse* of byte memory order on a
    // little-endian host.
    *format = DRM_FORMAT_ABGR8888;
    *stride = static_cast<size_t>(self->image->width) * 4;
    return true;
}

void wallpaperBufferEndDataPtrAccess(wlr_buffer* /*buffer*/) {
    // Nothing to flush -- begin_data_ptr_access above handed out a
    // pointer straight into WallpaperImage::pixels, not a mapped copy.
}

const wlr_buffer_impl kWallpaperBufferImpl = {
    .destroy               = wallpaperBufferDestroy,
    .get_dmabuf            = nullptr,
    .get_shm               = nullptr,
    .begin_data_ptr_access = wallpaperBufferBeginDataPtrAccess,
    .end_data_ptr_access   = wallpaperBufferEndDataPtrAccess,
};

// Wraps `image` in a brand-new wlr_buffer for the scene graph. The
// returned buffer starts with the implicit single producer reference
// wlr_buffer_init leaves it with; the caller must eventually
// wlr_buffer_drop() it exactly once (after every wlr_scene_buffer_create/
// set_buffer call that should keep it alive has taken its own consumer
// lock -- see wrapImageBuffer's call sites in applyWallpaper below for
// why that ordering matters).
wlr_buffer* wrapImageBuffer(std::shared_ptr<WallpaperImage> image) {
    auto* buf  = new WallpaperImageBuffer{};
    int   w    = image->width;
    int   h    = image->height;
    buf->image = std::move(image);
    wlr_buffer_init(&buf->base, &kWallpaperBufferImpl, w, h);
    return &buf->base;
}

// ---------------------------------------------------------------------------
// Placement math -- one wlr_fbox (source crop, in image pixels) and one
// wlr_box (destination position+size, in output-layout pixels) per scene
// node a mode needs. Fill/Fit/Stretch/Center produce exactly one; Tile
// produces a full grid, computed by tilePlacements() below. Every mode's
// destination box is intersected against the output's own layout_box, so
// none of these can ever paint past this output into a neighboring one
// on a multi-monitor layout -- that's the one invariant every branch
// below is written to preserve, not just an incidental property of the
// "normal" numbers.
// ---------------------------------------------------------------------------
struct Placement {
    wlr_fbox src;
    wlr_box  dst;
};

Placement fillPlacement(const Output& out, const WallpaperImage& img) {
    double out_aspect =
        static_cast<double>(out.layout_box.width) / out.layout_box.height;
    double img_aspect = static_cast<double>(img.width) / img.height;

    double crop_w = img.width;
    double crop_h = img.height;
    if(img_aspect > out_aspect) {
        crop_w = img.height * out_aspect;
    } else {
        crop_h = img.width / out_aspect;
    }

    Placement p;
    p.src = {(img.width - crop_w) / 2.0,
             (img.height - crop_h) / 2.0,
             crop_w,
             crop_h};
    p.dst = {out.layout_box.x,
             out.layout_box.y,
             out.layout_box.width,
             out.layout_box.height};
    return p;
}

Placement fitPlacement(const Output& out, const WallpaperImage& img) {
    double scale =
        std::min(static_cast<double>(out.layout_box.width) / img.width,
                 static_cast<double>(out.layout_box.height) / img.height);
    int dst_w = static_cast<int>(img.width * scale + 0.5);
    int dst_h = static_cast<int>(img.height * scale + 0.5);

    Placement p;
    p.src = {
        0, 0, static_cast<double>(img.width), static_cast<double>(img.height)};
    p.dst = {out.layout_box.x + (out.layout_box.width - dst_w) / 2,
             out.layout_box.y + (out.layout_box.height - dst_h) / 2,
             dst_w,
             dst_h};
    return p;
}

Placement stretchPlacement(const Output& out, const WallpaperImage& img) {
    Placement p;
    p.src = {
        0, 0, static_cast<double>(img.width), static_cast<double>(img.height)};
    p.dst = {out.layout_box.x,
             out.layout_box.y,
             out.layout_box.width,
             out.layout_box.height};
    return p;
}

Placement centerPlacement(const Output& out, const WallpaperImage& img) {
    // No scaling, ever -- if the image is larger than the output in a
    // given dimension, crop (centered) rather than overflow into a
    // neighboring monitor; if it's smaller, the destination box is
    // smaller than the output and background_color shows around it,
    // same letterbox behavior Fit gives you.
    int crop_w = std::min(img.width, out.layout_box.width);
    int crop_h = std::min(img.height, out.layout_box.height);

    Placement p;
    p.src = {(img.width - crop_w) / 2.0,
             (img.height - crop_h) / 2.0,
             static_cast<double>(crop_w),
             static_cast<double>(crop_h)};
    p.dst = {out.layout_box.x + (out.layout_box.width - crop_w) / 2,
             out.layout_box.y + (out.layout_box.height - crop_h) / 2,
             crop_w,
             crop_h};
    return p;
}

// Tile mode needs a grid of nodes, not one -- returned separately from
// the single-Placement modes above rather than shoehorned into the same
// shape.
std::vector<Placement> tilePlacements(const Output&         out,
                                      const WallpaperImage& img) {
    std::vector<Placement> tiles;
    if(img.width <= 0 || img.height <= 0) { return tiles; }

    int out_x0 = out.layout_box.x;
    int out_y0 = out.layout_box.y;
    int out_x1 = out_x0 + out.layout_box.width;
    int out_y1 = out_y0 + out.layout_box.height;

    for(int y = out_y0; y < out_y1; y += img.height) {
        for(int x = out_x0; x < out_x1; x += img.width) {
            int tile_w = std::min(img.width, out_x1 - x);
            int tile_h = std::min(img.height, out_y1 - y);
            if(tile_w <= 0 || tile_h <= 0) { continue; }

            Placement p;
            // Edge tiles crop the image's own top-left corner down to
            // however much room is left, rather than scaling -- a
            // partial tile at the boundary, exactly like every other
            // desktop's "tile" wallpaper mode.
            p.src = {
                0, 0, static_cast<double>(tile_w), static_cast<double>(tile_h)};
            p.dst = {x, y, tile_w, tile_h};
            tiles.push_back(p);
        }
    }
    return tiles;
}

wlr_scene_buffer*
makeNode(Output& out, wlr_buffer* buffer, const Placement& p) {
    wlr_scene_buffer* node = wlr_scene_buffer_create(
        out.server.layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND], buffer);
    wlr_scene_buffer_set_source_box(node, &p.src);
    wlr_scene_buffer_set_dest_size(node, p.dst.width, p.dst.height);
    wlr_scene_node_set_position(&node->node, p.dst.x, p.dst.y);
    // Stacked directly above background_rect -- both are children of the
    // same background layer tree, and a node created later is stacked
    // on top of one created earlier (same z-order rule as every other
    // wlr_scene_tree), so this always paints over the solid color rather
    // than under it, without needing to explicitly reorder anything.
    return node;
}

}  // namespace

const WallpaperRule* findWallpaperRule(Server& server, const char* name) {
    const WallpaperRule* wildcard = nullptr;
    for(const auto& rule : server.lua_cfg.wallpaper_rules) {
        if(rule.name == name) { return &rule; }
        if(rule.name == "*") { wildcard = &rule; }
    }
    return wildcard;
}

std::shared_ptr<WallpaperImage> loadWallpaperImage(const std::string& path) {
    if(auto it = g_decode_cache.find(path); it != g_decode_cache.end()) {
        return it->second;
    }

    int      w = 0, h = 0, channels_in_file = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels_in_file, 4);
    if(!pixels) {
        wlr_log(WLR_ERROR,
                "wallpaper: couldn't decode '%s': %s",
                path.c_str(),
                stbi_failure_reason());
        return nullptr;
    }

    auto image    = std::make_shared<WallpaperImage>();
    image->width  = w;
    image->height = h;
    image->pixels.assign(pixels, pixels + (static_cast<size_t>(w) * h * 4));
    stbi_image_free(pixels);

    g_decode_cache.emplace(path, image);
    return image;
}

void forgetWallpaperImage(const std::string& path) {
    g_decode_cache.erase(path);
}

void clearWallpaperNodes(Output& out) {
    for(wlr_scene_buffer* node : out.wallpaper_nodes) {
        wlr_scene_node_destroy(&node->node);
    }
    out.wallpaper_nodes.clear();
}

void applyWallpaper(Output& out) {
    clearWallpaperNodes(out);

    const WallpaperRule* rule =
        findWallpaperRule(out.server, out.wlr_output->name);
    if(!rule) { return; }  // background_color alone is still correct.

    std::shared_ptr<WallpaperImage> image = loadWallpaperImage(rule->path);
    if(!image || image->width <= 0 || image->height <= 0) {
        return;  // loadWallpaperImage already logged why.
    }

    if(rule->mode == WallpaperMode::Tile) {
        std::vector<Placement> tiles = tilePlacements(out, *image);
        if(tiles.empty()) { return; }

        // One producer reference (from wrapImageBuffer's wlr_buffer_init)
        // handed off across every tile node below; each
        // wlr_scene_buffer_create() call takes its own consumer lock on
        // the same underlying wlr_buffer (that's the whole point of
        // wlr_buffer's lock/unlock contract -- see wlr_buffer.h's own
        // "single producer, multiple consumers" doc comment), so the
        // single wlr_buffer_drop() after the loop only releases *our*
        // reference, not the scene graph's.
        wlr_buffer* buffer = wrapImageBuffer(image);
        for(const Placement& p : tiles) {
            out.wallpaper_nodes.push_back(makeNode(out, buffer, p));
        }
        wlr_buffer_drop(buffer);
        return;
    }

    Placement p;
    switch(rule->mode) {
        case WallpaperMode::Fill:
            p = fillPlacement(out, *image);
            break;
        case WallpaperMode::Fit:
            p = fitPlacement(out, *image);
            break;
        case WallpaperMode::Stretch:
            p = stretchPlacement(out, *image);
            break;
        case WallpaperMode::Center:
            p = centerPlacement(out, *image);
            break;
        case WallpaperMode::Tile:
            return;  // handled above, unreachable here
    }

    wlr_buffer* buffer = wrapImageBuffer(image);
    out.wallpaper_nodes.push_back(makeNode(out, buffer, p));
    wlr_buffer_drop(buffer);
}
