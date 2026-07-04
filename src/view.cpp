#include "view.hpp"
#include "popup.hpp"

extern "C" {
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
}

#include "layout.hpp"
#include "output.hpp"
#include "server.hpp"

#include <algorithm>
#include <cmath>

namespace {

void colorToRgba(uint32_t packed, float out[4]) {
    out[0] = ((packed >> 24) & 0xff) / 255.0f;
    out[1] = ((packed >> 16) & 0xff) / 255.0f;
    out[2] = ((packed >> 8) & 0xff) / 255.0f;
    out[3] = (packed & 0xff) / 255.0f;
}

}  // namespace

View::View(Server& server) : server(server) {}

View::~View() {
    if(server.focused_view == this) { server.focused_view = nullptr; }
    if(server.grabbed_view == this) {
        server.grabbed_view = nullptr;
        server.cursor_mode  = CursorMode::Passthrough;
    }
    // Recursive: tears down border_tree/border_rects and whatever of
    // content_tree is still alive (for XdgToplevel, the client's own
    // xdg-surface teardown may have already freed content_tree itself --
    // wlr_scene_node_destroy on an already-destroyed subtree is a no-op
    // for that branch, same as the old Toplevel destructor relied on).
    if(scene_tree) { wlr_scene_node_destroy(&scene_tree->node); }
}

void View::applyBoxToScene(const wlr_box& box) {
    int b = server.lua_cfg.settings.border_px;
    wlr_scene_node_set_position(&scene_tree->node, box.x, box.y);
    wlr_scene_node_set_position(&content_tree->node, b, b);

    int w = box.width, h = box.height;
    wlr_scene_rect_set_size(border_rects[0], w, b);
    wlr_scene_node_set_position(&border_rects[0]->node, 0, 0);
    wlr_scene_rect_set_size(border_rects[1], w, b);
    wlr_scene_node_set_position(&border_rects[1]->node, 0, h - b);
    wlr_scene_rect_set_size(border_rects[2], b, h);
    wlr_scene_node_set_position(&border_rects[2]->node, 0, 0);
    wlr_scene_rect_set_size(border_rects[3], b, h);
    wlr_scene_node_set_position(&border_rects[3]->node, w - b, 0);
}

void View::setGeometry(const wlr_box& box) {
    geo = box;
    configureBackend(box);
    applyBoxToScene(box);
}

void View::setTags(uint32_t new_tags) {
    tags = new_tags == 0 ? tags : new_tags;
    wlr_scene_node_set_enabled(&scene_tree->node, output && (tags & output->tagset));
    if(output) { layout::arrange(*output); }
}

void View::setFullscreen(bool fullscreen) {
    if(is_fullscreen == fullscreen) { return; }
    is_fullscreen = fullscreen;
    setFullscreenBackend(fullscreen);

    if(!output) { return; }

    if(fullscreen) {
        floating_geo = geo;
        setGeometry(output->layout_box);
        wlr_scene_node_set_enabled(&border_tree->node, false);
        wlr_scene_node_raise_to_top(&scene_tree->node);
    } else {
        wlr_scene_node_set_enabled(&border_tree->node, true);
        if(is_floating) {
            setGeometry(floating_geo);
        } else {
            layout::arrange(*output);
        }
    }

    output->fullscreen_active = fullscreen && (server.focused_view == this);
    output->updateAdaptiveSync();
}

void View::updateBorderColor(bool focused, float alpha) {
    float color[4];
    colorToRgba(focused ? server.lua_cfg.settings.border_color_active
                        : server.lua_cfg.settings.border_color_inactive,
                color);
    color[3] *= alpha;
    for(auto* rect : border_rects) { wlr_scene_rect_set_color(rect, color); }
}

void View::setFocused(bool focused) {
    updateBorderColor(focused);
    activateBackend(focused);
}

void View::close() {
    // Unmanaged (X11 override-redirect) windows aren't real tiled/focused
    // views and don't get the animated treatment -- just ask them to go.
    if(unmanaged || !server.lua_cfg.settings.anim_enabled) {
        closeBackend();
        return;
    }
    playCloseAnimation();
}

void View::beginInteractiveMove() {
    if(!is_floating || is_fullscreen) { return; }
    server.grabbed_view = this;
    server.cursor_mode  = CursorMode::Move;
    server.grab_x       = server.cursor->x - geo.x;
    server.grab_y       = server.cursor->y - geo.y;
}

void View::beginInteractiveResize(uint32_t edges) {
    if(!is_floating || is_fullscreen) { return; }
    server.grabbed_view = this;
    server.cursor_mode  = CursorMode::Resize;
    server.resize_edges = edges;
    server.grab_x       = server.cursor->x;
    server.grab_y       = server.cursor->y;
    server.grab_geobox  = geo;
}

void View::setOpacity(float alpha) {
    wlr_scene_node_for_each_buffer(
        &content_tree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
            wlr_scene_buffer_set_opacity(buffer, *static_cast<float*>(data));
        },
        &alpha);
    updateBorderColor(server.focused_view == this, alpha);
}

void View::startAnim(const wlr_box& from,
                     const wlr_box& to,
                     float          op_from,
                     float          op_to,
                     AnimEnd        end) {
    anim.emplace();
    anim->from        = from;
    anim->to          = to;
    anim->opacity_from = op_from;
    anim->opacity_to   = op_to;
    anim->duration_ms  = server.lua_cfg.settings.anim_duration_ms;
    anim->on_finish    = end;
    clock_gettime(CLOCK_MONOTONIC, &anim->start);

    wlr_scene_node_set_enabled(&scene_tree->node, true);
    applyBoxToScene(from);
    setOpacity(op_from);
}

void View::playOpenAnimation() {
    if(!server.lua_cfg.settings.anim_enabled) { return; }
    wlr_box from = geo;
    from.y += server.lua_cfg.settings.anim_slide_px;
    startAnim(from, geo, 0.0f, 1.0f, AnimEnd::None);
}

void View::playCloseAnimation() {
    wlr_box to = geo;
    to.y += server.lua_cfg.settings.anim_slide_px;
    startAnim(geo, to, 1.0f, 0.0f, AnimEnd::RequestClose);
}

void View::playSlideOut(int dx, int dy) {
    if(!server.lua_cfg.settings.anim_enabled) {
        wlr_scene_node_set_enabled(&scene_tree->node, false);
        return;
    }
    wlr_box to = geo;
    to.x += dx;
    to.y += dy;
    startAnim(geo, to, 1.0f, 1.0f, AnimEnd::Disable);
}

void View::playSlideIn(int dx, int dy) {
    if(!server.lua_cfg.settings.anim_enabled) { return; }
    wlr_box from = geo;
    from.x += dx;
    from.y += dy;
    startAnim(from, geo, 1.0f, 1.0f, AnimEnd::None);
}

void View::tickAnimation(const timespec& now) {
    if(!anim) { return; }

    double elapsed_ms = (now.tv_sec - anim->start.tv_sec) * 1000.0 +
                        (now.tv_nsec - anim->start.tv_nsec) / 1e6;
    double t = std::clamp(elapsed_ms / std::max(1, anim->duration_ms), 0.0, 1.0);
    double e = 1.0 - std::pow(1.0 - t, 3.0);  // ease-out cubic

    wlr_box cur;
    cur.x      = anim->from.x + static_cast<int>((anim->to.x - anim->from.x) * e);
    cur.y      = anim->from.y + static_cast<int>((anim->to.y - anim->from.y) * e);
    cur.width  = anim->from.width +
                static_cast<int>((anim->to.width - anim->from.width) * e);
    cur.height = anim->from.height +
                static_cast<int>((anim->to.height - anim->from.height) * e);
    applyBoxToScene(cur);
    setOpacity(static_cast<float>(
        anim->opacity_from + (anim->opacity_to - anim->opacity_from) * e));

    if(t >= 1.0) {
        AnimEnd end           = anim->on_finish;
        float   final_opacity = anim->opacity_to;
        anim.reset();

        applyBoxToScene(geo);
        if(end == AnimEnd::Disable) {
            // Restore full opacity for next time this view becomes visible
            // again -- otherwise it'd silently stay faded from this slide.
            setOpacity(1.0f);
            wlr_scene_node_set_enabled(&scene_tree->node, false);
        } else {
            setOpacity(final_opacity);
            if(end == AnimEnd::RequestClose) { closeBackend(); }
        }
    }
}
