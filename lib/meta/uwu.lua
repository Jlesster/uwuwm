---@meta uwu
-- Type annotations for uwuwm's `uwu` global -- consumed by
-- lua-language-server (LuaLS, the VSCode/Neovim/etc Lua LSP; this style
-- of comment is the LuaCATS/EmmyLua annotation format it reads). This
-- file is declarations only: `---@meta` at the top tells LuaLS never to
-- treat it as real, runnable code and never to flag it as an unused/
-- orphan file, so it doesn't need requiring and isn't on uwu's own
-- package.path search list (see extendPackagePath() in lua_config.cpp)
-- the way lib/nyaa and lib/paw are.
--
-- Point your LSP at this file by adding it to workspace.library in
-- .luarc.json, e.g.:
--
--   { "workspace.library": ["lib/meta"] }
--
-- `uwu` itself is injected straight into Lua's global state by the C++
-- side (see LuaConfig::init() in lua_config.cpp) -- there's no
-- lib/uwu/init.lua and there never should be one; this file exists
-- purely so your editor knows uwu's shape. Every function/field here
-- mirrors lua_config.cpp's actual luaL_Reg tables exactly -- if you add
-- a real uwu.* function on the C++ side, add its declaration here too or
-- your editor won't know it exists.

---@class uwu
uwu = {}

-- ── spawn / lifecycle ────────────────────────────────────────────────────

---Runs `cmd` through a shell, detached from uwuwm (see spawnCommand()).
---@param cmd string
function uwu.spawn(cmd) end

---Requests a clean shutdown.
function uwu.quit() end

---Hot-reloads rc.lua. Runs after the current callback returns, not
---synchronously -- see LuaConfig::requestReload()'s comment. `pkill -HUP
---uwuwm` from a shell does the same thing synchronously, from outside
---the process.
function uwu.reload() end

-- ── keybinds ─────────────────────────────────────────────────────────────

---Registers a keybind. `mods` is any of "mod"/"shift"/"ctrl"/"alt"
---(order doesn't matter); `key` is an xkbcommon keysym name
---(case-insensitive -- e.g. "Return", "space", "1", "F1"). An unknown key
---name is logged and skipped, not a hard error, so one typo doesn't
---abort the rest of rc.lua.
---@param mods string[]
---@param key string
---@param fn fun()
function uwu.bind(mods, key, fn) end

-- ── settings ─────────────────────────────────────────────────────────────

---Sets one of uwuwm's runtime settings (see kSettingSetters in
---lua_config.cpp): "gap", "border_width", "master_factor", "repeat_rate",
---"repeat_delay", "cursor_size", "terminal", "launcher",
---"border_color_active", "border_color_inactive", "background_color",
---"dwindle_preserve_split", "focus_follows_mouse", "inactive_opacity"
---(0.05-1.0, clamped; static per-view dim applied the moment focus leaves
---it -- see View::setFocused). An unknown name is logged and ignored, not
---an error.
---@param name string
---@param value string|number|boolean
function uwu.set(name, value) end

---Reads back one of the settings uwu.set() accepts. Returns nil (not
---an error) for an unknown name.
---@param name string
---@return string|number|boolean|nil
function uwu.get(name) end

-- ── focused-client actions ───────────────────────────────────────────────

---Closes the focused client, if any.
function uwu.kill() end

---Focuses the next mapped client on the current output's visible tagset.
function uwu.focus_next() end

---Focuses the previous mapped client on the current output's visible tagset.
function uwu.focus_prev() end

---Toggles floating on the focused client.
function uwu.toggle_floating() end

---Toggles fullscreen on the focused client.
function uwu.toggle_fullscreen() end

---Nudges the focused output's master_factor by `delta` (clamped
---internally -- see layout::incMasterFactor).
---@param delta number
function uwu.inc_master(delta) end

---Moves compositor focus to the next (dir >= 0) or previous (dir < 0)
---output in output order. No-op with fewer than two outputs.
---@param dir? integer
function uwu.focus_monitor(dir) end

-- ── layout ───────────────────────────────────────────────────────────────

---Switches the *focused output's* tiling algorithm.
---@param name "master"|"masterstack"|"master_stack"|"dwindle"
function uwu.set_layout(name) end

---Toggles the focused client's dwindle split orientation.
function uwu.dwindle_toggle_split() end

---Swaps the focused client with its dwindle sibling.
function uwu.dwindle_swap_split() end

---Rotates the focused client's dwindle split by `angle` degrees (default 90).
---@param angle? integer
function uwu.dwindle_rotate_split(angle) end

---Adjusts the focused client's dwindle split ratio by `delta`.
---@param delta number
function uwu.dwindle_splitratio(delta) end

---Moves the focused client back to the root of its output's dwindle
---tree. `stable` (default true) keeps the rest of the tree's existing
---split ratios; see dwindle::moveToRoot.
---@param stable? boolean
function uwu.dwindle_move_to_root(stable) end

-- ── hooks ──────────────────────────────────────────────────────────────

---Registers a listener for one of uwuwm's own events and returns an id
---for uwu.unhook(). Event names: "client::manage", "client::unmanage",
---"client::focus", "client::unfocus", "client::fullscreen" (fn receives
---the uwu.Client), "tag::change" (fn receives monitor_name: string,
---new_tagset: integer), "output::connect"/"output::disconnect" (fn
---receives monitor_name: string). Unknown event names are accepted, not
---rejected -- see l_hook's comment in lua_config.cpp.
---@param event string
---@param fn fun(...)
---@return integer id
function uwu.hook(event, fn) end

---Removes a hook registered by uwu.hook() or uwu.rule().
---@param id integer
function uwu.unhook(id) end

---@class uwu.RuleMatch
---@field app_id? string  Exact match, or a Lua pattern if prefixed "~".
---@field title? string   Exact match, or a Lua pattern if prefixed "~".
---@field floating? boolean  Matches the floating state uwuwm already auto-detected.

---@class uwu.RuleApply
---@field floating? boolean
---@field fullscreen? boolean
---@field tag? integer   1-based tag index.
---@field output? string  Output name (see uwu.monitor.list()).
---@field border_color_active? string  "#rrggbb"/"#rrggbbaa". Setting either color seeds both from current global settings first -- see l_rule_hook's comment.
---@field border_color_inactive? string  "#rrggbb"/"#rrggbbaa".

---Sugar over uwu.hook("client::manage", ...) -- registers one rule and
---returns its hook id (same as uwu.hook()'s return, usable with
---uwu.unhook()).
---@param spec { match: uwu.RuleMatch, apply: uwu.RuleApply }
---@return integer id
function uwu.rule(spec) end

-- ── tags ───────────────────────────────────────────────────────────────

---Number of tags uwuwm is configured with (compile-time cfg::kTagCount,
---currently 9). 1-based tag arguments elsewhere run 1..uwu.tag_count.
uwu.tag_count = 9

---@class uwu.tag
uwu.tag = {}

---Switches the focused output to showing only tag `n`.
---@param n integer
function uwu.tag.view(n) end

---Toggles tag `n` into/out of the focused output's visible tagset.
---@param n integer
function uwu.tag.toggle(n) end

---Moves the focused client to tag `n`.
---@param n integer
function uwu.tag.move_client_here(n) end

---Gracefully closes every mapped client on tag `n`. Destructive --
---not bound to a key by default in the shipped rc.lua.
---@param n integer
function uwu.tag.close_all(n) end

-- ── monitors ─────────────────────────────────────────────────────────────

---@class uwu.MonitorOpts
---@field x? integer
---@field y? integer
---@field width? integer
---@field height? integer
---@field refresh? integer
---@field scale? number
---@field enabled? boolean
---@field transform? "normal"|"90"|"180"|"270"|"flipped"|"flipped-90"|"flipped-180"|"flipped-270"
---@field adaptive_sync? boolean

---@class uwu.MonitorInfo
---@field name string
---@field x integer
---@field y integer
---@field width integer
---@field height integer
---@field scale number
---@field enabled boolean
---@field refresh? integer
---@field focused boolean

---@class uwu.monitor
uwu.monitor = {}

---Configures output `name` ("*" = fallback default for any output
---without a more specific rule). Only the fields given are applied;
---everything else keeps its current value.
---@param name string
---@param opts uwu.MonitorOpts
function uwu.monitor.set(name, opts) end

---Lists every currently connected output.
---@return uwu.MonitorInfo[]
function uwu.monitor.list() end

-- ── input devices ────────────────────────────────────────────────────────

---@class uwu.InputOpts
---@field tap? boolean
---@field tap_drag? boolean
---@field tap_drag_lock? boolean
---@field natural_scroll? boolean
---@field dwt? boolean
---@field left_handed? boolean
---@field middle_emulation? boolean
---@field accel_speed? number
---@field accel_profile? "flat"|"adaptive"
---@field scroll_method? "two_finger"|"edge"|"on_button_down"|"none"
---@field scroll_button? integer
---@field click_method? "button_areas"|"clickfinger"|"none"

---@class uwu.InputInfo
---@field name string
---@field type "touchpad"|"mouse"
---@field tap boolean
---@field natural_scroll boolean
---@field left_handed boolean
---@field accel_speed number
---@field accel_profile "flat"|"adaptive"

---@class uwu.input
uwu.input = {}

---Configures pointer devices matching `match` -- an exact libinput
---device name, a type selector ("type:touchpad"/"type:mouse"), or "*"
---for a fallback default. Exact name wins over type, type wins over "*".
---Applied immediately to every already-connected matching device, not
---just future ones.
---@param match string
---@param opts uwu.InputOpts
function uwu.input.set(match, opts) end

---Lists every currently connected pointer device, read live from libinput.
---@return uwu.InputInfo[]
function uwu.input.list() end

-- ── clients ──────────────────────────────────────────────────────────────

---@class uwu.ClientGeo
---@field x integer
---@field y integer
---@field width integer
---@field height integer

---A managed client window. Every property re-checks the window still
---exists (see checkClient() in lua_config.cpp) -- capturing one of these
---in a longer-lived closure (a hook callback, say) is always safe;
---calling anything on one after its window has closed raises a Lua
---error ("stale client reference") instead of touching freed memory.
---@class uwu.Client
---@field id integer  Read-only.
---@field title string  Read-only.
---@field app_id string  Read-only. Also covers X11's WM_CLASS.
---@field is_xwayland boolean  Read-only.
---@field tags integer  Read-only. Bitmask, bit (n-1) = tag n.
---@field floating boolean  Writable -- goes through the same code path as uwu.toggle_floating().
---@field fullscreen boolean  Writable -- goes through the same code path as uwu.toggle_fullscreen().
---@field output? string  Read-only. nil if somehow unassigned.
---@field geo uwu.ClientGeo  Read-only.
---@field border_color_active? string  Read/write, "#rrggbb"/"#rrggbbaa". nil until a rule or a direct write first sets an override; writing either color seeds both from current global settings.
---@field border_color_inactive? string  Read/write, "#rrggbb"/"#rrggbbaa". See border_color_active.
local Client = {}

---Focuses this client.
function Client:focus() end

---Closes this client.
function Client:kill() end

---Moves this client to tag `n`, replacing its current tagset.
---@param n integer
function Client:set_tag(n) end

---Toggles tag `n` into/out of this client's tagset (won't remove its
---last tag -- see l_client_toggle_tag's comment).
---@param n integer
function Client:toggle_tag(n) end

---Moves this client to output `name`, dropping floating placement and
---adopting the destination output's visible tagset.
---@param name string
function Client:move_to_output(name) end

---@class uwu.client
uwu.client = {}

---Every mapped, managed client (unmanaged X11 override-redirect windows
----- menus, tooltips, DND icons -- are excluded, same as everywhere else
---they're excluded from tiling/focus/tag membership).
---@return uwu.Client[]
function uwu.client.list() end

---The currently focused client, or nil if none.
---@return uwu.Client|nil
function uwu.client.focused() end

-- ── system ───────────────────────────────────────────────────────────────
-- OS/session state (backlight, PipeWire volume), not compositor state --
-- see the section comment above kSystemBrightnessFuncs in lua_config.cpp
-- for why this is its own namespace instead of flat on `uwu`.

---@class uwu.system
uwu.system = {}

---@class uwu.system.brightness
uwu.system.brightness = {}

---Current backlight brightness as a 0-100 percent, or nil if no
---/sys/class/backlight device exists.
---@return integer|nil
function uwu.system.brightness.get() end

---Sets backlight brightness. Clamped to 1..100 -- 0 is deliberately
---rejected (see l_system_brightness_set's comment); silent no-op if no
---backlight device exists or the sysfs write fails (check `uwuwm`'s log
---for a permissions hint).
---@param pct integer
function uwu.system.brightness.set(pct) end

---@class uwu.system.volume
uwu.system.volume = {}

---Current default sink volume as a 0-100+ percent (can exceed 100 if
---boosted), or nil if `wpctl` isn't available or the query failed.
---@return integer|nil
function uwu.system.volume.get() end

---Sets the default sink volume. Clamped to 0..150.
---@param pct integer
function uwu.system.volume.set(pct) end

---Sets mute state on the default sink explicitly.
---@param mute boolean
function uwu.system.volume.mute(mute) end

---Toggles mute on the default sink.
function uwu.system.volume.toggle_mute() end
