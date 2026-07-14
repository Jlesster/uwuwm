#pragma once

extern "C" {
struct lua_State;
}

#include "wallpaper.hpp"

#include <wayland-server-core.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Server;
struct View;
struct Output;
class LuaConfig;

// Runtime-tunable settings that used to be compile-time constants in
// config.hpp. Defaults below match the old cfg:: values exactly; rc.lua
// overrides them via uwuwm.set("name", value) calls handled in
// lua_config.cpp. Unlike config.hpp, changing these doesn't need a
// rebuild -- just a restart (uwuwm doesn't hot-reload, same as it didn't
// before).
struct RuntimeConfig {
    int    gap_px        = 8;
    int    border_px     = 2;
    double master_factor = 0.55;

    // See dwindle.hpp/dwindle.cpp. Matches Hyprland's dwindle:preserve_split
    // default (0/false): off means each split's orientation is recomputed
    // from its box's aspect ratio on every arrange, so
    // uwu.layout.dwindle.toggle_split()/uwu.layout.dwindle.rotate_split()
    // only have a lasting effect once this is turned on.
    bool dwindle_preserve_split = false;

    uint32_t border_color_active   = 0xcba6f7ff;  // catppuccin mauve
    uint32_t border_color_inactive = 0x313244ff;  // catppuccin surface0
    float    background_color[4]   = {0.067f, 0.071f, 0.090f, 1.0f};  // base

    // Static (not tweened) opacity applied to every unfocused view's
    // buffers + border alpha the moment focus leaves it -- View::setFocused
    // drives this through the same View::setOpacity/updateBorderColor path
    // ViewAnimation already uses for open/close tweens, so it composes with
    // those instead of fighting them. 1.0 (default) means "no dimming,"
    // matching every other opt-in visual knob's default. Clamped to
    // [0.05, 1.0] in the setter -- 0 would make an unfocused window
    // impossible to click back into visually.
    float inactive_opacity = 1.0f;

    int    repeat_rate_hz  = 25;
    int    repeat_delay_ms = 600;
    double cursor_size     = 24;

    // Empty (default) means "let wlr_xcursor_manager_create() fall back
    // to its own default, which reads the XCURSOR_THEME env var itself
    // if set" -- same "unset means defer to the underlying default"
    // convention every other string setting here follows (terminal/
    // launcher are the two other examples). Read once at startup by
    // input::setupCursor (input.cpp), same as cursor_size -- see that
    // call site's comment for why this doesn't need to be reload-aware.
    std::string cursor_theme;

    // Click-to-focus is the default (false) -- matches every other
    // uwuwm behavior default being "off until rc.lua asks for it".
    // Consulted once per pointer-motion event in processCursorMotion
    // (input.cpp); see that call site's comment for why it's safe to
    // check unconditionally there (no-ops via focusView's own
    // already-focused guard) rather than needing its own short-circuit.
    bool focus_follows_mouse = false;

    std::string terminal = "wezterm";
    std::string launcher = "fuzzel";

    // Animation: open (grow-in), close (shrink-out), and tag-switch slide
    // -- see view.hpp/View and Output::setTagset. Purely a scene-graph
    // tween (position + opacity), so it works identically for XDG and
    // X11 clients. NOTE: not yet exposed to rc.lua -- add uwuwm.set()
    // handling for these three in lua_config.cpp if you want them
    // user-tunable; for now they're compile-time defaults only.
    bool anim_enabled     = true;
    int  anim_duration_ms = 150;
    int  anim_slide_px    = 24;  // open/close vertical slide distance

    // How much smaller than its own final size a floating window's
    // pop-in animation starts from before growing/fading up to it (see
    // View::playFloatPopAnimation) -- 1.0 would be no visible grow at
    // all, values near 0 make for a very pronounced "zoom in" pop.
    float anim_pop_scale = 0.6f;

    // How big a window ends up, relative to its previous tiled content
    // size, when it's popped out of tiling into floating via
    // setFloating(true)/paw.client.toggle_floating()/`client.floating =
    // true` (see View::setFloating) -- NOT used for a window that's
    // floating from the moment it maps (dialogs/transients center at
    // their own natural size instead, see XdgToplevel::handleMap/
    // XWaylandView::handleMap). 1.0 would keep the old tile's full size;
    // smaller values are what make a popped-out window read as a small,
    // detached utility window rather than the same tile just unpinned.
    float float_size_ratio = 0.6f;
};

// One monitor-configuration rule, set via uwu.monitor.set(name, {...}) in
// rc.lua. `name` matches wlr_output->name (e.g. "DP-1", "eDP-1") exactly,
// or is the literal wildcard "*" to act as a fallback default applied to
// any output that doesn't have its own exact-name rule. Every field is
// optional -- has_* tells applyRule() whether to touch that piece of
// state at all, so e.g. uwu.monitor.set("DP-1", {scale = 2}) only ever
// changes scale and leaves position/mode/transform/enabled alone.
//
// transform is stored as the raw wl_output_transform enum value rather
// than pulling wlroots headers into this otherwise wlroots-free header;
// lua_config.cpp converts the rc.lua string ("normal", "90", "flipped",
// ...) to it, and output.cpp casts it back when building wlr_output_state.
struct MonitorRule {
    std::string name;

    bool has_position = false;
    int  x = 0, y = 0;

    bool has_mode = false;
    int  width = 0, height = 0;
    int  refresh_mhz = 0;  // milli-Hz, matches wlr_output_mode::refresh; 0
                           // means "any refresh rate at this resolution"

    bool  has_scale = false;
    float scale     = 1.0f;

    bool     has_transform = false;
    uint32_t transform     = 0;  // wl_output_transform value

    bool has_enabled = false;
    bool enabled     = true;

    bool has_adaptive_sync = false;
    bool adaptive_sync     = false;
};

// One uwu.input.set(match, {...}) rule, applied to matching libinput
// devices as they're plugged in (Server::input_devices / input.cpp's
// newInputDevice) and re-applied immediately to any already-connected
// match when the rule is (re)set -- same "store + apply-now-if-live"
// shape as MonitorRule/Output::applyRule above, just keyed on libinput
// config knobs instead of wlr_output state.
//
// `match` is one of:
//   - an exact wlr_input_device->name ("Logitech G Pro Wireless Gaming Mouse")
//   - a type selector: "type:touchpad" or "type:mouse" (pointer devices
//     only; keyboards don't have a libinput pointer-config surface, so
//     there's no "type:keyboard" here)
//   - the wildcard "*", matching any pointer device
// Precedence when several rules could match the same device: exact name >
// type selector > wildcard -- see findInputRule in input.cpp.
//
// Every field is optional (has_* guards it) exactly like MonitorRule, and
// each setter is additionally skipped at apply time if libinput reports
// the underlying device doesn't support that knob at all (e.g.
// natural-scroll on a device with no scroll capability) -- see
// applyInputRule's per-field libinput_device_config_*_is_available checks.
struct InputRule {
    std::string match;

    bool has_tap = false;
    bool tap     = false;

    bool has_tap_drag = false;
    bool tap_drag     = false;

    bool has_tap_drag_lock = false;
    bool tap_drag_lock     = false;

    bool has_natural_scroll = false;
    bool natural_scroll     = false;

    bool has_dwt = false;  // disable-while-typing
    bool dwt     = false;

    bool has_left_handed = false;
    bool left_handed     = false;

    bool has_middle_emulation = false;
    bool middle_emulation     = false;

    bool   has_accel_speed = false;
    double accel_speed     = 0.0;  // libinput range: -1.0 .. 1.0

    bool     has_accel_profile = false;
    uint32_t accel_profile     = 0;  // libinput_config_accel_profile

    bool     has_scroll_method = false;
    uint32_t scroll_method     = 0;  // libinput_config_scroll_method

    bool     has_click_method = false;
    uint32_t click_method     = 0;  // libinput_config_click_method

    bool     has_scroll_button = false;
    uint32_t scroll_button     = 0;  // linux/input-event-codes.h BTN_* value
};

// reference into the Lua registry for the callback to invoke. This is the
// AwesomeWM-equivalent of `awful.key` -- bindings are arbitrary Lua
// closures, not a closed Action enum, so rc.lua can do anything a real
// init script can do (compose actions, branch on state, etc.) rather than
// being limited to whatever cases config.hpp's old switch statement
// happened to implement.
struct LuaKeybind {
    uint32_t mods;
    uint32_t keysym;
    int      fn_ref;  // LUA_REGISTRYINDEX reference, valid for the
                      // lifetime of the owning LuaConfig's lua_State.
};

// uwu.mousebind(mods, button, fn) -- same idea as LuaKeybind above, but for
// a modifier+pointer-button combo instead of a modifier+key one. This is
// what floating-window drag-move/drag-resize is built on (sway/i3's
// floating_modifier equivalent): rc.lua binds e.g. {"mod"}+left to a
// closure that calls client:begin_move(), and {"mod"}+right to one that
// calls client:begin_resize(). Dispatched from the cursor-button handler
// in input.cpp against whichever view is under the cursor at press time,
// the same way LuaKeybind is dispatched against whichever keysym came in.
struct LuaMousebind {
    uint32_t mods;
    uint32_t button;  // linux/input-event-codes.h BTN_* value
    int      fn_ref;
};

// One uwu.hook(event, fn) registration. `id` is what uwu.unhook() takes;
// handed out sequentially by LuaConfig::next_hook_id rather than reusing
// the registry ref itself, so a hook can be unregistered from *inside*
// its own callback (removing an element from the vector being iterated
// mid-fire is exactly the bug that would cause) without that meaning
// anything about the registry ref's lifetime -- fire() copies the ids it's
// about to invoke before calling any of them, see lua_config.cpp.
struct LuaHook {
    int         id;
    std::string event;
    int         fn_ref;
};

// uwu.timer.set_interval(seconds, fn)/set_timeout(seconds, fn) -- the
// clock-tick primitive a bar widget needs (see bar.hpp) that nothing in
// uwuwm had before it. `source` is the real wl_event_source driving this;
// owning it in a unique_ptr-held struct (not a bare int id like LuaHook)
// is what lets ~LuaTimer clean it up via wl_event_source_remove just by
// going out of scope -- LuaConfig::timers below is keyed the same way
// hooks/hooks_id are, but holds unique_ptr<LuaTimer> instead of LuaTimer
// by value for exactly that reason.
struct LuaTimer {
    LuaConfig*       config = nullptr;
    wl_event_source* source = nullptr;
    // -2 is LUA_NOREF -- spelled out rather than pulling in <lua.h> for
    // one constant, matching this header's existing forward-declare-only
    // treatment of lua_State above.
    int fn_ref = -2;
    // Re-arm value in milliseconds; 0 means one-shot (set_timeout): the
    // callback removes this timer from LuaConfig::timers right after
    // firing once, instead of calling wl_event_source_timer_update
    // again. The *first* delay (same as interval_ms for set_interval,
    // independently given for set_timeout) is passed straight to
    // addTimer(), not stored here -- see its doc comment.
    int interval_ms = 0;
    int id          = 0;

    ~LuaTimer() {
        if(source) { wl_event_source_remove(source); }
    }
};

// Owns the Lua VM and exposes the `uwuwm` API table rc.lua programs
// against. This is uwuwm's equivalent of AwesomeWM's rc.lua runtime: one
// global table, plain functions and closures, loaded once at startup from
// a user script with a built-in fallback if none is found. See
// config/rc.lua for the shipped default/example script (also embedded as
// kDefaultConfig in lua_config.cpp).
class LuaConfig {
public:
    LuaConfig();
    ~LuaConfig();

    LuaConfig(const LuaConfig&)            = delete;
    LuaConfig& operator=(const LuaConfig&) = delete;

    // Must be called once, before load(). Stashes `&server` and `this` as
    // light userdata in the Lua registry so every exposed C function can
    // recover them without a global variable.
    void init(Server& server);

    // Loads, in order: $XDG_CONFIG_HOME/uwuwm/rc.lua, then
    // ~/.config/uwuwm/rc.lua, then the embedded default script if neither
    // exists. Returns false on a Lua syntax/runtime error in a *found*
    // user script -- callers should treat that as fatal the same way a
    // typo'd config.hpp simply wouldn't have compiled before.
    bool load();

    // Invokes the Lua function at `fn_ref` with no arguments. Logs (does
    // not throw/crash) on error -- one misbehaving keybind shouldn't take
    // the whole compositor down.
    void invoke(int fn_ref);

    // Same as invoke(), but calls fn_ref with a single argument: a fresh
    // Client userdata wrapping `view` (see checkClient()/pushClient() in
    // lua_config.cpp). Used for uwu.mousebind() closures, which -- unlike
    // plain keybinds -- are dispatched against whatever view happened to
    // be under the cursor at press time, so the closure needs a handle to
    // it (typically to call client:begin_move()/:begin_resize()).
    void invokeWithClient(int fn_ref, View* view);

    // Fully reloads rc.lua from scratch: clears keybinds/monitor_rules,
    // resets settings back to RuntimeConfig{} defaults, closes the
    // current Lua VM, and re-runs init()+load() on a brand new one --
    // so anything you deleted from rc.lua since the last (re)load
    // actually goes away instead of lingering, the same as a cold start
    // would produce. Only call this when execution is NOT currently
    // inside a Lua callback running on the *current* `L` (i.e. never
    // from inside invoke() itself) -- Server::reloadConfig()'s SIGHUP
    // handler is the intended caller for that reason. A reload
    // requested *from* a keybind goes through requestReload() instead.
    // Returns false (and leaves the previous config running, untouched)
    // if the new script fails to load -- see load()'s doc comment.
    bool reload();

    // Defers a reload until the current invoke() call has returned to
    // plain C++. uwu.reload() calls this rather than doing the reload
    // itself, because invoke() is the one calling us, and closing/
    // replacing `L` while a lua_pcall is still running *on* it (as it is
    // for the entire duration of the keybind callback that called
    // uwu.reload()) is undefined behaviour. invoke() checks this flag
    // right after its lua_pcall returns and performs the real reload
    // then, once the Lua call stack is empty.
    void requestReload() { reload_pending = true; }

    // Fires every uwu.hook("client::manage"/"unmanage"/"focus"/"unfocus"/
    // "fullscreen", fn) callback registered for `event`, passing a fresh
    // Client userdata wrapping `view` as the sole argument. Safe to call
    // with a view whose lifetime doesn't outlast the call (it does here,
    // in every call site) -- the userdata only stores the raw pointer,
    // methods/property reads re-validate it's still in server.views
    // before every use, see checkClient() in lua_config.cpp.
    void fireClientEvent(const std::string& event, View* view);

    // Fires uwu.hook("output::connect"/"disconnect", fn), passing the
    // output's name (a string) rather than a live object -- there's no
    // uwu.monitor object type yet, only the snapshot uwu.monitor.list()
    // already returns, and an about-to-be-destroyed Output* isn't safe to
    // wrap the same way Client is.
    void fireOutputEvent(const std::string& event, Output* output);

    // Fires uwu.hook("tag::change", fn) with (monitor_name, new_tagset)
    // -- called once from Output::setTagset, which is the single place
    // any output's visible tagset actually changes (see setTagset's own
    // comment for why l_view_tag/l_toggle_tag route through it too).
    void fireTagChange(Output* output, uint32_t new_tagset);

    // uwu.hook(event, fn) -> id / uwu.unhook(id). Implemented as thin
    // wrappers so lua_config.cpp's l_hook/l_unhook can stay plain
    // lua_CFunctions (no `this` available) the same way every other
    // uwu.* function already does via getConfig(L).
    int  addHook(const std::string& event, int fn_ref);
    void removeHook(int id);

    RuntimeConfig                       settings;
    std::multimap<uint32_t, LuaKeybind> keybinds;
    std::vector<LuaMousebind>           mousebinds;

    // Registered by uwu.hook(), consumed by fire*Event above. Cleared and
    // repopulated on every reload() exactly like keybinds/monitor_rules --
    // a hook from a deleted rc.lua block shouldn't linger after a reload
    // any more than a deleted keybind does.
    std::vector<LuaHook> hooks;
    int                  next_hook_id = 1;

    // uwu.timer.set_interval/set_timeout register here; l_timer_cancel
    // and reload() both remove through cancelTimer()/direct erase --
    // see LuaTimer's own comment for why this is unique_ptr-keyed rather
    // than a flat vector like hooks.
    std::unordered_map<int, std::unique_ptr<LuaTimer>> timers;
    int                                                next_timer_id = 1;

    // Registers a timer that calls fn_ref (already luaL_ref'd by the
    // caller -- l_timer_set_interval/set_timeout in lua_config.cpp) first
    // after delay_ms, then (if interval_ms > 0) every interval_ms after
    // that indefinitely; interval_ms == 0 means fire once, at delay_ms,
    // then self-remove. set_interval passes the same value for both;
    // set_timeout passes interval_ms = 0. Returns the id
    // uwu.timer.cancel() takes.
    int addTimer(int fn_ref, int delay_ms, int interval_ms);

    // uwu.timer.cancel(id) -- luaL_unref's fn_ref (the timer is being
    // individually removed while L stays alive, unlike a reload where
    // the whole L closing makes that unnecessary -- see removeHook's
    // identical situation) and erases the entry, whose destructor tears
    // down the wl_event_source.
    void cancelTimer(int id);

    // Populated by uwu.monitor.set() calls in rc.lua (and by any later
    // runtime uwu.monitor.set() calls from keybinds/scripts). Consulted by
    // Output's constructor when a monitor is (re)plugged in, and applied
    // immediately by l_monitor_set if the named monitor is already
    // connected. See lua_config.cpp for the exact-name-wins-over-wildcard
    // matching rule.
    std::vector<MonitorRule> monitor_rules;

    // Populated by uwu.input.set() calls in rc.lua (and by any later
    // runtime uwu.input.set() calls from keybinds/scripts). Consulted by
    // input::newInputDevice when a pointer device is (re)plugged in, and
    // applied immediately by l_input_set to every already-connected
    // matching device -- see InputRule's doc comment above for the
    // exact-name > type-selector > wildcard matching rule.
    std::vector<InputRule> input_rules;

    // Populated by uwu.wallpaper.set() calls in rc.lua (and by any later
    // runtime uwu.wallpaper.set()/clear() calls from keybinds/scripts).
    // Unlike monitor_rules (only exact-name matches are re-applied live;
    // a "*" wildcard needs a reload to reach already-connected outputs --
    // see findMonitorRule's doc comment), l_wallpaper_set/l_wallpaper_clear
    // re-sweep every currently-connected output through findWallpaperRule
    // immediately, since "*" is the common case here (one wallpaper for
    // every monitor) rather than the exception.
    std::vector<WallpaperRule> wallpaper_rules;

    bool reload_pending = false;

    lua_State* L = nullptr;

private:
    // Stashed by init() so reload() can call init(*server_) again without
    // requiring every caller to have a Server& handy -- Server::setup() is
    // the only site that calls init() directly, but reload() needs
    // to re-call it internally each time.
    Server* server_ = nullptr;

    // Calls whatever's on top of the Lua stack (pushed by the caller)
    // with `nargs` arguments already pushed above it, under a wall-clock
    // timeout and a hook-recursion depth cap -- see lua_config.cpp for
    // why this exists and doesn't just wrap lua_pcall directly. Used by
    // both invoke() (keybinds) and fire*Event (hooks/rules) so every path
    // that runs arbitrary rc.lua code on the compositor's single event-
    // loop thread gets the same guarantee: a broken callback logs an
    // error and returns, it never hangs or crashes the session.
    bool guardedCall(int nargs);

    // Incremented for the duration of every guardedCall, so a hook whose
    // callback (directly, or via another hook it triggers) causes another
    // fire*Event call can't recurse the C++ call stack (and, with it, the
    // Lua one) without bound. Mirrors Hyprland's Lua backend, which caps
    // dispatch depth at 5 for the same reason; picked 8 here since rule
    // chains (uwu.rule -> uwu.hook("client::manage") -> a setter that
    // itself fires no further hooks today) are shallower than layout
    // callbacks, but there's real headroom either way -- this is a
    // last-resort circuit breaker, not a tuned budget.
    int hook_dispatch_depth = 0;
};
