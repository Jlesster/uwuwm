-- uwuwm config -- copy this to ~/.config/uwuwm/rc.lua and edit freely.
--
-- Reloading: mod+shift+r (bound below) or `pkill -HUP uwuwm` from a shell
-- both re-run this whole file from scratch without restarting the
-- compositor. If the new file fails to load (syntax error, etc.) uwuwm
-- keeps running on the last-known-good config instead of losing your
-- keybinds -- see LuaConfig::reload() if you want the details.
--
-- This file is written against uwuwm's AwesomeWM-shaped Lua library
-- (lib/awful, lib/beautiful, lib/gears -- see extendPackagePath() in
-- lua_config.cpp for where those get found). Everything it does is
-- still just sugar over the raw `uwu` C API (uwu.spawn, uwu.bind,
-- uwu.hook, uwu.rule, ...) documented in full further down and in
-- MISSING.md -- drop to `uwu.*` directly any time the higher-level
-- wrapper doesn't have what you need; the two styles compose freely in
-- the same rc.lua.

local gears = require('gears')
local awful = require('awful')
local beautiful = require('beautiful')

-- ── Theme ────────────────────────────────────────────────────────────
-- beautiful.init() takes either a path to a separate theme.lua (the
-- AwesomeWM convention -- one file, `return`ing a table, that you can
-- swap out wholesale) or, as here, an inline table if you'd rather keep
-- everything in one file. Every field below is optional; anything you
-- don't set keeps beautiful's own built-in default (which matches
-- RuntimeConfig's compiled-in defaults exactly -- see lib/beautiful/
-- init.lua). Setting these calls straight through to uwu.set() as a
-- side effect of beautiful.apply(), which init() calls automatically.
beautiful.init({
  gap = 8,
  border_width = 2,
  master_factor = 0.55,
  border_focus = '#cba6f7', -- Catppuccin Mocha mauve
  border_normal = '#313244', -- Catppuccin Mocha surface0
  bg_normal = '#11111b', -- Catppuccin Mocha crust
  repeat_rate = 25,
  repeat_delay = 600,
  cursor_size = 24,
  terminal = 'wezterm',
  launcher = 'fuzzel',
})

local terminal = beautiful.terminal
local launcher = beautiful.launcher
local modkey = 'mod'

-- ── Global keybinds ──────────────────────────────────────────────────
-- awful.key(mods, key, fn) + gears.table.join build the whole binding
-- list up front, AwesomeWM-rc.lua-style; root.keys(...) at the bottom
-- pushes the finished list to uwu.bind() one call each. A plain
-- uwu.bind({ 'mod' }, 'x', fn) call anywhere else in this file works
-- exactly the same as an entry here -- this is purely about reading
-- like one declarative table instead of many separate statements, not
-- a different mechanism underneath.
local globalkeys = gears.table.join(
  awful.key({ modkey }, 'Return', function()
    awful.spawn(terminal)
  end),
  awful.key({ modkey }, 'd', function()
    awful.spawn(launcher)
  end),
  awful.key({ modkey, 'shift' }, 'q', function()
    awesome.quit()
  end),
  awful.key({ modkey, 'shift' }, 'r', function()
    awesome.restart()
  end),
  awful.key({ modkey }, 'j', function()
    awful.client.focus.byidx(1)
  end),
  awful.key({ modkey }, 'k', function()
    awful.client.focus.byidx(-1)
  end),
  awful.key({ modkey, 'shift' }, 'c', function()
    local c = client.focus
    if c then
      c:kill()
    end
  end),
  awful.key({ modkey }, 'space', function()
    awful.client.toggle_floating()
  end),
  awful.key({ modkey }, 'f', function()
    awful.client.toggle_fullscreen()
  end),
  awful.key({ modkey }, 'i', function()
    awful.layout.incMasterFactor(0.05)
  end),
  awful.key({ modkey }, 'u', function()
    awful.layout.incMasterFactor(-0.05)
  end),
  awful.key({ modkey }, 'Tab', function()
    uwu.focus_monitor(1)
  end),
  awful.key({ modkey }, 'e', function()
    awful.spawn('emacsclient -c')
  end),
  awful.key({ modkey, 'shift' }, 's', function()
    awful.spawn('grim -g "$(slurp)" ~/Pictures/screenshot-$(date +%s).png')
  end),
  awful.key({ modkey }, 'l', function()
    awful.spawn('swaylock')
  end)
)

-- Tags 1..9: mod+N to view, mod+shift+N to move focused window, and
-- mod+ctrl+N (only 1-5, keeps the modifier-heavy combo to the tags most
-- people actually reach for) to toggle a tag into/out of the visible set.
for i = 1, uwu.tag_count do
  globalkeys = gears.table.join(
    globalkeys,
    awful.key({ modkey }, tostring(i), function()
      awful.tag.viewonly(i)
    end),
    awful.key({ modkey, 'shift' }, tostring(i), function()
      awful.tag.movetotag(i)
    end)
  )
  if i <= 5 then
    globalkeys = gears.table.join(
      globalkeys,
      awful.key({ modkey, 'ctrl' }, tostring(i), function()
        awful.tag.viewtoggle(i)
      end)
    )
  end
end

root.keys(globalkeys)

-- awful.tag.close_all(n) gracefully closes every window on tag n --
-- destructive, so it's deliberately not bound to a key here. Wire it up
-- yourself if you want it, e.g. a dedicated "close everything on tag 9"
-- bind:
-- root.keys(gears.table.join(globalkeys,
--     awful.key({ modkey, 'shift', 'ctrl' }, '9', function() awful.tag.close_all(9) end)))

-- ── Monitor configuration ───────────────────────────────────────────────
-- Still uwu.monitor.* directly -- there's no awful.screen.set(), since
-- output configuration (position/mode/scale/transform/adaptive-sync)
-- isn't something AwesomeWM's own awful.screen concerns itself with
-- either (that's usually xrandr's job on X11); awful.screen here only
-- covers reading back what's connected (awful.screen.get()/focused()).
-- Every field is optional; only what's set is applied/stored. Run
-- `awful.screen.get()` (e.g. from a keybind that dumps it to
-- notify-send or a log) to see connected output names before filling
-- these in.
--
-- uwu.monitor.set("DP-1", {
--     x = 0, y = 0,
--     width = 2560, height = 1440, refresh = 144,
--     scale = 1.0,
--     transform = "normal", -- normal|90|180|270|flipped|flipped-90|...
--     adaptive_sync = true,
-- })
-- uwu.monitor.set("eDP-1", { x = 2560, y = 0, enabled = true })
-- uwu.monitor.set("*", { scale = 1.0 }) -- fallback default for anything else

-- ── Input device configuration ──────────────────────────────────────────
-- Also still uwu.input.* directly, same reasoning as monitors above --
-- `match` is an exact libinput device name (see uwu.input.list()), a
-- type selector ("type:touchpad" / "type:mouse"), or "*" for a fallback
-- default applied to any pointer device without a more specific rule --
-- exact name always wins over type, and type always wins over "*".
uwu.input.set('type:touchpad', {
  tap = true, -- tap-to-click
  tap_drag = true, -- tap-and-hold then move = drag
  tap_drag_lock = true, -- lift finger mid-drag without dropping it
  natural_scroll = true, -- content-follows-fingers scroll direction
  dwt = true, -- ignore touchpad while actively typing
})

-- ── Client rules ─────────────────────────────────────────────────────
-- awful.rules.rules is the AwesomeWM-shaped { rule = ..., properties =
-- ... } declarative table; awful.rules.apply() pushes the whole thing
-- to uwu.rule() in one go. `rule.class` is accepted as a synonym for
-- uwuwm's own `app_id` field (see lib/awful/rules.lua) so a rule copied
-- straight out of an AwesomeWM config matches identically.
awful.rules.rules = {
  { rule = { class = 'mpv' }, properties = { floating = true, tag = 3 } },
  { rule = { class = 'foot' }, properties = { tag = 1 } },
  { rule = { class = 'pavucontrol' }, properties = { floating = true } },
  { rule = { class = '~steam_app_.*' }, properties = { tag = 9 } }, -- "~" prefix = Lua pattern
  -- Anything already auto-detected as floating (a parent window, a
  -- fixed-size dialog, or an X11 client with
  -- _NET_WM_WINDOW_TYPE_DIALOG/UTILITY/SPLASH) that isn't caught by a
  -- more specific rule above still gets parked on the scratch tag
  -- (9) -- one rule instead of naming every file-picker/color-chooser
  -- app_id by hand.
  { rule = { floating = true }, properties = { tag = 9 } },
}
awful.rules.apply()

-- ── Signals / hooks ──────────────────────────────────────────────────
-- client.connect_signal("manage"/"unmanage"/"focus"/"unfocus"/
-- "fullscreen", fn) is AwesomeWM's own name for exactly the uwu.hook
-- events documented in MISSING.md -- fn(c) gets the same Client
-- userdata either way, so the field/method docs there still apply
-- verbatim: c.title, c.app_id, c.is_xwayland, c.tags, c.floating,
-- c.fullscreen, c.output, c.geo, c:focus(), c:kill(), c:set_tag(n),
-- c:toggle_tag(n), c:move_to_output(name).
--
-- client.disconnect_signal(id) removes a registration later -- handy
-- for ad-hoc hooks (a "focus this app" toggle keybind, say) without
-- leaving a permanent listener behind.
local focus_steam_id = client.connect_signal('focus', function(c)
  if c.app_id == 'steam' then
    -- ensure steam (and only steam) lands on tag 9 when it gets focus
    if c.tags ~= 256 then
      c:set_tag(9)
    end
  end
end)

-- tag.connect_signal("change", fn)        fn(monitor_name, new_tagset)
-- screen.connect_signal("connect", fn)    fn(monitor_name)
-- screen.connect_signal("disconnect", fn) fn(monitor_name)
--
-- client.disconnect_signal(focus_steam_id) -- to remove the hook above later.

-- ── Querying current state ─────────────────────────────────────────────
-- client.get() and client.focus round out the read side -- the same
-- Client userdata every signal callback gets, so any method on one of
-- these works too:
--
--   for _, c in ipairs(client.get()) do
--     if c.app_id == 'slack' and c.output ~= 'eDP-1' then
--       c:move_to_output('eDP-1')
--     end
--   end
--
-- Keybind this to mod+shift+s for "tidy slack back onto the laptop
-- panel" if you tend to drag it onto an external display by accident.
