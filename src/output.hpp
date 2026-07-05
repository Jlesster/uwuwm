#pragma once

extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
}

#include "layout.hpp"
#include "listener.hpp"

#include <cstdint>
#include <list>
#include <memory>

class Server;
struct MonitorRule;
struct SessionLockSurface;
namespace dwindle {
struct DwindleNode;
}

// One physical (or nested-window, in dev mode) display. Equivalent to dwl's
// `Monitor`.
struct Output {
    Output(Server& server, wlr_output* wlr_output);
    ~Output();

    Server&           server;
    wlr_output*       wlr_output;
    wlr_scene_output* scene_output    = nullptr;
    wlr_scene_rect*   background_rect = nullptr;

    // Set by SessionLock while server.session_locked -- see
    // session_lock.{hpp,cpp}. Both non-owning except lock_backdrop, which
    // this Output explicitly destroys in its own destructor (unlike
    // background_rect) since a stray full-black rect surviving an output
    // unplug mid-lock is a far worse failure mode than a stray
    // wallpaper-colored one. lock_surface is never owned here -- it's
    // owned by SessionLock::surfaces -- this is just how SessionLock
    // finds "does this output already have one" without its own
    // per-output map.
    wlr_scene_rect*     lock_backdrop = nullptr;
    SessionLockSurface* lock_surface  = nullptr;

    wlr_box
        layout_box{};  // position+size in output-layout (global) coordinates
    wlr_box usable_box{};  // layout_box minus space reserved by layer-shell

    uint32_t tagset = 1;  // bitmask of currently-viewed tags (bit 0 = tag 1)
    bool     fullscreen_active =
        false;  // is the focused client on this output fullscreened
    bool   adaptive_sync_on = false;
    double master_factor;  // fraction of usable_box width given to the master
                           // column

    // Which tiling algorithm layout::arrange() uses on this output. See
    // layout.hpp's LayoutMode and dwindle.hpp.
    layout::LayoutMode layout_mode = layout::LayoutMode::Dwindle;

    // Persistent BSP tree backing the dwindle layout, valid (and non-null
    // once at least one window has ever been tiled) only when layout_mode
    // == Dwindle -- untouched, and left empty, under master-stack. Owned
    // exclusively here; see dwindle.hpp's DwindleNode for why this is a
    // unique_ptr to a type this header only forward-declares.
    std::unique_ptr<dwindle::DwindleNode> dwindle_root;

    // Layer-shell surfaces anchored to this output, one list per
    // zwlr_layer_shell_v1_layer value. Kept here (not just in the scene
    // tree) because arrange_layers needs to reason about exclusive zones
    // per-output, not just render order.
    std::list<struct LayerSurface*> layers[4];

    // Animated tag/workspace switch: enables both the outgoing and
    // incoming tagset's views simultaneously and slides them past each
    // other (View::playSlideOut/playSlideIn), landing on the normal
    // single-tagset state. rc.lua's tag-switching binding should call
    // this instead of assigning `output.tagset` directly -- direct
    // assignment still works, it just skips the animation.
    void setTagset(uint32_t new_tagset);

    void setAdaptiveSync(bool enabled);
    void updateLayoutBox();
    void updateBackground();
    void updateAdaptiveSync();

    // Applies a monitor-config rule (position/mode/scale/transform/
    // enabled/adaptive_sync) to this already-connected output. Used both
    // by uwu.monitor.set() when it targets a currently-plugged-in
    // monitor, and internally by the constructor (via the free function
    // findMonitorRule in output.cpp) to apply any rule matching this
    // output the first time it appears. Fields left unset on `rule` are
    // left untouched on the output.
    void applyRule(const MonitorRule& rule);

private:
    void handleFrame();
    void handleRequestState(wlr_output_event_request_state* event);
    void handleDestroy();

    wl_listener frame_listener{};
    static void frameTrampoline(wl_listener* listener, void* data);

    Listener<wlr_output_event_request_state> request_state;
    VoidListener                             destroy;
};

// Finds the monitor rule that applies to an output named `name`: an
// exact-name match wins outright, otherwise the last "*" wildcard rule
// (if any) is used as a fallback default. Returns nullptr if no rule
// applies (rc.lua never called uwu.monitor.set for this output or "*").
// Defined in output.cpp; used there by Output's constructor for
// newly-appeared monitors, and by Server::reloadConfig() (server.cpp) to
// re-sweep already-connected monitors after a hot reload.
const MonitorRule* findMonitorRule(Server& server, const char* name);
