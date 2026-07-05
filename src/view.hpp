#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
}

#include "listener.hpp"

#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Server;
struct Output;
struct wlr_surface;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_foreign_toplevel_handle_v1_maximized_event;
struct wlr_foreign_toplevel_handle_v1_minimized_event;
struct wlr_foreign_toplevel_handle_v1_activated_event;
struct wlr_foreign_toplevel_handle_v1_fullscreen_event;

// What happens to a View when an in-progress animation finishes. Kept
// generic rather than "OpenDone/CloseDone" because the same tween drives
// three different visual jobs (open, close, tag-switch slide) that only
// differ in what -- if anything -- should happen once the box/opacity
// interpolation reaches its end.
enum class AnimEnd {
    None,          // just stop -- used by the open animation
    RequestClose,  // call closeBackend() -- used by the close animation
    Disable,       // hide the scene node -- used by the tag-switch "exit" slide
};

// A single in-progress geometry+opacity tween on a View. Pure scene-graph
// state: interpolating `from` -> `to` and `opacity_from` -> `opacity_to`
// never itself asks the client to resize (see View::applyBoxToScene vs.
// View::setGeometry) and never touches XDG/X11-specific state, which is
// what makes one animation implementation work for both View subclasses.
struct ViewAnimation {
    wlr_box  from{}, to{};
    float    opacity_from = 1.0f, opacity_to = 1.0f;
    timespec start{};
    int      duration_ms = 150;
    AnimEnd  on_finish   = AnimEnd::None;
};

// Base class for anything the compositor can tile/float/focus/border --
// an on-screen "window" regardless of which shell protocol backs it.
// XdgToplevel (toplevel.hpp) wraps wlr_xdg_toplevel; XWaylandView
// (xwayland_view.hpp) wraps wlr_xwayland_surface. Everything that used to
// live directly on Toplevel and never actually cared which protocol it
// was talking to -- tiling, tags, focus/border bookkeeping, move/resize
// grabs, animation -- lives here exactly once. The four "Backend"
// virtuals plus wlrSurface() are the only places XDG and X11 genuinely
// differ; everything else is shared.
struct View {
    explicit View(Server& server);
    virtual ~View();

    View(const View&)            = delete;
    View& operator=(const View&) = delete;

    Server& server;

    wlr_scene_tree* scene_tree = nullptr;  // outer container, positioned at
                                           // the *outer* (border-inclusive) box
    wlr_scene_tree* border_tree =
        nullptr;  // four thin rects, child of scene_tree
    wlr_scene_tree* content_tree =
        nullptr;  // surface content, offset by border
                  // thickness. For XdgToplevel this is
                  // the tree wlr_scene_xdg_surface_create
                  // returns directly; for XWaylandView
                  // it's a plain tree we create so the
                  // field type matches, with the actual
                  // surface content nested one level
                  // inside via wlr_scene_surface_create.
    wlr_scene_rect* border_rects[4] = {};  // top, bottom, left, right

    // Correction for the client's xdg_surface window-geometry x/y offset
    // (GTK/Qt use a non-zero offset here to reserve invisible space for
    // CSD drop-shadows around the real visible window). Always 0 for
    // XWaylandView, which has no equivalent concept. XdgToplevel keeps
    // this in sync with xdg_toplevel->base->geometry on every commit --
    // see XdgToplevel::handleCommit. applyBoxToScene uses it so the
    // border hugs the client's actual visible bounds instead of its
    // surface buffer's bounds.
    int content_offset_x = 0;
    int content_offset_y = 0;

    std::string title;
    std::string app_id;

    uint32_t tags   = 1;  // bitmask; which tag(s) this window appears on
    Output*  output = nullptr;

    bool is_floating   = false;
    bool is_fullscreen = false;
    bool mapped        = false;

    // Set only via setMinimized() (below), which is what
    // wlr-foreign-toplevel-management-v1's request_minimize handler
    // calls -- there's no compositor keybind for this, only a taskbar/
    // dock asking. A minimized view drops out of tiling (layout.cpp's
    // getTiledViews/arrange both check this, the same way they already
    // check tags) and its scene node is disabled, but it keeps its
    // output/tags/geo untouched so restoring it just re-enables and
    // re-arranges rather than re-placing it from scratch.
    bool is_minimized = false;

    // X11 override-redirect windows (dropdown menus, tooltips, DND icons):
    // no border, no tiling, no tag membership, no focus-follows-click, and
    // never touched by layout::arrange. Always false for XdgToplevel.
    bool unmanaged = false;

    // Popups owned by this view.
    std::vector<std::unique_ptr<struct Popup>> popups;

    // Geometry as last arranged by the tiling layout (or, for floating
    // windows, as last placed by the user). Layout-relative (global)
    // coordinates, matches wlr_scene_node_set_position semantics.
    wlr_box geo{};

    wlr_box floating_geo{};  // remembered geometry to restore on un-fullscreen

    // Non-null only while this view is mapped and managed (never set for
    // X11 override-redirect windows) -- see createForeignToplevel/
    // destroyForeignToplevel below.
    wlr_foreign_toplevel_handle_v1* foreign_toplevel = nullptr;

    // --- public API, identical regardless of backing protocol -----------
    void setGeometry(const wlr_box& box);
    void setTags(uint32_t new_tags);
    void setFullscreen(bool fullscreen);
    void setFocused(bool focused);
    void setMinimized(bool minimized);
    void close();

    // Workspace/tag-switch slide helpers -- see Output::setTagset.
    // playSlideOut: animate from `geo` by (dx, dy) then disable the node
    // (used for views leaving the visible tagset).
    // playSlideIn: animate from `geo` offset by (dx, dy) back to `geo`
    // (used for views entering the visible tagset).
    void playSlideOut(int dx, int dy);
    void playSlideIn(int dx, int dy);

    // Advances any in-progress animation to `now`. No-op if nothing is
    // animating. Called every frame from Output::handleFrame via
    // Server::tickAnimations() -- see server.cpp/output.cpp.
    void tickAnimation(const timespec& now);

    // --- backend seams ----------------------------------------------------
    // The only places an XdgToplevel and an XWaylandView actually differ.
    virtual void         configureBackend(const wlr_box& box)  = 0;
    virtual void         activateBackend(bool activated)       = 0;
    virtual void         closeBackend()                        = 0;
    virtual void         setFullscreenBackend(bool fullscreen) = 0;
    virtual wlr_surface* wlrSurface() const                    = 0;

protected:
    // Shared implementations for the request_move/request_resize handlers
    // every backend has (xdg_toplevel and xwayland both expose them) --
    // pure Server/View bookkeeping, no backend-specific call involved.
    void beginInteractiveMove();
    void beginInteractiveResize(uint32_t edges);

    // Starts the "grow-in" open animation (fade 0->1, slight upward
    // slide-in) from the view's current `geo`. Call once, right after the
    // subclass has placed the view at its real final geo (see
    // XdgToplevel::handleMap / XWaylandView::handleMap).
    void playOpenAnimation();

    // Starts the "shrink-out" close animation (fade 1->0, slight downward
    // slide) against the view's *current, still fully live* content, and
    // calls closeBackend() once it finishes. This deliberately runs
    // before the real close request goes out -- by construction the
    // client hasn't unmapped yet, so there's real content to animate
    // rather than needing to snapshot a soon-to-vanish buffer.
    void playCloseAnimation();

    void updateBorderColor(bool focused, float alpha = 1.0f);

    // Creates this view's wlr_foreign_toplevel_handle_v1 and wires its
    // request_maximize/request_minimize/request_activate/
    // request_fullscreen/request_close/destroy signals. Call once, from
    // handleMap, after mapped/output/tags/title/app_id are all already
    // set -- those are pushed to the handle at creation time and never
    // re-read from scratch afterwards (see syncForeignToplevelMeta for
    // the title/app_id case). No-op if server.foreign_toplevel_manager
    // is null (shouldn't happen once Server::setup runs, but a unit-style
    // caller without a real Server shouldn't crash either) or a handle
    // already exists.
    void createForeignToplevel();

    // Destroys the wlr_foreign_toplevel_handle_v1 if one exists and
    // disconnects its listeners. Call from handleUnmap -- a hidden
    // window has nothing to show a taskbar -- and unconditionally from
    // ~View as a safety net for any destruction path that skips unmap
    // (a client that disconnects without a clean unmap/destroy
    // sequence). Safe to call when foreign_toplevel is already null.
    void destroyForeignToplevel();

    // Re-pushes title/app_id to an already-created foreign_toplevel
    // handle. Call from handleSetTitle/handleSetAppId (both backends);
    // safe -- a no-op -- when foreign_toplevel is null, so call sites
    // don't need their own guard.
    void syncForeignToplevelMeta();

    // Border rects + content offset + scene_tree position, purely at the
    // scene-graph level -- no client configure. Used both by setGeometry
    // (the real, immediate placement) and by tickAnimation (the
    // intermediate, per-frame placement during a tween).
    void applyBoxToScene(const wlr_box& box);

    // Sets uniform opacity across every buffer node under content_tree
    // (via wlr_scene_buffer_set_opacity) and scales the border rects'
    // alpha channel to match (wlr_scene_rect has no opacity knob of its
    // own, but its color is already RGBA).
    void setOpacity(float alpha);

private:
    void startAnim(const wlr_box& from,
                   const wlr_box& to,
                   float          op_from,
                   float          op_to,
                   AnimEnd        end);

    std::optional<ViewAnimation> anim;

    // Wired up (and torn down) only inside createForeignToplevel/
    // destroyForeignToplevel -- these connect to foreign_toplevel's
    // signals, which don't exist until that handle does.
    Listener<wlr_foreign_toplevel_handle_v1_maximized_event>
        ft_request_maximize;
    Listener<wlr_foreign_toplevel_handle_v1_minimized_event>
        ft_request_minimize;
    Listener<wlr_foreign_toplevel_handle_v1_activated_event>
        ft_request_activate;
    Listener<wlr_foreign_toplevel_handle_v1_fullscreen_event>
                 ft_request_fullscreen;
    VoidListener ft_request_close;
    VoidListener ft_destroy;
};
