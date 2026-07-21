-- uwuwm config -- copy this to ~/.config/uwuwm/rc.lua and edit freely.
--
-- Reloading: mod+shift+r (bound below) or `pkill -HUP uwuwm` from a shell
-- both re-run this whole file from scratch without restarting the
-- compositor. If the new file fails to load (syntax error, etc.) uwuwm
-- keeps running on the last-known-good config instead of losing your
-- keybinds -- see LuaConfig::reload() if you want the details.
--
-- This file is written against uwuwm's own small Lua library, split the
-- same way AwesomeWM splits core/awful/beautiful/wibox:
--
--   uwu   the raw compositor API (uwu.spawn, uwu.bind, uwu.hook,
--         uwu.rule, uwu.system.*, uwu.monitor.*, uwu.input.*, ... --
--         documented in full below, in lib/meta/uwu.lua, and in
--         MISSING.md). uwu.layout.*/uwu.visual.*/uwu.behavior.* are the
--         primitive namespaces paw/nyaa wrap below -- reach for them
--         directly only when paw/nyaa genuinely don't cover something.
--   paw   window-management sugar over `uwu` -- keybind lists w/
--         descriptions, client rules, short-named hooks, and (paw.layout/
--         paw.client) the tiling and focus-navigation actions your
--         keybinds actually call
--   nyaa  aesthetics only -- two-color presets (nyaa.presets) or full
--         26-role palettes (nyaa.palettes/nyaa.palette(), Catppuccin's
--         four flavors plus a few others), applied via nyaa.wear() /
--         per-client via nyaa.rule() (border colors *and* opacity),
--         plus nyaa.export.* to render the same palette into config
--         snippets for the rest of your DE (GTK, kitty, foot, waybar,
--         dunst, wezterm, fuzzel)
--
-- (lib/paw, lib/nyaa -- see extendPackagePath() in lua_config.cpp for
-- where those get found). A wibox-equivalent (status bar / widgets) is
-- reserved for later, not implemented yet. Drop to `uwu.*` directly any
-- time paw/nyaa don't have what you need -- all three styles compose
-- freely in the same rc.lua.

local paw = require('paw')
local nyaa = require('nyaa')

local wallpaper_location =
  '/home/jless/Pictures/Wallpapers/wallhaven-o5zo2l.png'
uwu.wallpaper.set('*', { path = wallpaper_location })

paw.layout.set('dwindle')

-- ── Theme ────────────────────────────────────────────────────────────
-- nyaa.wear() only ever touches the seven *visual* uwu.visual.* fields
-- (gap, border_width, border_color_active/inactive, background_color,
-- cursor_size, inactive_opacity) -- it'll refuse master_factor/
-- repeat_rate/repeat_delay/terminal/launcher/dwindle_preserve_split below
-- with a pointer back to paw.defaults(), which owns those instead.
--
-- `preset` (see nyaa.presets) seeds just the two border colors, same as
-- before. `flavor` (see lib/nyaa/palettes.lua) seeds border colors *and*
-- background_color from the same full 26-role palette nyaa.export.*
-- below reads from -- use this instead of `preset` if you also want
-- nyaa.export.* to track the same colors uwuwm itself is drawing with.
-- Everything else is an override; every field is optional, and anything
-- you don't set keeps RuntimeConfig's compiled-in default.
nyaa.wear({
  flavor = 'catppuccin_mocha',
  gap = 8,
  border_width = 2,
  cursor_size = 24,
  cursor_theme = 'qogir',
  inactive_opacity = 0.25, -- dim unfocused windows slightly; 1.0 disables
})

-- nyaa.export.* renders the same palette nyaa.wear() just applied into
-- config snippets for the rest of your desktop (GTK, kitty, foot,
-- waybar, dunst, wezterm, fuzzel -- see lib/nyaa/export.lua for the full
-- target list and what each one assumes about where you'll point it).
-- write_all() writes every target's file into one directory in a single
-- call; point your other apps' configs at files under here (an @import
-- in gtk.css, `require("colors-uwuwm")` merged into `config.colors` in
-- wezterm.lua, an `include` in fuzzel.ini, ...) and they pick up
-- whatever nyaa.wear() above chose on every uwu.reload().
--
-- local ok, errs = nyaa.export.write_all(
--   os.getenv('HOME') .. '/.cache/uwuwm-theme',
--   'catppuccin_mocha'
-- )
-- for target, err in pairs(errs) do
--   print('nyaa.export failed for ' .. target .. ': ' .. err)
-- end

-- paw.defaults() owns the five *behavior*/app fields instead --
-- master_factor (initial tiling ratio), repeat_rate/repeat_delay
-- (keyboard timing), and terminal/launcher (just the two app strings
-- your own keybinds below reference). Returns the merged table, so
-- `paw.defaults({...}).terminal` below is the actual value that got
-- applied, not just whatever this file happened to pass in.
local apps = paw.defaults({
  master_factor = 0.55,
  repeat_rate = 25,
  repeat_delay = 240,
  terminal = 'kitty',
  launcher = 'kitty -e puffy',
  focus_follows_mouse = true, -- uncomment for hover-to-focus instead
  -- of click-to-focus. Off by default; see uwu.set's doc comment in
  -- lib/meta/uwu.lua. Menus/tooltips/DND icons never take focus this
  -- way, and moving over bare desktop leaves the last-focused window
  -- alone rather than clearing focus.
})

local terminal = apps.terminal
local launcher = apps.launcher
local mod = { 'mod' }

-- ── Special workspaces (scratchpads) ────────────────────────────────────
-- paw.specialworkspace -- Hyprland-style named scratchpads: a floating
-- window that toggles in and out of view on top of whatever tag you're
-- currently on, instead of living on a fixed tag you have to go find it
-- on. Built on client.minimized (see View::setMinimized in view.cpp) --
-- toggling one off doesn't kill it, just hides it; toggling back on
-- restores exactly where it was.
--
-- `spawn` is the command to launch if no instance is running yet;
-- `match` is how paw recognizes the resulting window once it maps
-- (usually the app's own --class/--app-id flag, so it doesn't
-- accidentally claim an unrelated window of the same app you already
-- had open); `size` is fractional (0-1) of the focused output's
-- dimensions, applied fresh every time it's shown.
paw.specialworkspace.set('term', {
  spawn = terminal .. ' --class scratch_term',
  match = { app_id = 'scratch_term' },
  size = { width = 0.6, height = 0.55 },
})

paw.specialworkspace.set('music', {
  spawn = 'kitty --class=scratch_music -e rmpc',
  match = { app_id = 'scratch_music' },
  size = { width = 0.4, height = 0.6 },
})

paw.keys({
  {
    mod,
    'grave',
    function()
      paw.specialworkspace.toggle('term')
    end,
    'toggle scratchpad terminal',
  },
  {
    { 'mod', 'shift' },
    'grave',
    function()
      paw.specialworkspace.toggle('music')
    end,
    'toggle scratchpad music player',
  },
})

-- ── Global keybinds ──────────────────────────────────────────────────
-- paw.keys() takes the whole list at once and calls uwu.bind() for
-- each entry -- { mods, key, fn, description } -- no separate "build a
-- key object, join it into a list, then push the list" steps; a plain
-- uwu.bind({ "mod" }, "x", fn) call anywhere else in this file works
-- exactly the same as an entry here. The description (4th field) isn't
-- read by uwuwm today; it's kept on paw.keymap for your own use (a
-- which-key-style overlay, a `--dump-keys` debug bind, etc).
local keys = {
  {
    mod,
    'Return',
    function()
      uwu.spawn(terminal)
    end,
    'open a terminal',
  },
  {
    mod,
    'd',
    function()
      uwu.spawn(launcher)
    end,
    'app launcher',
  },
  { { 'mod', 'shift' }, 'q', uwu.quit, 'quit uwuwm' },
  { { 'mod', 'shift' }, 'r', uwu.reload, 'reload config' },
  { mod, 'j', paw.client.focus_next, 'focus next window' },
  { mod, 'k', paw.client.focus_prev, 'focus previous window' },
  { { 'mod', 'shift' }, 'c', paw.client.kill, 'close focused window' },
  { mod, 'space', paw.client.toggle_floating, 'toggle floating' },
  { mod, 'f', paw.client.toggle_fullscreen, 'toggle fullscreen' },

  -- Floating-window nudges -- paw.client.move/resize (relative deltas on
  -- the focused client). No-op with nothing focused; errors loudly on a
  -- *tiled* client instead of silently doing nothing, same restriction
  -- the raw c:move()/c:resize() have (see their comment in
  -- lua_config.cpp) -- these binds are only meaningful once mod+space
  -- above has floated something.
  {
    { 'mod', 'alt' },
    'h',
    function()
      paw.client.move(-20, 0)
    end,
    'nudge floating window left',
  },
  {
    { 'mod', 'alt' },
    'l',
    function()
      paw.client.move(20, 0)
    end,
    'nudge floating window right',
  },
  {
    { 'mod', 'alt' },
    'j',
    function()
      paw.client.move(0, 20)
    end,
    'nudge floating window down',
  },
  {
    { 'mod', 'alt' },
    'k',
    function()
      paw.client.move(0, -20)
    end,
    'nudge floating window up',
  },
  {
    { 'mod', 'alt', 'shift' },
    'l',
    function()
      paw.client.resize(20, 0)
    end,
    'grow floating window width',
  },
  {
    { 'mod', 'alt', 'shift' },
    'h',
    function()
      paw.client.resize(-20, 0)
    end,
    'shrink floating window width',
  },
  {
    { 'mod', 'alt', 'shift' },
    'j',
    function()
      paw.client.resize(0, 20)
    end,
    'grow floating window height',
  },
  {
    { 'mod', 'alt', 'shift' },
    'k',
    function()
      paw.client.resize(0, -20)
    end,
    'shrink floating window height',
  },

  -- Keyboard-armed move/resize -- mod+z arms an interactive drag-move on
  -- the focused client, mod+x arms drag-resize; move the mouse to
  -- actually move/resize it, click any button to drop it. Same grab
  -- mod+drag/mod+right-drag start below (see uwu.mousebind() calls near
  -- the end of this file), just armed by a key on the focused client
  -- instead of a click on whatever's under the cursor. No-op with
  -- nothing focused, or on a tiled/fullscreen client.
  {
    mod,
    'z',
    paw.client.begin_move,
    'keyboard-arm drag-move (mouse to move, click to drop)',
  },
  {
    mod,
    'x',
    paw.client.begin_resize,
    'keyboard-arm drag-resize (mouse to resize, click to drop)',
  },

  {
    mod,
    'i',
    function()
      paw.layout.inc_master(0.05)
    end,
    'grow master area',
  },
  {
    mod,
    'u',
    function()
      paw.layout.inc_master(-0.05)
    end,
    'shrink master area',
  },
  {
    mod,
    'Tab',
    function()
      uwu.focus_monitor(1)
    end,
    'focus next monitor',
  },
  {
    { 'mod', 'shift' },
    'Tab',
    function()
      -- paw.monitor.focused()/each() -- workflow sugar over
      -- uwu.monitor.focused()/list(), same relationship paw.client has
      -- to uwu.client. Swap the print() for notify-send/eww/whatever
      -- your bar reads from for an actual on-screen monitor indicator.
      print('focused output: ' .. (paw.monitor.focused() or '?'))
      paw.monitor.each(function(m)
        print(
          ('  %s %dx%d @ %d,%d'):format(m.name, m.width, m.height, m.x, m.y)
        )
      end)
    end,
    'print monitor layout',
  },
  {
    mod,
    'e',
    function()
      uwu.spawn('emacsclient -c')
    end,
    'emacsclient',
  },
  {
    { 'mod', 'shift' },
    's',
    function()
      uwu.spawn(
        'grim -g "$(slurp)" ~/Pictures/screenshot-' .. os.time() .. '.png'
      )
    end,
    'screenshot a region',
  },
  {
    mod,
    'l',
    function()
      uwu.spawn('swaylock')
    end,
    'lock the screen',
  },

  -- volume / brightness -- uwu.system.* (see lib/meta/uwu.lua). Volume
  -- shells out to wpctl under the hood; brightness reads/writes
  -- /sys/class/backlight directly. Both are silent no-ops if the
  -- underlying tool/device isn't there, so these binds are always safe
  -- to leave in even on a desktop with no backlight.
  {
    {},
    'XF86AudioRaiseVolume',
    function()
      uwu.system.volume.set(math.min((uwu.system.volume.get() or 0) + 5, 150))
    end,
    'volume up',
  },
  {
    {},
    'XF86AudioLowerVolume',
    function()
      uwu.system.volume.set(math.max((uwu.system.volume.get() or 0) - 5, 0))
    end,
    'volume down',
  },
  { {}, 'XF86AudioMute', uwu.system.volume.toggle_mute, 'toggle mute' },
  {
    {},
    'XF86MonBrightnessUp',
    function()
      uwu.system.brightness.set(
        math.min((uwu.system.brightness.get() or 0) + 5, 100)
      )
    end,
    'brightness up',
  },
  {
    {},
    'XF86MonBrightnessDown',
    function()
      uwu.system.brightness.set(
        math.max((uwu.system.brightness.get() or 0) - 5, 1)
      )
    end,
    'brightness down',
  },

  -- dwindle: bound whether or not this output is currently in dwindle
  -- mode (paw.layout.set("dwindle")/("master") switches per-output --
  -- see uwu.layout.set's own comment in lua_config.cpp for why it's
  -- per-output, not global). These are no-ops on a master-stack output.
  {
    { 'mod', 'shift' },
    'space',
    paw.layout.dwindle.toggle_split,
    'toggle dwindle split orientation',
  },
  {
    { 'mod', 'shift' },
    'j',
    paw.layout.dwindle.swap_split,
    'swap dwindle split',
  },
  {
    mod,
    'r',
    function()
      paw.layout.dwindle.rotate_split(90)
    end,
    'rotate dwindle split 90°',
  },
}

-- Tags 1..9: mod+N to view, mod+shift+N to move focused window, and
-- mod+ctrl+N (only 1-5, keeps the modifier-heavy combo to the tags most
-- people actually reach for) to toggle a tag into/out of the visible set.
paw.tags(function(i)
  table.insert(keys, {
    mod,
    tostring(i),
    function()
      uwu.tag.view(i)
    end,
    'view tag ' .. i,
  })
  table.insert(keys, {
    { 'mod', 'shift' },
    tostring(i),
    function()
      uwu.tag.move_client_here(i)
    end,
    'move window to tag ' .. i,
  })
  if i <= 5 then
    table.insert(keys, {
      { 'mod', 'ctrl' },
      tostring(i),
      function()
        uwu.tag.toggle(i)
      end,
      'toggle tag ' .. i,
    })
  end
end)

paw.keys(keys)

-- Floating-window drag-move/drag-resize -- sway/i3's floating_modifier
-- equivalent. Hold `mod` and drag with the left button anywhere on a
-- floating client (not just a titlebar -- there isn't one) to move it;
-- mod+right-drag resizes it, picking which edge grows/shrinks from
-- whichever quadrant of the window the cursor was over when the drag
-- started. Both are silent no-ops on a tiled/fullscreen client, same as
-- the mod+alt nudge binds above -- click-dragging something that isn't
-- floating yet does nothing until mod+space has floated it first. See
-- mod+z/mod+x above for a keyboard-armed version of the same grab that
-- doesn't need a click to start it.
uwu.mousebind({ 'mod' }, 'left', function(c)
  c:begin_move()
end)
uwu.mousebind({ 'mod' }, 'right', function(c)
  c:begin_resize()
end)

-- You can bind floating window manipulation to individual keys alongside
-- the core mouse binds
uwu.bind({ 'mod' }, 'z', function(c)
  c:begin_move()
end)
uwu.bind({ 'mod' }, 'x', function(c)
  c:begin_resize()
end)

-- uwu.tag.close_all(n) gracefully closes every window on tag n --
-- destructive, so it's deliberately not bound to a key here. Wire it up
-- yourself if you want it, e.g. a dedicated "close everything on tag 9"
-- bind:
-- paw.keys({ { { "mod", "shift", "ctrl" }, "9", function() uwu.tag.close_all(9) end } })

-- ── Monitor configuration ───────────────────────────────────────────────
-- Output configuration (position/mode/scale/transform/adaptive-sync) is
-- lower level than anything a theme/keybind wrapper should paper over --
-- straight uwu.monitor.*, no paw sugar over this. Run `uwu.monitor.list()`
-- (e.g. from a keybind that dumps it to notify-send or a log) to see
-- connected output names before filling these in.
--
uwu.monitor.set('eDP-1', {
  x = 0,
  y = 0,
  width = 1920,
  height = 1080,
  refresh = 144,
  scale = 1.0,
  transform = 'normal', -- normal|90|180|270|flipped|flipped-90|...
  adaptive_sync = true,
})
-- uwu.monitor.set("eDP-1", { x = 2560, y = 0, enabled = true })
-- uwu.monitor.set("*", { scale = 1.0 }) -- fallback default for anything else

-- ── Input device configuration ──────────────────────────────────────────
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
-- paw.rule({ when = {...}, set = {...} }) -- `when` matches on the
-- client (app_id, title, or the floating state uwuwm already
-- auto-detected before this rule runs), `set` is what gets applied.
-- paw.like(pattern) marks a field as a Lua pattern (uwuwm's own "~"
-- prefix convention) instead of an exact string match.
paw.rule({ when = { app_id = 'mpv' }, set = { floating = true, tag = 3 } })
paw.rule({ when = { app_id = 'pavucontrol' }, set = { floating = true } })
paw.rule({ when = { app_id = paw.like('steam_app_.*') }, set = { tag = 9 } })
-- Anything already auto-detected as floating (a parent window, a
-- fixed-size dialog, or an X11 client with
-- _NET_WM_WINDOW_TYPE_DIALOG/UTILITY/SPLASH) that isn't caught by a
-- more specific rule above still gets parked on the scratch tag
-- (9) -- one rule instead of naming every file-picker/color-chooser
-- app_id by hand.
paw.rule({ when = { floating = true }, set = { tag = 9 } })

-- Per-client border theming -- nyaa.rule(), sugar over uwu.rule()'s
-- apply.border_color_active/inactive/opacity. `preset` seeds both colors
-- from nyaa.presets; explicit fields override/extend it, same as
-- nyaa.wear().
nyaa.rule({ when = { app_id = 'mpv' }, border_color_active = '#f9e2af' }) -- catppuccin yellow
nyaa.rule({ when = { app_id = paw.like('steam_app_.*') }, preset = 'nord' })

-- Per-client opacity -- only takes effect while the client is unfocused
-- (a focused window is always fully opaque, same as the global
-- inactive_opacity above). A picture-in-picture mpv instance (see the
-- floating+tag-3 paw.rule above) staying legible-but-see-through when
-- you're not actively looking at it is the obvious use, but anything
-- floating benefits -- pavucontrol, a scratch terminal, etc.
nyaa.rule({ when = { app_id = 'mpv' }, opacity = 0.85 })

-- ── Hooks ──────────────────────────────────────────────────────────────
-- paw.on(event, fn) is uwu.hook() with short verb names in place of the
-- raw "client::"-prefixed event strings -- "opens"/"closes"/"focuses"/
-- "unfocuses"/"fullscreens"/"retitles" (fn gets the same Client userdata
-- either way: c.title, c.app_id, c.is_xwayland, c.tags, c.floating,
-- c.fullscreen, c.output, c.geo, c.opacity, c:focus(), c:kill(),
-- c:set_tag(n), c:toggle_tag(n), c:move_to_output(name), c:move(x, y),
-- c:resize(w, h)), plus "tags_changed" (monitor_name, new_tagset),
-- "monitor_connected"/"monitor_disconnected" (monitor_name). Any raw
-- "namespace::event" string works too, unchanged -- paw.on() only
-- rewrites the names it recognizes.
--
-- paw.off(id) removes a registration later -- handy for ad-hoc hooks (a
-- "focus this app" toggle keybind, say) without leaving a permanent
-- listener behind.
local focus_steam_id = paw.on('focuses', function(c)
  if c.app_id == 'steam' then
    -- ensure steam (and only steam) lands on tag 9 when it gets focus
    if c.tags ~= 256 then
      c:set_tag(9)
    end
  end
end)

paw.off(focus_steam_id) -- to remove the hook above later.

-- "retitles" fires on every title change, browsers especially (a tab
-- switch retitles the whole window) -- useful for anything that wants a
-- live title without polling c.title from a timer, e.g. syncing a
-- specific client's title out to a status-bar widget:
paw.on('retitles', function(c)
  if c.app_id == 'firefox' then
    print('firefox title -> ' .. c.title)
  end
end)

-- ── Querying current state ─────────────────────────────────────────────
-- uwu.client.list() and uwu.client.focused() round out the read side --
-- the same Client userdata every hook callback gets, so any method on
-- one of these works too:
--
--   for _, c in ipairs(uwu.client.list()) do
--     if c.app_id == "slack" and c.output ~= "eDP-1" then
--       c:move_to_output("eDP-1")
--     end
--   end
--
-- Keybind this to mod+shift+s for "tidy slack back onto the laptop
-- panel" if you tend to drag it onto an external display by accident.

-- ── Status bar ───────────────────────────────────────────────────────
-- uwu.widget.window({...}) -- a retained-mode widget surface
-- (src/widget.cpp). Two modes:
--   mode = "bar"   (default) -- anchored to an output edge
--   mode = "popup" -- a fixed-size floating box (OSD/dashboard),
--                    positioned by screen anchor + margin, drawn
--                    above everything. Starts hidden -- call :show(),
--                    pair with uwu.timer.set_timeout() to auto-hide.
--
-- Widgets (rect()/text()) can nest via an optional `parent` field
-- and position with :anchor(my_edge, target, target_edge, margin) --
-- target is nil for "my own parent" or another widget from the same
-- window; edges are left/right/top/bottom/hcenter/vcenter.
-- :anchor_fill(target, margin) covers target entirely, inset by
-- margin (the one case anchors are allowed to resize a widget).

local function setup_bar()
  local bar = uwu.widget.window({ mode = 'bar', position = 'top', height = 28 })
  local bg = bar:rect({
    x = 0,
    y = 0,
    w = bar:width(),
    h = bar:height(),
    color = 0x1e1e2eff,
  })

  local clock = bar:text({
    text = '--:--:--',
    font_size = 14,
    color = 0xffffffff,
    parent = bg,
  })
  clock:anchor('right', bg, 'right', 8)
  clock:anchor('vcenter', bg, 'vcenter', 0)

  bar:commit()
  uwu.timer.set_interval(1, function()
    clock:set_text(os.date('%H:%M:%S'))
    bar:commit()
  end)
  return bar
end

-- A volume OSD: small popup, bottom-center, auto-hides after 1.5s.
-- Create once at startup; show()/re-show it on volume changes instead
-- of building a new window each time.
local function setup_volume_osd()
  local osd = uwu.widget.window({
    mode = 'popup',
    anchor = 'bottom',
    margin = 48,
    width = 240,
    height = 56,
  })
  local bg = osd:rect({ color = 0x1e1e2eee })
  bg:anchor_fill(nil, 0)

  local label = osd:text({
    text = 'Volume: 100%',
    font_size = 14,
    color = 0xffffffff,
    parent = bg,
  })
  label:anchor('hcenter', bg, 'hcenter', 0)
  label:anchor('vcenter', bg, 'vcenter', 0)
  osd:commit()

  local hide_timer = nil
  return function(level)
    label:set_text(('Volume: %d%%'):format(level))
    osd:commit()
    osd:show()
    if hide_timer then
      uwu.timer.cancel(hide_timer)
    end
    hide_timer = uwu.timer.set_timeout(1.5, function()
      osd:hide()
    end)
  end
end

setup_bar()
local show_volume_osd = setup_volume_osd()
-- call show_volume_osd(72) from a volume keybind, etc.

paw.on('monitor_connected', setup_bar)
