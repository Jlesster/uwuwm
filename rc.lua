-- uwuwm config -- copy this to ~/.config/uwuwm/rc.lua and edit freely.
--
-- Reloading: mod+shift+r (bound below) or `pkill -HUP uwuwm` from a shell
-- both re-run this whole file from scratch without restarting the
-- compositor. If the new file fails to load (syntax error, etc.) uwuwm
-- keeps running on the last-known-good config instead of losing your
-- keybinds -- see LuaConfig::reload() if you want the details.

-- ── Appearance / behavior settings ─────────────────────────────────────
uwu.set('gap', 8)
uwu.set('border_width', 2)
uwu.set('master_factor', 0.55)
uwu.set('border_color_active', '#cba6f7') -- Catppuccin Mocha mauve
uwu.set('border_color_inactive', '#313244') -- Catppuccin Mocha surface0
uwu.set('background_color', '#11111b') -- Catppuccin Mocha crust
uwu.set('repeat_rate', 25)
uwu.set('repeat_delay', 600)
uwu.set('cursor_size', 24)

local terminal = 'wezterm'
local launcher = 'fuzzel'
uwu.set('terminal', terminal)
uwu.set('launcher', launcher)

-- ── Core keybinds ───────────────────────────────────────────────────────
uwu.bind({ 'mod' }, 'Return', function()
  uwu.spawn(terminal)
end)
uwu.bind({ 'mod' }, 'd', function()
  uwu.spawn(launcher)
end)
uwu.bind({ 'mod', 'shift' }, 'q', function()
  uwu.quit()
end)
uwu.bind({ 'mod', 'shift' }, 'r', function()
  uwu.reload()
end)
uwu.bind({ 'mod' }, 'j', function()
  uwu.focus_next()
end)
uwu.bind({ 'mod' }, 'k', function()
  uwu.focus_prev()
end)
uwu.bind({ 'mod', 'shift' }, 'c', function()
  uwu.kill()
end)
uwu.bind({ 'mod' }, 'space', function()
  uwu.toggle_floating()
end)
uwu.bind({ 'mod' }, 'f', function()
  uwu.toggle_fullscreen()
end)
uwu.bind({ 'mod' }, 'i', function()
  uwu.inc_master(0.05)
end)
uwu.bind({ 'mod' }, 'u', function()
  uwu.inc_master(-0.05)
end)
uwu.bind({ 'mod' }, 'Tab', function()
  uwu.focus_monitor(1)
end)

-- Tags 1..9: mod+N to view, mod+shift+N to move focused window, and
-- mod+ctrl+N (only 1-5, keeps the modifier-heavy combo to the tags most
-- people actually reach for) to toggle a tag into/out of the visible set.
for i = 1, uwu.tag_count do
  uwu.bind({ 'mod' }, tostring(i), function()
    uwu.tag.view(i)
  end)
  uwu.bind({ 'mod', 'shift' }, tostring(i), function()
    uwu.tag.move_client_here(i)
  end)
  if i <= 5 then
    uwu.bind({ 'mod', 'ctrl' }, tostring(i), function()
      uwu.tag.toggle(i)
    end)
  end
end

-- uwu.tag.close_all(n) gracefully closes every window on tag n --
-- destructive, so it's deliberately not bound to a key here. Wire it up
-- yourself if you want it, e.g. a dedicated "close everything on tag 9"
-- bind:
-- uwu.bind({ 'mod', 'shift', 'ctrl' }, '9', function()
--   uwu.tag.close_all(9)
-- end)

-- Extra utility binds, not in the built-in fallback config:
uwu.bind({ 'mod' }, 'e', function()
  uwu.spawn('emacsclient -c')
end)
uwu.bind({ 'mod', 'shift' }, 's', function()
  uwu.spawn('grim -g "$(slurp)" ~/Pictures/screenshot-$(date +%s).png')
end)
uwu.bind({ 'mod' }, 'l', function()
  uwu.spawn('swaylock')
end)

-- ── Monitor configuration ───────────────────────────────────────────────
-- Every field is optional; only what's set is applied/stored. Run
-- `uwu.monitor.list()` (e.g. from a keybind that dumps it to a notify-send
-- or a log) to see connected output names before filling these in.
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

-- Example for the Lenovo LOQ's laptop panel + an external monitor at
-- 144Hz with VRR, uncomment and adjust names/resolution to match:
-- uwu.monitor.set("eDP-1", { width = 1920, height = 1080, refresh = 144, scale = 1.0 })
-- uwu.monitor.set("DP-2", { x = 1920, y = 0, width = 2560, height = 1440, refresh = 144, adaptive_sync = true })

-- ── Input device configuration ──────────────────────────────────────────
-- `match` is an exact libinput device name (see uwu.input.list(), e.g.
-- from a keybind that dumps it to a notify-send or a log), a type
-- selector ("type:touchpad" / "type:mouse"), or "*" for a fallback
-- default applied to any pointer device without a more specific rule --
-- exact name always wins over type, and type always wins over "*".
--
-- Sane touchpad defaults most laptop users want on day one:
uwu.input.set('type:touchpad', {
  tap = true, -- tap-to-click
  tap_drag = true, -- tap-and-hold then move = drag
  tap_drag_lock = true, -- lift finger mid-drag without dropping it
  natural_scroll = true, -- content-follows-fingers scroll direction
  dwt = true, -- ignore touchpad while actively typing
})

-- Flat (no acceleration curve) pointer response for every mouse by
-- default -- comment out if you'd rather keep libinput's adaptive curve:
-- uwu.input.set('type:mouse', { accel_profile = 'flat' })

-- Per-device override example: a gaming mouse where you want 1:1
-- physical-to-logical motion regardless of the "type:mouse" rule above
-- (exact name wins). Run `uwu.input.list()` to find the exact name
-- libinput reports for your own hardware:
-- uwu.input.set("Logitech G Pro Wireless Gaming Mouse", {
--     accel_profile = "flat",
--     accel_speed = 0.0,
-- })

-- Left-handed mouse, on-button-down scrolling with a specific button,
-- and clickfinger click detection are also available:
-- uwu.input.set("*", { left_handed = true })
-- uwu.input.set("type:touchpad", { click_method = "clickfinger" })
-- uwu.input.set("type:mouse", { scroll_method = "on_button_down", scroll_button = 274 }) -- BTN_MIDDLE

-- ── Client rules & event hooks ──────────────────────────────────────────
-- The Client object (uwu.hook("client::*", fn) callbacks get one as their
-- single argument) is a live, validated wrapper around the underlying
-- window: its property reads (c.title, c.app_id, c.tags, ...) and
-- methods (c:focus(), c:set_tag(n), ...) re-check the window still
-- exists on every access, so it's always safe to capture a Client in
-- a longer-lived closure (a keybind, a layout rule, a saved hook).
--
-- Client fields:
--   c.id           integer, stable for the client's lifetime -- same id
--                  `uwuwmctl get_clients` (IPC) reports, so a script can
--                  correlate the two
--   c.title        string
--   c.app_id       string (X11 WM_CLASS is folded into app_id by xwayland_view)
--   c.is_xwayland  bool, true for X11 clients, false for native xdg-shell
--   c.tags         integer bitmask, read-write via c:set_tag / c:toggle_tag
--   c.floating     bool, read-write
--   c.fullscreen   bool, read-write
--   c.output       monitor name string, or nil if the client is unmapped
--   c.geo          { x, y, width, height } read-only snapshot
--
-- Client methods: c:focus(), c:kill(), c:set_tag(n), c:toggle_tag(n),
-- c:move_to_output(name).
--
-- uwu.rule() is sugar over uwu.hook("client::manage", ...) -- every
-- rule is just one hook, and inherits the same timeout/recursion guard
-- every other hook gets. For anything that doesn't fit the match/apply
-- shape (per-app layout, focus-restoration, custom decoration, ...)
-- drop down to uwu.hook directly.

-- Walk every mapping rule through the same shape: `match` selects the
-- client, `apply` mutates it. Recognised match keys: app_id/title (exact
-- string, or a "~lua-pattern"), floating (bool -- matches whatever
-- handleMap's own parent/fixed-size/X11-dialog-window-type detection
-- already decided, *before* this rule's own apply.floating can override
-- it). Recognised apply keys: floating (bool), fullscreen (bool), tag
-- (1..9), output (monitor name string, same lookup as
-- c:move_to_output()).
uwu.rule({
  match = { app_id = 'mpv' }, -- any app id, or a "~lua-pattern"
  apply = { floating = true, tag = 3 },
})
uwu.rule({
  match = { app_id = 'foot' },
  apply = { tag = 1 },
})
uwu.rule({
  match = { app_id = 'pavucontrol' }, -- dialog: open centered/floating
  apply = { floating = true },
})
uwu.rule({
  match = { app_id = '~steam_app_.*' }, -- "~" prefix = Lua pattern
  apply = { tag = 9 },
})
-- Anything already auto-detected as floating (a parent window, a
-- fixed-size dialog, or an X11 client with _NET_WM_WINDOW_TYPE_DIALOG/
-- UTILITY/SPLASH) that isn't caught by a more specific rule above still
-- gets parked on the scratch tag -- one rule instead of naming every
-- file-picker/color-chooser app_id by hand.
uwu.rule({
  match = { floating = true },
  apply = { tag = 9 },
})

-- uwu.hook() for everything a rule can't express. The `id` return value
-- lets uwu.unhook() remove this exact registration later -- useful for
-- ad-hoc hooks (a "focus this app" toggle keybind, say) without having
-- to leave a permanent listener behind.
local focus_steam_id = uwu.hook('client::focus', function(c)
  if c.app_id == 'steam' then
    -- ensure steam (and only steam) lands on tag 9 when it gets focus
    if c.tags ~= 256 then
      c:set_tag(9)
    end
  end
end)

-- uwu.hook("client::manage", fn)  fn(c)  -- fires for every newly-mapped
--                                          managed window. Use this for
--                                          "this app should always ..."
--                                          setup. Fires after the new
--                                          client is focused, so
--                                          c:move_to_output("eDP-1")
--                                          in here does what you'd
--                                          expect.
--
-- uwu.hook("client::unmanage", fn)  fn(c)  -- fires when a managed
--                                            window is about to be
--                                            destroyed (the XDG/X11
--                                            destroy event, not the
--                                            unmap, so a tray app that
--                                            hides-then-shows still gets
--                                            exactly one manage/unmanage
--                                            pair over its lifetime).
--                                            X11 override-redirect
--                                            surfaces (dropdowns,
--                                            tooltips, DND icons) are
--                                            excluded from both manage
--                                            and unmanage, same as they
--                                            are from uwu.client.list().
--
-- uwu.hook("client::focus", fn)      fn(c)  -- fires when c becomes
-- uwu.hook("client::unfocus", fn)    fn(c)  -- keyboard-focused. Always
--                                            fired as a pair; uwu.unhook
--                                            from inside either one is
--                                            safe (the dispatcher
--                                            snapshots ids before
--                                            iterating).
-- uwu.hook("client::fullscreen", fn) fn(c)  -- fires on every fullscreen
--                                            transition, regardless of
--                                            who caused it (client
--                                            request, foreign-toplevel
--                                            request, mod+f keybind,
--                                            c.fullscreen = ... from a
--                                            rule or hook).
--
-- uwu.hook("tag::change", fn)        fn(monitor_name, new_tagset)
-- uwu.hook("output::connect", fn)    fn(monitor_name)
-- uwu.hook("output::disconnect", fn) fn(monitor_name)
--
-- Use uwu.unhook(id) to remove any of the above later:
-- uwu.unhook(focus_steam_id)

-- ── Querying current state ─────────────────────────────────────────────
-- uwu.client.list() and uwu.client.focused() round out the read side.
-- The return values are the same Client userdata the hooks get, so any
-- method on a Client works on them too:
--
--   for _, c in ipairs(uwu.client.list()) do
--     if c.app_id == 'slack' and c.output ~= 'eDP-1' then
--       c:move_to_output('eDP-1')
--     end
--   end
--
-- Keybind this to mod+shift+s for "tidy slack back onto the laptop
-- panel" if you tend to drag it onto an external display by accident.
