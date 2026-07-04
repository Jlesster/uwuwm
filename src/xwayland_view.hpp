#pragma once

extern "C" {
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

private:
    void handleAssociate();
    void handleDissociate();
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
