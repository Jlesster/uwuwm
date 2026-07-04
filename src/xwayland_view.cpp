#include "xwayland_view.hpp"

extern "C" {
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
}

#include "input.hpp"
#include "layout.hpp"
#include "output.hpp"
#include "server.hpp"

#include <algorithm>

XWaylandView::XWaylandView(Server& server, wlr_xwayland_surface* xsurface)
    : View(server), xsurface(xsurface) {
    xsurface->data = this;
    unmanaged      = xsurface->override_redirect;

    int border_px = server.lua_cfg.settings.border_px;

    scene_tree            = wlr_scene_tree_create(server.window_tree);
    scene_tree->node.data = this;

    border_tree = wlr_scene_tree_create(scene_tree);
    float color[4] = {
        ((server.lua_cfg.settings.border_color_inactive >> 24) & 0xff) / 255.0f,
        ((server.lua_cfg.settings.border_color_inactive >> 16) & 0xff) / 255.0f,
        ((server.lua_cfg.settings.border_color_inactive >> 8) & 0xff) / 255.0f,
        (server.lua_cfg.settings.border_color_inactive & 0xff) / 255.0f,
    };
    for(auto& rect : border_rects) { rect = wlr_scene_rect_create(border_tree, 0, 0, color); }

    // Plain empty tree so `content_tree`'s field type matches XdgToplevel
    // (a wlr_scene_tree*) -- the actual surface buffer only exists once
    // handleAssociate() fires and gets nested one level inside via
    // wlr_scene_surface_create(). Disabled until then: nothing to show.
    content_tree = wlr_scene_tree_create(scene_tree);
    wlr_scene_node_set_position(&content_tree->node, border_px, border_px);
    wlr_scene_node_set_enabled(&content_tree->node, false);

    if(xsurface->title) { title = xsurface->title; }
    if(xsurface->class_) { app_id = xsurface->class_; }

    associate.connect(&xsurface->events.associate,
                      [this](void*) { handleAssociate(); });
    dissociate.connect(&xsurface->events.dissociate,
                       [this](void*) { handleDissociate(); });
    destroy.connect(&xsurface->events.destroy, [this](void*) { handleDestroy(); });
    request_configure.connect(
        &xsurface->events.request_configure,
        [this](wlr_xwayland_surface_configure_event* event) {
            handleRequestConfigure(event);
        });
    request_move.connect(&xsurface->events.request_move,
                         [this](void*) { handleRequestMove(); });
    request_resize.connect(
        &xsurface->events.request_resize,
        [this](wlr_xwayland_resize_event* event) { handleRequestResize(event); });
    request_fullscreen.connect(&xsurface->events.request_fullscreen,
                               [this](void*) { handleRequestFullscreen(); });
    set_title.connect(&xsurface->events.set_title, [this](void*) { handleSetTitle(); });
    set_class.connect(&xsurface->events.set_class, [this](void*) { handleSetClass(); });
}

// View::~View() tears down scene_tree; nothing X11-specific left to do.
XWaylandView::~XWaylandView() = default;

void XWaylandView::handleAssociate() {
    // Only from this point does xsurface->surface exist -- hook the
    // surface-level map/unmap and put its content into our tree.
    map_listener.connect(&xsurface->surface->events.map, [this](void*) { handleMap(); });
    unmap.connect(&xsurface->surface->events.unmap, [this](void*) { handleUnmap(); });
    wlr_scene_surface_create(content_tree, xsurface->surface);
}

void XWaylandView::handleDissociate() {
    map_listener.disconnect();
    unmap.disconnect();
    // The scene node wlr_scene_surface_create attached above destroys
    // itself when xsurface->surface is destroyed, same guarantee
    // wlr_scene_xdg_surface_create gives XdgToplevel's content_tree.
}

void XWaylandView::handleMap() {
    mapped = true;
    wlr_scene_node_set_enabled(&content_tree->node, true);

    if(unmanaged) {
        // No tiling, no border, no focus-follows -- position exactly
        // where X11 put it and let it render above everything else, same
        // as every other compositor treats override-redirect windows.
        wlr_scene_node_set_enabled(&border_tree->node, false);
        geo = wlr_box{xsurface->x, xsurface->y, xsurface->width, xsurface->height};
        wlr_scene_node_set_position(&scene_tree->node, geo.x, geo.y);
        wlr_scene_node_raise_to_top(&scene_tree->node);
        return;
    }

    output =
        server.focused_output
            ? server.focused_output
            : (server.outputs.empty() ? nullptr : server.outputs.front().get());
    if(output) { tags = output->tagset; }

    // Same rule XdgToplevel uses for dialogs/transients: anything with a
    // parent floats, centered.
    is_floating = xsurface->parent != nullptr;

    if(xsurface->title) { title = xsurface->title; }
    if(xsurface->class_) { app_id = xsurface->class_; }

    if(output) { layout::arrange(*output); }

    if(is_floating && output) {
        int border_px = server.lua_cfg.settings.border_px;
        int w         = xsurface->width > 0 ? xsurface->width : 480;
        int h         = xsurface->height > 0 ? xsurface->height : 360;

        wlr_box box;
        box.width  = w + 2 * border_px;
        box.height = h + 2 * border_px;
        box.x =
            output->layout_box.x + (output->layout_box.width - box.width) / 2;
        box.y =
            output->layout_box.y + (output->layout_box.height - box.height) / 2;
        setGeometry(box);
    }

    server.focusView(this);
    playOpenAnimation();
}

void XWaylandView::handleUnmap() {
    mapped = false;
    wlr_scene_node_set_enabled(&content_tree->node, false);

    if(unmanaged) { return; }

    if(server.cursor_mode != CursorMode::Passthrough && server.grabbed_view == this) {
        input::resetCursorMode(server);
    }

    if(server.focused_view == this) {
        server.focused_view = nullptr;
        View* next          = nullptr;
        if(output) {
            for(auto& v : server.views) {
                if(v.get() != this && v->mapped && v->output == output &&
                   (v->tags & output->tagset)) {
                    next = v.get();
                    break;
                }
            }
        }
        server.focusView(next);
    }

    if(output) { layout::arrange(*output); }
}

void XWaylandView::handleDestroy() {
    server.views.remove_if(
        [this](const std::unique_ptr<View>& v) { return v.get() == this; });
}

void XWaylandView::handleRequestConfigure(wlr_xwayland_surface_configure_event* event) {
    // Not yet mapped, unmanaged (override-redirect), or floating: X11
    // clients are used to placing themselves, and we let them -- same as
    // dwl/sway do. A tiled, mapped, managed window ignores this; the
    // layout owns its geometry, the same way an XDG toplevel can't
    // resize itself out of tiling.
    if(mapped && !unmanaged && !is_floating) {
        configureBackend(geo);
        return;
    }

    wlr_xwayland_surface_configure(xsurface,
                                   static_cast<int16_t>(event->x),
                                   static_cast<int16_t>(event->y),
                                   static_cast<uint16_t>(event->width),
                                   static_cast<uint16_t>(event->height));

    if(mapped && !unmanaged) {
        int     b = server.lua_cfg.settings.border_px;
        wlr_box box{event->x - b, event->y - b, event->width + 2 * b,
                   event->height + 2 * b};
        geo = box;
        applyBoxToScene(box);
    }
}

void XWaylandView::handleRequestMove() { beginInteractiveMove(); }

void XWaylandView::handleRequestResize(wlr_xwayland_resize_event* event) {
    beginInteractiveResize(event->edges);
}

void XWaylandView::handleRequestFullscreen() {
    setFullscreen(xsurface->fullscreen);
}

void XWaylandView::handleSetTitle() {
    title = xsurface->title ? xsurface->title : "";
}

void XWaylandView::handleSetClass() {
    app_id = xsurface->class_ ? xsurface->class_ : "";
}

void XWaylandView::configureBackend(const wlr_box& box) {
    int b = server.lua_cfg.settings.border_px;
    wlr_xwayland_surface_configure(
        xsurface,
        static_cast<int16_t>(box.x + b),
        static_cast<int16_t>(box.y + b),
        static_cast<uint16_t>(std::max(1, box.width - 2 * b)),
        static_cast<uint16_t>(std::max(1, box.height - 2 * b)));
}

void XWaylandView::activateBackend(bool activated) {
    wlr_xwayland_surface_activate(xsurface, activated);
}

void XWaylandView::closeBackend() { wlr_xwayland_surface_close(xsurface); }

void XWaylandView::setFullscreenBackend(bool fullscreen) {
    wlr_xwayland_surface_set_fullscreen(xsurface, fullscreen);
}
