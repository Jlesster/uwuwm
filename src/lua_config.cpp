#include "lua_config.hpp"

#include "utils.hpp"

#include <functional>
#include <unordered_map>

extern "C" {

#include <lauxlib.h>
#include <libinput.h>
#include <lua.h>
#include <lualib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
}

#include "config.hpp"
#include "dwindle.hpp"
#include "input.hpp"
#include "ipc.hpp"
#include "layout.hpp"
#include "output.hpp"
#include "server.hpp"
#include "toplevel.hpp"
#include "view.hpp"
#include "xwayland_view.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

// ----------------------------------------------------------------------------
// Registry keys used to recover the Server/LuaConfig from inside the
// uwu.* C functions below, none of which take a `this`/closure argument
// -- they're plain lua_CFunctions, same restriction AwesomeWM's own C core
// works under.
// ----------------------------------------------------------------------------
constexpr const char* kServerKey = "uwu.server";
constexpr const char* kConfigKey = "uwu.config";

Server* getServer(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kServerKey);
    auto* server = static_cast<Server*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return server;
}

LuaConfig* getConfig(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kConfigKey);
    auto* cfg = static_cast<LuaConfig*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return cfg;
}

// fork+exec, same as the old anonymous-namespace helper in input.cpp --
// moved here since uwu.spawn() is now the only caller.
void spawnCommand(const char* cmd) {
    if(!cmd || !*cmd) { return; }
    pid_t pid = fork();
    if(pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, nullptr);
        _exit(127);
    }
}

uint32_t parseModTable(lua_State* L, int idx) {
    uint32_t mods = 0;
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
    for(lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        if(const char* name = lua_tostring(L, -1)) {
            std::string m(name);
            if(m == "mod" || m == "super" || m == "logo") {
                mods |= WLR_MODIFIER_LOGO;
            } else if(m == "shift") {
                mods |= WLR_MODIFIER_SHIFT;
            } else if(m == "ctrl" || m == "control") {
                mods |= WLR_MODIFIER_CTRL;
            } else if(m == "alt") {
                mods |= WLR_MODIFIER_ALT;
            }
        }
        lua_pop(L, 1);
    }
    return mods;
}

template <typename T>
void getOptionalField(
    lua_State* L, int table_idx, const char* field, T& out, bool& has_value) {
    lua_getfield(L, table_idx, field);
    if(!lua_isnil(L, -1)) {
        has_value = true;
        if constexpr(std::is_same_v<T, int>) {
            out = static_cast<int>(luaL_checknumber(L, -1));
        } else if constexpr(std::is_same_v<T, float>) {
            out = static_cast<float>(luaL_checknumber(L, -1));
        } else if constexpr(std::is_same_v<T, double>) {
            out = luaL_checknumber(L, -1);
        } else if constexpr(std::is_same_v<T, bool>) {
            out = lua_toboolean(L, -1);
        } else if constexpr(std::is_same_v<T, std::string>) {
            out = luaL_checkstring(L, -1);
        } else if constexpr(std::is_same_v<T, uint32_t>) {
            out = static_cast<uint32_t>(luaL_checknumber(L, -1));
        }
    }
    lua_pop(L, 1);
}

uint32_t parseColor(const char* s) {
    if(!s || s[0] != '#') { return 0x000000ffu; }
    std::string hex(s + 1);
    if(hex.size() == 6) { hex += "ff"; }
    if(hex.size() != 8) { return 0x000000ffu; }
    return static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
}

void parseColorRgba(const char* s, float out[4]) {
    utils::colorToRgba(parseColor(s), out);
}

// Inverse of parseColor -- "#rrggbbaa", lowercase, always 8 hex digits
// (even when alpha is ff) so round-tripping uwu.get() straight back into
// uwu.set() always produces a value parseColor accepts unchanged.
std::string colorToHexString(uint32_t packed) {
    char buf[10];
    std::snprintf(buf, sizeof(buf), "#%08x", packed);
    return buf;
}

uint32_t rgbaToPacked(const float in[4]) {
    auto byte = [](float f) {
        return static_cast<uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (byte(in[0]) << 24) | (byte(in[1]) << 16) | (byte(in[2]) << 8) |
           byte(in[3]);
}

using SetterFunc = std::function<void(lua_State*, RuntimeConfig&)>;
const std::unordered_map<std::string, SetterFunc> kSettingSetters = {
    {"gap",
     [](lua_State* L, RuntimeConfig& s) {
         s.gap_px = static_cast<int>(luaL_checknumber(L, 2));
     }},
    {"border_width",
     [](lua_State* L, RuntimeConfig& s) {
         s.border_px = static_cast<int>(luaL_checknumber(L, 2));
     }},
    {"master_factor",
     [](lua_State* L, RuntimeConfig& s) {
         s.master_factor = luaL_checknumber(L, 2);
     }},
    {"repeat_rate",
     [](lua_State* L, RuntimeConfig& s) {
         s.repeat_rate_hz = static_cast<int>(luaL_checknumber(L, 2));
     }},
    {"repeat_delay",
     [](lua_State* L, RuntimeConfig& s) {
         s.repeat_delay_ms = static_cast<int>(luaL_checknumber(L, 2));
     }},
    {"cursor_size",
     [](lua_State* L, RuntimeConfig& s) {
         s.cursor_size = luaL_checknumber(L, 2);
     }},
    {"terminal",
     [](lua_State* L, RuntimeConfig& s) {
         s.terminal = luaL_checkstring(L, 2);
     }},
    {"launcher",
     [](lua_State* L, RuntimeConfig& s) {
         s.launcher = luaL_checkstring(L, 2);
     }},
    {"border_color_active",
     [](lua_State* L, RuntimeConfig& s) {
         s.border_color_active = parseColor(luaL_checkstring(L, 2));
     }},
    {"border_color_inactive",
     [](lua_State* L, RuntimeConfig& s) {
         s.border_color_inactive = parseColor(luaL_checkstring(L, 2));
     }},
    {"background_color",
     [](lua_State* L, RuntimeConfig& s) {
         parseColorRgba(luaL_checkstring(L, 2), s.background_color);
     }},
    {"dwindle_preserve_split",
     [](lua_State* L, RuntimeConfig& s) {
         s.dwindle_preserve_split = lua_toboolean(L, 2);
     }},
};

// uwu.get()'s inverse map -- added alongside uwu.set() so a rule/hook can
// read current state back (e.g. "restore the old gap after a temporary
// override") instead of the settings path being write-only, which is
// what it was before uwu.get() existed. Every push* lambda leaves exactly
// one value on the stack, same contract lua_CFunction returns rely on.
using GetterFunc = std::function<void(lua_State*, const RuntimeConfig&)>;
const std::unordered_map<std::string, GetterFunc> kSettingGetters = {
    {"gap",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushinteger(L, s.gap_px);
     }},
    {"border_width",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushinteger(L, s.border_px);
     }},
    {"master_factor",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushnumber(L, s.master_factor);
     }},
    {"repeat_rate",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushinteger(L, s.repeat_rate_hz);
     }},
    {"repeat_delay",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushinteger(L, s.repeat_delay_ms);
     }},
    {"cursor_size",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushnumber(L, s.cursor_size);
     }},
    {"terminal",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushstring(L, s.terminal.c_str());
     }},
    {"launcher",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushstring(L, s.launcher.c_str());
     }},
    {"border_color_active",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushstring(L, colorToHexString(s.border_color_active).c_str());
     }},
    {"border_color_inactive",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushstring(L, colorToHexString(s.border_color_inactive).c_str());
     }},
    {"background_color",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushstring(
             L, colorToHexString(rgbaToPacked(s.background_color)).c_str());
     }},
    {"dwindle_preserve_split",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushboolean(L, s.dwindle_preserve_split);
     }},
};

// Maps the strings accepted by uwu.monitor.set({transform = ...}) to
// wl_output_transform. Names mirror wlr-randr / Hyprland conventions
// (rotation in degrees clockwise, "flipped*" = mirrored then rotated) so
// existing muscle memory for those tools carries over.
uint32_t parseTransform(const char* s) {
    if(!s) { return WL_OUTPUT_TRANSFORM_NORMAL; }
    std::string t(s);
    if(t == "normal") { return WL_OUTPUT_TRANSFORM_NORMAL; }
    if(t == "90") { return WL_OUTPUT_TRANSFORM_90; }
    if(t == "180") { return WL_OUTPUT_TRANSFORM_180; }
    if(t == "270") { return WL_OUTPUT_TRANSFORM_270; }
    if(t == "flipped") { return WL_OUTPUT_TRANSFORM_FLIPPED; }
    if(t == "flipped-90") { return WL_OUTPUT_TRANSFORM_FLIPPED_90; }
    if(t == "flipped-180") { return WL_OUTPUT_TRANSFORM_FLIPPED_180; }
    if(t == "flipped-270") { return WL_OUTPUT_TRANSFORM_FLIPPED_270; }
    wlr_log(
        WLR_ERROR, "uwu.monitor.set: unknown transform '%s', using normal", s);
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

// ----------------------------------------------------------------------------
// Timeout/recursion guard for arbitrary Lua callbacks (keybinds, hooks,
// rules). Every uwu.bind/uwu.hook callback ultimately runs through
// LuaConfig::guardedCall below, on the compositor's single event-loop
// thread -- there's no separate Lua thread to kill if a callback hangs.
// Before uwu.hook existed, this didn't matter much: a keybind is one
// deliberate user keypress, so a hang was self-inflicted and rare. Once
// hooks fire *automatically* on every client map/unmap/focus change,
// an infinite loop in a client::manage handler (or a misbehaving
// uwu.rule) freezes the whole session -- no VT switch, no nothing, since
// even that dispatch lives on this same thread. This is deliberately the
// same shape as Hyprland's Lua backend's guardedPCall: a wall-clock
// deadline enforced via a lua_sethook instruction-count hook, plus a
// dispatch-depth cap for hook-triggers-hook recursion.
//
// A plain file-static deadline (rather than something threaded through
// every call) is fine here specifically because Lua itself can't run
// concurrently with itself and uwuwm's event loop is single-threaded --
// there is never more than one guardedCall in flight at a time on a given
// lua_State, so there's nothing to race.
constexpr long kCallTimeoutMs               = 100;
constexpr int  kHookCheckEveryNInstructions = 1000;

timespec g_call_deadline{};
bool     g_call_has_deadline = false;

void timeoutHook(lua_State* L, lua_Debug* /*dbg*/) {
    if(!g_call_has_deadline) { return; }
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double over_ms = (now.tv_sec - g_call_deadline.tv_sec) * 1000.0 +
                     (now.tv_nsec - g_call_deadline.tv_nsec) / 1e6;
    if(over_ms > 0) {
        luaL_error(L,
                   "uwuwm: callback exceeded %ldms budget, aborting",
                   kCallTimeoutMs);
    }
}

// ----------------------------------------------------------------------------
// uwu.client: a userdata wrapper around a View*. Lifetime is the whole
// problem here -- Lua code (a stored hook, a rule, a variable captured by
// a closure) can hold onto one of these long after the View it points to
// is destroyed (window closed). Rather than threading a destruction
// callback through View (coupling the tiling/protocol core to the Lua
// layer for something only the Lua layer needs), every method/property
// access re-validates the pointer is still present in server.views --
// O(n) in open window count, which is never large enough for that to
// matter, and safe by construction: a freed View can never re-appear in
// that list under a stale pointer, since erasing it *is* what frees it
// (View::~View runs as part of the list::remove_if in handleDestroy).
constexpr const char* kClientMeta = "uwu.Client";

struct LuaClient {
    View* view;
};

bool clientAlive(Server* server, View* view) {
    for(auto& v : server->views) {
        if(v.get() == view) { return true; }
    }
    return false;
}

// Fetches and validates the View* behind the Client userdata at stack
// index `idx`. Raises a Lua error (longjmps, does not return) if the
// window this used to point to has since closed -- every uwu.client.*
// method/property below goes through this rather than trusting the
// stored pointer directly.
View* checkClient(lua_State* L, int idx) {
    auto*   c = static_cast<LuaClient*>(luaL_checkudata(L, idx, kClientMeta));
    Server* server = getServer(L);
    if(!clientAlive(server, c->view)) {
        luaL_error(L, "uwu: stale client reference (window has closed)");
    }
    return c->view;
}

void pushClient(lua_State* L, View* view) {
    auto* c = static_cast<LuaClient*>(lua_newuserdata(L, sizeof(LuaClient)));
    c->view = view;
    luaL_getmetatable(L, kClientMeta);
    lua_setmetatable(L, -2);
}

int l_client_focus(lua_State* L) {
    View* view = checkClient(L, 1);
    getServer(L)->focusView(view);
    return 0;
}

int l_client_kill(lua_State* L) {
    View* view = checkClient(L, 1);
    view->close();
    return 0;
}

int l_client_set_tag(lua_State* L) {
    View* view = checkClient(L, 1);
    int   n    = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    if(n >= 0 && n < cfg::kTagCount) { view->setTags(1u << n); }
    return 0;
}

int l_client_toggle_tag(lua_State* L) {
    View* view = checkClient(L, 1);
    int   n    = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    if(n < 0 || n >= cfg::kTagCount) { return 0; }
    uint32_t next = view->tags ^ (1u << n);
    // Same guard l_toggle_tag uses for an output's tagset: a client
    // disappearing from every tag simultaneously (which is what XORing
    // its last remaining tag off would do) is never useful, only
    // confusing -- keep at least the tag being toggled.
    view->setTags(next == 0 ? (1u << n) : next);
    return 0;
}

int l_client_move_to_output(lua_State* L) {
    View*       view   = checkClient(L, 1);
    const char* name   = luaL_checkstring(L, 2);
    Server*     server = getServer(L);

    for(auto& out : server->outputs) {
        if(name == out->wlr_output->name) {
            // Same migration shape Output::handleDestroy uses for an
            // orphaned view moving to a *replacement* output: drop
            // floating placement (it was computed for the old output's
            // geometry) and adopt the destination's currently-visible
            // tagset so the view is actually visible there instead of
            // silently parked on a tag nobody's looking at.
            view->is_floating = false;
            view->output      = out.get();
            view->setTags(out->tagset);
            return 0;
        }
    }
    wlr_log(
        WLR_ERROR, "uwu.client: move_to_output('%s') -- no such output", name);
    return 0;
}

const luaL_Reg kClientMethods[] = {
    {"focus",          l_client_focus         },
    {"kill",           l_client_kill          },
    {"set_tag",        l_client_set_tag       },
    {"toggle_tag",     l_client_toggle_tag    },
    {"move_to_output", l_client_move_to_output},
    {nullptr,          nullptr                },
};

// __index: first checks whether `key` names a read-only property computed
// straight off the View, then falls back to kClientMethods for everything
// else (":focus()" etc land here as ordinary table lookups, same as any
// other Lua OOP-via-metatable object). `client.class` doesn't exist as a
// separate property -- XWaylandView already folds X11's WM_CLASS into
// the shared `app_id` field at map time (see xwayland_view.cpp), so
// app_id alone covers both; `is_xwayland` is exposed instead for rules
// that specifically need to tell native and X11 clients apart.
int l_client_index(lua_State* L) {
    View*       view = checkClient(L, 1);
    const char* key  = luaL_checkstring(L, 2);
    std::string k(key);

    if(k == "id") {
        lua_pushinteger(L, view->id);
        return 1;
    }
    if(k == "title") {
        lua_pushstring(L, view->title.c_str());
        return 1;
    }
    if(k == "app_id") {
        lua_pushstring(L, view->app_id.c_str());
        return 1;
    }
    if(k == "is_xwayland") {
        lua_pushboolean(L, dynamic_cast<XWaylandView*>(view) != nullptr);
        return 1;
    }
    if(k == "tags") {
        lua_pushinteger(L, view->tags);
        return 1;
    }
    if(k == "floating") {
        lua_pushboolean(L, view->is_floating);
        return 1;
    }
    if(k == "fullscreen") {
        lua_pushboolean(L, view->is_fullscreen);
        return 1;
    }
    if(k == "output") {
        if(view->output) {
            lua_pushstring(L, view->output->wlr_output->name);
        } else {
            lua_pushnil(L);
        }
        return 1;
    }
    if(k == "geo") {
        lua_newtable(L);
        lua_pushinteger(L, view->geo.x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, view->geo.y);
        lua_setfield(L, -2, "y");
        lua_pushinteger(L, view->geo.width);
        lua_setfield(L, -2, "width");
        lua_pushinteger(L, view->geo.height);
        lua_setfield(L, -2, "height");
        return 1;
    }

    // Not a property -- fall back to the method table.
    luaL_getmetatable(L, kClientMeta);
    lua_getfield(L, -1, "__methods");
    lua_getfield(L, -1, key);
    return 1;
}

// __newindex: the two properties actually meant to be assignable
// (client.floating = true, client.fullscreen = true) go through the same
// View methods a keybind would call, so a rule setting `floating` gets
// exactly the tiling-policy side effects (re-arrange, geometry restore on
// fullscreen exit, etc.) that toggle_floating/toggle_fullscreen already
// give a keybind -- there's deliberately no separate "just flip the flag"
// path that would drift out of sync with those. Anything else is
// rejected outright rather than silently accepted and ignored, since a
// typo'd property name (e.g. "tag" instead of using set_tag()) failing
// loudly is much easier to debug than one that does nothing.
int l_client_newindex(lua_State* L) {
    View*       view = checkClient(L, 1);
    const char* key  = luaL_checkstring(L, 2);
    std::string k(key);

    if(k == "floating") {
        view->setFloating(lua_toboolean(L, 3));
        return 0;
    }
    if(k == "fullscreen") {
        view->setFullscreen(lua_toboolean(L, 3));
        return 0;
    }

    luaL_error(L, "uwu.client: '%s' is not a writable property", key);
    return 0;
}

void registerClientMetatable(lua_State* L) {
    luaL_newmetatable(L, kClientMeta);
    lua_pushcfunction(L, l_client_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_client_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_newtable(L);
    luaL_setfuncs(L, kClientMethods, 0);
    lua_setfield(L, -2, "__methods");

    lua_pop(L, 1);  // the metatable itself -- it's stashed in the
                    // registry by luaL_newmetatable, nothing else needs
                    // it on the stack from here on.
}

// ----------------------------------------------------------------------------
// uwu.* functions
// ----------------------------------------------------------------------------

int l_spawn(lua_State* L) {
    const char* cmd = luaL_checkstring(L, 1);
    spawnCommand(cmd);
    return 0;
}

int l_bind(lua_State* L) {
    uint32_t    mods    = parseModTable(L, 1);
    const char* keyname = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    xkb_keysym_t sym =
        xkb_keysym_from_name(keyname, XKB_KEYSYM_CASE_INSENSITIVE);
    if(sym == XKB_KEY_NoSymbol) {
        // Previously: luaL_error() here, which unwinds all the way out of
        // luaL_dofile() -- one typo'd key name anywhere in rc.lua silently
        // discarded every uwu.bind() call that came *after* it in the
        // file, while anything before it (e.g. an early mod+Return
        // terminal bind) kept working. That's very likely why only one
        // binding was surviving. Log and skip instead, so the rest of the
        // script still loads.
        wlr_log(WLR_ERROR,
                "uwu.bind: unknown key name '%s', skipping this binding",
                keyname);
        return 0;
    }

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    LuaConfig* cfg = getConfig(L);
    cfg->keybinds.insert({
        static_cast<uint32_t>(sym), {mods, static_cast<uint32_t>(sym), ref}
    });
    return 0;
}

int l_set(lua_State* L) {
    const char*    name = luaL_checkstring(L, 1);
    RuntimeConfig& s    = getConfig(L)->settings;

    if(auto it = kSettingSetters.find(name); it != kSettingSetters.end()) {
        it->second(L, s);
    } else {
        wlr_log(WLR_ERROR, "uwu.set: unknown setting '%s', ignoring", name);
    }
    return 0;
}

// uwu.get(name) -- the read half uwu.set() never had. Returns nil (not an
// error) for an unknown name, same tolerance uwu.set() already extends
// the other direction, so a typo surfaces as "always nil" rather than a
// script-ending exception either way.
int l_get(lua_State* L) {
    const char*          name = luaL_checkstring(L, 1);
    const RuntimeConfig& s    = getConfig(L)->settings;

    if(auto it = kSettingGetters.find(name); it != kSettingGetters.end()) {
        it->second(L, s);
    } else {
        wlr_log(
            WLR_ERROR, "uwu.get: unknown setting '%s', returning nil", name);
        lua_pushnil(L);
    }
    return 1;
}

int l_quit(lua_State* L) {
    getServer(L)->requestQuit();
    return 0;
}

// uwu.reload() -- hot-reloads rc.lua. Doesn't reload synchronously (see
// LuaConfig::requestReload()'s comment); the actual reload happens right
// after this keybind's callback returns. SIGHUP does the same thing from
// outside the process (e.g. `pkill -HUP uwuwm`) and, unlike this, runs
// synchronously since it's not called from inside a Lua callback.
int l_reload(lua_State* L) {
    getConfig(L)->requestReload();
    return 0;
}

int l_kill(lua_State* L) {
    Server* server = getServer(L);
    if(server->focused_view) { server->focused_view->close(); }
    return 0;
}

int l_focus_dir(lua_State* L, int dir) {
    Server* server = getServer(L);
    Output* out    = server->focused_output;
    if(!out) { return 0; }

    std::vector<View*> visible;
    for(auto& t : server->views) {
        if(t->mapped && t->output == out && (t->tags & out->tagset)) {
            visible.push_back(t.get());
        }
    }
    if(visible.empty()) { return 0; }

    auto   it = std::find(visible.begin(), visible.end(), server->focused_view);
    size_t idx =
        (it == visible.end()) ? 0 : static_cast<size_t>(it - visible.begin());
    size_t next = dir > 0 ? (idx + 1) % visible.size()
                          : (idx + visible.size() - 1) % visible.size();
    server->focusView(visible[next]);
    return 0;
}

int l_focus_next(lua_State* L) { return l_focus_dir(L, 1); }
int l_focus_prev(lua_State* L) { return l_focus_dir(L, -1); }

int l_toggle_floating(lua_State* L) {
    Server* server = getServer(L);
    View*   t      = server->focused_view;
    if(t) { t->setFloating(!t->is_floating); }
    return 0;
}

int l_toggle_fullscreen(lua_State* L) {
    Server* server = getServer(L);
    if(server->focused_view) {
        server->focused_view->setFullscreen(
            !server->focused_view->is_fullscreen);
    }
    return 0;
}

int l_inc_master(lua_State* L) {
    double  delta  = luaL_checknumber(L, 1);
    Server* server = getServer(L);
    if(server->focused_output) {
        layout::incMasterFactor(*server->focused_output, delta);
    }
    return 0;
}

// uwu.set_layout("master" | "dwindle") -- switches the *focused output's*
// tiling algorithm. Per-output rather than global, same reasoning as
// master_factor: layout::LayoutMode already lives on Output, not Server.
int l_set_layout(lua_State* L) {
    const char* name   = luaL_checkstring(L, 1);
    Server*     server = getServer(L);
    if(!server->focused_output) { return 0; }
    std::string n(name);
    if(n == "dwindle") {
        server->focused_output->layout_mode = layout::LayoutMode::Dwindle;
    } else if(n == "master" || n == "masterstack" || n == "master_stack") {
        server->focused_output->layout_mode = layout::LayoutMode::MasterStack;
    } else {
        wlr_log(WLR_ERROR, "uwu.set_layout: unknown layout \"%s\"", name);
        return 0;
    }
    layout::arrange(*server->focused_output);
    return 0;
}

int l_dwindle_toggle_split(lua_State* L) {
    dwindle::toggleSplit(getServer(L)->focused_view);
    return 0;
}

int l_dwindle_swap_split(lua_State* L) {
    dwindle::swapSplit(getServer(L)->focused_view);
    return 0;
}

int l_dwindle_rotate_split(lua_State* L) {
    int angle = luaL_optinteger(L, 1, 90);
    dwindle::rotateSplit(getServer(L)->focused_view, angle);
    return 0;
}

int l_dwindle_splitratio(lua_State* L) {
    float delta = static_cast<float>(luaL_checknumber(L, 1));
    dwindle::adjustSplitRatio(getServer(L)->focused_view, delta);
    return 0;
}

int l_dwindle_move_to_root(lua_State* L) {
    bool stable = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
    dwindle::moveToRoot(getServer(L)->focused_view, stable);
    return 0;
}

// Tag indices are 1-based on the Lua side (uwu.tag_count == 9, valid
// arguments 1..9) to match normal Lua array convention; converted to the
// internal 0-based bit index here.
//
// Both of these used to poke out->tagset directly and call
// layout::arrange() themselves instead of going through
// Output::setTagset() -- which meant mod+N and mod+ctrl+N never got the
// tag-switch slide animation setTagset drives (see output.cpp), even
// though monitor-unplug migration and every other tagset change already
// went through it. Routing through setTagset() here fixes that
// inconsistency and, as of uwu.hook, is also what makes "tag::change"
// fire uniformly regardless of what triggered the switch.
int l_view_tag(lua_State* L) {
    int     n      = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    Server* server = getServer(L);
    Output* out    = server->focused_output;
    if(out && n >= 0 && n < cfg::kTagCount) { out->setTagset(1u << n); }
    return 0;
}

int l_toggle_tag(lua_State* L) {
    int     n      = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    Server* server = getServer(L);
    Output* out    = server->focused_output;
    if(out && n >= 0 && n < cfg::kTagCount) {
        uint32_t next = out->tagset ^ (1u << n);
        out->setTagset(next == 0 ? (1u << n) : next);
    }
    return 0;
}

int l_move_to_tag(lua_State* L) {
    int     n      = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    Server* server = getServer(L);
    if(server->focused_view && n >= 0 && n < cfg::kTagCount) {
        server->focused_view->setTags(1u << n);
    }
    return 0;
}

// uwu.tag.close_all(n) -- gracefully closes every mapped, managed window
// on tag n (1..uwu.tag_count), via Server::closeOnTag. Not bound to a
// key by default (see closeOnTag's comment in server.cpp) -- deliberately
// destructive, opt-in only.
int l_close_all_on_tag(lua_State* L) {
    int n = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    if(n < 0 || n >= cfg::kTagCount) { return 0; }
    getServer(L)->closeOnTag(1u << n);
    return 0;
}

int l_focus_monitor(lua_State* L) {
    int     dir    = static_cast<int>(luaL_optinteger(L, 1, 1));
    Server* server = getServer(L);
    Output* out    = server->focused_output;
    if(server->outputs.size() < 2 || !out) { return 0; }

    auto it = std::find_if(
        server->outputs.begin(),
        server->outputs.end(),
        [out](const std::unique_ptr<Output>& o) { return o.get() == out; });
    if(it == server->outputs.end()) { return 0; }

    if(dir >= 0) {
        auto next = std::next(it);
        if(next == server->outputs.end()) { next = server->outputs.begin(); }
        server->focused_output = next->get();
    } else {
        auto prev              = (it == server->outputs.begin())
                                     ? std::prev(server->outputs.end())
                                     : std::prev(it);
        server->focused_output = prev->get();
    }
    return 0;
}

// ----------------------------------------------------------------------------
// uwu.client.* functions
// ----------------------------------------------------------------------------

// uwu.client.list() -> array of Client userdata, one per mapped, managed
// view (unmanaged X11 override-redirect windows -- menus, tooltips, DND
// icons -- are deliberately excluded, same as they're excluded from
// tiling/focus/tag membership everywhere else in this codebase).
int l_client_list(lua_State* L) {
    Server* server = getServer(L);
    lua_newtable(L);
    int i = 1;
    for(auto& v : server->views) {
        if(!v->mapped || v->unmanaged) { continue; }
        pushClient(L, v.get());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int l_client_focused(lua_State* L) {
    Server* server = getServer(L);
    if(server->focused_view) {
        pushClient(L, server->focused_view);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

const luaL_Reg kClientFuncs[] = {
    {"list",    l_client_list   },
    {"focused", l_client_focused},
    {nullptr,   nullptr         },
};

// ----------------------------------------------------------------------------
// uwu.hook / uwu.unhook / uwu.rule
// ----------------------------------------------------------------------------

// uwu.hook(event, fn) -> id. `event` is one of "client::manage",
// "client::unmanage", "client::focus", "client::unfocus",
// "client::fullscreen" (fn receives the Client), "tag::change" (fn
// receives monitor_name, new_tagset), or "output::connect"/
// "output::disconnect" (fn receives monitor_name). Unknown event names
// are accepted, not rejected -- a hook for an event nothing ever fires is
// harmless dead weight, not a bug worth a hard error, and it keeps this
// from needing to be updated every time a new event is added elsewhere.
int l_hook(lua_State* L) {
    const char* event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    int id = getConfig(L)->addHook(event, ref);
    lua_pushinteger(L, id);
    return 1;
}

int l_unhook(lua_State* L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    getConfig(L)->removeHook(id);
    return 0;
}

// The actual client::manage hook every uwu.rule() call registers. A
// plain lua_CFunction can't capture C++ state, so the match/apply spec
// is baked in as Lua-registry-safe *upvalues* instead (1: match app_id
// pattern string, 2: match title pattern string, 3: has match.floating,
// 4: match.floating value, 5: the apply table, copied at registration
// time) via lua_pushcclosure below -- the idiomatic way to parameterize a
// C closure across many uwu.rule() calls without a separate per-rule
// struct/registry of our own.
int l_rule_hook(lua_State* L) {
    View* view = checkClient(L, 1);

    const char* match_app_id       = lua_tostring(L, lua_upvalueindex(1));
    const char* match_title        = lua_tostring(L, lua_upvalueindex(2));
    bool        has_match_floating = lua_toboolean(L, lua_upvalueindex(3));
    bool        match_floating     = lua_toboolean(L, lua_upvalueindex(4));

    // Exact-string match unless the pattern is prefixed "~", in which
    // case the rest is a genuine Lua pattern run through string.match --
    // reusing Lua's own matcher here rather than hand-rolling pattern
    // syntax in C++ is both less code and exactly what the "~" prefix
    // promises a caller who reaches for it.
    auto matches = [L](const char* pattern, const std::string& value) {
        if(!pattern || !*pattern) { return true; }  // unconstrained field
        if(pattern[0] != '~') { return value == pattern; }
        lua_getglobal(L, "string");
        lua_getfield(L, -1, "match");
        lua_pushlstring(L, value.c_str(), value.size());
        lua_pushstring(L, pattern + 1);
        lua_call(L, 2, 1);
        bool ok = !lua_isnil(L, -1);
        lua_pop(L, 2);  // match result + the `string` table
        return ok;
    };

    if(!matches(match_app_id, view->app_id)) { return 0; }
    if(!matches(match_title, view->title)) { return 0; }
    // Matches against whatever handleMap's floating auto-detection (parent
    // / fixed-size / X11 dialog window-type -- see toplevel.cpp /
    // xwayland_view.cpp) already decided, *before* this rule's own
    // `apply.floating` (if any) can override it below -- lets a rule like
    // `match = { floating = true }` target "whatever the compositor
    // already guessed was a dialog," which a plain app_id/title match
    // can't express at all.
    if(has_match_floating && view->is_floating != match_floating) { return 0; }

    lua_pushvalue(L, lua_upvalueindex(5));  // the `apply` table
    bool        has_floating = false, has_fullscreen = false, has_tag = false;
    bool        floating = false, fullscreen = false;
    int         tag        = 0;
    bool        has_output = false;
    std::string output_name;
    getOptionalField(L, -1, "floating", floating, has_floating);
    getOptionalField(L, -1, "fullscreen", fullscreen, has_fullscreen);
    getOptionalField(L, -1, "tag", tag, has_tag);
    getOptionalField(L, -1, "output", output_name, has_output);
    lua_pop(L, 1);

    // Same effects a hand-written client::manage hook could reach for
    // individually (client:set_tag(), client.floating = ...,
    // client:move_to_output(), client.fullscreen = ...) -- deliberately
    // going through the exact same code paths (View::setTags /
    // View::setFloating / l_client_move_to_output's lookup loop /
    // View::setFullscreen) rather than a separate "apply a rule"
    // implementation that could drift from what a manual hook does.
    if(has_output) {
        Server* server = getServer(L);
        for(auto& out : server->outputs) {
            if(output_name == out->wlr_output->name) {
                // Force back to tiled on the new output regardless of
                // prior state: a floating window's geo is an absolute
                // position on its *old* output's layout box, which is
                // meaningless once `output` changes -- if this rule also
                // sets `apply.floating = true`, the has_floating branch
                // below re-floats (and re-centers, via setFloating) on
                // the *new* output correctly, after this.
                view->is_floating = false;
                view->output      = out.get();
                view->setTags(out->tagset);
                break;
            }
        }
    }
    if(has_tag && tag >= 1 && tag <= cfg::kTagCount) {
        view->setTags(1u << (tag - 1));
    }
    // setFloating (view.cpp) centers the view over its output when
    // floating turns on -- fixing what used to be a real bug here: this
    // branch used to just flip is_floating and re-arrange, which left a
    // rule-floated window sitting at wherever tiling had placed it
    // instead of centered, unlike a window that floats from the moment
    // it maps (see XdgToplevel::handleMap / XWaylandView::handleMap).
    if(has_floating) { view->setFloating(floating); }
    if(has_fullscreen) { view->setFullscreen(fullscreen); }

    return 0;
}

// uwu.rule({match = {app_id=, title=}, apply = {floating=, fullscreen=,
// tag=}}) -- sugar over uwu.hook("client::manage", ...), not a separate
// rule engine: it registers exactly one ordinary hook (l_rule_hook above)
// per call, so a rule inherits the same timeout/recursion guard every
// other hook gets, and "why didn't my rule fire" is debuggable by reading
// one function instead of a separate matcher implementation living
// somewhere else.
int l_rule(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    auto readMatchField = [L](const char* field) -> std::string {
        lua_getfield(L, 1, "match");
        std::string v;
        if(!lua_isnil(L, -1)) {
            lua_getfield(L, -1, field);
            if(lua_isstring(L, -1)) { v = lua_tostring(L, -1); }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return v;
    };
    std::string match_app_id = readMatchField("app_id");
    std::string match_title  = readMatchField("title");

    // match.floating is tri-state (unset/true/false), unlike app_id/title
    // which use "" as their unconstrained sentinel -- floating is a bool
    // field, so it needs an explicit has_value flag rather than reusing
    // an empty-string sentinel.
    bool has_match_floating = false, match_floating = false;
    lua_getfield(L, 1, "match");
    if(!lua_isnil(L, -1)) {
        getOptionalField(L, -1, "floating", match_floating, has_match_floating);
    }
    lua_pop(L, 1);

    lua_pushstring(L, match_app_id.c_str());  // upvalue 1
    lua_pushstring(L, match_title.c_str());   // upvalue 2
    lua_pushboolean(L, has_match_floating);   // upvalue 3
    lua_pushboolean(L, match_floating);       // upvalue 4

    lua_getfield(L, 1, "apply");  // upvalue 5
    if(lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);  // keep upvalue 5 always-a-table, never nil
    }

    lua_pushcclosure(L, l_rule_hook, 5);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    int id = getConfig(L)->addHook("client::manage", ref);
    lua_pushinteger(L, id);
    return 1;
}

// ----------------------------------------------------------------------------
// uwu.monitor.* functions
// ----------------------------------------------------------------------------

// uwu.monitor.set(name, {x=, y=, width=, height=, refresh=, scale=,
// transform=, enabled=, adaptive_sync=}). `name` is a wlr_output name
// ("DP-1", "eDP-1", ...) or the wildcard "*" for a fallback default
// applied to any monitor without its own exact-name rule -- see
// findMonitorRule in output.cpp. Every field in the table is optional;
// omitted fields are left untouched both in the stored rule and (if the
// monitor is already connected) on the live output. Safe to call more
// than once for the same name -- a later call replaces the earlier rule
// outright rather than merging into it, so re-set every field you still
// want if you're overwriting an existing rule for that name.
int l_monitor_set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    MonitorRule rule;
    rule.name = name;

    bool has_x = false, has_y = false;
    getOptionalField(L, 2, "x", rule.x, has_x);
    getOptionalField(L, 2, "y", rule.y, has_y);
    rule.has_position = has_x || has_y;

    bool has_width = false, has_height = false;
    getOptionalField(L, 2, "width", rule.width, has_width);
    getOptionalField(L, 2, "height", rule.height, has_height);

    if(has_width != has_height) {
        wlr_log(WLR_ERROR,
                "uwu.monitor.set('%s'): width and height must be given "
                "together, ignoring mode",
                name);
    } else {
        rule.has_mode = has_width;
    }

    bool has_ref = false, has_scale = false, has_en = false, has_sync = false;
    getOptionalField(L, 2, "refresh", rule.refresh_mhz, has_ref);
    if(has_ref) {
        rule.refresh_mhz = static_cast<int>(rule.refresh_mhz * 1000.0 + 0.5);
    }
    getOptionalField(L, 2, "scale", rule.scale, has_scale);
    rule.has_scale = has_scale;

    lua_getfield(L, 2, "transform");
    if(!lua_isnil(L, -1)) {
        rule.has_transform = true;
        rule.transform     = parseTransform(luaL_checkstring(L, -1));
    }
    lua_pop(L, 1);

    getOptionalField(L, 2, "enabled", rule.enabled, has_en);
    rule.has_enabled = has_en;
    getOptionalField(L, 2, "adaptive_sync", rule.adaptive_sync, has_sync);
    rule.has_adaptive_sync = has_sync;

    LuaConfig* cfg = getConfig(L);
    auto       it =
        std::find_if(cfg->monitor_rules.begin(),
                     cfg->monitor_rules.end(),
                     [&](const MonitorRule& r) { return r.name == rule.name; });
    if(it != cfg->monitor_rules.end()) {
        *it = rule;
    } else {
        cfg->monitor_rules.push_back(rule);
    }

    Server* server = getServer(L);
    for(auto& out : server->outputs) {
        if(rule.name == out->wlr_output->name) {
            out->applyRule(rule);
            break;
        }
    }
    return 0;
}

// uwu.monitor.list() -> array of {name, x, y, width, height, scale,
// refresh, enabled, focused} for every currently connected output.
int l_monitor_list(lua_State* L) {
    Server* server = getServer(L);
    lua_newtable(L);
    int i = 1;
    for(auto& out : server->outputs) {
        lua_newtable(L);
        lua_pushstring(L, out->wlr_output->name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, out->layout_box.x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, out->layout_box.y);
        lua_setfield(L, -2, "y");
        lua_pushinteger(L, out->layout_box.width);
        lua_setfield(L, -2, "width");
        lua_pushinteger(L, out->layout_box.height);
        lua_setfield(L, -2, "height");
        lua_pushnumber(L, out->wlr_output->scale);
        lua_setfield(L, -2, "scale");
        lua_pushboolean(L, out->wlr_output->enabled);
        lua_setfield(L, -2, "enabled");
        if(out->wlr_output->current_mode) {
            lua_pushnumber(L, out->wlr_output->current_mode->refresh / 1000.0);
            lua_setfield(L, -2, "refresh");
        }
        lua_pushboolean(L, server->focused_output == out.get());
        lua_setfield(L, -2, "focused");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

const luaL_Reg kMonitorFuncs[] = {
    {"set",   l_monitor_set },
    {"list",  l_monitor_list},
    {nullptr, nullptr       },
};

// ----------------------------------------------------------------------------
// uwu.input.* functions
// ----------------------------------------------------------------------------

// String <-> libinput_config_accel_profile, mirroring parseTransform's
// shape above. "flat" disables acceleration entirely (1:1 physical-to-
// logical motion, what most FPS/gaming-mouse users want); "adaptive" is
// libinput's default speed-dependent curve.
uint32_t parseAccelProfile(const char* s) {
    if(!s) { return LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE; }
    std::string p(s);
    if(p == "flat") { return LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT; }
    if(p == "adaptive") { return LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE; }
    wlr_log(WLR_ERROR,
            "uwu.input.set: unknown accel_profile '%s', using adaptive",
            s);
    return LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
}

const char* accelProfileToString(uint32_t profile) {
    return profile == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT ? "flat" : "adaptive";
}

uint32_t parseScrollMethod(const char* s) {
    if(!s) { return LIBINPUT_CONFIG_SCROLL_NO_SCROLL; }
    std::string m(s);
    if(m == "two_finger") { return LIBINPUT_CONFIG_SCROLL_2FG; }
    if(m == "edge") { return LIBINPUT_CONFIG_SCROLL_EDGE; }
    if(m == "on_button_down") { return LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN; }
    if(m == "none") { return LIBINPUT_CONFIG_SCROLL_NO_SCROLL; }
    wlr_log(
        WLR_ERROR, "uwu.input.set: unknown scroll_method '%s', ignoring", s);
    return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
}

const char* scrollMethodToString(uint32_t method) {
    switch(method) {
        case LIBINPUT_CONFIG_SCROLL_2FG:
            return "two_finger";
        case LIBINPUT_CONFIG_SCROLL_EDGE:
            return "edge";
        case LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN:
            return "on_button_down";
        default:
            return "none";
    }
}

uint32_t parseClickMethod(const char* s) {
    if(!s) { return LIBINPUT_CONFIG_CLICK_METHOD_NONE; }
    std::string m(s);
    if(m == "button_areas") {
        return LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
    }
    if(m == "clickfinger") { return LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER; }
    if(m == "none") { return LIBINPUT_CONFIG_CLICK_METHOD_NONE; }
    wlr_log(WLR_ERROR, "uwu.input.set: unknown click_method '%s', ignoring", s);
    return LIBINPUT_CONFIG_CLICK_METHOD_NONE;
}

const char* clickMethodToString(uint32_t method) {
    switch(method) {
        case LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS:
            return "button_areas";
        case LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER:
            return "clickfinger";
        default:
            return "none";
    }
}

// uwu.input.set(match, {tap=, tap_drag=, tap_drag_lock=, natural_scroll=,
// dwt=, left_handed=, middle_emulation=, accel_speed=, accel_profile=,
// scroll_method=, scroll_button=, click_method=}). `match` is an exact
// wlr_input_device name, "type:touchpad"/"type:mouse", or "*" -- see
// InputRule's doc comment in lua_config.hpp for precedence when several
// rules could apply to the same device. Safe to call repeatedly for the
// same `match`: a later call replaces the earlier rule outright (same
// semantics as uwu.monitor.set()), and is applied immediately to every
// already-connected matching device, not just future ones.
int l_input_set(lua_State* L) {
    const char* match = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    InputRule rule;
    rule.match = match;

    getOptionalField(L, 2, "tap", rule.tap, rule.has_tap);
    getOptionalField(L, 2, "tap_drag", rule.tap_drag, rule.has_tap_drag);
    getOptionalField(
        L, 2, "tap_drag_lock", rule.tap_drag_lock, rule.has_tap_drag_lock);
    getOptionalField(
        L, 2, "natural_scroll", rule.natural_scroll, rule.has_natural_scroll);
    getOptionalField(L, 2, "dwt", rule.dwt, rule.has_dwt);
    getOptionalField(
        L, 2, "left_handed", rule.left_handed, rule.has_left_handed);
    getOptionalField(L,
                     2,
                     "middle_emulation",
                     rule.middle_emulation,
                     rule.has_middle_emulation);
    getOptionalField(
        L, 2, "accel_speed", rule.accel_speed, rule.has_accel_speed);
    getOptionalField(
        L, 2, "scroll_button", rule.scroll_button, rule.has_scroll_button);

    lua_getfield(L, 2, "accel_profile");
    if(!lua_isnil(L, -1)) {
        rule.has_accel_profile = true;
        rule.accel_profile     = parseAccelProfile(luaL_checkstring(L, -1));
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "scroll_method");
    if(!lua_isnil(L, -1)) {
        rule.has_scroll_method = true;
        rule.scroll_method     = parseScrollMethod(luaL_checkstring(L, -1));
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "click_method");
    if(!lua_isnil(L, -1)) {
        rule.has_click_method = true;
        rule.click_method     = parseClickMethod(luaL_checkstring(L, -1));
    }
    lua_pop(L, 1);

    LuaConfig* cfg = getConfig(L);
    auto       it =
        std::find_if(cfg->input_rules.begin(),
                     cfg->input_rules.end(),
                     [&](const InputRule& r) { return r.match == rule.match; });
    if(it != cfg->input_rules.end()) {
        *it = rule;
    } else {
        cfg->input_rules.push_back(rule);
    }

    // Apply immediately to every already-connected device this rule now
    // *wins* for (per findInputRule's exact > type > wildcard precedence)
    // -- a rule set from a keybind (e.g. a "toggle natural scroll" bind)
    // should take effect on the current session, not just future
    // replugs. Compared by `match` string rather than rule pointer/
    // iterator identity, since the push_back above may have reallocated
    // cfg->input_rules and invalidated `it`.
    Server* server = getServer(L);
    for(auto& dev : server->input_devices) {
        bool             touchpad = input::isTouchpad(dev->device);
        const InputRule* winner =
            input::findInputRule(*server, dev->device, touchpad);
        if(winner && winner->match == rule.match) {
            input::applyInputRule(dev->device, rule);
        }
    }
    return 0;
}

// uwu.input.list() -> array of {name, type ("touchpad"/"mouse"), tap,
// natural_scroll, left_handed, accel_speed, accel_profile} for every
// currently connected pointer device. Read back directly from libinput
// rather than from stored rules, so it reflects the live device state
// (including anything set outside uwuwm, or a device with no matching
// rule at all still showing its libinput defaults).
int l_input_list(lua_State* L) {
    Server* server = getServer(L);
    lua_newtable(L);
    int i = 1;
    for(auto& dev : server->input_devices) {
        bool is_libinput = wlr_input_device_is_libinput(dev->device);
        libinput_device* ldev =
            is_libinput ? wlr_libinput_get_device_handle(dev->device) : nullptr;

        lua_newtable(L);
        lua_pushstring(L, dev->device->name);
        lua_setfield(L, -2, "name");

        bool touchpad = input::isTouchpad(dev->device);
        lua_pushstring(L, touchpad ? "touchpad" : "mouse");
        lua_setfield(L, -2, "type");

        if(ldev) {
            lua_pushboolean(L,
                            libinput_device_config_tap_get_enabled(ldev) ==
                                LIBINPUT_CONFIG_TAP_ENABLED);
            lua_setfield(L, -2, "tap");

            lua_pushboolean(
                L,
                libinput_device_config_scroll_get_natural_scroll_enabled(ldev));
            lua_setfield(L, -2, "natural_scroll");

            lua_pushboolean(L, libinput_device_config_left_handed_get(ldev));
            lua_setfield(L, -2, "left_handed");

            lua_pushnumber(L, libinput_device_config_accel_get_speed(ldev));
            lua_setfield(L, -2, "accel_speed");

            lua_pushstring(L,
                           accelProfileToString(
                               libinput_device_config_accel_get_profile(ldev)));
            lua_setfield(L, -2, "accel_profile");
        }
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

const luaL_Reg kInputFuncs[] = {
    {"set",   l_input_set },
    {"list",  l_input_list},
    {nullptr, nullptr     },
};

// uwu.tag.* -- nested the same way uwu.monitor.* already is, for the same
// reason: "uwu.tag.view(3)" reads as a namespaced action on tags, where
// the old flat "uwu.view_tag(3)" read more like a typo of "uwu.set" than
// its own thing. move_to_tag is renamed move_client_here here to read
// correctly now that "move_to_tag(n)" would otherwise be ambiguous with
// uwu.client:set_tag(n) -- this one moves *the focused client* to a tag,
// not a tag to anywhere.
const luaL_Reg kTagFuncs[] = {
    {"view",             l_view_tag        },
    {"toggle",           l_toggle_tag      },
    {"move_client_here", l_move_to_tag     },
    {"close_all",        l_close_all_on_tag},
    {nullptr,            nullptr           },
};

const luaL_Reg kuwuwmFuncs[] = {
    {"spawn",                l_spawn               },
    {"bind",                 l_bind                },
    {"set",                  l_set                 },
    {"get",                  l_get                 },
    {"quit",                 l_quit                },
    {"reload",               l_reload              },
    {"hook",                 l_hook                },
    {"unhook",               l_unhook              },
    {"rule",                 l_rule                },
    {"kill",                 l_kill                },
    {"focus_next",           l_focus_next          },
    {"focus_prev",           l_focus_prev          },
    {"toggle_floating",      l_toggle_floating     },
    {"toggle_fullscreen",    l_toggle_fullscreen   },
    {"inc_master",           l_inc_master          },
    {"focus_monitor",        l_focus_monitor       },
    {"set_layout",           l_set_layout          },
    {"dwindle_toggle_split", l_dwindle_toggle_split},
    {"dwindle_swap_split",   l_dwindle_swap_split  },
    {"dwindle_rotate_split", l_dwindle_rotate_split},
    {"dwindle_splitratio",   l_dwindle_splitratio  },
    {"dwindle_move_to_root", l_dwindle_move_to_root},
    {nullptr,                nullptr               },
};

// Embedded fallback, used only if no rc.lua is found on disk. Mirrors the
// keybinds/settings that used to be hardcoded in config.hpp's kKeybinds
// array. Kept identical in spirit to config/rc.lua, which is the version
// meant to actually be copied and edited.
constexpr const char* kDefaultConfig = R"LUACFG(
uwu.set("gap", 8)
uwu.set("border_width", 2)
uwu.set("master_factor", 0.55)
uwu.set("border_color_active", "#cba6f7")
uwu.set("border_color_inactive", "#313244")
uwu.set("background_color", "#111217")
uwu.set("repeat_rate", 25)
uwu.set("repeat_delay", 600)
uwu.set("cursor_size", 24)

local terminal = "wezterm"
local launcher = "fuzzel"
uwu.set("terminal", terminal)
uwu.set("launcher", launcher)

uwu.bind({"mod"}, "Return", function() uwu.spawn(terminal) end)
uwu.bind({"mod"}, "d", function() uwu.spawn(launcher) end)
uwu.bind({"mod", "shift"}, "q", function() uwu.quit() end)
uwu.bind({"mod", "shift"}, "r", function() uwu.reload() end)  -- hot-reload rc.lua
uwu.bind({"mod"}, "j", function() uwu.focus_next() end)
uwu.bind({"mod"}, "k", function() uwu.focus_prev() end)
uwu.bind({"mod", "shift"}, "c", function() uwu.kill() end)
uwu.bind({"mod"}, "space", function() uwu.toggle_floating() end)
uwu.bind({"mod"}, "f", function() uwu.toggle_fullscreen() end)
uwu.bind({"mod"}, "i", function() uwu.inc_master(0.05) end)
uwu.bind({"mod"}, "u", function() uwu.inc_master(-0.05) end)
uwu.bind({"mod"}, "Tab", function() uwu.focus_monitor(1) end)

-- Dwindle (Hyprland-style BSP) tiling is available as an alternative to
-- the default master-stack layout -- uncomment to switch the focused
-- monitor to it, and to get togglesplit/swapsplit/splitratio bindings:
-- uwu.set_layout("dwindle")
-- uwu.bind({"mod"}, "p", function() uwu.dwindle_toggle_split() end)
-- uwu.bind({"mod", "shift"}, "p", function() uwu.dwindle_swap_split() end)
-- uwu.bind({"mod"}, "r", function() uwu.dwindle_rotate_split() end)
-- uwu.bind({"mod"}, "minus", function() uwu.dwindle_splitratio(-0.1) end)
-- uwu.bind({"mod"}, "equal", function() uwu.dwindle_splitratio(0.1) end)

for i = 1, uwu.tag_count do
	uwu.bind({"mod"}, tostring(i), function() uwu.tag.view(i) end)
	uwu.bind({"mod", "shift"}, tostring(i), function() uwu.tag.move_client_here(i) end)
	if i <= 5 then
		uwu.bind({"mod", "ctrl"}, tostring(i), function() uwu.tag.toggle(i) end)
	end
end

-- Monitor configuration, uncomment and adjust output names (see
-- `uwu.monitor.list()`, e.g. from a keybind, to find yours) as needed.
-- Every field is optional; only what's set is applied/stored.
--
-- Both uwu.reload() (bound to mod+shift+r above) and `pkill -HUP uwuwm`
-- from a shell re-run this whole file from scratch, which is the fast
-- way to test changes here (e.g. a refresh rate) without restarting the
-- whole compositor and losing your session.
--
-- uwu.monitor.set("DP-1", {
--     x = 0, y = 0,
--     width = 2560, height = 1440, refresh = 144,
--     scale = 1.0,
--     transform = "normal", -- normal|90|180|270|flipped|flipped-90|...
--     adaptive_sync = true,
-- })
-- uwu.monitor.set("HDMI-A-1", { x = 2560, y = 0, enabled = true })
-- uwu.monitor.set("*", { scale = 1.0 }) -- fallback default for anything else

-- ── Client rules & event hooks ──────────────────────────────────────────
-- The Client object passed to every uwu.hook("client::*", fn) callback
-- exposes the window's live state (c.title, c.app_id, c.tags, c.geo, ...)
-- and mutating methods (c:focus(), c:set_tag(n), c:move_to_output(name),
-- ...). Property/method reads re-check the window still exists, so
-- capturing a Client in a longer-lived closure is always safe.
--
-- uwu.rule() is sugar over uwu.hook("client::manage", ...) -- one
-- `match` block (app_id, title -- "~foo" = Lua pattern; floating --
-- true/false to match whatever handleMap's own dialog/fixed-size
-- detection already decided) selects the client, the `apply` block
-- mutates it (floating/fullscreen/tag/output). See rc.lua for the full
-- Client field/method list and all event names.

-- Dialogs/transients (anything with a parent), fixed-size utility
-- windows, and X11 clients that set _NET_WM_WINDOW_TYPE_DIALOG/UTILITY/
-- SPLASH already float themselves automatically -- no rule needed. This
-- one just also centers pavucontrol, which doesn't set any of those.
uwu.rule({ match = { app_id = "pavucontrol" }, apply = { floating = true } })

-- Keep media players (mpv) on tag 3 so the launcher always knows where
-- to find them; float so resizing the player doesn't reshuffle tiling.
uwu.rule({ match = { app_id = "mpv" }, apply = { floating = true, tag = 3 } })

-- Anything the compositor already auto-detected as a dialog (see
-- match.floating above) gets parked on the scratch tag (9) instead of
-- wherever it happened to open -- handy for keeping one-off file
-- pickers/color choosers out of the way without naming every app_id.
uwu.rule({ match = { floating = true }, apply = { tag = 9 } })

-- Pin a chat client to a specific monitor regardless of which output was
-- focused when it launched.
-- uwu.rule({ match = { app_id = "discord" }, apply = { output = "DP-1" } })

-- A hook for the same surface -- anything uwu.rule can't express (a
-- conditional tag move, a focus-side effect, ...) drops down to
-- uwu.hook directly. Returns an id; uwu.unhook(id) removes it later.
local id = uwu.hook("client::focus", function(c)
  if c.app_id == "steam" and c.tags ~= 256 then
    c:set_tag(9)  -- bit 8 = tag 9
  end
end)
)LUACFG";

std::string userConfigPath() {
    if(const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/uwuwm/rc.lua";
    }
    if(const char* home = getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/uwuwm/rc.lua";
    }
    return {};
}

// Same resolution order as userConfigPath(), minus the "/rc.lua" suffix
// -- this is where `require("awful")`/`require("beautiful")` look for a
// user override of the shipped lib/ tree (e.g. ~/.config/uwuwm/lib/
// awful/init.lua replacing the built-in one) before falling back to the
// dev/installed copies extendPackagePath() also adds below.
std::string userConfigDir() {
    if(const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/uwuwm";
    }
    if(const char* home = getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/uwuwm";
    }
    return {};
}

// Prepends every place rc.lua's require("awful")/require("beautiful")
// (and anything either of those transitively requires -- gears.table,
// etc.) should be found, highest-priority first:
//
//   1. <user config dir>/lib/          -- lets a user drop in their own
//      awful/beautiful (or add a sibling module) that shadows the
//      shipped one, same override precedence rc.lua itself already has
//      over the embedded kDefaultConfig.
//   2. <source tree>/lib/              -- only exists/matters when
//      running an uninstalled ./builddir/uwuwm straight out of a git
//      checkout (see UWUWM_SRCDIR in meson.build); harmless to have on
//      the path for an installed binary too, since that directory
//      simply won't exist there and Lua's loader silently skips path
//      entries that don't resolve to a file.
//   3. <datadir>/uwuwm/lib/            -- the real installed location
//      (see meson.build's install_subdir('lib', ...)) for a
//      `meson install`ed uwuwm.
//
// then leaves the rest of the interpreter's own default package.path
// appended after all three, so system-installed third-party Lua
// libraries (luarocks packages, etc.) are still reachable too.
void extendPackagePath(lua_State* L) {
    std::string prefix;

    auto addDir = [&](const std::string& dir) {
        if(dir.empty()) { return; }
        prefix += dir + "/?.lua;" + dir + "/?/init.lua;";
    };

    if(std::string cfg = userConfigDir(); !cfg.empty()) {
        addDir(cfg + "/lib");
    }
#ifdef UWUWM_SRCDIR
    addDir(std::string(UWUWM_SRCDIR) + "/lib");
#endif
#ifdef UWUWM_DATADIR
    addDir(std::string(UWUWM_DATADIR) + "/lib");
#endif

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    std::string existing = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    std::string combined = prefix + existing;
    lua_pushstring(L, combined.c_str());
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);  // pop `package`
}

}  // namespace

LuaConfig::LuaConfig() = default;

LuaConfig::~LuaConfig() {
    if(L) { lua_close(L); }
}

void LuaConfig::init(Server& server) {
    server_ = &server;

    L = luaL_newstate();
    luaL_openlibs(L);
    extendPackagePath(L);

    lua_pushlightuserdata(L, &server);
    lua_setfield(L, LUA_REGISTRYINDEX, kServerKey);
    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kConfigKey);

    // The uwu.Client metatable (used by uwu.client.list()/focused() and
    // every uwu.hook("client::*", ...)/uwu.rule() callback) must exist
    // before anything below can push one -- registerClientMetatable
    // stashes it in the registry under kClientMeta, it doesn't need a
    // slot on `uwu` itself.
    registerClientMetatable(L);

    lua_newtable(L);
    luaL_setfuncs(L, kuwuwmFuncs, 0);
    lua_pushinteger(L, cfg::kTagCount);
    lua_setfield(L, -2, "tag_count");

    // uwu.monitor.{set,list}, uwu.tag.{view,toggle,move_client_here,close_all},
    // and uwu.client.{list,focused} all live in their own subtables rather than
    // flat in `uwu` alongside spawn/bind/etc -- "uwu.monitor.set(...)" reads
    // much closer to wlr-randr/Hyprland monitor-config syntax than a flat
    // "uwu.set_monitor(...)" would, and namespacing the rest the same way keeps
    // the top-level table from becoming an undifferentiated pile as more gets
    // added (see uwu.hook's event names, which follow this same
    // "namespace::action" shape for the same reason).
    lua_newtable(L);
    luaL_setfuncs(L, kMonitorFuncs, 0);
    lua_setfield(L, -2, "monitor");

    lua_newtable(L);
    luaL_setfuncs(L, kTagFuncs, 0);
    lua_setfield(L, -2, "tag");

    lua_newtable(L);
    luaL_setfuncs(L, kInputFuncs, 0);
    lua_setfield(L, -2, "input");

    lua_newtable(L);
    luaL_setfuncs(L, kClientFuncs, 0);
    lua_setfield(L, -2, "client");

    lua_setglobal(L, "uwu");
}

bool LuaConfig::load() {
    std::string path = userConfigPath();
    int         status;

    if(!path.empty() && access(path.c_str(), R_OK) == 0) {
        wlr_log(WLR_INFO, "loading config from %s", path.c_str());
        status = luaL_dofile(L, path.c_str());
    } else {
        wlr_log(
            WLR_INFO,
            "no rc.lua found (looked for %s), using built-in default config",
            path.empty() ? "<no HOME/XDG_CONFIG_HOME>" : path.c_str());
        status = luaL_dostring(L, kDefaultConfig);
    }

    if(status != LUA_OK) {
        wlr_log(WLR_ERROR, "rc.lua error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }

    wlr_log(
        WLR_INFO, "rc.lua loaded: %zu keybind(s) registered", keybinds.size());
    return true;
}

bool LuaConfig::guardedCall(int nargs) {
    // Recursion guard first, before touching the clock -- a hook whose
    // callback (directly, or through a chain of other hooks it triggers)
    // causes another fire*Event call shouldn't be able to blow the C++
    // call stack or spin the event loop forever. This is the one failure
    // mode a wall-clock timeout alone doesn't catch: each individual call
    // can finish well inside budget and still recurse unboundedly.
    if(hook_dispatch_depth >= 8) {
        wlr_log(WLR_ERROR,
                "uwuwm: hook recursion depth exceeded, dropping callback");
        lua_pop(L, nargs + 1);  // discard the function + its pushed args
        return false;
    }

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    timespec prev_deadline     = g_call_deadline;
    bool     prev_has_deadline = g_call_has_deadline;

    g_call_deadline = now;
    g_call_deadline.tv_nsec += kCallTimeoutMs * 1000000L;
    if(g_call_deadline.tv_nsec >= 1000000000L) {
        g_call_deadline.tv_sec += 1;
        g_call_deadline.tv_nsec -= 1000000000L;
    }
    g_call_has_deadline = true;

    lua_sethook(L, timeoutHook, LUA_MASKCOUNT, kHookCheckEveryNInstructions);
    hook_dispatch_depth++;
    int status = lua_pcall(L, nargs, 0, 0);
    hook_dispatch_depth--;
    lua_sethook(L, nullptr, 0, 0);

    // Restore the *caller's* deadline/flag rather than always clearing --
    // a hook firing from inside another hook's callback (recursion, still
    // allowed up to the depth cap above) should stay bound by whichever
    // deadline started first, not get a fresh budget for the inner call.
    g_call_deadline     = prev_deadline;
    g_call_has_deadline = prev_has_deadline;

    if(status != LUA_OK) {
        wlr_log(
            WLR_ERROR, "uwuwm: Lua callback error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void LuaConfig::invoke(int fn_ref) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    guardedCall(0);

    // lua_pcall above has now fully returned -- there is no Lua frame
    // left on the stack -- so it's safe to close/replace `L` here even
    // though we're still inside this C++ call. This is the only place
    // a uwu.reload()-requested reload actually happens; see
    // requestReload()'s comment for why it can't happen synchronously
    // inside the uwu.reload() C function itself.
    if(reload_pending) {
        wlr_log(WLR_INFO, "deferred reload requested from keybind callback");
        reload_pending = false;
        server_->reloadConfig();
    }
}

void LuaConfig::fireClientEvent(const std::string& event, View* view) {
    // Copy the ids of hooks registered for this event *before* invoking
    // any of them -- a hook is free to call uwu.unhook() on itself or on
    // a sibling from inside its own callback, and mutating `hooks` (the
    // vector we'd otherwise be iterating) mid-fire is exactly the
    // iterator-invalidation bug that setup avoids.
    std::vector<int> fn_refs;
    for(auto& h : hooks) {
        if(h.event == event) { fn_refs.push_back(h.fn_ref); }
    }
    for(int fn_ref : fn_refs) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
        pushClient(L, view);
        guardedCall(1);
    }
    // IPC subscribers (ipc.hpp) get told about the same event, right
    // here, rather than every call site above (toplevel.cpp,
    // xwayland_view.cpp, view.cpp, server.cpp) separately remembering to
    // notify both systems -- this function *is* "client::event just
    // happened," Lua hooks are one consumer of that and IPC is another.
    if(server_->ipc) { server_->ipc->onClientEvent(event, view); }
}

void LuaConfig::fireOutputEvent(const std::string& event, Output* output) {
    std::vector<int> fn_refs;
    for(auto& h : hooks) {
        if(h.event == event) { fn_refs.push_back(h.fn_ref); }
    }
    for(int fn_ref : fn_refs) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
        lua_pushstring(L, output->wlr_output->name);
        guardedCall(1);
    }
    if(server_->ipc) {
        server_->ipc->onOutputEvent(event, output->wlr_output->name);
    }
}

void LuaConfig::fireTagChange(Output* output, uint32_t new_tagset) {
    std::vector<int> fn_refs;
    for(auto& h : hooks) {
        if(h.event == "tag::change") { fn_refs.push_back(h.fn_ref); }
    }
    for(int fn_ref : fn_refs) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
        lua_pushstring(L, output->wlr_output->name);
        lua_pushinteger(L, new_tagset);
        guardedCall(2);
    }
    if(server_->ipc) {
        server_->ipc->onTagChange(output->wlr_output->name, new_tagset);
    }
}

int LuaConfig::addHook(const std::string& event, int fn_ref) {
    int id = next_hook_id++;
    hooks.push_back({id, event, fn_ref});
    return id;
}

void LuaConfig::removeHook(int id) {
    auto it = std::find_if(hooks.begin(), hooks.end(), [id](const LuaHook& h) {
        return h.id == id;
    });
    if(it == hooks.end()) { return; }
    luaL_unref(L, LUA_REGISTRYINDEX, it->fn_ref);
    hooks.erase(it);
}

bool LuaConfig::reload() {
    // Snapshot the current, known-working state before touching anything.
    // If the new script fails to load (syntax error, runtime error before
    // any/all uwu.bind() calls run, etc.) we roll back to this instead of
    // leaving the compositor with an empty keybind set -- which would
    // include losing the very reload keybind you just used to get here.
    lua_State*                          old_L        = L;
    std::multimap<uint32_t, LuaKeybind> old_keybinds = std::move(keybinds);
    std::vector<MonitorRule>            old_rules    = std::move(monitor_rules);
    std::vector<InputRule> old_input_rules           = std::move(input_rules);
    std::vector<LuaHook>   old_hooks                 = std::move(hooks);
    RuntimeConfig          old_settings              = settings;
    int                    old_next_hook_id          = next_hook_id;

    keybinds.clear();
    monitor_rules.clear();
    input_rules.clear();
    hooks.clear();
    next_hook_id = 1;
    settings     = RuntimeConfig{};
    L            = nullptr;

    init(*server_);
    bool ok = load();

    if(ok) {
        if(old_L) { lua_close(old_L); }
        wlr_log(WLR_INFO,
                "rc.lua reload ok: %zu keybind(s), %zu monitor rule(s), "
                "%zu input rule(s), %zu hook(s)",
                keybinds.size(),
                monitor_rules.size(),
                input_rules.size(),
                hooks.size());
    } else {
        if(L) { lua_close(L); }
        L             = old_L;
        keybinds      = std::move(old_keybinds);
        monitor_rules = std::move(old_rules);
        input_rules   = std::move(old_input_rules);
        hooks         = std::move(old_hooks);
        next_hook_id  = old_next_hook_id;
        settings      = old_settings;
        wlr_log(WLR_ERROR,
                "rc.lua reload failed, keeping previous config running");
    }
    return ok;
}
