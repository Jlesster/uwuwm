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

    // Stable for the lifetime of the view, assigned from
    // Server::next_view_id at construction. Exists so something outside
    // the process (the IPC socket -- see ipc.hpp) has a way to name a
    // specific window that survives it being re-tagged/refocused/moved,
    // unlike a raw View* (not meaningful across a socket) or app_id
    // (multiple windows can share one).
    uint32_t id;

    wlr_scene_tree* scene_tree = nullptr;  // outer container, positioned at
                                           // the *outer* (border-inclusive) box
    wlr_scene_tree* border_tree =
        nullptr;  // four thin rects, child of scene_tree
    wlr_scene_tree* content_tree =
        nullptr;  // surface content, offset by border
                  // thickness. For XdgToplevel this is
                  // the tree wlr_scene_xdg_surface_create
                  // returns directly (a real subsurface
                  // tree via the xdg_shell.c-internal
                  // wlr_scene_subsurface_tree_create);
                  // for XWaylandView this is the
                  // wlr_scene_subsurface_tree_create tree
                  // created in handleAssociate, with the
                  // actual scene_surface nested one level
                  // inside it. Both backends must give us
                  // a real subsurface tree here -- that's
                  // what makes the
                  // wlr_scene_subsurface_tree_set_clip
                  // call in applyBoxToScene actually
                  // apply (it asserts-and-silently-no-ops
                  // on plain trees, see subsurface_tree.c
                  // :328-369). Null between XWaylandView
                  // construction and handleAssociate, then
                  // non-null for the lifetime of the
                  // wl_surface.
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

    // Last-applied clip size (xdg_surface geometry width/height). Tracked
    // separately from content_offset_x/y because geometry can change size
    // on a commit without its x/y origin moving -- GTK/Gecko clients in
    // particular resize their declared window geometry without shifting
    // the CSD shadow origin, and each such commit still needs the scene
    // clip re-applied or the client's surface buffer (which can lag or
    // overshoot the declared geometry by a frame, notably in Firefox/Zen)
    // bleeds past the border instead of being cut off at the chrome
    // boundary. 0 means "no clip applied yet".
    int content_clip_w = 0;
    int content_clip_h = 0;

    std::string title;
    std::string app_id;

    uint32_t tags   = 1;  // bitmask; which tag(s) this window appears on
    Output*  output = nullptr;

    bool is_floating   = false;
    bool is_fullscreen = false;
    bool mapped        = false;

    // Per-client border color override, set via uwu.rule()'s
    // apply.border_color_active/inactive or client.border_color_active/
    // inactive = "#hex" (see l_client_newindex in lua_config.cpp). Unset
    // (has_border_override == false) means "use
    // server.lua_cfg.settings.border_color_{active,inactive}", same as
    // every view before this existed -- nyaa.rule() (lib/nyaa) is the
    // Lua-side sugar over this, the same way nyaa.wear() sugars the
    // global uwu.set("border_color_active", ...) pair. Only one flag
    // covers both colors rather than two independent has_* fields,
    // since a rule that wants to override the border at all virtually
    // always wants both states themed together; add a second has_*
    // flag here if that assumption turns out wrong.
    bool     has_border_override            = false;
    uint32_t border_color_active_override   = 0;
    uint32_t border_color_inactive_override = 0;

    // Per-client *unfocused* opacity override, set via uwu.rule()'s
    // apply.opacity (see l_rule_hook in lua_config.cpp) -- nyaa.rule()'s
    // `opacity` field is the Lua-side sugar over this, the same
    // relationship nyaa.rule()'s border fields have to has_border_override
    // above. Unset (has_opacity_override == false) means "use
    // server.lua_cfg.settings.inactive_opacity", same as every view
    // before this existed. Only applies while unfocused -- a focused
    // view is always fully opaque regardless of this override, matching
    // how inactive_opacity itself already behaves (see the setFocused
    // call site in view.cpp).
    bool  has_opacity_override = false;
    float opacity_override     = 1.0f;

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

    // Canonical floating-toggle setter. Replaces what used to be three
    // separate hand-rolled copies of "flip is_floating, snapshot
    // floating_geo, re-arrange" (l_client_newindex's `client.floating =`,
    // l_toggle_floating's keybind, and l_rule_hook's `apply.floating`) --
    // see lua_config.cpp. Floating into tiled re-arranges the output;
    // tiled into floating centers the view over its output at its
    // current content size via centeredFloatBox() below, which is the
    // one thing the old l_rule_hook copy never did (a rule floating an
    // already-mapped, already-tiled window left it at its tiled
    // position/size instead of centering it). No-op if `floating`
    // already matches is_floating, or while fullscreened (fullscreen
    // owns geometry until it's cleared -- see setFullscreen).
    void setFloating(bool floating);

    // Pure math: a `content_w`x`content_h` (client-visible, border
    // excluded) box centered on `output`'s layout box, with the border
    // added back on -- the exact placement every "float, centered" path
    // wants (initial map, setFloating above). Returns a zeroed box if
    // `output` is null. `content_w`/`content_h` <= 0 fall back to a
    // sane default (480x360) the same way the old inline handleMap copies
    // of this math already did for a client that hasn't committed a real
    // size yet.
    wlr_box centeredFloatBox(int content_w, int content_h) const;

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

    // Recomputes and re-applies this view's border color for the given
    // focused-state -- called internally by setFocused()/setFullscreen()
    // etc, and also by lua_config.cpp's client.border_color_active/
    // inactive property setter and uwu.rule()'s border_color_* apply
    // fields (l_client_newindex/l_rule_hook), which need to refresh the
    // border immediately after changing has_border_override/
    // border_color_*_override without waiting for the next focus change.
    // Public (unlike the rest of this section) specifically because of
    // that external caller -- everything else below stays a View-only
    // implementation detail.
    void updateBorderColor(bool focused, float alpha = 1.0f);

    // Same external-caller exception as updateBorderColor just above --
    // l_client_newindex/l_rule_hook (lua_config.cpp) need to re-push
    // opacity_override immediately after setting it, same as a
    // border-color override does via updateBorderColor, rather than
    // waiting for the next focus change to pick it up. Only does
    // anything while unfocused, since a focused view is always fully
    // opaque -- see setFocused's own comment on has_opacity_override.
    // A thin public wrapper rather than making setOpacity() itself
    // public -- this is a View member function, so it can call the
    // still-protected setOpacity() below just fine; lua_config.cpp
    // (a free function, not a View member) can't, which is exactly
    // why this wrapper exists. Declared here, defined in view.cpp
    // (not inline) -- an inline body touching server.focused_view
    // would need Server to be a complete type in every TU that
    // includes view.hpp, and view.hpp only forward-declares it.
    void applyOpacityOverride();

    // Thin public wrappers around the protected beginInteractiveMove()/
    // beginInteractiveResizeFromCursor() below, for the exact same reason
    // applyOpacityOverride() above is a wrapper around protected
    // setOpacity(): uwu.mousebind() closures call client:begin_move()/
    // :begin_resize() (l_client_begin_move/l_client_begin_resize in
    // lua_config.cpp), which are free C functions, not View members, and
    // so can't reach the protected methods directly the way
    // XdgToplevel::handleRequestMove/XWaylandView::handleRequestMove can.
    void beginMoveFromMousebind() { beginInteractiveMove(); }
    void beginResizeFromMousebind() { beginInteractiveResizeFromCursor(); }

protected:
    // Shared implementations for the request_move/request_resize handlers
    // every backend has (xdg_toplevel and xwayland both expose them) --
    // pure Server/View bookkeeping, no backend-specific call involved.
    void beginInteractiveMove();
    void beginInteractiveResize(uint32_t edges);

    // Same as beginInteractiveResize(edges), but picks `edges` itself from
    // which quadrant of the window the cursor is currently over (left half
    // drags the left edge, top half drags the top edge, etc) instead of
    // requiring the caller to know which edge it wants up front -- the
    // same "resize from the nearest edge/corner" convention i3/sway's
    // floating_modifier resize-drag uses. Meant for uwu.mousebind()
    // closures (client:begin_resize(), via beginResizeFromMousebind()
    // above) where the bind fires from wherever the cursor happened to
    // be, not from a specific titlebar corner the way
    // XdgToplevel::handleRequestResize's edges (driven by the client's
    // own CSD hit-test) are.
    void beginInteractiveResizeFromCursor();

    // Starts the "grow-in" open animation (fade 0->1, slight upward
    // slide-in) from the view's current `geo`. Call once, right after the
    // subclass has placed the view at its real final geo (see
    // XdgToplevel::handleMap / XWaylandView::handleMap).
    void playOpenAnimation();

    // Starts the "pop-in" animation used for floating windows: grows
    // from a smaller box centered on the same point as the view's
    // current (already-final) `geo`, fading in at the same time, rather
    // than open's vertical slide -- a floating window appearing already
    // detached in the middle of the screen reads as a slide from
    // nowhere-in-particular, whereas scaling up from its own center
    // reads as "popping out". Call once, right after the view has been
    // placed at its real final floating geo (same call-site convention
    // as playOpenAnimation) -- see XdgToplevel::handleMap/
    // XWaylandView::handleMap (initial floating map) and
    // View::setFloating (tiled -> floating toggle).
    void playFloatPopAnimation();

    // Starts the "shrink-out" close animation (fade 1->0, slight downward
    // slide) against the view's *current, still fully live* content, and
    // calls closeBackend() once it finishes. This deliberately runs
    // before the real close request goes out -- by construction the
    // client hasn't unmapped yet, so there's real content to animate
    // rather than needing to snapshot a soon-to-vanish buffer.
    void playCloseAnimation();

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

    // Keeps scene_tree parented under the correct always-ordered layer
    // (server.tiled_tree or server.floating_tree) for the view's current
    // is_floating/is_fullscreen state -- see server.hpp's comment on
    // those two trees. Call any time either flag changes (setFloating,
    // setFullscreen) or once at initial map, after is_floating is first
    // known. A no-op scene-graph move only; never touches geometry.
    void updateZOrder();

    // Border rects + content offset + scene_tree position, purely at the
    // scene-graph level -- no client configure. Used both by setGeometry
    // (the real, immediate placement) and by tickAnimation (the
    // intermediate, per-frame placement during a tween).
    void applyBoxToScene(const wlr_box& box);

    // The clip box for content_tree, in the wl_surface coordinate
    // space of the surface content_tree wraps. The default empty box
    // (`{0}`) means "no clip"; both backends override. `box` is
    // whatever applyBoxToScene is currently placing this view at
    // (the final geo, or an in-progress animation frame). XdgToplevel
    // uses the client's declared xdg_surface window-geometry
    // (toplevel.cpp) so the visible chrome (geometry.{x,y,width,
    // height}) is exactly what gets rendered; XWaylandView uses
    // (0, 0, box - 2b) so the X window's local content area is exactly
    // what gets rendered -- same hard-clip approach dwl uses. This
    // exists so a surface buffer that overshoots or lags behind what
    // it was actually allocated -- a client (Gecko-based ones, namely
    // Firefox/Zen, are particularly prone to this mid-resize) committing
    // a differently-sized buffer than expected -- gets cropped at the
    // chrome boundary instead of bleeding past the border. Width == 0
    // means "no clip".
    //
    // Implemented via wlr_scene_subsurface_tree_set_clip in
    // applyBoxToScene, which requires content_tree to be a real
    // subsurface tree (XdgToplevel: wlr_scene_xdg_surface_create
    // nests one; XWaylandView: wlr_scene_subsurface_tree_create
    // directly). On a plain wlr_scene_tree the API is a silent
    // no-op in release builds (subsurface_tree.c:328-369), which
    // is exactly the bug this code is structured to avoid.
    virtual wlr_box contentClipBox(const wlr_box& /*box*/) const {
        return wlr_box{};
    }

    // Sets uniform opacity across every buffer node under content_tree
    // (via wlr_scene_buffer_set_opacity) and scales the border rects'
    // alpha channel to match (wlr_scene_rect has no opacity knob of its
    // own, but its color is already RGBA).
    void setOpacity(float alpha);

    // Explicit-focused overload -- used by setFocused() itself, where
    // server.focused_view can't be trusted yet (Server::focusView updates
    // it *after* calling the outgoing view's setFocused(false), so
    // `server.focused_view == this` would still read true for the view
    // losing focus at that exact call site). Every other caller
    // (tickAnimation, startAnim) keeps using the single-arg overload,
    // where focus is already settled and server.focused_view is correct.
    void setOpacity(float alpha, bool focused);

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
