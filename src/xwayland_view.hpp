#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
}

// See server.hpp's include block for why this needs the `class`->`class_`
// macro guard (a real struct field is named `class`, valid C, not valid
// C++) rather than the `namespace` trick used elsewhere in this codebase.
#include <pthread.h>
#pragma push_macro("class")
#define class class_
extern "C" {
#include <wlr/xwayland.h>
}
#pragma pop_macro("class")
#include "listener.hpp"
#include "view.hpp"

class Server;

// An X11 client's window, running under Xwayland. Wraps
// wlr_xwayland_surface the way XdgToplevel (toplevel.hpp) wraps
// wlr_xdg_toplevel -- see view.hpp for what's shared between them.
//
// X11 has none of xdg-shell's clean single map/configure/close-request
// cycle: a wlr_xwayland_surface exists (and can already be positioned)
// from the moment the X server creates the window, well before it
// "associates" with an actual wl_surface (the point at which there's
// finally something to put in the scene graph), and separately again from
// when it later maps. We track all three transitions. override_redirect
// windows (menus, tooltips, DND icons -- anything that places itself) are
// tracked as `unmanaged`: no border, no tiling, no tag membership, not
// touched by layout::arrange, positioned exactly where X11 put them.
struct XWaylandView : public View {
    XWaylandView(Server& server, wlr_xwayland_surface* xsurface);
    ~XWaylandView() override;

    wlr_xwayland_surface* xsurface;

    void         configureBackend(const wlr_box& box) override;
    void         activateBackend(bool activated) override;
    void         closeBackend() override;
    void         setFullscreenBackend(bool fullscreen) override;
    wlr_surface* wlrSurface() const override { return xsurface->surface; }
    wlr_box      contentClipBox(const wlr_box& box) const override;
    void         applyContentOffsetToScene(const wlr_box& box) override;

    // _GTK_FRAME_EXTENTS right/bottom components (left/top reuse the
    // base View's content_offset_x/y -- see xwayland_view.cpp's
    // fetchGtkFrameExtents and handleMap for where these get set, and
    // configureBackend/contentClipBox for where they're consumed). 0 for
    // any client that doesn't set the property, which keeps this a no-op
    // for non-GTK / non-CSD X11 clients.
    int frame_right  = 0;
    int frame_bottom = 0;

private:
    // content_tree is created in handleAssociate, not the constructor,
    // because wlr_scene_subsurface_tree_create needs a real wl_surface*
    // (the XWaylandView constructor runs in the X server's "window
    // exists but not yet bound to a wl_surface" window). The base
    // View::content_tree field stays null between construction and
    // associate, which is harmless -- nothing visualizes before
    // associate anyway, and View::applyBoxToScene guards the
    // content-tree operations on it being non-null. The tree
    // self-destructs when xsurface->surface does (wlroots hooks
    // surface_destroy in wlr_scene_subsurface_tree_create) -- see
    // handleDissociate.

    wlr_scene_tree* scene_surface = nullptr;

    void handleAssociate();
    void handleDissociate();
    void handleSurfaceCommit();
    void handleMap();
    void handleUnmap();
    void handleDestroy();
    void handleRequestMove();
    void handleRequestResize(wlr_xwayland_resize_event* event);
    void handleRequestConfigure(wlr_xwayland_surface_configure_event* event);
    void handleRequestFullscreen();
    void handleSetTitle();
    void handleSetClass();

    VoidListener                                   associate;
    VoidListener                                   dissociate;
    VoidListener                                   surface_commit;
    VoidListener                                   map_listener;
    VoidListener                                   unmap;
    VoidListener                                   destroy;
    VoidListener                                   request_move;
    Listener<wlr_xwayland_resize_event>            request_resize;
    Listener<wlr_xwayland_surface_configure_event> request_configure;
    VoidListener                                   request_fullscreen;
    VoidListener                                   set_title;
    VoidListener                                   set_class;
};
