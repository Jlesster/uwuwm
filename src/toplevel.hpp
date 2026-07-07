#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
}

#include "listener.hpp"
#include "view.hpp"

class Server;

// One mapped XDG-shell window (a native Wayland client). See view.hpp for
// everything shared with XWaylandView -- tiling, tags, focus/border
// bookkeeping, move/resize grabs, animation. This class is only the
// wlr_xdg_toplevel-specific plumbing: its listeners and the backend seams
// that tell View how to actually configure/activate/close/fullscreen an
// XDG client.
struct XdgToplevel : public View {
    XdgToplevel(Server& server, wlr_xdg_toplevel* xdg_toplevel);
    ~XdgToplevel() override;

    wlr_xdg_toplevel* xdg_toplevel;

    void         configureBackend(const wlr_box& box) override;
    void         activateBackend(bool activated) override;
    void         closeBackend() override;
    void         setFullscreenBackend(bool fullscreen) override;
    wlr_surface* wlrSurface() const override {
        return xdg_toplevel->base->surface;
    }
    wlr_box contentClipBox(const wlr_box& /*box*/) const override {
        return xdg_toplevel->base->geometry;
    }

private:
    void handleMap();
    void handleUnmap();
    void handleNewPopup(wlr_xdg_popup* popup);
    void handleDestroy();
    void handleCommit(wlr_surface* surface);
    void handleRequestMove();
    void handleRequestResize(wlr_xdg_toplevel_resize_event* event);
    void handleRequestMaximize();
    void handleRequestFullscreen();
    void handleSetTitle();
    void handleSetAppId();

    VoidListener                            map_listener;
    VoidListener                            unmap;
    VoidListener                            destroy;
    Listener<wlr_surface>                   commit;
    Listener<wlr_xdg_popup>                 new_popup;
    VoidListener                            request_move;
    Listener<wlr_xdg_toplevel_resize_event> request_resize;
    VoidListener                            request_maximize;
    VoidListener                            request_fullscreen;
    VoidListener                            set_title;
    VoidListener                            set_app_id;
};
