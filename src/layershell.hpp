#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
}

#pragma push_macro("namespace")
#define namespace namespace_
extern "C" {
#include <wlr/types/wlr_layer_shell_v1.h>
}
#pragma pop_macro("namespace")

#include "listener.hpp"

class Server;
struct Output;

// A layer-shell client: status bars, launchers, ext-session-lock-style
// overlays. Unlike Toplevels these are never tiled by the layout -- they
// request an anchor + size (or full-width/height stretch) and optionally
// reserve an "exclusive zone" that shrinks the usable area windows get
// arranged into. This is M4 from the doc.
struct LayerSurface {
    LayerSurface(Server& server, wlr_layer_surface_v1* layer_surface);
    ~LayerSurface();

    Server&                     server;
    wlr_layer_surface_v1*       layer_surface;
    wlr_scene_layer_surface_v1* scene_layer_surface = nullptr;
    Output*                     output              = nullptr;

    bool mapped = false;

    // False if construction couldn't resolve an output to attach to (no
    // output specified by the client and none focused yet). In that case
    // the constructor has already destroyed `layer_surface` itself and no
    // listeners are connected -- callers (see server.cpp's new_surface
    // handler) must check this and discard the object rather than storing
    // it, or they end up holding a wrapper around freed memory forever.
    bool valid = true;

private:
    void handleMap();
    void handleUnmap();
    void handleDestroy();
    void handleCommit(wlr_surface* surface);
    void handleNewPopup(wlr_xdg_popup* popup);
    void handleOutputDestroy();

    VoidListener            map_listener;
    VoidListener            unmap;
    VoidListener            destroy;
    Listener<wlr_surface>   commit;
    Listener<wlr_xdg_popup> new_popup;
    VoidListener            output_destroy;
};

// Recomputes each output's usable_box from the exclusive zones of its
// mapped layer-shell surfaces, and repositions every layer surface
// accordingly. Call after any layer surface maps/unmaps/commits a new
// exclusive zone, and after output mode changes. Also re-arranges the
// tiling layout afterward, since usable_box feeding the layout is the
// whole point.
void arrangeLayers(Output& output);
