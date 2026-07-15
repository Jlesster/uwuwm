#include "bar.hpp"

#include "layershell.hpp"
#include "output.hpp"
#include "server.hpp"

extern "C" {
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
}

#include <algorithm>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// A minimal custom wlr_buffer wrapping one committed frame of a Bar's
// pixels -- same technique as wallpaper.cpp's WallpaperImageBuffer, just
// holding its own owned copy rather than a shared decoded image, since a
// bar's `pixels` keeps getting mutated by the next round of draw calls
// as soon as commit() returns. `base` must stay the first member for
// wl_container_of() below to be valid.
// ---------------------------------------------------------------------------
struct BarPixelBuffer {
    wlr_buffer           base;
    std::vector<uint8_t> pixels;
    int                  width, height;
};

void barBufferDestroy(wlr_buffer* buffer) {
    BarPixelBuffer* self = wl_container_of(buffer, self, base);
    delete self;
}

bool barBufferBeginDataPtrAccess(wlr_buffer* buffer,
                                 uint32_t /*flags*/,
                                 void**    data,
                                 uint32_t* format,
                                 size_t*   stride) {
    BarPixelBuffer* self = wl_container_of(buffer, self, base);
    *data                = self->pixels.data();
    // Same R,G,B,A-in-memory-order reasoning as wallpaper.cpp's identical
    // format choice -- see that file's comment.
    *format = DRM_FORMAT_ABGR8888;
    *stride = static_cast<size_t>(self->width) * 4;
    return true;
}

void barBufferEndDataPtrAccess(wlr_buffer* /*buffer*/) {}

const wlr_buffer_impl kBarBufferImpl = {
    .destroy               = barBufferDestroy,
    .get_dmabuf            = nullptr,
    .get_shm               = nullptr,
    .begin_data_ptr_access = barBufferBeginDataPtrAccess,
    .end_data_ptr_access   = barBufferEndDataPtrAccess,
};

// Takes ownership of `pixels` (moved in). Same single-producer-reference
// contract as wallpaper.cpp's wrapImageBuffer -- caller wlr_buffer_drop()s
// exactly once, after every consumer (here, always exactly one
// wlr_scene_buffer_set_buffer call) has taken its own lock.
wlr_buffer* wrapBarPixels(std::vector<uint8_t> pixels, int width, int height) {
    auto* buf   = new BarPixelBuffer{};
    buf->pixels = std::move(pixels);
    buf->width  = width;
    buf->height = height;
    wlr_buffer_init(&buf->base, &kBarBufferImpl, width, height);
    return &buf->base;
}

inline void blendPixel(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, float a) {
    dst[0] = static_cast<uint8_t>(dst[0] * (1.0f - a) + r * a);
    dst[1] = static_cast<uint8_t>(dst[1] * (1.0f - a) + g * a);
    dst[2] = static_cast<uint8_t>(dst[2] * (1.0f - a) + b * a);
    dst[3] = 255;
}

}  // namespace

Bar::Bar(Server& server_, Output& output_, BarPosition position_, int height_)
    : server(server_), output(&output_), position(position_), height(height_) {
    reposition();
    clear(0x000000ffu);  // opaque black until rc.lua's first real draw
    commit();
}

Bar::~Bar() {
    if(scene_node) { wlr_scene_node_destroy(&scene_node->node); }
    // Output::~Output already walked its own `bars` and called
    // detachFromOutput() (which nulls `output`) on every one of them
    // before any Output is actually destroyed -- see Output's
    // destructor. Only remove ourselves from a *still-alive* output's
    // list here; if output is already null there's nothing left to
    // remove ourselves from.
    if(output) {
        auto& v = output->bars;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }
}

void Bar::detachFromOutput() {
    if(scene_node) {
        wlr_scene_node_destroy(&scene_node->node);
        scene_node = nullptr;
    }
    output = nullptr;
}

void Bar::reposition() {
    if(!output) { return; }
    width = output->layout_box.width;
    if(width <= 0 || height <= 0) { return; }

    pixels.assign(static_cast<size_t>(width) * height * 4, 0);

    int y = (position == BarPosition::Top)
                ? output->layout_box.y
                : output->layout_box.y + output->layout_box.height - height;

    if(!scene_node) {
        // TOP layer -- above every tiled/floating window (those live
        // under server.window_tree, a sibling of layer_tree[...], not a
        // descendant of it), below OVERLAY (session lock). Matches
        // where a real status bar sits by default (waybar's own default
        // layer is "top").
        std::vector<uint8_t> empty(static_cast<size_t>(width) * height * 4, 0);
        wlr_buffer* buf = wrapBarPixels(std::move(empty), width, height);
        scene_node      = wlr_scene_buffer_create(
            server.layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_TOP], buf);
        wlr_buffer_drop(buf);
        // Tags this scene node as belonging to a Bar for input.cpp's
        // barAt() hit-test -- nothing else this compositor creates
        // directly (wallpaper nodes, background_rect) ever sets a bare
        // wlr_scene_buffer node's own .data, so this is safe without a
        // separate type tag.
        scene_node->node.data = this;
    } else {
        wlr_scene_buffer_set_dest_size(scene_node, width, height);
    }
    wlr_scene_node_set_position(&scene_node->node, output->layout_box.x, y);
}

void Bar::commit() {
    if(!output || !scene_node || width <= 0 || height <= 0) { return; }
    wlr_buffer* buf = wrapBarPixels(pixels, width, height);  // copies pixels
    wlr_scene_buffer_set_buffer(scene_node, buf);
    wlr_buffer_drop(buf);
}

void Bar::clear(uint32_t color) {
    if(width <= 0 || height <= 0) { return; }
    uint8_t r = static_cast<uint8_t>((color >> 24) & 0xff);
    uint8_t g = static_cast<uint8_t>((color >> 16) & 0xff);
    uint8_t b = static_cast<uint8_t>((color >> 8) & 0xff);
    for(size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i]     = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
}

void Bar::fillRect(int x, int y, int w, int h, uint32_t color) {
    if(width <= 0 || height <= 0) { return; }
    uint8_t r  = static_cast<uint8_t>((color >> 24) & 0xff);
    uint8_t g  = static_cast<uint8_t>((color >> 16) & 0xff);
    uint8_t b  = static_cast<uint8_t>((color >> 8) & 0xff);
    uint8_t a8 = static_cast<uint8_t>(color & 0xff);
    float   a  = a8 / 255.0f;

    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(width, x + w), y1 = std::min(height, y + h);
    for(int py = y0; py < y1; py++) {
        for(int px = x0; px < x1; px++) {
            blendPixel(&pixels[static_cast<size_t>(py) * width * 4 + px * 4],
                       r,
                       g,
                       b,
                       a);
        }
    }
}

int Bar::drawText(int                x,
                  int                y,
                  const std::string& utf8,
                  const std::string& font_path,
                  int                pixel_size,
                  uint32_t           color) {
    if(width <= 0 || height <= 0) { return 0; }
    return server.text_renderer.drawText(pixels.data(),
                                         width * 4,
                                         width,
                                         height,
                                         x,
                                         y,
                                         utf8,
                                         font_path,
                                         pixel_size,
                                         color);
}

int Bar::textWidth(const std::string& utf8,
                   const std::string& font_path,
                   int                pixel_size) {
    return server.text_renderer.measureText(utf8, font_path, pixel_size);
}

int Bar::lineHeight(const std::string& font_path, int pixel_size) {
    return server.text_renderer.lineHeight(font_path, pixel_size);
}
