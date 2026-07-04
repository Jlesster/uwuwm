#include "output.hpp"

extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
}

#include "layershell.hpp"
#include "layout.hpp"
#include "server.hpp"
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
}

Output::~Output() { wl_list_remove(&frame_listener.link); }

void Output::updateLayoutBox() {
    wlr_output_layout_get_box(server.output_layout, wlr_output, &layout_box);
    usable_box = layout_box;

    if(background_rect) {
        wlr_scene_rect_set_size(
            background_rect, layout_box.width, layout_box.height);
        wlr_scene_node_set_position(
            &background_rect->node, layout_box.x, layout_box.y);
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

    if(!wlr_scene_output_commit(so, nullptr)) {
        // Commit failed (e.g. no DRM master yet). Don't reschedule --
        // that's what turns one failure into a tight loop. Rely on
        // session_active_listener to kick scheduling again once the
        // session is genuinely usable.
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

    server.outputs.remove_if(
        [this](const std::unique_ptr<Output>& o) { return o.get() == this; });

    if(replacement) { arrangeLayers(*replacement); }
}

void Output::setAdaptiveSync(bool enabled) {
    if(adaptive_sync_on == enabled) { return; }

    // §5.2: only ever toggled on fullscreen enter/exit (see
    // View::setFullscreen), never left on globally -- avoids the
    // desktop-content resync flicker the doc calls out.
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
        if(!v->mapped || v->output != this) { continue; }
        if((v->tags & tagset) == 0) { continue; }
        if(++count > 1) { break; }
        only = v.get();
    }
    setAdaptiveSync(count == 1 && server.focused_view == only);
}

void Output::setTagset(uint32_t new_tagset) {
    if(new_tagset == 0 || new_tagset == tagset) { return; }

    if(!server.lua_cfg.settings.anim_enabled) {
        tagset = new_tagset;
        layout::arrange(*this);
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
