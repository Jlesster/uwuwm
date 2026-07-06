#include "view.hpp"

#include "popup.hpp"

extern "C" {
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
}

#include "idle.hpp"
#include "input.hpp"
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

View::View(Server& server) : server(server), id(server.next_view_id++) {}

View::~View() {
    // Safety net for any destruction path that skips handleUnmap (a
    // client that disconnects uncleanly) -- handleUnmap already calls
    // this in the normal case, and it's a no-op when foreign_toplevel is
    // already null, so there's no double-destroy risk either way.
    destroyForeignToplevel();

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
    // Subtracting content_offset_x/y shifts the client's surface tree so
    // its *visible* window geometry -- not its raw buffer origin -- lands
    // at (b, b), i.e. flush against the inside of the border. See the
    // content_offset_x/y comment in view.hpp for why this offset exists.
    wlr_scene_node_set_position(
        &content_tree->node, b - content_offset_x, b - content_offset_y);

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

wlr_box View::centeredFloatBox(int content_w, int content_h) const {
    wlr_box box{};
    if(!output) { return box; }

    int border_px = server.lua_cfg.settings.border_px;
    int w         = content_w > 0 ? content_w : 480;
    int h         = content_h > 0 ? content_h : 360;

    box.width  = w + 2 * border_px;
    box.height = h + 2 * border_px;
    box.x = output->layout_box.x + (output->layout_box.width - box.width) / 2;
    box.y = output->layout_box.y + (output->layout_box.height - box.height) / 2;
    return box;
}

void View::setFloating(bool floating) {
    if(is_fullscreen || floating == is_floating) { return; }
    is_floating = floating;

    if(!floating) {
        if(output) { layout::arrange(*output); }
        return;
    }

    if(!output) { return; }

    // Use the view's current (pre-float, tiled) content size as the
    // basis for the centered box -- same size, just centered and
    // detached from the tiling grid, rather than snapping to the
    // 480x360 default every time something already has a real size.
    int border_px = server.lua_cfg.settings.border_px;
    int content_w = geo.width > 2 * border_px ? geo.width - 2 * border_px : 0;
    int content_h = geo.height > 2 * border_px ? geo.height - 2 * border_px : 0;

    wlr_box box  = centeredFloatBox(content_w, content_h);
    floating_geo = box;
    setGeometry(box);
}

void View::setTags(uint32_t new_tags) {
    tags = new_tags == 0 ? tags : new_tags;
    wlr_scene_node_set_enabled(&scene_tree->node,
                               output && (tags & output->tagset));
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

    // Keep any taskbar/dock watching via foreign-toplevel-management in
    // sync regardless of who triggered this -- the xdg/xwayland client
    // itself (request_fullscreen), or a foreign-toplevel client's own
    // request_fullscreen (View::createForeignToplevel's handler, which
    // calls back into this same setFullscreen).
    if(foreign_toplevel) {
        wlr_foreign_toplevel_handle_v1_set_fullscreen(foreign_toplevel,
                                                      fullscreen);
    }

    // Single fire site covers every entry point: the client's own
    // request_fullscreen, a foreign-toplevel client's request, the
    // mod+f keybind's toggle, `client.fullscreen = ...` from rc.lua,
    // and the fullscreen branch of `uwu.rule`. The early-return at the
    // top of this function guarantees we only fire on actual state
    // transitions, not no-op re-asserts.
    server.lua_cfg.fireClientEvent("client::fullscreen", this);
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
    if(foreign_toplevel) {
        wlr_foreign_toplevel_handle_v1_set_activated(foreign_toplevel, focused);
    }
}

void View::setMinimized(bool minimized) {
    if(is_minimized == minimized) { return; }
    is_minimized = minimized;

    if(foreign_toplevel) {
        wlr_foreign_toplevel_handle_v1_set_minimized(foreign_toplevel,
                                                     minimized);
    }

    wlr_scene_node_set_enabled(&scene_tree->node,
                               !minimized && output && (tags & output->tagset));

    if(minimized) {
        // Same grab-release guard handleUnmap uses -- a client asking to
        // minimize mid-drag shouldn't leave the compositor's move/resize
        // state pointing at a now-hidden view.
        if(server.cursor_mode != CursorMode::Passthrough &&
           server.grabbed_view == this) {
            input::resetCursorMode(server);
        }
        if(server.focused_view == this) {
            server.focused_view = nullptr;
            View* next          = nullptr;
            if(output) {
                for(auto& v : server.views) {
                    if(v.get() != this && v->mapped && !v->is_minimized &&
                       v->output == output && (v->tags & output->tagset)) {
                        next = v.get();
                        break;
                    }
                }
            }
            server.focusView(next);
        }
    } else {
        server.focusView(this);
    }

    if(output) { layout::arrange(*output); }
    idle::updateInhibitState(server);
}

void View::createForeignToplevel() {
    if(foreign_toplevel || !server.foreign_toplevel_manager) { return; }

    foreign_toplevel =
        wlr_foreign_toplevel_handle_v1_create(server.foreign_toplevel_manager);
    wlr_foreign_toplevel_handle_v1_set_title(foreign_toplevel, title.c_str());
    wlr_foreign_toplevel_handle_v1_set_app_id(foreign_toplevel, app_id.c_str());
    wlr_foreign_toplevel_handle_v1_set_fullscreen(foreign_toplevel,
                                                  is_fullscreen);
    wlr_foreign_toplevel_handle_v1_set_minimized(foreign_toplevel,
                                                 is_minimized);
    wlr_foreign_toplevel_handle_v1_set_activated(foreign_toplevel,
                                                 server.focused_view == this);
    if(output) {
        wlr_foreign_toplevel_handle_v1_output_enter(foreign_toplevel,
                                                    output->wlr_output);
    }

    ft_request_maximize.connect(
        &foreign_toplevel->events.request_maximize,
        [this](wlr_foreign_toplevel_handle_v1_maximized_event* event) {
            // Same position as XdgToplevel::handleRequestMaximize: no
            // distinct maximize state exists here -- tiling already
            // gives a lone window on a tag a maximized-equivalent size
            // -- so just echo the requested state back instead of
            // leaving a taskbar's maximize toggle looking stuck.
            wlr_foreign_toplevel_handle_v1_set_maximized(foreign_toplevel,
                                                         event->maximized);
        });
    ft_request_minimize.connect(
        &foreign_toplevel->events.request_minimize,
        [this](wlr_foreign_toplevel_handle_v1_minimized_event* event) {
            setMinimized(event->minimized);
        });
    ft_request_activate.connect(
        &foreign_toplevel->events.request_activate,
        [this](wlr_foreign_toplevel_handle_v1_activated_event*) {
            // Ignore ->seat -- this compositor only ever has one seat
            // ("seat0", see Server::setup), so there's nothing to
            // disambiguate.
            if(is_minimized) { setMinimized(false); }
            server.focusView(this);
        });
    ft_request_fullscreen.connect(
        &foreign_toplevel->events.request_fullscreen,
        [this](wlr_foreign_toplevel_handle_v1_fullscreen_event* event) {
            // Ignore ->output -- we don't support requesting fullscreen
            // on a specific *other* output via this path, only toggling
            // it on the view's current one, same as request_fullscreen
            // from the client itself.
            setFullscreen(event->fullscreen);
        });
    ft_request_close.connect(&foreign_toplevel->events.request_close,
                             [this](void*) { close(); });
    // events.destroy fires before wlroots frees the handle, whether that
    // free was triggered by us (destroyForeignToplevel calling
    // wlr_foreign_toplevel_handle_v1_destroy) or, in principle, by
    // something else -- listen unconditionally rather than assume only
    // our own call can ever trigger it. Disconnecting each listener
    // during its own signal's emission is the standard, supported
    // wl_signal_emit_mutable pattern for exactly this "clean up as I'm
    // torn down" case.
    ft_destroy.connect(&foreign_toplevel->events.destroy, [this](void*) {
        foreign_toplevel = nullptr;
        ft_request_maximize.disconnect();
        ft_request_minimize.disconnect();
        ft_request_activate.disconnect();
        ft_request_fullscreen.disconnect();
        ft_request_close.disconnect();
        ft_destroy.disconnect();
    });
}

void View::destroyForeignToplevel() {
    if(!foreign_toplevel) { return; }
    // Triggers the events.destroy handler above, which nulls
    // foreign_toplevel out and disconnects everything -- nothing left to
    // do here afterwards.
    wlr_foreign_toplevel_handle_v1_destroy(foreign_toplevel);
}

void View::syncForeignToplevelMeta() {
    if(!foreign_toplevel) { return; }
    wlr_foreign_toplevel_handle_v1_set_title(foreign_toplevel, title.c_str());
    wlr_foreign_toplevel_handle_v1_set_app_id(foreign_toplevel, app_id.c_str());
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
    anim->from         = from;
    anim->to           = to;
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
    double t =
        std::clamp(elapsed_ms / std::max(1, anim->duration_ms), 0.0, 1.0);
    double e = 1.0 - std::pow(1.0 - t, 3.0);  // ease-out cubic

    wlr_box cur;
    cur.x = anim->from.x + static_cast<int>((anim->to.x - anim->from.x) * e);
    cur.y = anim->from.y + static_cast<int>((anim->to.y - anim->from.y) * e);
    cur.width  = anim->from.width +
                 static_cast<int>((anim->to.width - anim->from.width) * e);
    cur.height = anim->from.height +
                 static_cast<int>((anim->to.height - anim->from.height) * e);
    applyBoxToScene(cur);
    setOpacity(static_cast<float>(anim->opacity_from +
                                  (anim->opacity_to - anim->opacity_from) * e));

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
