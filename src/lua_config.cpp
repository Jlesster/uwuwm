#include "lua_config.hpp"
#include "utils.hpp"
#include <unordered_map>
#include <functional>

extern "C" {

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
}

#include "config.hpp"
#include "layout.hpp"
#include "output.hpp"
#include "server.hpp"
#include "toplevel.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
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

template<typename T>
void getOptionalField(lua_State* L, int table_idx, const char* field, T& out, bool& has_value) {
    lua_getfield(L, table_idx, field);
    if(!lua_isnil(L, -1)) {
        has_value = true;
        if constexpr (std::is_same_v<T, int>) {
            out = static_cast<int>(luaL_checknumber(L, -1));
        } else if constexpr (std::is_same_v<T, float>) {
            out = static_cast<float>(luaL_checknumber(L, -1));
        } else if constexpr (std::is_same_v<T, bool>) {
            out = lua_toboolean(L, -1);
        } else if constexpr (std::is_same_v<T, std::string>) {
            out = luaL_checkstring(L, -1);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
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

using SetterFunc = std::function<void(lua_State*, RuntimeConfig&)>;
const std::unordered_map<std::string, SetterFunc> kSettingSetters = {
    {"gap",               [](lua_State* L, RuntimeConfig& s) { s.gap_px = static_cast<int>(luaL_checknumber(L, 2)); }},
    {"border_width",      [](lua_State* L, RuntimeConfig& s) { s.border_px = static_cast<int>(luaL_checknumber(L, 2)); }},
    {"master_factor",     [](lua_State* L, RuntimeConfig& s) { s.master_factor = luaL_checknumber(L, 2); }},
    {"repeat_rate",       [](lua_State* L, RuntimeConfig& s) { s.repeat_rate_hz = static_cast<int>(luaL_checknumber(L, 2)); }},
    {"repeat_delay",       [](lua_State* L, RuntimeConfig& s) { s.repeat_delay_ms = static_cast<int>(luaL_checknumber(L, 2)); }},
    {"cursor_size",       [](lua_State* L, RuntimeConfig& s) { s.cursor_size = luaL_checknumber(L, 2); }},
    {"terminal",          [](lua_State* L, RuntimeConfig& s) { s.terminal = luaL_checkstring(L, 2); }},
    {"launcher",          [](lua_State* L, RuntimeConfig& s) { s.launcher = luaL_checkstring(L, 2); }},
    {"border_color_active",   [](lua_State* L, RuntimeConfig& s) { s.border_color_active = parseColor(luaL_checkstring(L, 2)); }},
    {"border_color_inactive", [](lua_State* L, RuntimeConfig& s) { s.border_color_inactive = parseColor(luaL_checkstring(L, 2)); }},
    {"background_color",      [](lua_State* L, RuntimeConfig& s) { parseColorRgba(luaL_checkstring(L, 2), s.background_color); }},
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
    cfg->keybinds.insert({static_cast<uint32_t>(sym), {mods, static_cast<uint32_t>(sym), ref}});
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

    auto it =
        std::find(visible.begin(), visible.end(), server->focused_view);
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
    Server*   server = getServer(L);
    View* t      = server->focused_view;
    if(t && !t->is_fullscreen) {
        t->is_floating = !t->is_floating;
        if(t->is_floating) { t->floating_geo = t->geo; }
        if(t->output) { layout::arrange(*t->output); }
    }
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

// Tag indices are 1-based on the Lua side (uwu.tag_count == 9, valid
// arguments 1..9) to match normal Lua array convention; converted to the
// internal 0-based bit index here.
int l_view_tag(lua_State* L) {
    int     n      = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    Server* server = getServer(L);
    Output* out    = server->focused_output;
    if(out && n >= 0 && n < cfg::kTagCount) {
        out->tagset = 1u << n;
        layout::arrange(*out);
    }
    return 0;
}

int l_toggle_tag(lua_State* L) {
    int     n      = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    Server* server = getServer(L);
    Output* out    = server->focused_output;
    if(out && n >= 0 && n < cfg::kTagCount) {
        out->tagset ^= 1u << n;
        if(out->tagset == 0) { out->tagset = 1u << n; }
        layout::arrange(*out);
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
    if(has_ref) { rule.refresh_mhz = static_cast<int>(rule.refresh_mhz * 1000.0 + 0.5); }
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

const luaL_Reg kuwuwmFuncs[] = {
    {"spawn",             l_spawn            },
    {"bind",              l_bind             },
    {"set",               l_set              },
    {"quit",              l_quit             },
    {"reload",            l_reload           },
    {"kill",              l_kill             },
    {"focus_next",        l_focus_next       },
    {"focus_prev",        l_focus_prev       },
    {"toggle_floating",   l_toggle_floating  },
    {"toggle_fullscreen", l_toggle_fullscreen},
    {"inc_master",        l_inc_master       },
    {"view_tag",          l_view_tag         },
    {"toggle_tag",        l_toggle_tag       },
    {"move_to_tag",       l_move_to_tag      },
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

for i = 1, uwu.tag_count do
	uwu.bind({"mod"}, tostring(i), function() uwu.view_tag(i) end)
	uwu.bind({"mod", "shift"}, tostring(i), function() uwu.move_to_tag(i) end)
	if i <= 5 then
		uwu.bind({"mod", "ctrl"}, tostring(i), function() uwu.toggle_tag(i) end)
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

}  // namespace

LuaConfig::LuaConfig() = default;

LuaConfig::~LuaConfig() {
    if(L) { lua_close(L); }
}

void LuaConfig::init(Server& server) {
    server_ = &server;

    L = luaL_newstate();
    luaL_openlibs(L);

    lua_pushlightuserdata(L, &server);
    lua_setfield(L, LUA_REGISTRYINDEX, kServerKey);
    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kConfigKey);

    lua_newtable(L);
    luaL_setfuncs(L, kuwuwmFuncs, 0);
    lua_pushinteger(L, cfg::kTagCount);
    lua_setfield(L, -2, "tag_count");

    // uwu.monitor.{set,list} live in their own subtable rather than flat
    // in `uwu` alongside spawn/bind/etc, since "uwu.monitor.set(...)"
    // reads much closer to wlr-randr/Hyprland monitor-config syntax than
    // a flat "uwu.set_monitor(...)" would, and leaves room to grow (e.g.
    // uwu.monitor.focus) without further crowding the top-level table.
    lua_newtable(L);
    luaL_setfuncs(L, kMonitorFuncs, 0);
    lua_setfield(L, -2, "monitor");

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

void LuaConfig::invoke(int fn_ref) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    if(lua_pcall(L, 0, 0, 0) != LUA_OK) {
        wlr_log(WLR_ERROR, "keybind callback error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

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

bool LuaConfig::reload() {
    // Snapshot the current, known-working state before touching anything.
    // If the new script fails to load (syntax error, runtime error before
    // any/all uwu.bind() calls run, etc.) we roll back to this instead of
    // leaving the compositor with an empty keybind set -- which would
    // include losing the very reload keybind you just used to get here.
    lua_State*               old_L        = L;
    std::multimap<uint32_t, LuaKeybind> old_keybinds = std::move(keybinds);
    std::vector<MonitorRule> old_rules    = std::move(monitor_rules);
    RuntimeConfig            old_settings = settings;

    keybinds.clear();
    monitor_rules.clear();
    settings = RuntimeConfig{};
    L        = nullptr;

    init(*server_);
    bool ok = load();

    if(ok) {
        if(old_L) { lua_close(old_L); }
        wlr_log(WLR_INFO,
                "rc.lua reload ok: %zu keybind(s), %zu monitor rule(s)",
                keybinds.size(),
                monitor_rules.size());
    } else {
        if(L) { lua_close(L); }
        L             = old_L;
        keybinds      = std::move(old_keybinds);
        monitor_rules = std::move(old_rules);
        settings      = old_settings;
        wlr_log(WLR_ERROR,
                "rc.lua reload failed, keeping previous config running");
    }
    return ok;
}
