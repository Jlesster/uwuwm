#pragma once

extern "C" {
struct lua_State;
}

#include <cstdint>
#include <string>
#include <vector>
#include <map>

class Server;

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

    uint32_t border_color_active   = 0xcba6f7ff;  // catppuccin mauve
    uint32_t border_color_inactive = 0x313244ff;  // catppuccin surface0
    float    background_color[4]   = {0.067f, 0.071f, 0.090f, 1.0f};  // base

    int    repeat_rate_hz  = 25;
    int    repeat_delay_ms = 600;
    double cursor_size     = 24;

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

    RuntimeConfig           settings;
    std::multimap<uint32_t, LuaKeybind> keybinds;

    // Populated by uwu.monitor.set() calls in rc.lua (and by any later
    // runtime uwu.monitor.set() calls from keybinds/scripts). Consulted by
    // Output's constructor when a monitor is (re)plugged in, and applied
    // immediately by l_monitor_set if the named monitor is already
    // connected. See lua_config.cpp for the exact-name-wins-over-wildcard
    // matching rule.
    std::vector<MonitorRule> monitor_rules;

    bool reload_pending = false;

    lua_State* L = nullptr;

private:
    // Stashed by init() so reload() can call init(*server_) again without
    // requiring every caller to have a Server& handy -- Server::setup() is
    // the only site that calls init() directly, but reload() needs
    // to re-call it internally each time.
    Server* server_ = nullptr;
};
