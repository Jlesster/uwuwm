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
//
// A real fixed-size window (color picker, confirm prompt, file chooser)
// is dialog-sized -- comfortably under a display's usable area. But
// GTK/Gecko toplevels can transiently report min==max at the exact
// dimensions of their *current* natural size before their real content
// has settled (observed with Firefox's crash-recovery "Restore Session"
// page, which briefly pins min==max to whatever size it first painted
// at). That's a false positive: a plain top-level browser window, not a
// dialog, and treating it as fixed-size latches is_floating permanently
// for it (handleMap only checks once), which later lets
// handleRequestConfigure honor the client's own self-chosen size instead
// of re-asserting the tile -- the window's content and border end up
// sized to whatever Gecko wants rather than the assigned tile, bleeding
// past the tile's right/bottom edge with no border or gap left to show
// for it. kMaxFixedSizeDialogDim rules those out: anything claiming a
// "fixed" size larger than a generous dialog ceiling in either dimension
// is far more likely to be a normal resizable toplevel caught mid-negotiation
// than an actual dialog.
constexpr int kMaxFixedSizeDialogDim = 800;

bool hasFixedSize(wlr_xwayland_surface* xsurface) {
    xcb_size_hints_t* sh = xsurface->size_hints;
    if(!sh) { return false; }
    if(!(sh->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) ||
       !(sh->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)) {
        return false;
    }
    return sh->min_width > 0 && sh->min_height > 0 &&
           sh->min_width == sh->max_width && sh->min_height == sh->max_height &&
           sh->min_width <= kMaxFixedSizeDialogDim &&
           sh->min_height <= kMaxFixedSizeDialogDim;
}

}  // namespace

// _GTK_FRAME_EXTENTS fetching machinery was removed in favor of dwl's
// proven approach: hard-clip every XWayland client to its tile's content
// box (border-inset), unconditionally, on every commit. The CSD-shadow
// arithmetic it used to perform (content_offset_x/y plus
// frame_right/frame_bottom sliding content_tree around) added fragile
// complexity for essentially zero benefit on the common case -- most
// X11 GTK/Gecko builds (Firefox, Zen, Chrome via XWayland) run SSD by
// default, so _GTK_FRAME_EXTENTS is usually 0 anyway, leaving the
// elaborate sliding math as dead-on-arrival complexity that introduced
// the residual rendering bugs we're fixing here.

XWaylandView::XWaylandView(Server& server, wlr_xwayland_surface* xsurface)
    : View(server), xsurface(xsurface) {
    xsurface->data = this;
    unmanaged      = xsurface->override_redirect;

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

    // content_tree is created in handleAssociate (deferred, because
    // wlr_scene_subsurface_tree_create needs a real wl_surface* -- the
    // XWaylandView constructor runs in the X server's "window exists
    // but not yet bound to a wl_surface" window). The base View's
    // content_tree field stays null between construction and associate,
    // which is harmless -- nothing visualizes before associate anyway,
    // and View::applyBoxToScene guards the content-tree operations on
    // it being non-null.

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
    map_listener.connect(&xsurface->surface->events.map,
                         [this](void*) { handleMap(); });
    unmap.connect(&xsurface->surface->events.unmap,
                  [this](void*) { handleUnmap(); });
    // wlr_scene_subsurface_tree_create needs a real wl_surface*, which
    // only exists from associate onward (the XWaylandView constructor
    // runs in the X server's "window exists but not yet bound to a
    // wl_surface" window). Match the XDG constructor pattern
    // (toplevel.cpp:36-41): create the subsurface tree parented under
    // scene_tree and place it at (border_px, border_px) right away.
    // applyBoxToScene will keep re-asserting that position on every
    // commit, but setting it here means the very first pre-arrange
    // render is already correctly positioned flush against the inside
    // of the border.
    int border_px = server.lua_cfg.settings.border_px;
    content_tree = wlr_scene_subsurface_tree_create(
        scene_tree, xsurface->surface);
    wlr_scene_node_set_position(&content_tree->node, border_px, border_px);
    surface_commit.connect(&xsurface->surface->events.commit,
                           [this](void*) { handleSurfaceCommit(); });
}

void XWaylandView::handleDissociate() {
    map_listener.disconnect();
    unmap.disconnect();
    surface_commit.disconnect();
}

void XWaylandView::handleSurfaceCommit() {
    if(!mapped || unmanaged) { return; }

    applyBoxToScene(geo);

    if(is_floating) { return; }

    int b = server.lua_cfg.settings.border_px;
    int expected_w = std::max(1, geo.width - 2 * b);
    int expected_h = std::max(1, geo.height - 2 * b);
    if(xsurface->width != expected_w || xsurface->height != expected_h) {
        configureBackend(geo);
    }
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
        // content_tree is created in handleAssociate (deferred, because
        // the subsurface tree needs a real wl_surface*); it's normally
        // non-null here, but a malformed OR window that associated
        // without a backing wl_surface would leave it null -- skip the
        // enable rather than deref a null node.
        if(content_tree) {
            wlr_scene_node_set_enabled(&content_tree->node, true);
        }
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

// X11 has no xdg_surface-style client-declared window geometry, so the
// clip is unconditionally the tile's border-inset content box, expressed
// in the X window's local coordinate space (the API in
// subprojects/wlroots/types/scene/subsurface_tree.c sets a source-box
// crop on the surface buffer in surface space). dwl, the mature
// reference wlroots compositor, takes exactly this same approach:
// hard-clip every XWayland client to its tile's content box on every
// commit, no CSD-shadow compensation. Any client whose surface buffer
// lags behind or overshoots the size we configured it to -- Firefox,
// Zen, and other Gecko-based X11 clients are routine offenders here --
// gets cropped at the chrome boundary instead of bleeding past the
// border. (Previously this code fetched _GTK_FRAME_EXTENTS and slid
// content_tree by the CSD shadow size; that complexity was solving a
// problem that mostly doesn't apply -- most X11 GTK/Gecko builds run
// SSD by default, so the property is typically 0 anyway.)
wlr_box XWaylandView::contentClipBox(const wlr_box& box) const {
    int b = server.lua_cfg.settings.border_px;
    int w = box.width - 2 * b;
    int h = box.height - 2 * b;
    return wlr_box{0, 0, w > 0 ? w : 0, h > 0 ? h : 0};
}

void XWaylandView::configureBackend(const wlr_box& box) {
    int b  = server.lua_cfg.settings.border_px;
    int vw = box.width - 2 * b;
    int vh = box.height - 2 * b;
    if(vw < 1) { vw = 1; }
    if(vh < 1) { vh = 1; }
    wlr_xwayland_surface_configure(xsurface,
                                   static_cast<int16_t>(box.x + b),
                                   static_cast<int16_t>(box.y + b),
                                   static_cast<uint16_t>(vw),
                                   static_cast<uint16_t>(vh));
}

void XWaylandView::activateBackend(bool activated) {
    wlr_xwayland_surface_activate(xsurface, activated);
}

void XWaylandView::closeBackend() { wlr_xwayland_surface_close(xsurface); }

void XWaylandView::setFullscreenBackend(bool fullscreen) {
    wlr_xwayland_surface_set_fullscreen(xsurface, fullscreen);
}
