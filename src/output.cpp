#include "output.hpp"

extern "C" {
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/log.h>
}

#include "dwindle.hpp"
#include "idle.hpp"
#include "layershell.hpp"
#include "layout.hpp"
#include "server.hpp"
#include "session_lock.hpp"
#include "view.hpp"

#include <algorithm>
#include <ctime>
#include <vector>

namespace {

// Picks the best mode on `output` matching (width, height), and matching
// refresh_mhz too if it's nonzero (0 means "any refresh at this
// resolution" -- picks the highest-refresh match, same tie-break
// wlr_output_preferred_mode uses internally). Returns nullptr if nothing
// matches, in which case callers fall back to wlr_output_preferred_mode.
//
// If an exact refresh-match fails, falls back to resolution-only matching
// and picks the highest refresh at that resolution (some monitors report
// mode refresh values off by 1 mHz from what uwu.monitor.set gets).
wlr_output_mode*
findMode(wlr_output* output, int width, int height, int refresh_mhz) {
    wlr_output_mode* best     = nullptr;
    wlr_output_mode* fallback = nullptr;
    wlr_output_mode* mode;
    wl_list_for_each(mode, &output->modes, link) {
        if(mode->width != width || mode->height != height) { continue; }
        if(refresh_mhz > 0 && mode->refresh == refresh_mhz) { best = mode; }
        if(!best && (!fallback || mode->refresh > fallback->refresh)) {
            fallback = mode;
        }
    }
    if(!best && refresh_mhz > 0 && fallback) {
        wlr_log(WLR_DEBUG,
                "monitor %s: refresh %d mHz not available at %dx%d, "
                "using %d mHz instead",
                output->name,
                refresh_mhz,
                width,
                height,
                fallback->refresh);
        return fallback;
    }
    return best ? best : fallback;
}

// True only when this output's sole reasonable tearing candidate --
// the currently-focused view, mapped, fullscreen, actually on this
// output -- has hinted (via wp_tearing_control_v1) that its content is
// fine being shown with tearing. Deliberately never true for anything
// else: a windowed game, a background client that happens to hold a
// tearing object, or a fullscreen-but-unfocused view (e.g. a fullscreen
// video paused behind a focused launcher) should never get to tear the
// whole output out from under whatever else is on screen.
//
// Checked fresh every frame rather than cached on the Output like
// adaptive_sync_on is -- unlike setAdaptiveSync, which does its own
// dedicated wlr_output_commit_state to avoid an extra desktop-content
// resync flicker, this rides along on the commit handleFrame() was
// already about to do, so there's no extra-commit cost to avoid.
bool wantsTearing(Output& out) {
    Server& server = out.server;
    if(!server.tearing_control_manager) { return false; }

    View* focused = server.focused_view;
    if(!focused || !focused->mapped || focused->output != &out) {
        return false;
    }
    if(!focused->is_fullscreen) { return false; }

    wlr_surface* surface = focused->wlrSurface();
    if(!surface) { return false; }

    return wlr_tearing_control_manager_v1_surface_hint_from_surface(
               server.tearing_control_manager, surface) ==
           WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}

// True when the currently-focused view on this output has hinted (via
// wp_content_type_v1) that its surface is CONTENT_TYPE_GAME. Used by
// Output::updateAdaptiveSync as a second, protocol-level path into
// adaptive sync alongside the pre-existing "exactly one visible window,
// and it's focused" heuristic below -- a windowed game sharing a tag with
// e.g. a chat client shouldn't lose VRR just because it isn't alone.
// Deliberately only ever looks at the focused view, same reasoning
// wantsTearing above uses for fullscreen: an unfocused/backgrounded
// client hinting content-type=game shouldn't be able to pull every other
// output-bound client into tearing-prone adaptive-sync territory on its
// behalf.
bool focusedWantsGameSync(Output& out) {
    Server& server = out.server;
    if(!server.content_type_manager) { return false; }

    View* focused = server.focused_view;
    if(!focused || !focused->mapped || focused->output != &out) {
        return false;
    }

    wlr_surface* surface = focused->wlrSurface();
    if(!surface) { return false; }

    return wlr_surface_get_content_type_v1(server.content_type_manager,
                                           surface) ==
           WP_CONTENT_TYPE_V1_TYPE_GAME;
}

}  // namespace

// Not in the anonymous namespace above (unlike findMode) because
// Server::reloadConfig() also calls this, to re-sweep every connected
// output against whatever rc.lua's uwu.monitor.set() calls just produced
// -- an exact-name rule already gets applied live by l_monitor_set while
// the script runs, but a "*" wildcard rule only affects monitors that
// appear *after* it, so a hot reload needs this extra pass to also catch
// already-connected ones. See output.hpp for the declaration.
const MonitorRule* findMonitorRule(Server& server, const char* name) {
    const MonitorRule* wildcard = nullptr;
    for(const auto& rule : server.lua_cfg.monitor_rules) {
        if(rule.name == name) { return &rule; }
        if(rule.name == "*") { wildcard = &rule; }
    }
    return wildcard;
}

Output::Output(Server& server, struct wlr_output* wlr_output)
    : server(server), wlr_output(wlr_output) {
    wlr_output->data = this;
    master_factor    = server.lua_cfg.settings.master_factor;

    wlr_output_init_render(wlr_output, server.allocator, server.renderer);

    // Enable the output with its preferred mode, unless rc.lua configured
    // something else for it via uwu.monitor.set(). Nested-mode (developing
    // inside an existing Hyprland session) ignores most of this and just
    // gives you a window at whatever size the host compositor picks, which
    // is exactly what you want while iterating.
    const MonitorRule* rule    = findMonitorRule(server, wlr_output->name);
    bool               enabled = !rule || !rule->has_enabled || rule->enabled;

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, enabled);
    if(enabled) {
        wlr_output_mode* mode = nullptr;
        if(rule && rule->has_mode) {
            mode = findMode(
                wlr_output, rule->width, rule->height, rule->refresh_mhz);
            if(!mode) {
                wlr_log(WLR_ERROR,
                        "monitor %s: no mode matching %dx%d @ %d Hz, "
                        "falling back to preferred",
                        wlr_output->name,
                        rule->width,
                        rule->height,
                        rule->refresh_mhz / 1000);
            }
        }
        if(!mode) { mode = wlr_output_preferred_mode(wlr_output); }
        if(mode) { wlr_output_state_set_mode(&state, mode); }

        if(rule && rule->has_scale) {
            wlr_output_state_set_scale(&state, rule->scale);
        }
        if(rule && rule->has_transform) {
            wlr_output_state_set_transform(
                &state, static_cast<wl_output_transform>(rule->transform));
        }
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    // Position: explicit uwu.monitor.set({x=, y=}) wins, otherwise fall
    // back to the old auto-placement behaviour. Disabled outputs (rule
    // says enabled = false) are left out of the layout entirely.
    wlr_output_layout_output* l_output = nullptr;
    if(enabled) {
        l_output =
            (rule && rule->has_position)
                ? wlr_output_layout_add(
                      server.output_layout, wlr_output, rule->x, rule->y)
                : wlr_output_layout_add_auto(server.output_layout, wlr_output);
    }

    scene_output = wlr_scene_output_create(server.scene, wlr_output);
    if(l_output) {
        wlr_scene_output_layout_add_output(
            server.scene_layout, l_output, scene_output);
    }

    updateLayoutBox();

    background_rect = wlr_scene_rect_create(
        server.layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
        layout_box.width,
        layout_box.height,
        server.lua_cfg.settings.background_color);
    wlr_scene_node_set_position(
        &background_rect->node, layout_box.x, layout_box.y);

    // Hot path: raw notify, no Listener<T> wrapper. See listener.hpp's
    // header comment and the doc's §2.1 for why this one callback is
    // different from every other one in the codebase.
    frame_listener.notify = &Output::frameTrampoline;
    wl_signal_add(&wlr_output->events.frame, &frame_listener);

    request_state.connect(&wlr_output->events.request_state,
                          [this](wlr_output_event_request_state* event) {
                              handleRequestState(event);
                          });

    destroy.connect(&wlr_output->events.destroy,
                    [this](void*) { handleDestroy(); });

    if(rule && rule->has_adaptive_sync) {
        setAdaptiveSync(rule->adaptive_sync);
    }

    server.arrangeAllOutputs();
    if(!server.focused_output) { server.focused_output = this; }

    // Fires for every output, whether it came up at compositor startup
    // or hotplugged later. The spec is silent on the distinction and
    // "fires for every output" matches what every other compositor's
    // analogous output::connect does.
    server.lua_cfg.fireOutputEvent("output::connect", this);
}

Output::~Output() {
    wl_list_remove(&frame_listener.link);

    // Unlike background_rect (never explicitly destroyed -- it lives
    // under a shared layer_tree that outlives any one Output, and letting
    // it go stale silently is harmless), a black lock backdrop surviving
    // an output unplug mid-lock would be actively misleading: nothing
    // else will ever destroy this one, since it isn't parented under
    // anything this Output owns end-to-end. SessionLock::~SessionLock
    // also nulls this field out on a clean unlock, so this is purely the
    // mid-lock-unplug case.
    if(lock_backdrop) { wlr_scene_node_destroy(&lock_backdrop->node); }

    // lock_surface, if set, is owned by SessionLock::surfaces, not here --
    // this Output going away doesn't own it and must not delete it.
    // Whether wlroots tears down the underlying wlr_session_lock_surface_
    // v1 synchronously when its output disappears (letting
    // SessionLockSurface::handleDestroy clean itself up the normal way)
    // is unverified without a live wlroots-0.20 install; treat this as
    // the one spot to check first if unplugging a monitor mid-lock is
    // ever observed to crash or leave a dangling surface.
}

void Output::updateLayoutBox() {
    wlr_output_layout_get_box(server.output_layout, wlr_output, &layout_box);
    usable_box = layout_box;

    if(background_rect) {
        wlr_scene_rect_set_size(
            background_rect, layout_box.width, layout_box.height);
        wlr_scene_node_set_position(
            &background_rect->node, layout_box.x, layout_box.y);
    }

    // A resolution/scale/transform change (uwu.monitor.set via rc.lua
    // hot-reload, or a monitor renegotiating its mode) must keep the
    // lock backdrop covering the whole output -- an unresized black rect
    // next to a resized real output would leave a gap that isn't black,
    // which defeats the entire point.
    if(lock_backdrop) {
        wlr_scene_rect_set_size(
            lock_backdrop, layout_box.width, layout_box.height);
        wlr_scene_node_set_position(
            &lock_backdrop->node, layout_box.x, layout_box.y);
    }
    if(lock_surface) {
        wlr_scene_node_set_position(
            &lock_surface->scene_tree->node, layout_box.x, layout_box.y);
        // Deliberately not re-calling wlr_session_lock_surface_v1_configure
        // here -- the client would need to redraw at the new size, which
        // is exactly the kind of live-resize-while-locked edge case not
        // worth chasing for a first pass. Repositioning at least keeps a
        // stale-sized surface in the right corner rather than drifting.
    }
}

void Output::updateBackground() {
    wlr_scene_rect_set_color(background_rect,
                             server.lua_cfg.settings.background_color);
}

void Output::frameTrampoline(wl_listener* listener, void* /*data*/) {
    Output* self = wl_container_of(listener, self, frame_listener);
    self->handleFrame();
}

void Output::handleFrame() {
    if(!server.session_active) { return; }
    server.tickAnimations();
    wlr_scene_output* so = wlr_scene_get_scene_output(server.scene, wlr_output);
    if(!so) { return; }

    // Building the state ourselves (instead of the one-call
    // wlr_scene_output_commit helper) is only so we get a wlr_output_state
    // to poke tearing_page_flip into before committing -- everything else
    // here is exactly what that helper does internally.
    wlr_output_state state;
    wlr_output_state_init(&state);
    if(!wlr_scene_output_build_state(so, &state, nullptr)) {
        wlr_output_state_finish(&state);
        return;
    }

    state.tearing_page_flip = wantsTearing(*this);

    bool ok = wlr_output_commit_state(wlr_output, &state);
    if(!ok && state.tearing_page_flip) {
        // Per the protocol: the backend may reject a tearing page-flip
        // (some drivers/multi-GPU setups can't always honor it) even
        // when everything else about the state was fine. Fall back to a
        // regular page-flip right away rather than dropping the frame
        // and waiting on something else to reschedule -- there's no
        // "tearing didn't work, try again next frame" flag to thread
        // through session_active_listener, and this output is otherwise
        // healthy.
        state.tearing_page_flip = false;
        ok                      = wlr_output_commit_state(wlr_output, &state);
    }
    wlr_output_state_finish(&state);

    if(!ok) {
        // Commit failed for some other reason (e.g. no DRM master yet).
        // Don't reschedule -- that's what turns one failure into a tight
        // loop. Rely on session_active_listener to kick scheduling again
        // once the session is genuinely usable.
        return;
    }

    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(so, &now);
    wlr_output_schedule_frame(wlr_output);
}

void Output::handleRequestState(wlr_output_event_request_state* event) {
    // Some backends (the headless/nested wayland one in particular) ask us
    // to apply a state change driven by the host compositor, e.g. a resize
    // of the nested window. Just apply it.
    wlr_output_commit_state(wlr_output, event->state);
    updateLayoutBox();
    server.arrangeAllOutputs();
}

void Output::handleDestroy() {
    // Previously this just set every toplevel's `output` to nullptr and
    // never reassigned them anywhere -- if you unplugged a monitor (or it
    // went to sleep and dropped, or the nested dev window closed) any
    // window that had been on it became permanently unfocusable and
    // untileable: focus-next/prev filter by `t->output == out`, and
    // layout::arrange only ever looks at views matching the output
    // it's arranging, so an orphaned toplevel with output == nullptr just
    // never gets touched by anything again. With multiple outputs in play
    // this is the common case, not an edge case, so it needs a real
    // migration path: move every orphaned toplevel onto whatever output
    // is about to become focused_output and re-tile it there.
    Output* replacement = nullptr;
    for(auto& o : server.outputs) {
        if(o.get() != this) {
            replacement = o.get();
            break;
        }
    }

    for(auto& v : server.views) {
        if(v->output != this) { continue; }
        v->output = replacement;
        if(replacement) {
            // Re-tile rather than trust floating geometry computed for a
            // monitor that no longer exists.
            v->is_floating = false;
            v->setTags(replacement->tagset);
        }
    }

    if(server.focused_output == this) { server.focused_output = replacement; }
    if(server.focused_view && server.focused_view->output == nullptr) {
        server.focused_view = nullptr;
    }

    // Fires before remove_if so the hook can still inspect this output
    // via server.outputs / uwu.monitor.list(). wlr_output->name is still
    // valid here -- wlroots frees it as part of its own destroy
    // signal emission that brought us here, which completes after
    // this listener returns.
    server.lua_cfg.fireOutputEvent("output::disconnect", this);

    server.outputs.remove_if(
        [this](const std::unique_ptr<Output>& o) { return o.get() == this; });

    if(replacement) { arrangeLayers(*replacement); }
}

void Output::setAdaptiveSync(bool enabled) {
    if(adaptive_sync_on == enabled) { return; }

    // §5.2: driven by Output::updateAdaptiveSync's sole-visible-window/
    // game-content-type heuristic (called from layout::arrange on every
    // arrange, and directly from View::setFullscreen), never left on
    // globally -- avoids the desktop-content resync flicker the doc
    // calls out. Guarded by the early-return above so a no-op toggle
    // never pays for a state commit.
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_adaptive_sync_enabled(&state, enabled);
    if(wlr_output_commit_state(wlr_output, &state)) {
        adaptive_sync_on = enabled;
    }
    wlr_output_state_finish(&state);
}

void Output::updateAdaptiveSync() {
    View* only  = nullptr;
    int   count = 0;

    for(auto& v : server.views) {
        if(!v->mapped || v->output != this || v->is_minimized) { continue; }
        if((v->tags & tagset) == 0) { continue; }
        if(++count > 1) { break; }
        only = v.get();
    }

    bool sync_worthy = count == 1 && server.focused_view == only;
    if(!sync_worthy) { sync_worthy = focusedWantsGameSync(*this); }

    setAdaptiveSync(sync_worthy);
}

void Output::setTagset(uint32_t new_tagset) {
    if(new_tagset == 0 || new_tagset == tagset) { return; }

    if(!server.lua_cfg.settings.anim_enabled) {
        tagset = new_tagset;
        layout::arrange(*this);
        idle::updateInhibitState(server);
        server.lua_cfg.fireTagChange(this, new_tagset);
        return;
    }

    uint32_t old_tagset = tagset;
    int      dx         = usable_box.width + server.lua_cfg.settings.gap_px;

    // Snapshot which currently-visible views are leaving *before*
    // arrange() below reassigns `tagset` -- arrange() only knows about
    // the new tagset once we flip it, so this is the only point we can
    // still tell "was visible, won't be after" apart from "was already
    // hidden".
    std::vector<View*> leaving;
    for(auto& v : server.views) {
        if(v->mapped && v->output == this && (v->tags & old_tagset) &&
           !(v->tags & new_tagset)) {
            leaving.push_back(v.get());
        }
    }

    tagset = new_tagset;
    layout::arrange(*this);  // computes + applies final geo for every
                             // *entering* tiled view, and disables every
                             // view that matches neither tagset

    for(View* v : leaving) { v->playSlideOut(-dx, 0); }
    for(auto& v : server.views) {
        if(v->mapped && v->output == this && (v->tags & new_tagset) &&
           !(v->tags & old_tagset)) {
            v->playSlideIn(dx, 0);
        }
    }
    idle::updateInhibitState(server);
    server.lua_cfg.fireTagChange(this, new_tagset);
}

void Output::applyRule(const MonitorRule& rule) {
    bool enabled = !rule.has_enabled || rule.enabled;

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, enabled);
    if(enabled) {
        wlr_output_mode* mode = nullptr;
        if(rule.has_mode) {
            mode =
                findMode(wlr_output, rule.width, rule.height, rule.refresh_mhz);
            if(!mode) {
                wlr_log(WLR_ERROR,
                        "monitor %s: no mode matching %dx%d @ %d Hz, "
                        "keeping current mode",
                        wlr_output->name,
                        rule.width,
                        rule.height,
                        rule.refresh_mhz / 1000);
            }
        }
        if(mode) { wlr_output_state_set_mode(&state, mode); }
        if(rule.has_scale) { wlr_output_state_set_scale(&state, rule.scale); }
        if(rule.has_transform) {
            wlr_output_state_set_transform(
                &state, static_cast<wl_output_transform>(rule.transform));
        }
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    if(enabled) {
        // wlr_output_layout_add repositions an already-added output as
        // well as adding a new one, so this works whether the output
        // started out auto-placed or explicitly positioned.
        if(rule.has_position) {
            wlr_output_layout_add(
                server.output_layout, wlr_output, rule.x, rule.y);
        } else if(!wlr_output_layout_get(server.output_layout, wlr_output)) {
            // Only auto-place if it isn't in the layout at all yet (e.g.
            // it was just re-enabled after being disabled with no
            // explicit position ever set).
            wlr_output_layout_add_auto(server.output_layout, wlr_output);
        }
        updateLayoutBox();
    }

    if(rule.has_adaptive_sync) { setAdaptiveSync(rule.adaptive_sync); }

    server.arrangeAllOutputs();
}
