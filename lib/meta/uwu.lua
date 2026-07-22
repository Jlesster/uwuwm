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

---Registers a mousebind: `mods` (same names as uwu.bind above) plus a
---pointer button -- "left"/"right"/"middle"/"side"/"extra" (or a raw
---BTN_* evdev name/number) -- pressed while the cursor is over a client.
---`fn` is called as `fn(client)` with that client, and the whole click is
---consumed by the compositor (not forwarded to the app underneath) if a
---bind matches. This is what floating-window drag-move/drag-resize is
---built on -- sway/i3's floating_modifier equivalent:
---
---    uwu.mousebind({"mod"}, "left", function(c) c:begin_move() end)
---    uwu.mousebind({"mod"}, "right", function(c) c:begin_resize() end)
---
---Both begin_move()/begin_resize() are silent no-ops on a tiled or
---fullscreen client -- a mousebind fires on whatever's under the cursor,
---so no error just because that happens to not be floating right now. An
---unknown button name is logged and skipped, same as an unknown key name
---in uwu.bind above.
---@param mods string[]
---@param button string
---@param fn fun(client: Client)
function uwu.mousebind(mods, button, fn) end

-- ── settings ─────────────────────────────────────────────────────────────

---@deprecated Use uwu.visual.set() (7 appearance fields) or
---uwu.behavior.set() (7 app/behavior fields) instead -- each refuses the
---other's fields outright (see kSettingCategory in lua_config.cpp), which
---this flat form can't. Kept only so an rc.lua predating that split still
---runs; logs a one-line notice pointing at the right one each call.
---@param name string
---@param value string|number|boolean
function uwu.set(name, value) end

---@deprecated Read-side counterpart to uwu.set() -- see its deprecation
---note. Use uwu.visual.get()/uwu.behavior.get() instead.
---@param name string
---@return string|number|boolean|nil
function uwu.get(name) end

---@class uwu.visual
uwu.visual = {}

---Sets one of the eight *appearance* settings: "gap", "border_width",
---"cursor_size", "cursor_theme" (xcursor theme name, e.g. "Qogir" --
---empty/unset falls back to the XCURSOR_THEME env var if that's set,
---then wlroots' own built-in default), "border_color_active",
---"border_color_inactive", "background_color", "inactive_opacity"
---(0.05-1.0, clamped; static per-view dim applied the moment focus
---leaves it -- see View::setFocused). This is what nyaa.wear() calls
---underneath; prefer that unless you're building your own theming
---layer. Any of the seven *behavior* names uwu.behavior.set() owns --
---or a genuine typo -- is refused the same way: logged and ignored,
---not an error.
---@param name string
---@param value string|number|boolean
function uwu.visual.set(name, value) end

---Reads back one of the settings uwu.visual.set() accepts. Returns nil
---(not an error) for a name it doesn't own, same as a typo.
---@param name string
---@return string|number|boolean|nil
function uwu.visual.get(name) end

---@class uwu.behavior
uwu.behavior = {}

---Sets one of the seven *app/behavior* settings: "master_factor",
---"repeat_rate", "repeat_delay", "terminal", "launcher",
---"focus_follows_mouse", "dwindle_preserve_split". This is what
---paw.defaults() calls underneath; prefer that unless you're building
---your own config layer. Any of the eight *visual* names uwu.visual.set()
---owns -- or a genuine typo -- is refused the same way: logged and
---ignored, not an error.
---@param name string
---@param value string|number|boolean
function uwu.behavior.set(name, value) end

---Reads back one of the settings uwu.behavior.set() accepts. Returns nil
---(not an error) for a name it doesn't own, same as a typo.
---@param name string
---@return string|number|boolean|nil
function uwu.behavior.get(name) end

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

---Moves compositor focus to the next (dir >= 0) or previous (dir < 0)
---output in output order. No-op with fewer than two outputs.
---@param dir? integer
function uwu.focus_monitor(dir) end

-- ── layout ───────────────────────────────────────────────────────────────
-- uwu.layout -- tiling primitives, grouped in their own namespace instead
-- of sitting flat on `uwu` alongside spawn/bind/quit. paw.layout
-- (lib/paw/init.lua) wraps this the same way paw.client wraps
-- uwu.focus_next/toggle_floating/etc; prefer paw.layout in an rc.lua
-- unless you're writing your own tiling-facing module.

---@class uwu.layout
uwu.layout = {}

---Switches the *focused output's* tiling algorithm.
---@param name "master"|"masterstack"|"master_stack"|"dwindle"
function uwu.layout.set(name) end

---Nudges the focused output's master_factor by `delta` (clamped
---internally -- see layout::incMasterFactor). The *absolute* starting
---value is uwu.behavior.set("master_factor", ...) / paw.defaults's job,
---not this.
---@param delta number
function uwu.layout.inc_master(delta) end

---@class uwu.layout.dwindle
uwu.layout.dwindle = {}

---Toggles the focused client's dwindle split orientation.
function uwu.layout.dwindle.toggle_split() end

---Swaps the focused client with its dwindle sibling.
function uwu.layout.dwindle.swap_split() end

---Rotates the focused client's dwindle split by `angle` degrees (default 90).
---@param angle? integer
function uwu.layout.dwindle.rotate_split(angle) end

---Adjusts the focused client's dwindle split ratio by `delta`.
---@param delta number
function uwu.layout.dwindle.splitratio(delta) end

---Moves the focused client back to the root of its output's dwindle
---tree. `stable` (default true) keeps the rest of the tree's existing
---split ratios; see dwindle::moveToRoot.
---@param stable? boolean
function uwu.layout.dwindle.move_to_root(stable) end

-- ── hooks ──────────────────────────────────────────────────────────────

---Registers a listener for one of uwuwm's own events and returns an id
---for uwu.unhook(). Event names: "client::manage", "client::unmanage",
---"client::focus", "client::unfocus", "client::fullscreen",
---"client::title_changed" (fn receives the uwu.Client), "tag::change" (fn
---receives monitor_name: string, new_tagset: integer),
---"output::connect"/"output::disconnect" (fn receives monitor_name:
---string). Unknown event names are accepted, not rejected -- see l_hook's
---comment in lua_config.cpp.
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
---@field opacity? number  0.0-1.0. Only applies while the client is unfocused -- a focused client is always fully opaque.

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

---Name of the currently focused output, or nil in the brief window
---before the first output has connected. Same data as the `focused`
---field on one of uwu.monitor.list()'s entries, without the scan.
---@return string?
function uwu.monitor.focused() end

-- ── wallpaper ────────────────────────────────────────────────────────────

---@class uwu.WallpaperOpts
---@field path string Image path. A leading "~" is expanded to $HOME.
---@field mode? "fill"|"fit"|"stretch"|"center"|"tile" Default "fill".

---@class uwu.WallpaperInfo
---@field name string Output name, or "*" for the fallback default.
---@field path string
---@field mode "fill"|"fit"|"stretch"|"center"|"tile"

---@class uwu.wallpaper
uwu.wallpaper = {}

---Sets output `name`'s wallpaper ("*" = fallback default for any output
---without a more specific wallpaper rule). Decoded natively -- there's
---no external process involved, see src/wallpaper.hpp. A bad path or an
---undecodable image is logged and leaves background_color as the only
---thing visible on the affected output(s), never a hard error.
---@param name string
---@param opts uwu.WallpaperOpts
function uwu.wallpaper.set(name, opts) end

---Clears output `name`'s wallpaper rule ("*" clears the fallback
---default, not every output at once -- clear each exact name too if
---that's what you want), uncovering background_color.
---@param name string
function uwu.wallpaper.clear(name) end

---Lists every wallpaper rule currently set (one entry per name/"*"
---selector, not one per connected output).
---@return uwu.WallpaperInfo[]
function uwu.wallpaper.list() end

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
---@field opacity? number  Read/write, 0.0-1.0. nil until a rule or a direct write first sets an override. Only applies while unfocused.
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

---Repositions a *floating* client to an absolute (x, y). Errors on a
---tiled client -- its geo is recomputed by every layout pass, so a
---one-off move here would just be overwritten by the next arrange().
---@param x integer
---@param y integer
function Client:move(x, y) end

---Resizes a *floating* client to (width, height), keeping its current
---top-left corner fixed. Same tiled-client restriction as Client:move().
---@param width integer
---@param height integer
function Client:resize(width, height) end

---Starts an interactive drag-move grab for this client -- the mouse
---equivalent of dragging a titlebar, driven by cursor motion instead of
---an absolute (x, y). Meant to be called from a uwu.mousebind() closure.
---Silent no-op on a tiled or fullscreen client.
function Client:begin_move() end

---Starts an interactive drag-resize grab for this client. Which edges
---grow/shrink is picked from which quadrant of the window the cursor was
---over when the bind fired (left half drags the left edge, top half
---drags the top edge, etc), the same convention i3/sway's
---floating_modifier resize-drag uses. Meant to be called from a
---uwu.mousebind() closure. Silent no-op on a tiled or fullscreen client.
function Client:begin_resize() end

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

-- ── timer ────────────────────────────────────────────────────────────

---@class uwu.timer
uwu.timer = {}

---Calls fn every `seconds` (fractional allowed), starting `seconds`
---from now, until uwu.timer.cancel(id) or a reload. fn receives no
---arguments. Cleared (not carried across) a reload, same as uwu.hook()
----- register it again from rc.lua if you want it back.
---@param seconds number
---@param fn fun()
---@return integer id
function uwu.timer.set_interval(seconds, fn) end

---Calls fn exactly once, `seconds` from now, then removes itself.
---@param seconds number
---@param fn fun()
---@return integer id
function uwu.timer.set_timeout(seconds, fn) end

---Cancels a set_interval before its next tick, or a set_timeout before
---it ever fires. No-op on an unknown/already-fired/already-cancelled id.
---@param id integer
function uwu.timer.cancel(id) end

-- ── bar ──────────────────────────────────────────────────────────────
-- A `uwu.Bar` is a low-level pixel buffer you draw rects and glyphs
-- into, then commit -- immediate mode: nothing persists between
-- commits except what you explicitly redraw. Most users want the
-- higher-level `uwu.widget.window` below -- a retained-mode rect/text
-- API with anchoring between widgets and parent/child nesting, so the
-- widget section after this one is what new code should reach for.
-- uwu.bar.* stays public for the cases uwu.widget genuinely can't
-- cover: custom glyph rendering with an arbitrary font_path per call,
-- a 1-pixel-tall scratch buffer, or direct pixel poking for something
-- like a video preview.

---@class uwu.bar
uwu.bar = {}

---@class uwu.BarCreateOpts
---@field output? string  Output name; defaults to the currently focused one.
---@field position? "top"|"bottom"  Defaults to "top".
---@field height? integer  Pixels; defaults to 30.

---Creates a native, compositor-drawn status bar -- not a Wayland
---client, no external process. Immediately reserves its own
---exclusive-zone strip of screen (same mechanism a layer-shell bar like
---waybar uses), so tiled windows on that output shrink to make room
---right away. Draw into it with the Bar methods below, then :commit()
---to push what you've drawn on screen -- nothing changes until commit()
---is called. Survives a uwu.reload() (unlike a hook or timer) -- only
---:destroy() removes it.
---@param opts uwu.BarCreateOpts
---@return uwu.Bar
function uwu.bar.create(opts) end

---@class uwu.Bar
local Bar = {}

---Fills the entire bar with one color -- 0xRRGGBBAA, straight alpha.
---Bars are always opaque once drawn: a color's own alpha blends into
---whatever's already there, it doesn't punch a transparent hole.
---@param color integer
function Bar:clear(color) end

---Fills an (x, y, w, h) rectangle with `color` (0xRRGGBBAA). Clipped to
---the bar's own bounds -- an out-of-range rect is silently trimmed, not
---an error.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@param color integer
function Bar:rect(x, y, w, h, color) end

---Draws `text` with its baseline at (x, y) -- not the top of the glyph
---box; see Bar:line_height() for vertical-centering math. `font_path`
---is a path to a .ttf/.otf/.ttc file (no bundled/system font lookup by
---name -- point at one directly, e.g.
---"/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf"). Returns
---the horizontal advance in pixels, for chaining more text after this
---or right/center-aligning by measuring with Bar:text_width() first.
---@param x integer
---@param y integer
---@param text string
---@param font_path string
---@param pixel_size integer
---@param color integer
---@return integer advance_px
function Bar:text(x, y, text, font_path, pixel_size, color) end

---Same shaping pass as Bar:text(), without drawing anything -- just the
---width, for alignment math.
---@param text string
---@param font_path string
---@param pixel_size integer
---@return integer width_px
function Bar:text_width(text, font_path, pixel_size) end

---Recommended line height for `font_path` at `pixel_size` -- lets you
---vertically center a label in a bar of known height without a
---hand-picked fudge factor.
---@param font_path string
---@param pixel_size integer
---@return integer height_px
function Bar:line_height(font_path, pixel_size) end

---Uploads everything drawn since the last commit() to the screen.
---Nothing you draw is visible before this is called.
function Bar:commit() end

---Registers a click handler -- fn(x, y, button), x/y relative to this
---bar's own top-left corner, button a BTN_LEFT/BTN_RIGHT/... evdev
---code. Replaces any previously-registered handler. With no handler
---set, a click on the bar is still consumed (not passed through to
---whatever's tiled underneath), it just does nothing.
---@param fn fun(x: integer, y: integer, button: integer)
function Bar:on_click(fn) end

---This bar's current pixel width (tracks its output's width).
---@return integer
function Bar:width() end

---This bar's height, as given to uwu.bar.create() (or the 30px default).
---@return integer
function Bar:height() end

---Destroys this bar: releases its exclusive-zone reservation (tiled
---windows on that output re-expand into the freed space) and removes
---it from the screen. The Bar handle is invalid after this.
function Bar:destroy() end

-- ── widget ───────────────────────────────────────────────────────────
-- `uwu.widget.window()` is a retained-mode widget surface: a window
-- holds rect/text widgets, each widget is positioned by (x, y) and
-- optional anchor constraints against another widget (or its own
-- parent), and :commit() resolves positions and repaints the whole
-- window in one go. uwu.widget.window can do anything uwu.bar can do
-- and more (nested widgets, anchoring, fixed-size popup mode for
-- OSDs), and at the cost of a full repaint on every commit -- fine
-- for status bars and OSDs, not for video.
--
-- Two modes, picked by `mode` in the create options:
--
--   mode = "bar"   (default) -- anchored to an output edge, full
--                              output width, always visible. Reserves
--                              its own exclusive-zone strip (same as
--                              a Bar or a layer-shell bar).
--   mode = "popup"            -- a fixed-size floating box positioned
--                              by `anchor` + `margin`, drawn above
--                              everything (fullscreen views, bars,
--                              popups). Starts hidden; toggle with
--                              :show() / :hide(). Pair with
--                              uwu.timer.set_timeout() for auto-hide.

---@class uwu.widget
uwu.widget = {}

---@class uwu.WidgetBarOpts
---@field output? string  Output name; defaults to the currently focused one.
---@field mode? "bar"  Defaults to "bar" if omitted entirely.
---@field position? "top"|"bottom"  Defaults to "top".
---@field height? integer  Pixels; defaults to 30.

---@class uwu.WidgetPopupOpts
---@field output? string  Output name; defaults to the currently focused one.
---@field mode "popup"
---@field anchor? "center"|"top"|"bottom"|"left"|"right"|"top-left"|"top-right"|"bottom-left"|"bottom-right"  Defaults to "center".
---@field margin? integer  Pixels from the screen edge/center; defaults to 0.
---@field width integer  Required -- no natural default for a floating popup's width.
---@field height integer  Required -- no natural default for a floating popup's height.

---Creates a widget window. Bar-mode windows claim their exclusive
---zone immediately (tiled windows on that output shrink to make room
---right away, same as a Bar). Popup-mode windows are hidden by
---default; :show() to reveal, :hide() to put away, both are cheap
---`wlr_scene_node_set_enabled` toggles -- no create/destroy churn,
---so a single popup can be re-shown over and over. Survives
---uwu.reload() (only :destroy() removes it).
---@param opts uwu.WidgetBarOpts|uwu.WidgetPopupOpts
---@return uwu.WidgetWindow
function uwu.widget.window(opts) end

---@class uwu.WidgetWindow
local WidgetWindow = {}

---@class uwu.WidgetRectOpts
---@field x? integer  Parent-relative, defaults to 0.
---@field y? integer  Parent-relative, defaults to 0.
---@field w? integer  Width, defaults to 0.
---@field h? integer  Height, defaults to 0.
---@field color? integer  0xRRGGBBAA, defaults to 0x000000ff (opaque black).
---@field parent? uwu.Widget  Parent widget, or nil for the window root. Defaults to nil.

---Adds a solid-color rectangle widget to this window. Returns a
---widget handle for later :set_color / :set_pos / :set_size /
---:anchor / :clear_anchor / :anchor_fill / :clear_fill. Position
---changes only take effect on the next :commit().
---@param opts uwu.WidgetRectOpts
---@return uwu.Widget
function WidgetWindow:rect(opts) end

---@class uwu.WidgetTextOpts
---@field x? integer  Parent-relative, defaults to 0.
---@field y? integer  Parent-relative, defaults to 0.
---@field text? string  Initial text, defaults to "".
---@field font_size? integer  Pixels, defaults to 14.
---@field color? integer  0xRRGGBBAA, defaults to 0xffffffff (opaque white).
---@field parent? uwu.Widget  Parent widget, or nil for the window root. Defaults to nil.

---Adds a text widget to this window. The font is the compositor's
---default sans-serif (no per-widget font_path here -- uwu.bar:text
---is the entry point for custom fonts). Returns a widget handle;
---change the text with :set_text, change position with :set_pos,
---change color with :set_color -- nothing is repainted until
---:commit().
---@param opts uwu.WidgetTextOpts
---@return uwu.Widget
function WidgetWindow:text(opts) end

---Resolves every widget's position (running the anchor solver) and
---repaints the whole window in one go. Anchors only ever move a
---widget (its width/height come from set_size / the constructor)
---and anchor_fill is the one explicit operation that's allowed to
---resize, so a position is always fully determined by already-known
---widths/heights -- there's no "solve position and size
---simultaneously" case and therefore no way for two anchors to
---disagree about a size. Nothing drawn is visible before this is
---called.
function WidgetWindow:commit() end

---Registers a click handler -- fn(x, y, button), x/y relative to
---this window's own top-left, button a BTN_LEFT/BTN_RIGHT/... evdev
---code. Replaces any previously-registered handler. With no handler
---set, a click on the window is still consumed (not passed through
---to whatever's underneath), it just does nothing. Clicks on a
---widget inside the window that has no handler of its own fall
---through to the window's handler.
---@param fn fun(x: integer, y: integer, button: integer)
function WidgetWindow:on_click(fn) end

---This window's current pixel width. Bar mode tracks the output's
---width; popup mode is the fixed width passed to :window().
---@return integer
function WidgetWindow:width() end

---This window's height. Bar mode is the height passed to :window();
---popup mode is its fixed height.
---@return integer
function WidgetWindow:height() end

---Reveals this window. No-op if already shown. Cheap (a
---wlr_scene_node_set_enabled toggle, no re-create), so a popup can
---be re-shown on every volume-change / notification event without
---churning scene-graph nodes.
function WidgetWindow:show() end

---Hides this window. No-op if already hidden. Cheap (same toggle
---under the hood as :show()). A hidden popup's anchor solver is
---still in place; :show() puts it back where it was.
function WidgetWindow:hide() end

---Destroys this window: removes all its widgets, releases its
---exclusive zone (bar mode, tiled windows on that output re-expand
---into the freed space), and tears the scene node down. The
---WidgetWindow handle and every widget handle inside it are invalid
---after this.
function WidgetWindow:destroy() end

---@class uwu.Widget
local Widget = {}

---Replaces this widget's text. Only meaningful for text widgets.
---Nothing is repainted until the owning window's :commit() is
---called.
---@param text string
function Widget:set_text(text) end

---Replaces this widget's color (0xRRGGBBAA). Nothing is repainted
---until :commit().
---@param color integer
function Widget:set_color(color) end

---Sets this widget's parent-relative (x, y). Only takes visible
---effect on an axis that has no anchor line set -- an anchor, once
---set on an axis (via :anchor), always takes priority over plain x/y
---on that axis, on every :commit(), regardless of whether set_pos was
---called more recently. Call :clear_anchor()/:clear_anchors() first if
---you want set_pos to actually move an anchored widget. Nothing is
---repainted until the owning window's :commit().
---@param x integer
---@param y integer
function Widget:set_pos(x, y) end

---Sets this widget's box size directly. For a Rect this is exactly
---what gets painted. For a Text widget this does *not* change what
---gets drawn -- text always renders at its natural glyph size -- it
---only overrides the w/h another widget's :anchor() would read from
---this one as a target; a subsequent :set_text() call recomputes
---width (not height) from the new content regardless, so an override
---here doesn't survive a text change. Nothing is repainted until
---:commit().
---@param w integer
---@param h integer
function Widget:set_size(w, h) end

---This widget's measured width. For a Rect this is whatever
---set_size() last set; for a Text widget it's the auto-measured text
---width from the most recent :commit(). Reading this is the natural
---way to size a sibling widget to wrap this one -- e.g. a pill rect
---that should hug its label's text. The value is whatever the last
---:commit() used to paint, not whatever is queued for the next one.
---@return integer
function Widget:width() end

---This widget's measured height. Same story as :width(): Rect is
---set_size(), Text is the auto-measured ascent+descent from the last
---:commit().
---@return integer
function Widget:height() end

---@alias uwu.WidgetEdge "left"|"right"|"top"|"bottom"|"hcenter"|"vcenter"

---Sets one anchor line: "my `my_edge` sits at `target`'s
---`target_edge` + `margin`". `target` is nil for "this widget's
---own parent" (or the window root, if this widget has no parent),
---or another widget from the same window. A stale or wrong-window
---target is a safe fallback at layout time, not an error -- same
---leniency the rest of the widget API has. At most one of
---left/right/hcenter applies on the x axis (priority left > right
---> hcenter); same for top/bottom/vcenter on y. Nothing is
---repositioned until :commit().
---@param my_edge uwu.WidgetEdge
---@param target? uwu.Widget
---@param target_edge uwu.WidgetEdge
---@param margin? integer  Defaults to 0.
function Widget:anchor(my_edge, target, target_edge, margin) end

---Clears a single anchor line. The widget falls back to its plain
---x/y on that axis, or to whatever the other anchor line on the
---same axis said (priority: left > right > hcenter).
---@param my_edge uwu.WidgetEdge
function Widget:clear_anchor(my_edge) end

---Clears all six anchor lines. The widget returns to plain x/y
---positioning (anchors resume only if :anchor() is called again).
function Widget:clear_anchors() end

---Sets this widget's box to fully cover `target`'s box (or this
---widget's own parent, if target is nil), inset by `margin` on all
---four sides -- recomputed fresh every :commit(). The one widget
---shape allowed to resize itself via anchoring. While active, this
---takes priority over *all six* anchor lines (left/right/top/bottom/
---hcenter/vcenter alike -- not just the box-shape ones) and over
---set_size, all of which are simply not consulted rather than
---cleared -- see Widget:clear_fill for what that means when you turn
---fill back off. Nothing is repainted until :commit().
---@param target? uwu.Widget
---@param margin? integer  Defaults to 0.
function Widget:anchor_fill(target, margin) end

---Turns fill mode off. Any anchor lines set before/during fill was
---active are still there and resume immediately. Size is a rougher
---edge: while fill was active, every :commit() overwrote this
---widget's own w/h with fill's computed size, so clear_fill() alone
---leaves it at whatever fill last computed -- not whatever set_size
---said before fill started (that old value was never preserved
---anywhere). Call :set_size() again after clear_fill() if you need a
---specific size back.
function Widget:clear_fill() end
