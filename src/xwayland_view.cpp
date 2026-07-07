#include "xwayland_view.hpp"

extern "C" {
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xcb/xcb_icccm.h>
}

#include "idle.hpp"
#include "input.hpp"
#include "layout.hpp"
#include "output.hpp"
#include "server.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

// Lazily interned, process-lifetime cache of one _NET_WM_WINDOW_TYPE_*
// atom name -> id. X atoms are a stable name<->id mapping for the
// lifetime of the X server (which for Xwayland is uwuwm's own lifetime),
// so one intern per distinct name, cached in the `static` below, is
// correct for every XWaylandView for the rest of the session.
// only_if_exists=1: if wlr's own xwm never interned this name (because no
// client has ever presented it), there's nothing for us to match against
// either, so failing the lookup rather than creating a fresh unused atom
// is the right behavior here.
xcb_atom_t internAtomCached(xcb_connection_t* conn, const char* name) {
    xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(conn, 1, static_cast<uint16_t>(strlen(name)), name);
    xcb_intern_atom_reply_t* reply =
        xcb_intern_atom_reply(conn, cookie, nullptr);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

// True if xsurface's _NET_WM_WINDOW_TYPE property names any of the
// "should float, not tile" types -- DIALOG (file pickers, color
// choosers, confirm prompts), UTILITY (tool palettes), and SPLASH (splash
// screens) cover the vast majority of real-world cases. MENU/TOOLTIP/DND
// are deliberately excluded: those arrive as override_redirect and are
// already caught by the `unmanaged` branch in handleMap before this is
// ever consulted.
bool isDialogWindowType(Server& server, wlr_xwayland_surface* xsurface) {
    if(xsurface->window_type_len == 0 || !server.xwayland) { return false; }
    xcb_connection_t* conn = wlr_xwayland_get_xwm_connection(server.xwayland);
    if(!conn) { return false; }

    static xcb_atom_t dialog =
        internAtomCached(conn, "_NET_WM_WINDOW_TYPE_DIALOG");
    static xcb_atom_t utility =
        internAtomCached(conn, "_NET_WM_WINDOW_TYPE_UTILITY");
    static xcb_atom_t splash =
        internAtomCached(conn, "_NET_WM_WINDOW_TYPE_SPLASH");

    for(size_t i = 0; i < xsurface->window_type_len; i++) {
        xcb_atom_t t = xsurface->window_type[i];
        if(t == dialog || t == utility || t == splash) { return true; }
    }
    return false;
}

// True if xsurface has announced (via WM_NORMAL_HINTS) that its min size
// equals its max size -- ICCCM's way of saying "I never want to be
// resized," the same signal a fixed-size dialog gives on the XDG side
// via xdg_toplevel's min/max state (see XdgToplevel::handleMap).
// xsurface->size_hints is a plain xcb_size_hints_t* (wlroots doesn't
// wrap it in a type of its own), so unlike xdg_toplevel's state, min/
// max here are only meaningful if the client actually set the
// corresponding ICCCM flag bits -- an all-zero, never-touched
// xcb_size_hints_t would otherwise misread as "0x0 fixed size."
bool hasFixedSize(wlr_xwayland_surface* xsurface) {
    xcb_size_hints_t* sh = xsurface->size_hints;
    if(!sh) { return false; }
    if(!(sh->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) ||
       !(sh->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)) {
        return false;
    }
    return sh->min_width > 0 && sh->min_height > 0 &&
           sh->min_width == sh->max_width && sh->min_height == sh->max_height;
}

}  // namespace

// GTK CSD clients (including Firefox and Zen) on X11 set _GTK_FRAME_EXTENTS
// -- four CARDINALs (left, right, top, bottom) -- to tell the window
// manager how much of their actual X window is an invisible-ish drop
// shadow around the *real* visible chrome, rather than content that
// should be treated as the window's real bounds. This is the X11
// equivalent of xdg_surface's window-geometry that XdgToplevel::handleCommit
// already corrects for via content_offset_x/y; X11 has no built-in
// protocol concept for it; it's purely an ad-hoc GTK convention that a
// compositor has to opt into reading, and until now uwuwm never did.
// Without this, both the position sent to the client (border ends up
// hugging the shadow's outer edge, not the real chrome) and the clip
// (which cut at the whole window's bounds, i.e. shadow included) treat
// the shadow band as if it were real content -- exactly what looks like
// "the shadow is included in the bounding box."
static bool fetchGtkFrameExtents(Server&               server,
                                 wlr_xwayland_surface* xsurface,
                                 int&                  left,
                                 int&                  right,
                                 int&                  top,
                                 int&                  bottom) {
    if(!server.xwayland) { return false; }
    xcb_connection_t* conn = wlr_xwayland_get_xwm_connection(server.xwayland);
    if(!conn) { return false; }

    static xcb_atom_t frame_extents_atom =
        internAtomCached(conn, "_GTK_FRAME_EXTENTS");
    if(frame_extents_atom == XCB_ATOM_NONE) { return false; }

    xcb_get_property_cookie_t cookie = xcb_get_property(conn,
                                                        0,
                                                        xsurface->window_id,
                                                        frame_extents_atom,
                                                        XCB_ATOM_CARDINAL,
                                                        0,
                                                        4);
    xcb_get_property_reply_t* reply =
        xcb_get_property_reply(conn, cookie, nullptr);
    if(!reply) { return false; }

    bool ok = reply->type == XCB_ATOM_CARDINAL && reply->format == 32 &&
              xcb_get_property_value_length(reply) >= 16;
    if(ok) {
        auto* vals = static_cast<uint32_t*>(xcb_get_property_value(reply));
        left       = static_cast<int>(vals[0]);
        right      = static_cast<int>(vals[1]);
        top        = static_cast<int>(vals[2]);
        bottom     = static_cast<int>(vals[3]);
    }
    free(reply);
    return ok;
}

XWaylandView::XWaylandView(Server& server, wlr_xwayland_surface* xsurface)
    : View(server), xsurface(xsurface) {
    xsurface->data = this;
    unmanaged      = xsurface->override_redirect;

    int border_px = server.lua_cfg.settings.border_px;

    scene_tree            = wlr_scene_tree_create(server.window_tree);
    scene_tree->node.data = this;

    border_tree    = wlr_scene_tree_create(scene_tree);
    float color[4] = {
        ((server.lua_cfg.settings.border_color_inactive >> 24) & 0xff) / 255.0f,
        ((server.lua_cfg.settings.border_color_inactive >> 16) & 0xff) / 255.0f,
        ((server.lua_cfg.settings.border_color_inactive >> 8) & 0xff) / 255.0f,
        (server.lua_cfg.settings.border_color_inactive & 0xff) / 255.0f,
    };
    for(auto& rect : border_rects) {
        rect = wlr_scene_rect_create(border_tree, 0, 0, color);
    }

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
    destroy.connect(&xsurface->events.destroy,
                    [this](void*) { handleDestroy(); });
    request_configure.connect(
        &xsurface->events.request_configure,
        [this](wlr_xwayland_surface_configure_event* event) {
            handleRequestConfigure(event);
        });
    request_move.connect(&xsurface->events.request_move,
                         [this](void*) { handleRequestMove(); });
    request_resize.connect(&xsurface->events.request_resize,
                           [this](wlr_xwayland_resize_event* event) {
                               handleRequestResize(event);
                           });
    request_fullscreen.connect(&xsurface->events.request_fullscreen,
                               [this](void*) { handleRequestFullscreen(); });
    set_title.connect(&xsurface->events.set_title,
                      [this](void*) { handleSetTitle(); });
    set_class.connect(&xsurface->events.set_class,
                      [this](void*) { handleSetClass(); });
}

// View::~View() tears down scene_tree; nothing X11-specific left to do.
XWaylandView::~XWaylandView() = default;

void XWaylandView::handleAssociate() {
    // Only from this point does xsurface->surface exist -- hook the
    // surface-level map/unmap and put its content into our tree.
    map_listener.connect(&xsurface->surface->events.map,
                         [this](void*) { handleMap(); });
    unmap.connect(&xsurface->surface->events.unmap,
                  [this](void*) { handleUnmap(); });
    scene_surface =
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

    if(unmanaged) {
        // No tiling, no border, no focus-follows -- position exactly
        // where X11 put it and let it render above everything else, same
        // as every other compositor treats override-redirect windows.
        wlr_scene_node_set_enabled(&border_tree->node, false);
        geo = wlr_box{
            xsurface->x, xsurface->y, xsurface->width, xsurface->height};
        wlr_scene_node_set_position(&scene_tree->node, geo.x, geo.y);
        wlr_scene_node_raise_to_top(&scene_tree->node);
        wlr_scene_node_set_enabled(&content_tree->node, true);
        return;
    }

    output =
        server.focused_output
            ? server.focused_output
            : (server.outputs.empty() ? nullptr : server.outputs.front().get());
    if(output) { tags = output->tagset; }

    // Same rule XdgToplevel uses for dialogs/transients (parent implies
    // floating), extended with the two signals a plain app_id/title rule
    // can't express: a fixed min==max size (hasFixedSize) and an
    // explicit _NET_WM_WINDOW_TYPE of DIALOG/UTILITY/SPLASH
    // (isDialogWindowType) -- see both above.
    is_floating = xsurface->parent != nullptr || hasFixedSize(xsurface) ||
                  isDialogWindowType(server, xsurface);

    if(xsurface->title) { title = xsurface->title; }
    if(xsurface->class_) { app_id = xsurface->class_; }

    // Must happen before the arrange()/setGeometry() calls below --
    // configureBackend and contentClipBox both read content_offset_x/y
    // and frame_right/frame_bottom, and we want the very first configure
    // this window ever gets to already account for its shadow, not a
    // visible correction one frame later.
    int fl = 0, fr = 0, ft = 0, fb = 0;
    if(fetchGtkFrameExtents(server, xsurface, fl, fr, ft, fb)) {
        content_offset_x = fl;
        content_offset_y = ft;
        frame_right      = fr;
        frame_bottom     = fb;
    }

    if(output) { layout::arrange(*output); }

    if(is_floating && output) {
        wlr_box box  = centeredFloatBox(xsurface->width, xsurface->height);
        floating_geo = box;
        setGeometry(box);
    }

    // Only now -- after scene_tree/content_tree have been placed at
    // their real tiled-or-floating position by arrange()/setGeometry()
    // above -- do we make content visible. Enabling it any earlier risks
    // a frame showing content_tree at its stale construction-time
    // position (0,0 relative to its parent, i.e. the output's top-left
    // corner) rather than wherever this window actually belongs, if a
    // repaint is ever triggered from within arrange()/setGeometry()
    // itself (e.g. a synchronous frame callback fired by the backend) --
    // exactly the "chrome floats at the top-left corner" symptom.
    wlr_scene_node_set_enabled(&content_tree->node, true);

    createForeignToplevel();
    server.focusView(this);
    playOpenAnimation();

    idle::updateInhibitState(server);

    // See XdgToplevel::handleMap for the fire-order rationale (after
    // focusView so client.focused() returns this view). The earlier
    // `if(unmanaged) return;` gate means this is reached only for
    // managed X11 windows, mirroring how uwu.client.list() excludes
    // override-redirect surfaces.
    server.lua_cfg.fireClientEvent("client::manage", this);
}

void XWaylandView::handleUnmap() {
    mapped = false;
    wlr_scene_node_set_enabled(&content_tree->node, false);

    if(unmanaged) { return; }
    destroyForeignToplevel();
    idle::updateInhibitState(server);

    if(server.cursor_mode != CursorMode::Passthrough &&
       server.grabbed_view == this) {
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
    // Pair with handleMap's !unmanaged gate: an X11 override-redirect
    // window's manage was never fired, so its unmanage mustn't be
    // either. See XdgToplevel::handleDestroy for the fire-before-remove
    // ordering rationale.
    if(!unmanaged) { server.lua_cfg.fireClientEvent("client::unmanage", this); }
    server.views.remove_if(
        [this](const std::unique_ptr<View>& v) { return v.get() == this; });
}

void XWaylandView::handleRequestConfigure(
    wlr_xwayland_surface_configure_event* event) {
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
        wlr_box box{event->x - b,
                    event->y - b,
                    event->width + 2 * b,
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
    syncForeignToplevelMeta();
}

void XWaylandView::handleSetClass() {
    app_id = xsurface->class_ ? xsurface->class_ : "";
    syncForeignToplevelMeta();
}

// X11 has no xdg_surface-style client-declared window geometry, but GTK
// CSD clients on X11 get the closest equivalent via _GTK_FRAME_EXTENTS
// (fetchGtkFrameExtents, above/handleMap) -- content_offset_x/y hold its
// left/top components (reusing the same base-View fields XdgToplevel
// populates from client_geo.x/y), so the clip's origin sits inset past
// the shadow exactly like XdgToplevel's does. Clients that never set the
// property leave content_offset_x/y at 0, which reduces this to "clip to
// box's own inner dimensions starting at (0,0)" -- still a needed guard
// on its own, since an Xwayland client's actual surface buffer can lag
// behind or overshoot the size uwuwm configured it to (Gecko-based
// clients -- Firefox, Zen -- are the routine offenders, especially right
// after mapping with a restored/stale window size before their first
// post-tile redraw catches up), which previously bled straight past the
// border with nothing to stop it.
wlr_box XWaylandView::contentClipBox(const wlr_box& box) const {
    int b = server.lua_cfg.settings.border_px;
    int w = box.width - 2 * b;
    int h = box.height - 2 * b;
    return wlr_box{
        content_offset_x, content_offset_y, w > 0 ? w : 0, h > 0 ? h : 0};
}

// XWayland has no xdg_surface-style auto-shifted inner scene tree the
// way wlr_scene_xdg_surface_create gives XdgToplevel -- content_tree is
// a plain wlr_scene_tree, and the scene_surface (a wlr_scene_buffer)
// inside it sits at (0, 0) by default. The X window's visible chrome,
// though, starts at (content_offset_x, content_offset_y) in X-window-
// local coordinates (the _GTK_FRAME_EXTENTS top/left inset); with
// content_tree now at (b, b) of scene_tree (see View::applyBoxToScene),
// the chrome would otherwise land at (b + content_offset_x, b +
// content_offset_y) -- past the border's inner edge by the CSD shadow
// size. Shifting the buffer by (-content_offset_x, -content_offset_y)
// pulls the chrome's top-left back to (0, 0) of content_tree = (b, b)
// of scene_tree, where the border wants it. No-op when the
// scene_surface hasn't been created yet (pre-associate XWayland
// surface) or when content_offset_x/y are both 0 (non-CSD X11 client --
// common, no shifting needed).
void XWaylandView::applyContentOffsetToScene(const wlr_box& /*box*/) {
    if(!scene_surface) { return; }
    wlr_scene_node_set_position(
        &scene_surface->buffer->node, -content_offset_x, -content_offset_y);
}

void XWaylandView::configureBackend(const wlr_box& box) {
    int b  = server.lua_cfg.settings.border_px;
    int vw = box.width - 2 * b;
    int vh = box.height - 2 * b;
    if(vw < 1) { vw = 1; }
    if(vh < 1) { vh = 1; }
    wlr_xwayland_surface_configure(
        xsurface,
        static_cast<int16_t>(box.x + b - content_offset_x),
        static_cast<int16_t>(box.y + b - content_offset_y),
        static_cast<uint16_t>(vw + content_offset_x + frame_right),
        static_cast<uint16_t>(vh + content_offset_y + frame_bottom));
}

void XWaylandView::activateBackend(bool activated) {
    wlr_xwayland_surface_activate(xsurface, activated);
}

void XWaylandView::closeBackend() { wlr_xwayland_surface_close(xsurface); }

void XWaylandView::setFullscreenBackend(bool fullscreen) {
    wlr_xwayland_surface_set_fullscreen(xsurface, fullscreen);
}
