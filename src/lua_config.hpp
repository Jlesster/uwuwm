#pragma once

extern "C" {
struct lua_State;
}

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Server;
struct View;
struct Output;

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

    // Registered by uwu.hook(), consumed by fire*Event above. Cleared and
    // repopulated on every reload() exactly like keybinds/monitor_rules --
    // a hook from a deleted rc.lua block shouldn't linger after a reload
    // any more than a deleted keybind does.
    std::vector<LuaHook> hooks;
    int                  next_hook_id = 1;

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
