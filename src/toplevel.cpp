#include "toplevel.hpp"

#include "utils.hpp"

extern "C" {
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
}

#include "idle.hpp"
#include "input.hpp"
#include "layout.hpp"
#include "output.hpp"
#include "popup.hpp"
#include "server.hpp"

#include <algorithm>

XdgToplevel::XdgToplevel(Server& server, wlr_xdg_toplevel* xdg_toplevel)
    : View(server), xdg_toplevel(xdg_toplevel) {
    xdg_toplevel->base->data = this;

    int border_px = server.lua_cfg.settings.border_px;

    scene_tree            = wlr_scene_tree_create(server.window_tree);
    scene_tree->node.data = this;

    border_tree = wlr_scene_tree_create(scene_tree);
    float color[4];
    utils::colorToRgba(server.lua_cfg.settings.border_color_inactive, color);
    for(auto& rect : border_rects) {
        rect = wlr_scene_rect_create(border_tree, 0, 0, color);
    }

    content_tree = wlr_scene_xdg_surface_create(scene_tree, xdg_toplevel->base);
    wlr_scene_node_set_position(&content_tree->node, border_px, border_px);

    map_listener.connect(&xdg_toplevel->base->surface->events.map,
                         [this](void*) { handleMap(); });
    unmap.connect(&xdg_toplevel->base->surface->events.unmap,
                  [this](void*) { handleUnmap(); });
    destroy.connect(&xdg_toplevel->events.destroy,
                    [this](void*) { handleDestroy(); });
    commit.connect(&xdg_toplevel->base->surface->events.commit,
                   [this](wlr_surface* surface) { handleCommit(surface); });

    request_move.connect(&xdg_toplevel->events.request_move,
                         [this](void*) { handleRequestMove(); });
    request_resize.connect(&xdg_toplevel->events.request_resize,
                           [this](wlr_xdg_toplevel_resize_event* event) {
                               handleRequestResize(event);
                           });
    new_popup.connect(&xdg_toplevel->base->events.new_popup,
                      [this](wlr_xdg_popup* popup) { handleNewPopup(popup); });
    request_maximize.connect(&xdg_toplevel->events.request_maximize,
                             [this](void*) { handleRequestMaximize(); });
    request_fullscreen.connect(&xdg_toplevel->events.request_fullscreen,
                               [this](void*) { handleRequestFullscreen(); });
    set_title.connect(&xdg_toplevel->events.set_title,
                      [this](void*) { handleSetTitle(); });
    set_app_id.connect(&xdg_toplevel->events.set_app_id,
                       [this](void*) { handleSetAppId(); });
}

// View::~View() tears down scene_tree (and everything under it); nothing
// XDG-specific to clean up here.
XdgToplevel::~XdgToplevel() = default;

void XdgToplevel::handleMap() {
    mapped = true;

    output = server.getFocusedOrDefaultOutput();
    if(output) { tags = output->tagset; }

    is_floating = false;
    if(xdg_toplevel->parent) {
        // Dialogs / transient children default to floating, centered over
        // their parent, the way they almost always expect to be placed.
        is_floating = true;
    }

    if(xdg_toplevel->title) { title = xdg_toplevel->title; }
    if(xdg_toplevel->app_id) { app_id = xdg_toplevel->app_id; }

    if(output) { layout::arrange(*output); }

    if(is_floating && output) {
        int     border_px = server.lua_cfg.settings.border_px;
        wlr_box geo_box   = xdg_toplevel->base->geometry;
        int     w         = geo_box.width > 0 ? geo_box.width : 480;
        int     h         = geo_box.height > 0 ? geo_box.height : 360;

        wlr_box box;
        box.width  = w + 2 * border_px;
        box.height = h + 2 * border_px;
        box.x =
            output->layout_box.x + (output->layout_box.width - box.width) / 2;
        box.y =
            output->layout_box.y + (output->layout_box.height - box.height) / 2;
        setGeometry(box);
    }

    createForeignToplevel();
    server.focusView(this);
    playOpenAnimation();

    // A newly-mapped surface might be the one an existing
    // zwp_idle_inhibitor_v1 was created against before it ever mapped
    // (e.g. a game requesting the inhibitor from its splash screen).
    idle::updateInhibitState(server);
}

void XdgToplevel::handleUnmap() {
    mapped = false;
    destroyForeignToplevel();
    idle::updateInhibitState(server);

    if(server.cursor_mode != CursorMode::Passthrough &&
       server.grabbed_view == this) {
        input::resetCursorMode(server);
    }

    if(server.focused_view == this) {
        server.focused_view = nullptr;
        // Hand focus to the next mapped view sharing a tag with the
        // output's current view, if any.
        View* next = nullptr;
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

void XdgToplevel::handleNewPopup(wlr_xdg_popup* popup) {
    // content_tree, not scene_tree -- content_tree is exactly the
    // coordinate origin the client's popup geometry is expressed
    // relative to, and it already carries the border-width offset, so
    // popups line up correctly regardless of border thickness.
    auto p =
        std::make_unique<Popup>(server, popup, content_tree, this, nullptr);
    popups.push_back(std::move(p));
}

void XdgToplevel::handleDestroy() {
    server.views.remove_if(
        [this](const std::unique_ptr<View>& v) { return v.get() == this; });
}

void XdgToplevel::handleCommit(wlr_surface* /*surface*/) {
    if(xdg_toplevel->base->initial_commit) {
        Output* target = server.getFocusedOrDefaultOutput();

        // Dialogs/transients (anything with a parent) end up floating at
        // a client-chosen or centered size anyway (see handleMap), so the
        // tiling preview doesn't apply to them -- only to windows that
        // will actually be tiled.
        if(target && !xdg_toplevel->parent) {
            int     border_px = server.lua_cfg.settings.border_px;
            wlr_box preview   = layout::previewTileBox(*target);
            if(preview.width > 0 && preview.height > 0) {
                wlr_xdg_toplevel_set_size(
                    xdg_toplevel,
                    std::max(0, preview.width - 2 * border_px),
                    std::max(0, preview.height - 2 * border_px));
                return;
            }
        }

        wlr_xdg_toplevel_set_size(xdg_toplevel, 0, 0);
    }
}

void XdgToplevel::configureBackend(const wlr_box& box) {
    int b = server.lua_cfg.settings.border_px;
    wlr_xdg_toplevel_set_size(xdg_toplevel,
                              std::max(0, box.width - 2 * b),
                              std::max(0, box.height - 2 * b));
}

void XdgToplevel::activateBackend(bool activated) {
    wlr_xdg_toplevel_set_activated(xdg_toplevel, activated);
}

void XdgToplevel::closeBackend() { wlr_xdg_toplevel_send_close(xdg_toplevel); }

void XdgToplevel::setFullscreenBackend(bool fullscreen) {
    wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, fullscreen);
}

void XdgToplevel::handleRequestMove() { beginInteractiveMove(); }

void XdgToplevel::handleRequestResize(wlr_xdg_toplevel_resize_event* event) {
    beginInteractiveResize(event->edges);
}

void XdgToplevel::handleRequestMaximize() {
    // We don't implement a distinct maximize state; tiling already gives
    // every window a maximized-equivalent size when it's the lone window
    // on a tag. Just ack so well-behaved clients don't spin waiting.
    wlr_xdg_surface_schedule_configure(xdg_toplevel->base);
}

void XdgToplevel::handleRequestFullscreen() {
    setFullscreen(xdg_toplevel->requested.fullscreen);
}

void XdgToplevel::handleSetTitle() {
    title = xdg_toplevel->title ? xdg_toplevel->title : "";
    syncForeignToplevelMeta();
}

void XdgToplevel::handleSetAppId() {
    app_id = xdg_toplevel->app_id ? xdg_toplevel->app_id : "";
    syncForeignToplevelMeta();
}
