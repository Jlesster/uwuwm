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
    // No committed geometry exists yet at construction time, so the
    // offset is 0/0 here -- handleCommit takes over correcting this
    // (and re-applying border_px on top of it) from the first commit
    // onward.
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

    // Dialogs/transient children (has a parent) default to floating,
    // centered -- the way they almost always expect to be placed.
    // Beyond that, a client that pins its min size equal to its max size
    // is telling us -- via the same xdg_toplevel state every configure
    // negotiates -- that it never wants to be resized at all, which in
    // practice is exactly the signal dialog-shaped utility windows (a
    // save-file picker, a color chooser, a small settings panel with no
    // parent set) give that a plain app_id/title rule would otherwise
    // have to hardcode per-client. A window that hasn't announced any
    // min/max yet (both still 0, xdg-shell's "unconstrained" default)
    // never matches this.
    const wlr_xdg_toplevel_state& state = xdg_toplevel->current;
    bool fixed_size = state.min_width > 0 && state.min_height > 0 &&
                      state.min_width == state.max_width &&
                      state.min_height == state.max_height;
    is_floating     = xdg_toplevel->parent != nullptr || fixed_size;

    wlr_log(WLR_DEBUG,
            "[uwuwm xdg] handleMap title='%s' parent=%p min=%dx%d "
            "max=%dx%d fixed_size=%d -> is_floating=%d",
            xdg_toplevel->title ? xdg_toplevel->title : "?",
            (void*)xdg_toplevel->parent,
            state.min_width,
            state.min_height,
            state.max_width,
            state.max_height,
            fixed_size,
            is_floating);

    if(xdg_toplevel->title) { title = xdg_toplevel->title; }
    if(xdg_toplevel->app_id) { app_id = xdg_toplevel->app_id; }

    if(output) { layout::arrange(*output); }

    // A client can call xdg_toplevel.set_fullscreen() before its first
    // commit -- output was still null then, so View::setFullscreen
    // recorded is_fullscreen but couldn't call into the backend or place
    // the geometry (see that function). output exists now; re-derive the
    // client's actual requested state directly from xdg_toplevel (not our
    // own is_fullscreen, which setFullscreen's early-return may have left
    // set without ever having applied anything) and apply it for real.
    if(xdg_toplevel->requested.fullscreen) {
        is_fullscreen = false;
        setFullscreen(true);
    }

    // is_fullscreen check: setFullscreen(true) above already placed this
    // view at output->layout_box and owns its geometry until it's
    // cleared. Centering it into a floating box here would immediately
    // clobber that -- see the matching guard in XWaylandView::handleMap.
    if(is_floating && !is_fullscreen && output) {
        wlr_box geo_box = xdg_toplevel->base->geometry;
        wlr_box box     = centeredFloatBox(geo_box.width, geo_box.height);
        floating_geo    = box;
        setGeometry(box);
    }

    createForeignToplevel();
    server.focusView(this);
    playOpenAnimation();

    // A newly-mapped surface might be the one an existing
    // zwp_idle_inhibitor_v1 was created against before it ever mapped
    // (e.g. a game requesting the inhibitor from its splash screen).
    idle::updateInhibitState(server);

    // Fire *after* focusView so a uwu.hook("client::manage", fn) callback
    // can call client.focused() and see the just-mapped view (matching
    // what every other compositor's manage hook observes).
    server.lua_cfg.fireClientEvent("client::manage", this);
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
    // Fires from handleDestroy, not handleUnmap: handleUnmap is "hidden"
    // (a tray app can re-map later and re-fire client::manage), whereas
    // handleDestroy is "gone" -- the View is about to leave
    // server.views, and the fire happens before remove_if so any hook
    // capturing a Client userdata can still resolve it via checkClient.
    server.lua_cfg.fireClientEvent("client::unmanage", this);
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

    // The client's window geometry (xdg_surface.set_window_geometry) can
    // carry a non-zero x/y offset -- GTK/Qt use this to reserve invisible
    // space for CSD drop-shadows around the actual visible window bounds.
    // wlr_scene_xdg_surface_create roots content_tree at the wl_surface's
    // own buffer origin, which sits *outside* that visible rectangle for
    // such clients -- without correcting for it, our border hugs the
    // buffer edge instead of the client's real visible edge, clipping
    // into the shadow on one side and cutting off real content (or
    // leaving a border-sized gap) on the opposite side. Geometry is
    // double-buffered client state that can change on any commit, wholly
    // independent of any resize we initiated, so re-check every time
    // rather than only on initial_commit.
    const wlr_box& client_geo = xdg_toplevel->base->geometry;

    // client_geo (xdg_surface.set_window_geometry) is what Firefox
    // *declares* as its visible rectangle -- everything above and in
    // previous rounds has assumed that's an accurate description of what
    // actually landed in its wl_surface buffer. That's never been
    // directly checked. surface->current.width/height is the real,
    // already-scale-normalized (surface-local) size of the buffer Firefox
    // actually committed; surface->current.scale is the buffer_scale it
    // used. If these disagree with client_geo in a way that tracks the
    // output's scale factor, the mismatch is a HiDPI/buffer_scale
    // conversion bug, not a resize-negotiation problem -- which would
    // explain content rendering wider than the tile even though every
    // negotiated *number* checks out, since the clip is being computed
    // in one coordinate space while the client painted in another.
    int surf_w     = xdg_toplevel->base->surface->current.width;
    int surf_h     = xdg_toplevel->base->surface->current.height;
    int buf_w      = xdg_toplevel->base->surface->current.buffer_width;
    int buf_h      = xdg_toplevel->base->surface->current.buffer_height;
    int surf_scale = xdg_toplevel->base->surface->current.scale;
    int out_scale =
        output ? static_cast<int>(output->wlr_output->scale * 100) : -1;

    bool geo_changed = client_geo.x != content_offset_x ||
                       client_geo.y != content_offset_y ||
                       client_geo.width != content_clip_w ||
                       client_geo.height != content_clip_h;

    // Only worth a log line when something actually changed -- this used
    // to fire unconditionally on every commit, for every toplevel
    // (kitty, wezterm, everything), which was pure overhead once the
    // investigation that needed it was done. The mismatch/re-assert path
    // below still logs unconditionally when it fires, since that's rare
    // and always worth seeing.
    if(geo_changed) {
        wlr_log(WLR_DEBUG,
                "[uwuwm xdg] commit title='%s' geo(tile)=%dx%d "
                "client_geo=%d,%d %dx%d surface_current=%dx%d "
                "raw_buffer=%dx%d surface_scale=%d output_scale=%d%% "
                "prev_recorded=%d,%d %dx%d",
                title.c_str(),
                geo.width,
                geo.height,
                client_geo.x,
                client_geo.y,
                client_geo.width,
                client_geo.height,
                surf_w,
                surf_h,
                buf_w,
                buf_h,
                surf_scale,
                out_scale,
                content_offset_x,
                content_offset_y,
                content_clip_w,
                content_clip_h);

        content_offset_x = client_geo.x;
        content_offset_y = client_geo.y;
        content_clip_w   = client_geo.width;
        content_clip_h   = client_geo.height;
        applyBoxToScene(geo);
    }

    // Force convergence on the tile's actual rect instead of accepting
    // whatever the client settles on. xdg-shell's configure size is a
    // hint the client is allowed to ignore or renegotiate away from
    // (min-size floors, its own layout constraints, a debounced resize
    // path, whatever) -- ack_configure only promises "I've applied state
    // up to this serial," not "I used the size you asked for." kitty and
    // wezterm never diverge here because a terminal's grid resize is
    // trivial and instant; Firefox is the routine offender because its
    // layout engine can decide -- even after acking -- to keep rendering
    // at a size that isn't the one requested. Re-issuing the same
    // request in that case isn't a race we're waiting out, it's a
    // negotiation we need to keep winning: keep re-asking on every commit
    // where the sizes disagree, gated only by whether a configure is
    // currently in flight (checked via wlroots' own serial bookkeeping,
    // not a flag we maintain ourselves) so this can't turn into a flood.
    if(mapped && !unmanaged && !is_floating) {
        int b          = server.lua_cfg.settings.border_px;
        int expected_w = std::max(1, geo.width - 2 * b);
        int expected_h = std::max(1, geo.height - 2 * b);
        int min_w      = xdg_toplevel->current.min_width;
        int min_h      = xdg_toplevel->current.min_height;
        if(min_w > 0) { expected_w = std::max(expected_w, min_w); }
        if(min_h > 0) { expected_h = std::max(expected_h, min_h); }

        // client_geo.width/height IS already the content size -- proven
        // by log, not assumed (see configureBackend): requesting a
        // content size with wlr_xdg_toplevel_set_size gets back a
        // client_geo.width/height that matches it exactly, while the
        // client pads its own raw surface buffer with its own shadow on
        // top independently. No conversion needed here; comparing
        // client_geo directly against expected_w/h is the correct,
        // apples-to-apples comparison. (An earlier version of this
        // subtracted 2*content_offset before comparing, on the wrong
        // assumption that client_geo was the full buffer size -- that
        // made this comparison permanently, spuriously fail every
        // commit, which combined with configureBackend's now-removed
        // double-padding to spiral the buffer size upward instead of
        // converging.)
        bool configure_in_flight = xdg_toplevel->base->scheduled_serial !=
                                   xdg_toplevel->base->current.configure_serial;
        bool size_matches =
            client_geo.width == expected_w && client_geo.height == expected_h;

        if(!size_matches && !configure_in_flight) {
            wlr_log(WLR_DEBUG,
                    "[uwuwm xdg] commit title='%s' size mismatch after ack "
                    "(client=%dx%d want=%dx%d) -- re-asserting configure",
                    title.c_str(),
                    client_geo.width,
                    client_geo.height,
                    expected_w,
                    expected_h);
            configureBackend(geo);
        }
    }
}

void XdgToplevel::configureBackend(const wlr_box& box) {
    int b = server.lua_cfg.settings.border_px;
    // wlr_xdg_toplevel_set_size requests a CONTENT size -- no shadow
    // compensation needed here. Proven by log, not assumed: requesting
    // 1900 (raw, before content_offset was even known) got back
    // client_geo.width == 1900 exactly, while the client's own raw
    // surface buffer came back at 1952 (1900 + 2*26, its own CSD shadow
    // padded on *top* of what we asked for). GTK/Gecko clients allocate
    // their own shadow margin around whatever content size they're
    // given; they don't expect the compositor to pre-pad the request.
    // The previous version of this function added 2*content_offset here
    // to "compensate" for the shadow -- since the client was already
    // adding its own shadow independently, that was double-counting:
    // each re-assert compounded another 52px onto the buffer instead of
    // converging (see handleCommit's log around the "size mismatch"
    // case, where requesting 1952 got a 2004-wide buffer back). Request
    // content size, plain and simple; content_offset_x/y is only needed
    // as the clip's origin (see contentClipBox) to skip past whatever
    // shadow the client independently decided to add.
    int w = box.width - 2 * b;
    int h = box.height - 2 * b;

    // xdg-shell's suggested size is a hint the client is free to ignore,
    // and Firefox/Zen's chrome genuinely can't render below some minimum
    // (toolbar + tab strip + hamburger + window controls all need room).
    // A client that declares that minimum via xdg_toplevel min_width/
    // min_height and gets asked for less anyway just renders at its own
    // minimum instead -- wider than the tile, wider than what
    // contentClipBox (which always hard-clips to the tile's own width)
    // will show. That mismatch is exactly what produces a hard-edge
    // cutoff through live chrome: we ask for a size the client can't
    // honor, then clip to the size we asked for instead of the size it
    // actually used. Clamp our own request up to the declared floor so
    // request and clip stay in agreement -- see contentClipBox for the
    // matching clamp on the other side.
    int min_w = xdg_toplevel->current.min_width;
    int min_h = xdg_toplevel->current.min_height;
    if(min_w > 0 && w < min_w) {
        wlr_log(WLR_DEBUG,
                "[uwuwm xdg] configureBackend title='%s' tile too narrow: "
                "wanted w=%d, client min_width=%d -- clamping up",
                title.c_str(),
                w,
                min_w);
        w = min_w;
    }
    if(min_h > 0 && h < min_h) {
        wlr_log(WLR_DEBUG,
                "[uwuwm xdg] configureBackend title='%s' tile too short: "
                "wanted h=%d, client min_height=%d -- clamping up",
                title.c_str(),
                h,
                min_h);
        h = min_h;
    }

    wlr_log(WLR_DEBUG,
            "[uwuwm xdg] configureBackend title='%s' target_box=%dx%d "
            "min=%dx%d -> requested_content_size=%dx%d",
            title.c_str(),
            box.width,
            box.height,
            min_w,
            min_h,
            std::max(1, w),
            std::max(1, h));
    wlr_xdg_toplevel_set_size(xdg_toplevel, std::max(1, w), std::max(1, h));
}

void XdgToplevel::activateBackend(bool activated) {
    wlr_xdg_toplevel_set_activated(xdg_toplevel, activated);
}

// contentClipBox hard-clips to the tile's own content box (border-inset
// target width/height) regardless of what size the client's surface
// buffer actually is. This is deliberate, not a fallback: GTK/Gecko
// clients (Firefox, Zen) allocate their own buffer around whatever
// content size they're given, padded with their own CSD shadow margin --
// confirmed directly by log, not assumed (see configureBackend). We
// don't need to know or care how big that padded buffer ends up being;
// pinning the clip to the target size is what keeps it from ever
// bleeding past the tile no matter what the client's buffer looks like.
// (wlr_scene_subsurface_tree_set_clip does a pure source-box crop --
// verified directly against wlroots' subsurface_tree.c -- there's no
// destination-size scaling involved, so this clip can only shrink what's
// visible, never stretch it.)
//
// geometry.x/y (the client's declared CSD shadow offset) is still needed
// as the clip's *origin*: wlr_scene_xdg_surface_create's internal
// auto-shift of the inner surface tree by (-geometry.x, -geometry.y),
// combined with this clip's (x, y) being interpreted in that same
// already-shifted tree's local space, is what lands the client's real
// visible chrome at content_tree's (0, 0) -- verified against
// xdg_shell.c and subsurface_tree.c directly, not assumed. The `w > 0`
// guard covers the degenerate zero-geometry case before a client's
// first set_window_geometry call.
wlr_box XdgToplevel::contentClipBox(const wlr_box& box) const {
    const wlr_box& g = xdg_toplevel->base->geometry;
    int            b = server.lua_cfg.settings.border_px;
    int            w = box.width - 2 * b;
    int            h = box.height - 2 * b;

    // Mirror configureBackend's min-size floor: if the tile is narrower
    // than the client's declared xdg_toplevel min_width/min_height, we
    // already asked the client to render at that larger minimum instead
    // of the tile's own size (see configureBackend). Clipping to the
    // tile's own (too-small) width here regardless would crop straight
    // through the chrome that request just told the client it's allowed
    // to draw -- request and clip have to agree on the floor, not just
    // the common case. min_width/min_height are already content-space
    // (xdg-shell defines them as the toplevel's content size, same units
    // wlr_xdg_toplevel_set_size takes) -- no shadow conversion needed,
    // same as configureBackend's request no longer needs one.
    int min_w = xdg_toplevel->current.min_width;
    int min_h = xdg_toplevel->current.min_height;
    if(min_w > 0) { w = std::max(w, min_w); }
    if(min_h > 0) { h = std::max(h, min_h); }

    wlr_log(WLR_DEBUG,
            "[uwuwm xdg] clip title='%s' box=%dx%d client_geo=%d,%d %dx%d "
            "min=%dx%d -> clip=%d,%d %dx%d",
            title.c_str(),
            box.width,
            box.height,
            g.x,
            g.y,
            g.width,
            g.height,
            min_w,
            min_h,
            g.x,
            g.y,
            w > 0 ? w : 0,
            h > 0 ? h : 0);
    return wlr_box{g.x, g.y, w > 0 ? w : 0, h > 0 ? h : 0};
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
    //
    // Guard on initialized: per the xdg-shell protocol, a client is
    // explicitly allowed to request maximized *during its initial setup,
    // before the first commit* ("Even without attaching a buffer the
    // compositor must respond to initial committed configuration, for
    // instance sending a configure event... if the client maximized its
    // surface during initialization" -- xdg_toplevel.set_maximized doc).
    // If that happens before XdgToplevel::handleCommit has processed
    // initial_commit for this surface, base->initialized will still be
    // false and schedule_configure() would hit the same
    // "surface->initialized" assertion decoration.cpp works around.
    // Dropping the request here is safe: XdgToplevel::handleCommit's own
    // initial_commit handling (which runs once, unconditionally, as part
    // of first-commit processing) will send the first real configure
    // shortly after regardless.
    if(!xdg_toplevel->base->initialized) { return; }
    wlr_xdg_surface_schedule_configure(xdg_toplevel->base);
}

void XdgToplevel::handleRequestFullscreen() {
    setFullscreen(xdg_toplevel->requested.fullscreen);
}

void XdgToplevel::handleSetTitle() {
    title = xdg_toplevel->title ? xdg_toplevel->title : "";
    syncForeignToplevelMeta();
    // Fired unconditionally, same as client::manage/unmanage -- a rule-only
    // rc.lua (no title_changed hook registered) pays nothing for this, and
    // one that wants live tab-title-in-titlebar behavior (browsers retitle
    // constantly) doesn't need to poll client.title from a timer to get it.
    server.lua_cfg.fireClientEvent("client::title_changed", this);
}

void XdgToplevel::handleSetAppId() {
    app_id = xdg_toplevel->app_id ? xdg_toplevel->app_id : "";
    syncForeignToplevelMeta();
}
