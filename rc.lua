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
    uwu.view_tag(i)
  end)
  uwu.bind({ 'mod', 'shift' }, tostring(i), function()
    uwu.move_to_tag(i)
  end)
  if i <= 5 then
    uwu.bind({ 'mod', 'ctrl' }, tostring(i), function()
      uwu.toggle_tag(i)
    end)
  end
end

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
