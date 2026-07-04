#pragma once

#include "listener.hpp"

class Server;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_surface;

// One live zxdg_toplevel_decoration_v1 object, wrapping the wlroots-side
// wlr_xdg_toplevel_decoration_v1. uwuwm always draws its own thin border
// (view.hpp's border_tree/border_rects) for every tiled and floating
// window regardless of shell backend -- there's no code path that leaves
// a mapped window without one. Letting a client draw CSD on top of that
// would either double up the chrome (client titlebar *and* our border) or
// hand a client-drawn titlebar to a tiled window that has no spare pixels
// for one. So this never actually negotiates: it forces
// WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE as soon as the
// underlying xdg_surface is ready, and re-forces it on every
// request_mode, regardless of what the client asked for -- the same
// "compositor always wins" policy dwl/river-style tiling compositors
// use, and well within what the protocol allows (a compositor is only
// ever obligated to *respond* to set_mode with a configure, never to
// grant the requested mode).
//
// "As soon as the underlying xdg_surface is ready" matters: the new
// decoration object is delivered in the new_toplevel_decoration signal,
// which fires *before* the client has committed a single buffer to the
// xdg_surface. wlr_xdg_toplevel_decoration_v1_set_mode() ends up
// calling wlr_xdg_surface_schedule_configure(), which wlroots asserts
// requires surface->initialized -- and initialized only flips to true
// on the first commit. Calling set_mode eagerly from the constructor
// would crash the compositor (assertion failure) the moment a
// decoration-aware client (kitty, gtk apps, ...) connects. We work
// around it by listening for the surface's first commit and only then
// forcing the mode.
struct ToplevelDecoration {
    ToplevelDecoration(Server&                         server,
                       wlr_xdg_toplevel_decoration_v1* decoration);
    ~ToplevelDecoration();

    Server&                         server;
    wlr_xdg_toplevel_decoration_v1* decoration;

private:
    void handleDestroy();
    void handleRequestMode();
    void handleCommit(wlr_surface* surface);

    VoidListener destroy;
    VoidListener request_mode;
    Listener<wlr_surface> commit;
    bool mode_set = false;
};

namespace decoration {

// Wraps a newly-created wlr_xdg_toplevel_decoration_v1 in a
// ToplevelDecoration, owned by server.toplevel_decorations. Called from
// the xdg-decoration manager's new_toplevel_decoration signal, wired up
// in Server::setup().
void newToplevelDecoration(Server&                         server,
                           wlr_xdg_toplevel_decoration_v1* decoration);

}  // namespace decoration
