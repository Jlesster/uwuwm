#include "lua_config.hpp"

#include "utils.hpp"

#include <functional>
#include <unordered_map>

extern "C" {

#include <lauxlib.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
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
#include "bar.hpp"
#include "qml_bar.hpp"
#include "dwindle.hpp"
#include "input.hpp"
#include "ipc.hpp"
#include "layershell.hpp"
#include "layout.hpp"
#include "output.hpp"
#include "server.hpp"
#include "toplevel.hpp"
#include "view.hpp"
#include "xwayland_view.hpp"

#include <dirent.h>

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

// ----------------------------------------------------------------------------
// uwu.system.* helpers (brightness, volume) -- see kSystemFuncs below.
// ----------------------------------------------------------------------------

// Synchronous fork+exec+capture, unlike spawnCommand's fire-and-forget --
// only ever used for short-lived, near-instant queries (wpctl get-volume)
// where a keybind or a status-bar poll genuinely needs the answer back on
// the same call, not a callback later. This blocks the compositor's
// single event-loop thread for as long as the child takes, same tradeoff
// Awesome's easy_async avoids and Hyprland's hyprctl doesn't need to
// (it's a separate process) -- acceptable here only because everything
// this backs (wpctl) returns in low single-digit milliseconds. Don't
// reach for this for anything that could block on network/disk/a hung
// process; spawnCommand + a hook/callback is the right shape for that.
std::string runCommandCapture(const std::string& cmd) {
    std::string result;
    FILE*       pipe = popen(cmd.c_str(), "r");
    if(!pipe) { return result; }
    char buf[256];
    while(fgets(buf, sizeof(buf), pipe)) { result += buf; }
    pclose(pipe);
    while(!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                              result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

// Finds the first backlight device under /sys/class/backlight (e.g.
// "amdgpu_bl1", "intel_backlight") -- there's normally exactly one on a
// laptop, and uwuwm doesn't try to disambiguate multiple the way
// brightnessctl's `-d` flag would; add a uwu.system.brightness.set(pct,
// device) overload if that ever matters. Returns "" if the directory
// doesn't exist or is empty (desktops with no backlight sysfs node at
// all), which the caller treats as "no brightness control available."
std::string findBacklightDevice() {
    DIR* dir = opendir("/sys/class/backlight");
    if(!dir) { return ""; }
    std::string name;
    while(dirent* entry = readdir(dir)) {
        std::string d(entry->d_name);
        if(d == "." || d == "..") { continue; }
        name = d;
        break;  // first entry is good enough -- see doc comment above.
    }
    closedir(dir);
    return name;
}

// Reads a small integer out of /sys/class/backlight/<device>/<file>.
// Returns -1 on any failure (missing device, permission, non-numeric
// content) so callers can tell "no backlight" apart from "0%".
int readBacklightFile(const std::string& device, const char* file) {
    std::string path = "/sys/class/backlight/" + device + "/" + file;
    FILE*       f    = fopen(path.c_str(), "r");
    if(!f) { return -1; }
    int value = -1;
    if(fscanf(f, "%d", &value) != 1) { value = -1; }
    fclose(f);
    return value;
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

// Parses a uwu.mousebind() button argument: "left"/"right"/"middle" (the
// common case), the underlying BTN_LEFT/BTN_RIGHT/BTN_MIDDLE evdev name if
// someone wants to be explicit, or a bare number for anything else (e.g. a
// side button whose code isn't worth naming here). Returns 0 -- never a
// real evdev button code -- on anything unrecognized, which l_mousebind
// below logs and skips (same treatment l_bind gives an unknown key name)
// rather than registering a bind that can never fire.
uint32_t parseButtonName(lua_State* L, int idx) {
    if(lua_isnumber(L, idx)) {
        return static_cast<uint32_t>(lua_tointeger(L, idx));
    }
    std::string b = luaL_checkstring(L, idx);
    if(b == "left" || b == "BTN_LEFT") { return BTN_LEFT; }
    if(b == "right" || b == "BTN_RIGHT") { return BTN_RIGHT; }
    if(b == "middle" || b == "BTN_MIDDLE") { return BTN_MIDDLE; }
    if(b == "side" || b == "BTN_SIDE") { return BTN_SIDE; }
    if(b == "extra" || b == "BTN_EXTRA") { return BTN_EXTRA; }
    return 0;
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

// Single source of truth for which module owns which uwu.set()/uwu.get()
// field. Before this, "is this setting visual or behavioral" only existed
// as two independently hand-copied Lua tables (nyaa's VISUAL_FIELDS and
// paw's BEHAVIOR_FIELDS) that could only be checked against each other by
// reading both files side by side -- nothing stopped them drifting apart,
// or a new setting landing here without ever being added to either. This
// map is that boundary now, enforced once, in the one place that also
// defines what each setting *does* -- see l_visual_set/l_visual_get and
// l_behavior_set/l_behavior_get below, which are the only dispatchers nyaa
// and paw call into as of this pass. uwu.set()/uwu.get() (l_set/l_get)
// stay as deprecated, category-blind aliases over the same table, purely
// so an old rc.lua doesn't break.
enum class SettingCategory { Visual, Behavior };

const std::unordered_map<std::string, SettingCategory> kSettingCategory = {
    {"gap",                    SettingCategory::Visual  },
    {"border_width",           SettingCategory::Visual  },
    {"border_color_active",    SettingCategory::Visual  },
    {"border_color_inactive",  SettingCategory::Visual  },
    {"background_color",       SettingCategory::Visual  },
    {"cursor_size",            SettingCategory::Visual  },
    {"cursor_theme",           SettingCategory::Visual  },
    {"inactive_opacity",       SettingCategory::Visual  },
    {"master_factor",          SettingCategory::Behavior},
    {"repeat_rate",            SettingCategory::Behavior},
    {"repeat_delay",           SettingCategory::Behavior},
    {"terminal",               SettingCategory::Behavior},
    {"launcher",               SettingCategory::Behavior},
    {"focus_follows_mouse",    SettingCategory::Behavior},
    {"dwindle_preserve_split", SettingCategory::Behavior},
};

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
    {"cursor_theme",
     [](lua_State* L, RuntimeConfig& s) {
         s.cursor_theme = luaL_checkstring(L, 2);
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
    {"focus_follows_mouse",
     [](lua_State* L, RuntimeConfig& s) {
         s.focus_follows_mouse = lua_toboolean(L, 2);
     }},
    {"inactive_opacity",
     [](lua_State* L, RuntimeConfig& s) {
         s.inactive_opacity = std::clamp(
             static_cast<float>(luaL_checknumber(L, 2)), 0.05f, 1.0f);
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
    {"cursor_theme",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushstring(L, s.cursor_theme.c_str());
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
    {"focus_follows_mouse",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushboolean(L, s.focus_follows_mouse);
     }},
    {"inactive_opacity",
     [](lua_State* L, const RuntimeConfig& s) {
         lua_pushnumber(L, s.inactive_opacity);
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

// String <-> WallpaperMode, same shape as parseTransform above. Returns
// false (leaving `out` untouched) for an unrecognized name instead of
// silently defaulting -- unlike a transform, there's no single "safe"
// wallpaper mode to fall back to that wouldn't surprise whoever typo'd
// the string, so l_wallpaper_set treats this as a hard error.
bool parseWallpaperMode(const char* s, WallpaperMode& out) {
    if(!s) { return false; }
    std::string m(s);
    if(m == "fill") {
        out = WallpaperMode::Fill;
        return true;
    }
    if(m == "fit") {
        out = WallpaperMode::Fit;
        return true;
    }
    if(m == "stretch") {
        out = WallpaperMode::Stretch;
        return true;
    }
    if(m == "center") {
        out = WallpaperMode::Center;
        return true;
    }
    if(m == "tile") {
        out = WallpaperMode::Tile;
        return true;
    }
    return false;
}

// Expands a leading "~" (or "~/...") to $HOME, same convention rc.lua
// itself already uses via os.getenv('HOME') string concatenation --
// uwu.wallpaper.set() accepts a bare "~/Pictures/wall.png" directly
// rather than requiring every rc.lua to spell out the concatenation
// itself. Leaves the path untouched (including any other "~" not at
// position 0) if $HOME isn't set, same "degrade, don't crash" spirit as
// everywhere else path-like input is handled in this file.
std::string expandHome(const std::string& path) {
    if(path.empty() || path[0] != '~') { return path; }
    const char* home = getenv("HOME");
    if(!home || !*home) { return path; }
    return std::string(home) + path.substr(1);
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

// c:set_tags_mask(mask) -- like set_tag(n), but takes the raw 0-indexed
// bitmask View::tags/setTags() already use internally (same value
// `client.tags` already reads back) instead of a single 1-based index.
// set_tag/toggle_tag cover the common single-active-tag case; this is
// the escape hatch for anything that needs to land a client on
// whatever *combination* of tags is currently visible (e.g. a
// multi-tag view via uwu.tag.toggle, or paw.specialworkspace re-tagging
// a scratchpad client onto the target output's exact live tagset rather
// than guessing at a single tag within it).
int l_client_set_tags_mask(lua_State* L) {
    View*    view = checkClient(L, 1);
    uint32_t mask = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    view->setTags(mask);
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

// c:move(x, y) / c:resize(w, h) -- floating-only, same restriction
// setFloating already implies: a tiled view's geo is computed by
// layout::arrange() every time anything changes, so a one-off setGeometry
// here would just get overwritten by the next arrange() pass (on focus
// change, another window opening/closing, etc.) instead of sticking --
// silently doing nothing is worse than the loud, cheap-to-debug error
// below. Both update floating_geo alongside geo, same as setFloating/
// setFullscreen already do, so a resize/move survives a fullscreen
// round-trip instead of being discarded on un-fullscreen restore.
int l_client_move(lua_State* L) {
    View* view = checkClient(L, 1);
    if(!view->is_floating) {
        luaL_error(L, "uwu.client: move() only works on a floating client");
        return 0;
    }
    wlr_box box        = view->geo;
    box.x              = static_cast<int>(luaL_checkinteger(L, 2));
    box.y              = static_cast<int>(luaL_checkinteger(L, 3));
    view->floating_geo = box;
    view->setGeometry(box);
    return 0;
}

int l_client_resize(lua_State* L) {
    View* view = checkClient(L, 1);
    if(!view->is_floating) {
        luaL_error(L, "uwu.client: resize() only works on a floating client");
        return 0;
    }
    wlr_box box        = view->geo;
    box.width          = std::max(1, static_cast<int>(luaL_checkinteger(L, 2)));
    box.height         = std::max(1, static_cast<int>(luaL_checkinteger(L, 3)));
    view->floating_geo = box;
    view->setGeometry(box);
    return 0;
}

// c:begin_move() / c:begin_resize() -- kick the compositor into an
// interactive drag-move/drag-resize grab for this client, the same grab
// XdgToplevel::handleRequestMove/handleRequestResize enter when a
// client's own CSD asks for one. Meant to be called from a
// uwu.mousebind() closure so rc.lua decides *when* a drag starts (which
// modifier+button combo) rather than it being hardwired into the button
// handler. Both are quiet no-ops on a tiled or fullscreen client (see
// View::beginInteractiveMove/Resize's own guard) rather than the loud
// luaL_error move()/resize() above use -- a mousebind fires on whatever's
// under the cursor, tiled or floating, and that's the expected common
// case rather than a mistake worth erroring over. begin_resize() doesn't
// take edges: it picks them from which quadrant of the window the cursor
// is currently over, see View::beginInteractiveResizeFromCursor.
int l_client_begin_move(lua_State* L) {
    View* view = checkClient(L, 1);
    view->beginMoveFromMousebind();
    return 0;
}

int l_client_begin_resize(lua_State* L) {
    View* view = checkClient(L, 1);
    view->beginResizeFromMousebind();
    return 0;
}

const luaL_Reg kClientMethods[] = {
    {"focus",          l_client_focus         },
    {"kill",           l_client_kill          },
    {"set_tag",        l_client_set_tag       },
    {"toggle_tag",     l_client_toggle_tag    },
    {"set_tags_mask",  l_client_set_tags_mask },
    {"move_to_output", l_client_move_to_output},
    {"move",           l_client_move          },
    {"resize",         l_client_resize        },
    {"begin_move",     l_client_begin_move    },
    {"begin_resize",   l_client_begin_resize  },
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
    if(k == "minimized") {
        lua_pushboolean(L, view->is_minimized);
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
    if(k == "border_color_active") {
        if(view->has_border_override) {
            lua_pushstring(
                L,
                colorToHexString(view->border_color_active_override).c_str());
        } else {
            lua_pushnil(L);
        }
        return 1;
    }
    if(k == "border_color_inactive") {
        if(view->has_border_override) {
            lua_pushstring(
                L,
                colorToHexString(view->border_color_inactive_override).c_str());
        } else {
            lua_pushnil(L);
        }
        return 1;
    }
    if(k == "opacity") {
        if(view->has_opacity_override) {
            lua_pushnumber(L, view->opacity_override);
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

// __newindex: the properties actually meant to be assignable
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
    if(k == "minimized") {
        view->setMinimized(lua_toboolean(L, 3));
        return 0;
    }
    if(k == "opacity") {
        view->has_opacity_override = true;
        view->opacity_override     = static_cast<float>(luaL_checknumber(L, 3));
        view->applyOpacityOverride();
        return 0;
    }
    if(k == "border_color_active" || k == "border_color_inactive") {
        Server* server = getServer(L);
        // First write on a view with no override yet seeds *both* colors
        // from the current global settings, so setting only one of the
        // pair (e.g. just border_color_active = "#f38ba8" for a "danger"
        // app) doesn't leave the other silently at some earlier default
        // -- it starts from whatever the rest of the compositor is
        // currently themed with, same as nyaa.wear() would produce.
        if(!view->has_border_override) {
            view->border_color_active_override =
                server->lua_cfg.settings.border_color_active;
            view->border_color_inactive_override =
                server->lua_cfg.settings.border_color_inactive;
            view->has_border_override = true;
        }
        uint32_t color = parseColor(luaL_checkstring(L, 3));
        if(k == "border_color_active") {
            view->border_color_active_override = color;
        } else {
            view->border_color_inactive_override = color;
        }
        view->updateBorderColor(server->focused_view == view);
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

// uwu.mousebind(mods, button, fn) -- see LuaMousebind's doc comment in
// lua_config.hpp. `fn` is called as fn(client) -- the view under the
// cursor when the bound button+modifier combo is pressed -- typically to
// call client:begin_move() or client:begin_resize(). Same "log and skip
// rather than luaL_error" treatment l_bind gives an unknown key name: one
// typo'd button name in rc.lua shouldn't silently discard every mousebind
// that follows it in the file.
int l_mousebind(lua_State* L) {
    uint32_t mods   = parseModTable(L, 1);
    uint32_t button = parseButtonName(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    if(button == 0) {
        wlr_log(WLR_ERROR,
                "uwu.mousebind: unknown button name, skipping this binding");
        return 0;
    }

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    LuaConfig* cfg = getConfig(L);
    cfg->mousebinds.push_back({mods, button, ref});
    return 0;
}

// Category-scoped setter/getter core, shared by uwu.visual.set/get and
// uwu.behavior.set/get below. `want` restricts which category this call
// site is allowed to touch -- a visual name reaching uwu.behavior.set (or
// vice versa) is refused with the same "unknown setting" tone as a
// genuine typo, rather than silently reaching across the boundary, since
// from the caller's side those two failure modes should look identical:
// either way, this isn't a field you get from here.
void dispatchSet(lua_State* L, SettingCategory want, const char* ns) {
    const char*    name = luaL_checkstring(L, 1);
    RuntimeConfig& s    = getConfig(L)->settings;

    auto cat = kSettingCategory.find(name);
    if(cat == kSettingCategory.end() || cat->second != want) {
        wlr_log(WLR_ERROR, "%s.set: unknown setting '%s', ignoring", ns, name);
        return;
    }
    kSettingSetters.at(name)(L, s);
}

void dispatchGet(lua_State* L, SettingCategory want, const char* ns) {
    const char*          name = luaL_checkstring(L, 1);
    const RuntimeConfig& s    = getConfig(L)->settings;

    auto cat = kSettingCategory.find(name);
    if(cat == kSettingCategory.end() || cat->second != want) {
        wlr_log(
            WLR_ERROR, "%s.get: unknown setting '%s', returning nil", ns, name);
        lua_pushnil(L);
        return;
    }
    kSettingGetters.at(name)(L, s);
}

// uwu.visual.set(name, value) -- nyaa's only way into RuntimeConfig.
// Refuses every name kSettingCategory doesn't tag Visual (that includes
// every Behavior field, not just typos), so nyaa can never reach a
// setting paw owns even if its own Lua-side field list were ever wrong or
// out of date.
int l_visual_set(lua_State* L) {
    dispatchSet(L, SettingCategory::Visual, "uwu.visual");
    return 0;
}

int l_visual_get(lua_State* L) {
    dispatchGet(L, SettingCategory::Visual, "uwu.visual");
    return 1;
}

// uwu.behavior.set(name, value) -- paw's counterpart to uwu.visual.set().
// Same refusal, mirrored: every Visual field is off limits here too.
int l_behavior_set(lua_State* L) {
    dispatchSet(L, SettingCategory::Behavior, "uwu.behavior");
    return 0;
}

int l_behavior_get(lua_State* L) {
    dispatchGet(L, SettingCategory::Behavior, "uwu.behavior");
    return 1;
}

// uwu.set(name, value) / uwu.get(name) -- deprecated, category-blind
// aliases kept only so an rc.lua written before uwu.visual/uwu.behavior
// existed keeps working. Reaches every setting regardless of category,
// same as before this pass; new config should call uwu.visual.*/
// uwu.behavior.* (or nyaa.wear()/paw.defaults(), which now do exactly
// that) directly instead.
int l_set(lua_State* L) {
    const char*    name = luaL_checkstring(L, 1);
    RuntimeConfig& s    = getConfig(L)->settings;

    if(auto it = kSettingSetters.find(name); it != kSettingSetters.end()) {
        wlr_log(WLR_INFO,
                "uwu.set: deprecated, use uwu.visual.set/uwu.behavior.set for "
                "'%s' instead",
                name);
        it->second(L, s);
    } else {
        wlr_log(WLR_ERROR, "uwu.set: unknown setting '%s', ignoring", name);
    }
    return 0;
}

// uwu.get(name) -- the read half uwu.set() never had. Returns nil (not an
// error) for an unknown name, same tolerance uwu.set() already extends
// the other direction, so a typo surfaces as "always nil" rather than a
// script-ending exception either way. Deprecated for the same reason
// uwu.set() is -- see its comment just above.
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

// ----------------------------------------------------------------------------
// uwu.layout.* -- tiling primitives, namespaced off the flat uwu table.
//
// set_layout/inc_master/dwindle_* used to sit directly on `uwu` alongside
// spawn/quit/bind -- lifecycle calls with nothing to do with tiling. paw
// (lib/paw/init.lua) is documented as "sugar over the raw uwu C API", and
// uwu.layout groups exactly the primitives paw.layout (see lib/paw/init.lua)
// wraps, so that relationship is a real namespace-to-namespace one instead
// of "sugar over eleven miscellaneous entries in a flat 22-function table."
// The old flat names (uwu.set_layout/uwu.inc_master/uwu.dwindle_*) have been
// removed outright -- uwu.layout.*/uwu.layout.dwindle.* is the only path to
// these now, so uwu/paw/nyaa stay three cleanly separated namespaces instead
// of uwu carrying both a namespaced and a flat copy of the same primitives.
int l_layout_set(lua_State* L) { return l_set_layout(L); }
int l_layout_inc_master(lua_State* L) { return l_inc_master(L); }
int l_layout_dwindle_toggle_split(lua_State* L) {
    return l_dwindle_toggle_split(L);
}
int l_layout_dwindle_swap_split(lua_State* L) {
    return l_dwindle_swap_split(L);
}
int l_layout_dwindle_rotate_split(lua_State* L) {
    return l_dwindle_rotate_split(L);
}
int l_layout_dwindle_splitratio(lua_State* L) {
    return l_dwindle_splitratio(L);
}
int l_layout_dwindle_move_to_root(lua_State* L) {
    return l_dwindle_move_to_root(L);
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

// uwu.tag.current([output_name]) -> raw tagset bitmask (the same value
// View::tags/client.tags/set_tags_mask() use) currently visible on
// `output_name`, or the focused output if omitted. nil if that output
// doesn't exist (or nothing's focused yet). This is the read side of
// what setTagset()/l_view_tag write -- exposed on its own because
// nothing before now needed to *read back* a tagset from Lua, only ever
// set one.
int l_tag_current(lua_State* L) {
    Server*     server = getServer(L);
    const char* name   = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    Output*     out    = server->focused_output;
    if(name) {
        out = nullptr;
        for(auto& o : server->outputs) {
            if(name == o->wlr_output->name) {
                out = o.get();
                break;
            }
        }
    }
    if(!out) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, out->tagset);
    }
    return 1;
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
// uwu.system.brightness / uwu.system.volume
// ----------------------------------------------------------------------------
//
// Deliberately outside every wlroots/libinput code path this file otherwise
// touches -- brightness is plain sysfs, volume shells out to `wpctl`
// (PipeWire's own CLI, not a libpipewire client embedded here) the same
// way uwu.spawn() already shells out for everything else uwuwm doesn't
// want to own a client library for. This is uwu.system rather than flat
// on `uwu` alongside spawn/bind -- it's OS/session state, not compositor
// state the way uwu.monitor/uwu.input are, and keeping that boundary
// visible in the namespace matches why those two are already nested.
//
// uwu.system.volume.* assumes a PipeWire session (wireplumber's `wpctl`),
// which is Arch/CachyOS's default audio stack today -- there's no
// PulseAudio/ALSA fallback. If wpctl isn't on $PATH, get() returns nil and
// set()/mute() are silent no-ops (same "missing tool, do nothing rather
// than crash" contract runCommandCapture's empty-string-on-failure return
// already gives everything downstream).

// uwu.system.brightness.get() -> integer percent 0-100, or nil if no
// backlight sysfs node exists at all (most desktops).
int l_system_brightness_get(lua_State* L) {
    std::string device = findBacklightDevice();
    if(device.empty()) {
        lua_pushnil(L);
        return 1;
    }
    int current = readBacklightFile(device, "brightness");
    int max     = readBacklightFile(device, "max_brightness");
    if(current < 0 || max <= 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>((current * 100.0) / max + 0.5));
    return 1;
}

// uwu.system.brightness.set(pct) -- pct clamped to 1..100 (0 is
// deliberately excluded: sysfs happily accepts a literal 0 and that's
// indistinguishable from a dead panel/backlight driver until you go
// find another way to turn the screen back on, so uwuwm just never
// writes it -- use a DPMS/output-power off, not brightness 0, for
// "screen off").
int l_system_brightness_set(lua_State* L) {
    int pct = static_cast<int>(luaL_checkinteger(L, 1));
    pct     = std::clamp(pct, 1, 100);

    std::string device = findBacklightDevice();
    if(device.empty()) {
        wlr_log(WLR_ERROR, "uwu.system.brightness: no backlight device found");
        return 0;
    }
    int max = readBacklightFile(device, "max_brightness");
    if(max <= 0) { return 0; }

    int   target = static_cast<int>((pct / 100.0) * max + 0.5);
    auto  path   = "/sys/class/backlight/" + device + "/brightness";
    FILE* f      = fopen(path.c_str(), "w");
    if(!f) {
        wlr_log(WLR_ERROR,
                "uwu.system.brightness: cannot write %s (permissions? "
                "need a udev rule granting your user write access)",
                path.c_str());
        return 0;
    }
    fprintf(f, "%d", target);
    fclose(f);
    return 0;
}

const luaL_Reg kSystemBrightnessFuncs[] = {
    {"get",   l_system_brightness_get},
    {"set",   l_system_brightness_set},
    {nullptr, nullptr                },
};

// uwu.system.volume.get() -> integer percent, or nil if wpctl isn't
// available/the query failed. Parses wpctl's own
// "Volume: 0.45\n" / "Volume: 0.45 [MUTED]\n" output format.
int l_system_volume_get(lua_State* L) {
    std::string out =
        runCommandCapture("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    double frac = 0.0;
    if(out.empty() || sscanf(out.c_str(), "Volume: %lf", &frac) != 1) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(frac * 100.0 + 0.5));
    return 1;
}

// uwu.system.volume.set(pct) -- pct clamped to 0..150 (wpctl allows
// boosting past 100%, same headroom wireplumber itself exposes; clamped
// here rather than left unbounded so a typo'd extra zero can't blast the
// output to some absurd multiple).
int l_system_volume_set(lua_State* L) {
    int pct         = static_cast<int>(luaL_checkinteger(L, 1));
    pct             = std::clamp(pct, 0, 150);
    std::string cmd = "wpctl set-volume @DEFAULT_AUDIO_SINK@ " +
                      std::to_string(pct) + "% 2>/dev/null";
    spawnCommand(cmd.c_str());
    return 0;
}

// uwu.system.volume.mute(bool) -- explicit set, not a toggle (see
// toggle_mute below for that).
int l_system_volume_mute(lua_State* L) {
    bool        mute = lua_toboolean(L, 1);
    std::string cmd  = std::string("wpctl set-mute @DEFAULT_AUDIO_SINK@ ") +
                       (mute ? "1" : "0") + " 2>/dev/null";
    spawnCommand(cmd.c_str());
    return 0;
}

int l_system_volume_toggle_mute(lua_State* L) {
    (void)L;
    spawnCommand("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle 2>/dev/null");
    return 0;
}

const luaL_Reg kSystemVolumeFuncs[] = {
    {"get",         l_system_volume_get        },
    {"set",         l_system_volume_set        },
    {"mute",        l_system_volume_mute       },
    {"toggle_mute", l_system_volume_toggle_mute},
    {nullptr,       nullptr                    },
};

// ----------------------------------------------------------------------------
// uwu.hook / uwu.unhook / uwu.rule
// ----------------------------------------------------------------------------

// uwu.hook(event, fn) -> id. `event` is one of "client::manage",
// "client::unmanage", "client::focus", "client::unfocus",
// "client::fullscreen", "client::title_changed" (fn receives the Client),
// "tag::change" (fn receives monitor_name, new_tagset), or "output::connect"/
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

// ----------------------------------------------------------------------------
// uwu.timer.set_interval / set_timeout / cancel
// ----------------------------------------------------------------------------

// uwu.timer.set_interval(seconds, fn) -> id. Calls fn every `seconds`
// (fractional allowed -- 0.5 is a valid half-second tick), starting
// `seconds` after this call, until uwu.timer.cancel(id) or a reload.
// Nothing in uwuwm had a clock-tick primitive before this -- a bar
// widget (see bar.hpp) redrawing a clock, or anything else that wants
// "run this periodically" instead of only ever reacting to a hook, needs
// it. fn receives no arguments, invoked through the same guardedCall
// timeout/recursion-guard path every keybind/hook already runs under.
int l_timer_set_interval(lua_State* L) {
    double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int ms  = static_cast<int>(seconds * 1000.0);
    int id  = getConfig(L)->addTimer(ref, ms, ms);
    lua_pushinteger(L, id);
    return 1;
}

// uwu.timer.set_timeout(seconds, fn) -> id. Calls fn exactly once,
// `seconds` from now, then automatically removes itself -- the id is
// still valid to pass to uwu.timer.cancel() beforehand, to call it off
// before it ever fires.
int l_timer_set_timeout(lua_State* L) {
    double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int ms  = static_cast<int>(seconds * 1000.0);
    int id  = getConfig(L)->addTimer(ref, ms, 0);
    lua_pushinteger(L, id);
    return 1;
}

// uwu.timer.cancel(id) -- stops a set_interval before its next tick, or
// a set_timeout before it ever fires. A no-op (not an error) on an
// already-fired one-shot or an already-cancelled id, same "unknown id is
// harmless" convention uwu.unhook() follows.
int l_timer_cancel(lua_State* L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    getConfig(L)->cancelTimer(id);
    return 0;
}

const luaL_Reg kTimerFuncs[] = {
    {"set_interval", l_timer_set_interval},
    {"set_timeout",  l_timer_set_timeout },
    {"cancel",       l_timer_cancel      },
    {nullptr,        nullptr             },
};

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
    //
    // That guarantee is per-rule only, not global: every uwu.rule() call
    // registers its own ordinary client::manage hook (see l_rule below),
    // and fireClientEvent runs every matching hook in registration order
    // against the same live View -- so a `match.floating = true` rule
    // registered *after* another rule that sets `apply.floating = true`
    // WILL see that earlier rule's change already applied. A "catch every
    // auto-detected floater" rule (rc.lua's own shipped example is one)
    // needs to be registered before any rule that itself floats a
    // client, or it'll also catch whatever those later rules just
    // floated -- registration order is the only lever here, there's no
    // per-rule priority/phase to reach for instead.
    if(has_match_floating && view->is_floating != match_floating) { return 0; }

    lua_pushvalue(L, lua_upvalueindex(5));  // the `apply` table
    bool        has_floating = false, has_fullscreen = false, has_tag = false;
    bool        floating = false, fullscreen = false;
    int         tag        = 0;
    bool        has_output = false;
    std::string output_name;
    bool        has_border_active = false, has_border_inactive = false;
    std::string border_active_str, border_inactive_str;
    bool        has_opacity = false;
    double      opacity     = 1.0;
    getOptionalField(L, -1, "floating", floating, has_floating);
    getOptionalField(L, -1, "fullscreen", fullscreen, has_fullscreen);
    getOptionalField(L, -1, "tag", tag, has_tag);
    getOptionalField(L, -1, "output", output_name, has_output);
    getOptionalField(
        L, -1, "border_color_active", border_active_str, has_border_active);
    getOptionalField(L,
                     -1,
                     "border_color_inactive",
                     border_inactive_str,
                     has_border_inactive);
    getOptionalField(L, -1, "opacity", opacity, has_opacity);
    lua_pop(L, 1);

    // Same seed-both-from-current-globals behavior as the client.
    // border_color_active/inactive property setter (l_client_newindex)
    // -- a rule setting only one of the pair still gets a fully-defined
    // override rather than half of it falling back to global settings.
    if(has_border_active || has_border_inactive) {
        Server* server = getServer(L);
        if(!view->has_border_override) {
            view->border_color_active_override =
                server->lua_cfg.settings.border_color_active;
            view->border_color_inactive_override =
                server->lua_cfg.settings.border_color_inactive;
            view->has_border_override = true;
        }
        if(has_border_active) {
            view->border_color_active_override =
                parseColor(border_active_str.c_str());
        }
        if(has_border_inactive) {
            view->border_color_inactive_override =
                parseColor(border_inactive_str.c_str());
        }
        view->updateBorderColor(server->focused_view == view);
    }

    // Same has_opacity_override/opacity_override pair uwu.client's opacity
    // setter (l_client_newindex) would seed -- see the View::opacity_override
    // comment in view.hpp. applyOpacityOverride() (also view.hpp) only
    // takes effect while unfocused (matching inactive_opacity's own
    // behavior); a currently-focused view stays at full opacity either
    // way.
    if(has_opacity) {
        view->has_opacity_override = true;
        view->opacity_override     = static_cast<float>(opacity);
        view->applyOpacityOverride();
    }

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
// tag=, output=, border_color_active=, border_color_inactive=, opacity=}})
// -- sugar over uwu.hook("client::manage", ...), not a separate
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
        lua_pushinteger(L, out->tagset);
        lua_setfield(L, -2, "tagset");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// uwu.monitor.focused() -> name string, or nil if nothing's focused yet
// (true only in the brief window before the first output connects). Every
// entry in list() already carries a `focused` bool -- this is the same
// data, just without every caller that only wants "which one" needing its
// own linear scan/pairs() loop over list()'s result.
int l_monitor_focused(lua_State* L) {
    Server* server = getServer(L);
    if(server->focused_output) {
        lua_pushstring(L, server->focused_output->wlr_output->name);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

const luaL_Reg kMonitorFuncs[] = {
    {"set",     l_monitor_set    },
    {"list",    l_monitor_list   },
    {"focused", l_monitor_focused},
    {nullptr,   nullptr          },
};

// ----------------------------------------------------------------------------
// uwu.wallpaper.* functions
// ----------------------------------------------------------------------------

// uwu.wallpaper.set(name, { path = "...", mode = "fill" }). `name` is a
// wlr_output name ("DP-1", "eDP-1", ...) or the wildcard "*" -- same
// ----------------------------------------------------------------------------
// uwu.bar.* -- the native status bar (bar.hpp/bar.cpp)
// ----------------------------------------------------------------------------

constexpr const char* kBarMeta = "uwu.Bar";

struct LuaBar {
    int id;
};

// Bar's own id -> unique_ptr<Bar> map (Server::bars) is the single
// source of truth for whether a bar is still alive -- keyed lookup by id
// rather than View's "scan a list for this raw pointer" (checkClient)
// specifically because a Bar's id, unlike a View*, has nothing else that
// could coincidentally reuse the same value after destruction to worry
// about.
Bar* checkBar(lua_State* L, int idx) {
    auto*   b      = static_cast<LuaBar*>(luaL_checkudata(L, idx, kBarMeta));
    Server* server = getServer(L);
    auto    it     = server->bars.find(b->id);
    if(it == server->bars.end()) {
        luaL_error(L, "uwu.bar: stale bar reference (already destroyed)");
    }
    return it->second.get();
}

void pushBar(lua_State* L, int id) {
    auto* b = static_cast<LuaBar*>(lua_newuserdata(L, sizeof(LuaBar)));
    b->id   = id;
    luaL_getmetatable(L, kBarMeta);
    lua_setmetatable(L, -2);
}

// uwu.bar.create({output=, position="top"/"bottom", height=}) -> Bar.
// `output` defaults to the currently focused one; `position` defaults
// to "top"; `height` defaults to 30px. Every field is a plain
// uwu.set()-style value, not a nested rule table -- a bar is a single
// concrete thing you're creating right now, not a pattern matched
// against clients that come and go later the way uwu.rule() is.
int l_bar_create(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    Server* server = getServer(L);

    std::string output_name;
    bool        has_output = false;
    getOptionalField(L, 1, "output", output_name, has_output);

    std::string position_str = "top";
    bool        has_position = false;
    getOptionalField(L, 1, "position", position_str, has_position);
    BarPosition position =
        (position_str == "bottom") ? BarPosition::Bottom : BarPosition::Top;

    double height_d    = 30;
    bool   has_height  = false;
    getOptionalField(L, 1, "height", height_d, has_height);
    int height = static_cast<int>(height_d);

    Output* target = nullptr;
    if(has_output) {
        for(auto& out : server->outputs) {
            if(output_name == out->wlr_output->name) {
                target = out.get();
                break;
            }
        }
        if(!target) {
            luaL_error(
                L, "uwu.bar.create: no such output '%s'", output_name.c_str());
            return 0;
        }
    } else {
        target = server->focused_output;
        if(!target && !server->outputs.empty()) {
            target = server->outputs.front().get();
        }
    }
    if(!target) {
        luaL_error(L, "uwu.bar.create: no output to attach to yet");
        return 0;
    }

    auto bar = std::make_unique<Bar>(*server, *target, position, height);
    int  id  = server->next_bar_id++;
    bar->id  = id;
    target->bars.push_back(bar.get());
    server->bars.emplace(id, std::move(bar));

    // Claims this bar's exclusive zone immediately -- the same
    // re-sweep-right-away treatment l_wallpaper_set gives "*" wallpaper
    // rules below, for the same reason: waiting for the next reload to
    // reserve space a bar that already exists is visibly wrong, not
    // just a stale-cache inconvenience.
    arrangeLayers(*target);

    pushBar(L, id);
    return 1;
}

int l_bar_destroy(lua_State* L) {
    Bar*    bar    = checkBar(L, 1);
    int     id     = bar->id;
    Output* output = bar->output;
    if(bar->click_fn_ref != -2) {
        luaL_unref(L, LUA_REGISTRYINDEX, bar->click_fn_ref);
    }
    getServer(L)->bars.erase(id);  // ~Bar() detaches from output->bars itself
    if(output) { arrangeLayers(*output); }  // release its exclusive zone
    return 0;
}

int l_bar_clear(lua_State* L) {
    Bar*     bar   = checkBar(L, 1);
    uint32_t color = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    bar->clear(color);
    return 0;
}

int l_bar_rect(lua_State* L) {
    Bar* bar = checkBar(L, 1);
    int  x   = static_cast<int>(luaL_checkinteger(L, 2));
    int  y   = static_cast<int>(luaL_checkinteger(L, 3));
    int  w   = static_cast<int>(luaL_checkinteger(L, 4));
    int  h   = static_cast<int>(luaL_checkinteger(L, 5));
    uint32_t color = static_cast<uint32_t>(luaL_checkinteger(L, 6));
    bar->fillRect(x, y, w, h, color);
    return 0;
}

// bar:text(x, y, str, font_path, pixel_size, color) -> advance_px. Pen
// position (x, y) is the text *baseline*, same convention text.hpp's
// TextRenderer::drawText uses -- y is where the bottom of a flat letter
// like "x" sits, not the top of the glyph box; a caller centering a
// label in a bar of known height wants bar:line_height() for that math,
// not this call's own return value (which is horizontal advance only).
int l_bar_text(lua_State* L) {
    Bar*        bar   = checkBar(L, 1);
    int         x     = static_cast<int>(luaL_checkinteger(L, 2));
    int         y     = static_cast<int>(luaL_checkinteger(L, 3));
    const char* utf8  = luaL_checkstring(L, 4);
    const char* font  = luaL_checkstring(L, 5);
    int         size  = static_cast<int>(luaL_checkinteger(L, 6));
    uint32_t    color = static_cast<uint32_t>(luaL_checkinteger(L, 7));
    lua_pushinteger(L, bar->drawText(x, y, utf8, font, size, color));
    return 1;
}

int l_bar_text_width(lua_State* L) {
    Bar*        bar  = checkBar(L, 1);
    const char* utf8 = luaL_checkstring(L, 2);
    const char* font = luaL_checkstring(L, 3);
    int         size = static_cast<int>(luaL_checkinteger(L, 4));
    lua_pushinteger(L, bar->textWidth(utf8, font, size));
    return 1;
}

int l_bar_line_height(lua_State* L) {
    Bar*        bar  = checkBar(L, 1);
    const char* font = luaL_checkstring(L, 2);
    int         size = static_cast<int>(luaL_checkinteger(L, 3));
    lua_pushinteger(L, bar->lineHeight(font, size));
    return 1;
}

int l_bar_commit(lua_State* L) {
    checkBar(L, 1)->commit();
    return 0;
}

// bar:on_click(fn) -- fn(x, y, button). Passing a new fn replaces
// whatever was registered before (unref'd first, not leaked); there's
// no uwu.bar.off_click() separate from this because a bar only ever
// needs at most one click handler, unlike uwu.hook()'s many-listeners-
// per-event model.
int l_bar_on_click(lua_State* L) {
    Bar* bar = checkBar(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if(bar->click_fn_ref != -2) {
        luaL_unref(L, LUA_REGISTRYINDEX, bar->click_fn_ref);
    }
    lua_pushvalue(L, 2);
    bar->click_fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

int l_bar_width(lua_State* L) {
    lua_pushinteger(L, checkBar(L, 1)->width);
    return 1;
}
int l_bar_height(lua_State* L) {
    lua_pushinteger(L, checkBar(L, 1)->height);
    return 1;
}

const luaL_Reg kBarMethods[] = {
    {"clear",       l_bar_clear      },
    {"rect",        l_bar_rect       },
    {"text",        l_bar_text       },
    {"text_width",  l_bar_text_width },
    {"line_height", l_bar_line_height},
    {"commit",      l_bar_commit     },
    {"on_click",    l_bar_on_click   },
    {"destroy",     l_bar_destroy    },
    {"width",       l_bar_width      },
    {"height",      l_bar_height     },
    {nullptr,       nullptr          },
};

const luaL_Reg kBarFuncs[] = {
    {"create", l_bar_create},
    {nullptr,  nullptr     },
};

// Pure-method userdata (no property-style uwu.bar.Client-esque
// getters/setters needed) -- self-referencing metatable is the standard
// idiom for that, so unlike registerClientMetatable this doesn't need
// its own __index/__newindex C functions, just __methods folded
// straight into __index.
void registerBarMetatable(lua_State* L) {
    luaL_newmetatable(L, kBarMeta);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, kBarMethods, 0);
    lua_pop(L, 1);
}

// ----------------------------------------------------------------------------
// uwu.qml.* -- the retained-mode QtQuick bar (qml_bar.hpp/qml_bar.cpp).
// Mirrors uwu.bar.*'s own structure immediately above almost exactly
// (id-keyed userdata looked up through Server::qml_bars, same
// stale-reference error convention) -- the one addition is
// uwu.QmlWidget, a second, lighter userdata returned by bar:rect()/
// bar:text() so a clock-style widget updated from uwu.timer() doesn't
// have to carry its parent bar around by hand to call set_text() on
// itself.
// ----------------------------------------------------------------------------

constexpr const char* kQmlBarMeta    = "uwu.QmlBar";
constexpr const char* kQmlWidgetMeta = "uwu.QmlWidget";

struct LuaQmlBar {
    int id;
};

struct LuaQmlWidget {
    int bar_id;
    int widget_id;
};

QmlBar* checkQmlBar(lua_State* L, int idx) {
    auto*   b      = static_cast<LuaQmlBar*>(luaL_checkudata(L, idx, kQmlBarMeta));
    Server* server = getServer(L);
    auto    it     = server->qml_bars.find(b->id);
    if(it == server->qml_bars.end()) {
        luaL_error(L, "uwu.qml: stale bar reference (already destroyed)");
    }
    return it->second.get();
}

void pushQmlBar(lua_State* L, int id) {
    auto* b = static_cast<LuaQmlBar*>(lua_newuserdata(L, sizeof(LuaQmlBar)));
    b->id   = id;
    luaL_getmetatable(L, kQmlBarMeta);
    lua_setmetatable(L, -2);
}

// Unlike checkQmlBar (a hard error on a stale bar, matching checkBar's
// own behavior), a stale *widget* -- its bar still alive but the
// widget_id no longer present -- is not an error here: QmlBar::setText
// & co already treat an unknown widget_id as a silent no-op (see
// qml_bar.hpp's comment on why), and this just extends that same
// leniency one layer out.
QmlBar* checkQmlWidgetBar(lua_State* L, int idx, int& out_widget_id) {
    auto*   w      = static_cast<LuaQmlWidget*>(luaL_checkudata(L, idx, kQmlWidgetMeta));
    Server* server = getServer(L);
    auto    it     = server->qml_bars.find(w->bar_id);
    if(it == server->qml_bars.end()) {
        luaL_error(L, "uwu.qml: stale widget reference (bar already destroyed)");
    }
    out_widget_id = w->widget_id;
    return it->second.get();
}

void pushQmlWidget(lua_State* L, int bar_id, int widget_id) {
    auto* w      = static_cast<LuaQmlWidget*>(lua_newuserdata(L, sizeof(LuaQmlWidget)));
    w->bar_id    = bar_id;
    w->widget_id = widget_id;
    luaL_getmetatable(L, kQmlWidgetMeta);
    lua_setmetatable(L, -2);
}

// uwu.qml.create({output=, position="top"/"bottom", height=}) -> QmlBar.
// Same fields, same defaults, same output-resolution fallback chain as
// uwu.bar.create() above -- see that function's comment.
int l_qml_create(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    Server* server = getServer(L);

    std::string output_name;
    bool        has_output = false;
    getOptionalField(L, 1, "output", output_name, has_output);

    std::string position_str = "top";
    bool        has_position = false;
    getOptionalField(L, 1, "position", position_str, has_position);
    BarPosition position =
        (position_str == "bottom") ? BarPosition::Bottom : BarPosition::Top;

    double height_d   = 30;
    bool   has_height = false;
    getOptionalField(L, 1, "height", height_d, has_height);
    int height = static_cast<int>(height_d);

    Output* target = nullptr;
    if(has_output) {
        for(auto& out : server->outputs) {
            if(output_name == out->wlr_output->name) {
                target = out.get();
                break;
            }
        }
        if(!target) {
            luaL_error(
                L, "uwu.qml.create: no such output '%s'", output_name.c_str());
            return 0;
        }
    } else {
        target = server->focused_output;
        if(!target && !server->outputs.empty()) {
            target = server->outputs.front().get();
        }
    }
    if(!target) {
        luaL_error(L, "uwu.qml.create: no output to attach to yet");
        return 0;
    }

    auto bar = std::make_unique<QmlBar>(
        *server, *target, position, height, *server->qt_runtime);
    int id  = server->next_qml_bar_id++;
    bar->id = id;
    target->qml_bars.push_back(bar.get());
    server->qml_bars.emplace(id, std::move(bar));

    // Same immediate re-sweep uwu.bar.create() does -- see that
    // function's comment.
    arrangeLayers(*target);

    pushQmlBar(L, id);
    return 1;
}

int l_qml_destroy(lua_State* L) {
    QmlBar* bar    = checkQmlBar(L, 1);
    int     id     = bar->id;
    Output* output = bar->output;
    if(bar->click_fn_ref != -2) {
        luaL_unref(L, LUA_REGISTRYINDEX, bar->click_fn_ref);
    }
    getServer(L)->qml_bars.erase(id);  // ~QmlBar() detaches from output itself
    if(output) { arrangeLayers(*output); }
    return 0;
}

// bar:rect({x=, y=, w=, h=, color=}) -> QmlWidget. Instantiated exactly
// once here; every later change goes through widget:set_color()/
// set_pos()/set_size() below, never a second rect() call for the same
// visual element.
int l_qml_rect(lua_State* L) {
    QmlBar* bar = checkQmlBar(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    int      x = 0, y = 0, w = 0, h = 0;
    uint32_t color      = 0x000000ffu;
    bool     has_unused = false;
    getOptionalField(L, 2, "x", x, has_unused);
    getOptionalField(L, 2, "y", y, has_unused);
    getOptionalField(L, 2, "w", w, has_unused);
    getOptionalField(L, 2, "h", h, has_unused);
    getOptionalField(L, 2, "color", color, has_unused);

    int widget_id = bar->createRect(x, y, w, h, color);
    pushQmlWidget(L, bar->id, widget_id);
    return 1;
}

// bar:text({x=, y=, text=, font_size=, color=}) -> QmlWidget.
int l_qml_text(lua_State* L) {
    QmlBar* bar = checkQmlBar(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    int         x = 0, y = 0, font_size = 14;
    std::string text;
    uint32_t    color      = 0xffffffffu;
    bool        has_unused = false;
    getOptionalField(L, 2, "x", x, has_unused);
    getOptionalField(L, 2, "y", y, has_unused);
    getOptionalField(L, 2, "text", text, has_unused);
    getOptionalField(L, 2, "font_size", font_size, has_unused);
    getOptionalField(L, 2, "color", color, has_unused);

    int widget_id = bar->createText(x, y, text, font_size, color);
    pushQmlWidget(L, bar->id, widget_id);
    return 1;
}

int l_qml_commit(lua_State* L) {
    checkQmlBar(L, 1)->commit();
    return 0;
}

int l_qml_on_click(lua_State* L) {
    QmlBar* bar = checkQmlBar(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if(bar->click_fn_ref != -2) {
        luaL_unref(L, LUA_REGISTRYINDEX, bar->click_fn_ref);
    }
    lua_pushvalue(L, 2);
    bar->click_fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

int l_qml_width(lua_State* L) {
    lua_pushinteger(L, checkQmlBar(L, 1)->width);
    return 1;
}
int l_qml_height(lua_State* L) {
    lua_pushinteger(L, checkQmlBar(L, 1)->height);
    return 1;
}

// widget:set_text(str) -- plain property poke, no re-parsing/
// recreation (see qml_bar.hpp's own comment on why this is the whole
// point of the retained-mode redesign).
int l_qml_widget_set_text(lua_State* L) {
    int         widget_id;
    QmlBar*     bar  = checkQmlWidgetBar(L, 1, widget_id);
    const char* text = luaL_checkstring(L, 2);
    bar->setText(widget_id, text);
    return 0;
}

int l_qml_widget_set_color(lua_State* L) {
    int      widget_id;
    QmlBar*  bar   = checkQmlWidgetBar(L, 1, widget_id);
    uint32_t color = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    bar->setColor(widget_id, color);
    return 0;
}

int l_qml_widget_set_pos(lua_State* L) {
    int     widget_id;
    QmlBar* bar = checkQmlWidgetBar(L, 1, widget_id);
    int     x   = static_cast<int>(luaL_checkinteger(L, 2));
    int     y   = static_cast<int>(luaL_checkinteger(L, 3));
    bar->setPos(widget_id, x, y);
    return 0;
}

int l_qml_widget_set_size(lua_State* L) {
    int     widget_id;
    QmlBar* bar = checkQmlWidgetBar(L, 1, widget_id);
    int     w   = static_cast<int>(luaL_checkinteger(L, 2));
    int     h   = static_cast<int>(luaL_checkinteger(L, 3));
    bar->setSize(widget_id, w, h);
    return 0;
}

const luaL_Reg kQmlBarMethods[] = {
    {"rect",     l_qml_rect    },
    {"text",     l_qml_text    },
    {"commit",   l_qml_commit  },
    {"on_click", l_qml_on_click},
    {"destroy",  l_qml_destroy },
    {"width",    l_qml_width   },
    {"height",   l_qml_height  },
    {nullptr,    nullptr       },
};

const luaL_Reg kQmlWidgetMethods[] = {
    {"set_text",  l_qml_widget_set_text },
    {"set_color", l_qml_widget_set_color},
    {"set_pos",   l_qml_widget_set_pos  },
    {"set_size",  l_qml_widget_set_size },
    {nullptr,     nullptr               },
};

const luaL_Reg kQmlFuncs[] = {
    {"create", l_qml_create},
    {nullptr,  nullptr     },
};

void registerQmlBarMetatable(lua_State* L) {
    luaL_newmetatable(L, kQmlBarMeta);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, kQmlBarMethods, 0);
    lua_pop(L, 1);
}

void registerQmlWidgetMetatable(lua_State* L) {
    luaL_newmetatable(L, kQmlWidgetMeta);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, kQmlWidgetMethods, 0);
    lua_pop(L, 1);
}


// uwu.wallpaper.set(name, {path=, mode=}) -- name is an output name or
// "*" for every output not otherwise matched by a more specific rule,
// exact-name-wins-over-wildcard precedence as uwu.monitor.set, see
// findWallpaperRule in wallpaper.cpp. `path` is required; `mode`
// defaults to "fill" if omitted (one of fill/fit/stretch/center/tile --
// see WallpaperMode's doc comment in wallpaper.hpp for what each one
// does). A leading "~" in `path` is expanded to $HOME.
//
// Unlike l_monitor_set (which only re-applies live to an exact-name
// match; a "*" rule needs a reload to reach already-connected outputs),
// this re-sweeps *every* currently-connected output through
// findWallpaperRule immediately after storing the rule -- "*" is the
// common case for a wallpaper (one image for every monitor), not the
// exception, so it isn't treated as a second-class case here the way it
// is for uwu.monitor.set.
//
// Decode errors (missing file, corrupt/unsupported image) are logged by
// loadWallpaperImage (wallpaper.cpp) and leave background_color as the
// only thing visible on the affected output(s) -- never a hard Lua
// error, so a bad path in rc.lua can't take the rest of a reload down
// with it.
int l_wallpaper_set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_getfield(L, 2, "path");
    const char* raw_path = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    WallpaperRule rule;
    rule.name = name;
    rule.path = expandHome(raw_path);

    lua_getfield(L, 2, "mode");
    if(!lua_isnil(L, -1)) {
        const char* mode_str = luaL_checkstring(L, -1);
        if(!parseWallpaperMode(mode_str, rule.mode)) {
            lua_pop(L, 1);
            return luaL_error(L,
                              "uwu.wallpaper.set: unknown mode '%s' (known: "
                              "fill, fit, stretch, center, tile)",
                              mode_str);
        }
    }
    lua_pop(L, 1);

    // Re-decoding on every uwu.wallpaper.set() call for the same path
    // (rather than trusting the cache) means editing a wallpaper file in
    // place and re-running the same rc.lua line always picks up the
    // change -- see loadWallpaperImage/forgetWallpaperImage's own doc
    // comments in wallpaper.hpp for why the cache exists at all (sharing
    // one decode across every output/tile that uses the same path).
    forgetWallpaperImage(rule.path);

    LuaConfig* cfg = getConfig(L);
    auto       it  = std::find_if(
        cfg->wallpaper_rules.begin(),
        cfg->wallpaper_rules.end(),
        [&](const WallpaperRule& r) { return r.name == rule.name; });
    if(it != cfg->wallpaper_rules.end()) {
        *it = rule;
    } else {
        cfg->wallpaper_rules.push_back(rule);
    }

    Server* server = getServer(L);
    for(auto& out : server->outputs) { applyWallpaper(*out); }
    return 0;
}

// uwu.wallpaper.clear(name) -- removes name's wallpaper rule ("*" clears
// the fallback default, not every output's wallpaper at once; clear each
// exact name individually too if that's what you want). Re-sweeps every
// connected output the same way l_wallpaper_set does, so an output that
// was only getting a wallpaper via "*" immediately falls back to
// background_color if you clear the wildcard, and vice versa. A no-op,
// not an error, if `name` had no rule to begin with.
int l_wallpaper_clear(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    LuaConfig* cfg = getConfig(L);
    std::erase_if(cfg->wallpaper_rules,
                  [&](const WallpaperRule& r) { return r.name == name; });

    Server* server = getServer(L);
    for(auto& out : server->outputs) { applyWallpaper(*out); }
    return 0;
}

// uwu.wallpaper.list() -> array of {name, path, mode} for every rule
// currently set via uwu.wallpaper.set() -- one entry per *rule* (i.e.
// per name/"*" selector), not one per connected output the way
// uwu.monitor.list() is. Cross-reference against uwu.monitor.list()'s
// own `name` field plus findWallpaperRule's exact-then-wildcard
// precedence if you need "what wallpaper is output X actually showing".
int l_wallpaper_list(lua_State* L) {
    LuaConfig* cfg = getConfig(L);
    lua_newtable(L);
    int i = 1;
    for(const WallpaperRule& rule : cfg->wallpaper_rules) {
        lua_newtable(L);
        lua_pushstring(L, rule.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, rule.path.c_str());
        lua_setfield(L, -2, "path");
        const char* mode_str = "fill";
        switch(rule.mode) {
            case WallpaperMode::Fill:
                mode_str = "fill";
                break;
            case WallpaperMode::Fit:
                mode_str = "fit";
                break;
            case WallpaperMode::Stretch:
                mode_str = "stretch";
                break;
            case WallpaperMode::Center:
                mode_str = "center";
                break;
            case WallpaperMode::Tile:
                mode_str = "tile";
                break;
        }
        lua_pushstring(L, mode_str);
        lua_setfield(L, -2, "mode");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

const luaL_Reg kWallpaperFuncs[] = {
    {"set",   l_wallpaper_set  },
    {"clear", l_wallpaper_clear},
    {"list",  l_wallpaper_list },
    {nullptr, nullptr          },
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
    {"current",          l_tag_current     },
    {nullptr,            nullptr           },
};

const luaL_Reg kDwindleFuncs[] = {
    {"toggle_split", l_layout_dwindle_toggle_split},
    {"swap_split",   l_layout_dwindle_swap_split  },
    {"rotate_split", l_layout_dwindle_rotate_split},
    {"splitratio",   l_layout_dwindle_splitratio  },
    {"move_to_root", l_layout_dwindle_move_to_root},
    {nullptr,        nullptr                      },
};

const luaL_Reg kLayoutFuncs[] = {
    {"set",        l_layout_set       },
    {"inc_master", l_layout_inc_master},
    {nullptr,      nullptr            },
};

const luaL_Reg kVisualFuncs[] = {
    {"set",   l_visual_set},
    {"get",   l_visual_get},
    {nullptr, nullptr     },
};

const luaL_Reg kBehaviorFuncs[] = {
    {"set",   l_behavior_set},
    {"get",   l_behavior_get},
    {nullptr, nullptr       },
};

const luaL_Reg kuwuwmFuncs[] = {
    {"spawn",             l_spawn            },
    {"bind",              l_bind             },
    {"mousebind",         l_mousebind        },
    {"set",               l_set              },
    {"get",               l_get              },
    {"quit",              l_quit             },
    {"reload",            l_reload           },
    {"hook",              l_hook             },
    {"unhook",            l_unhook           },
    {"rule",              l_rule             },
    {"kill",              l_kill             },
    {"focus_next",        l_focus_next       },
    {"focus_prev",        l_focus_prev       },
    {"toggle_floating",   l_toggle_floating  },
    {"toggle_fullscreen", l_toggle_fullscreen},
    {"focus_monitor",     l_focus_monitor    },
    {nullptr,             nullptr            },
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
-- uwu.set("cursor_theme", "Qogir")  -- must be installed under an Xcursor search path

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
uwu.bind({"mod"}, "i", function() uwu.layout.inc_master(0.05) end)
uwu.bind({"mod"}, "u", function() uwu.layout.inc_master(-0.05) end)
uwu.bind({"mod"}, "Tab", function() uwu.focus_monitor(1) end)

-- Dwindle (Hyprland-style BSP) tiling is available as an alternative to
-- the default master-stack layout -- uncomment to switch the focused
-- monitor to it, and to get togglesplit/swapsplit/splitratio bindings:
-- uwu.layout.set("dwindle")
-- uwu.bind({"mod"}, "p", function() uwu.layout.dwindle.toggle_split() end)
-- uwu.bind({"mod", "shift"}, "p", function() uwu.layout.dwindle.swap_split() end)
-- uwu.bind({"mod"}, "r", function() uwu.layout.dwindle.rotate_split() end)
-- uwu.bind({"mod"}, "minus", function() uwu.layout.dwindle.splitratio(-0.1) end)
-- uwu.bind({"mod"}, "equal", function() uwu.layout.dwindle.splitratio(0.1) end)

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
// -- this is where `require("paw")`/`require("nyaa")` look for a
// user override of the shipped lib/ tree (e.g. ~/.config/uwuwm/lib/
// paw/init.lua replacing the built-in one) before falling back to the
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

// Prepends every place rc.lua's require("paw")/require("nyaa")
// (and anything either of those transitively requires) should be
// found, highest-priority first:
//
//   1. <user config dir>/lib/          -- lets a user drop in their own
//      paw/nyaa (or add a sibling module) that shadows the
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
    registerBarMetatable(L);
    registerQmlBarMetatable(L);
    registerQmlWidgetMetatable(L);

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
    luaL_setfuncs(L, kWallpaperFuncs, 0);
    lua_setfield(L, -2, "wallpaper");

    lua_newtable(L);
    luaL_setfuncs(L, kTimerFuncs, 0);
    lua_setfield(L, -2, "timer");

    lua_newtable(L);
    luaL_setfuncs(L, kBarFuncs, 0);
    lua_setfield(L, -2, "bar");

    lua_newtable(L);
    luaL_setfuncs(L, kQmlFuncs, 0);
    lua_setfield(L, -2, "qml");

    lua_newtable(L);
    luaL_setfuncs(L, kTagFuncs, 0);
    lua_setfield(L, -2, "tag");

    lua_newtable(L);
    luaL_setfuncs(L, kInputFuncs, 0);
    lua_setfield(L, -2, "input");

    lua_newtable(L);
    luaL_setfuncs(L, kClientFuncs, 0);
    lua_setfield(L, -2, "client");

    // uwu.layout.{set,inc_master,dwindle.*} -- tiling primitives, split out
    // of the flat kuwuwmFuncs table they used to share with spawn/bind/quit.
    // paw.layout (lib/paw/init.lua) wraps exactly this subtable now, the
    // same "sugar over a raw uwu.* namespace" relationship uwu.monitor/
    // uwu.tag/uwu.input already have with their own callers. See the
    // kDwindleFuncs/kLayoutFuncs comment above l_layout_set for the reasoning.
    lua_newtable(L);
    luaL_setfuncs(L, kLayoutFuncs, 0);
    lua_newtable(L);
    luaL_setfuncs(L, kDwindleFuncs, 0);
    lua_setfield(L, -2, "dwindle");
    lua_setfield(L, -2, "layout");

    // uwu.visual.{set,get} / uwu.behavior.{set,get} -- the category-enforced
    // successors to the flat uwu.set()/uwu.get() (still registered above as
    // deprecated aliases). nyaa calls uwu.visual.*; paw calls uwu.behavior.*;
    // neither can reach the other's fields even by accident, because the
    // category check lives in dispatchSet/dispatchGet against
    // kSettingCategory, not in either caller. See the kSettingCategory
    // comment near the top of this file for what this replaces.
    lua_newtable(L);
    luaL_setfuncs(L, kVisualFuncs, 0);
    lua_setfield(L, -2, "visual");

    lua_newtable(L);
    luaL_setfuncs(L, kBehaviorFuncs, 0);
    lua_setfield(L, -2, "behavior");

    // uwu.system.{brightness,volume} -- OS/session state, not compositor
    // state, hence its own top-level namespace rather than living flat
    // alongside uwu.monitor/uwu.input (see the section comment above
    // kSystemBrightnessFuncs for why).
    lua_newtable(L);
    lua_newtable(L);
    luaL_setfuncs(L, kSystemBrightnessFuncs, 0);
    lua_setfield(L, -2, "brightness");
    lua_newtable(L);
    luaL_setfuncs(L, kSystemVolumeFuncs, 0);
    lua_setfield(L, -2, "volume");
    lua_setfield(L, -2, "system");

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

void LuaConfig::invokeWithClient(int fn_ref, View* view) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    pushClient(L, view);
    guardedCall(1);

    // Same deferred-reload handling as invoke() above -- a mousebind
    // closure is just as free to call uwu.reload() as a regular keybind
    // is, and needs the same "wait until the Lua call stack is empty"
    // treatment.
    if(reload_pending) {
        wlr_log(WLR_INFO, "deferred reload requested from mousebind callback");
        reload_pending = false;
        server_->reloadConfig();
    }
}

void LuaConfig::invokeBarClick(int fn_ref, int x, int y, uint32_t button) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    lua_pushinteger(L, static_cast<lua_Integer>(button));
    guardedCall(3);

    // Same deferred-reload treatment as invoke()/invokeWithClient() --
    // see invoke()'s own comment for why this can't just reload inline.
    if(reload_pending) {
        wlr_log(WLR_INFO, "deferred reload requested from bar click callback");
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

namespace {
// The wl_event_source callback every LuaTimer's `source` invokes.
// `data` is the LuaTimer* itself (see addTimer below) -- everything this
// needs out of it is copied into locals before calling invoke(), because
// invoke() running arbitrary Lua can itself call uwu.timer.cancel() on
// *this exact timer* (or trigger a reload), either of which erases the
// LuaTimer this callback's `data` pointer was into. Nothing below the
// invoke() call may dereference `timer` again for that reason -- the
// one-shot branch only erases by id, never through the (possibly
// already-dangling) pointer.
int timerTrampoline(void* data) {
    auto*      timer       = static_cast<LuaTimer*>(data);
    LuaConfig* config      = timer->config;
    int        fn_ref      = timer->fn_ref;
    int        interval_ms = timer->interval_ms;
    int        id          = timer->id;

    config->invoke(fn_ref);

    // A reload (or uwu.timer.cancel(id) called on itself, from fn_ref)
    // may have already destroyed this timer from inside invoke() above
    // -- re-look-up by id rather than trusting `timer` still points at
    // live memory before touching it again.
    auto it = config->timers.find(id);
    if(it == config->timers.end()) { return 0; }

    if(interval_ms > 0) {
        wl_event_source_timer_update(it->second->source, interval_ms);
    } else {
        config->timers.erase(it);
    }
    return 0;
}
}  // namespace

int LuaConfig::addTimer(int fn_ref, int delay_ms, int interval_ms) {
    auto timer          = std::make_unique<LuaTimer>();
    timer->config       = this;
    timer->fn_ref       = fn_ref;
    timer->interval_ms  = interval_ms;
    timer->id           = next_timer_id++;
    int id              = timer->id;

    wl_event_loop* loop = wl_display_get_event_loop(server_->display);
    timer->source = wl_event_loop_add_timer(loop, timerTrampoline, timer.get());
    wl_event_source_timer_update(timer->source, std::max(delay_ms, 1));

    timers.emplace(id, std::move(timer));
    return id;
}

void LuaConfig::cancelTimer(int id) {
    auto it = timers.find(id);
    if(it == timers.end()) { return; }
    luaL_unref(L, LUA_REGISTRYINDEX, it->second->fn_ref);
    timers.erase(it);
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
    lua_State*                          old_L          = L;
    std::multimap<uint32_t, LuaKeybind> old_keybinds   = std::move(keybinds);
    std::vector<LuaMousebind>           old_mousebinds = std::move(mousebinds);
    std::vector<MonitorRule>            old_rules  = std::move(monitor_rules);
    std::vector<InputRule>     old_input_rules     = std::move(input_rules);
    std::vector<WallpaperRule> old_wallpaper_rules = std::move(wallpaper_rules);
    std::vector<LuaHook>       old_hooks           = std::move(hooks);
    std::unordered_map<int, std::unique_ptr<LuaTimer>> old_timers =
        std::move(timers);
    RuntimeConfig old_settings     = settings;
    int           old_next_hook_id = next_hook_id;

    keybinds.clear();
    mousebinds.clear();
    monitor_rules.clear();
    input_rules.clear();
    wallpaper_rules.clear();
    hooks.clear();
    timers.clear();
    next_hook_id = 1;
    settings     = RuntimeConfig{};
    L            = nullptr;

    init(*server_);
    bool ok = load();

    if(ok) {
        if(old_L) { lua_close(old_L); }
        // old_timers' unique_ptrs go out of scope right here, each
        // ~LuaTimer tearing down its wl_event_source -- their fn_refs
        // pointed into old_L, already closed above, so there's nothing
        // left to individually luaL_unref (same reasoning as hooks: a
        // whole-state lua_close() invalidates every ref in it without
        // needing per-ref cleanup first).
        wlr_log(WLR_INFO,
                "rc.lua reload ok: %zu keybind(s), %zu mousebind(s), "
                "%zu monitor rule(s), %zu input rule(s), %zu wallpaper "
                "rule(s), %zu hook(s), %zu timer(s)",
                keybinds.size(),
                mousebinds.size(),
                monitor_rules.size(),
                input_rules.size(),
                wallpaper_rules.size(),
                hooks.size(),
                timers.size());
    } else {
        if(L) { lua_close(L); }
        L               = old_L;
        keybinds        = std::move(old_keybinds);
        mousebinds      = std::move(old_mousebinds);
        monitor_rules   = std::move(old_rules);
        input_rules     = std::move(old_input_rules);
        wallpaper_rules = std::move(old_wallpaper_rules);
        hooks           = std::move(old_hooks);
        // Same treatment as the success branch, just the other way
        // around -- `timers` (populated during the *failed* load, before
        // it errored out) gets torn down here instead, and old_timers
        // (which survived because old_L is being restored, not closed)
        // comes back.
        timers.clear();
        timers          = std::move(old_timers);
        next_hook_id    = old_next_hook_id;
        settings        = old_settings;
        wlr_log(WLR_ERROR,
                "rc.lua reload failed, keeping previous config running");
    }
    return ok;
}
