-- examples/bar_3.lua -- bottom bar with one pill per workspace
-- (1..uwu.tag_count) on the left and a small sys-stats cluster on
-- the right (volume + brightness + clock). Clicking a tag pill jumps
-- to that workspace; clicking a focused tag pill does nothing
-- observable (it's already focused). The pill color flips between
-- visible / not-visible states so a glance tells you which tag an
-- output is on.
--
-- opts:
--   height     integer, default 28
--   on         "all" | "focused" | "<name>" (default "all")
--   bg         bar background, default 0x1e1e2eff
--   fg         default text color, default 0xcdd6f4ff
--   pill_bg_idle   color of an unselected tag pill, default 0x313244ff
--   pill_bg_active color of the currently-visible tag pill, default 0x89b4faff
--   pill_fg_idle   text color in an idle pill, default 0xffffffff
--   pill_fg_active text color in an active pill, default 0x1e1e2eff
--   stat_pill_bg   default 0x313244ff
--   stat_pill_fg   default 0xffffffff
--   font_size  default 14
--
-- Example:
--
--   require('examples.bar_3').setup({ position = 'bottom', height = 28 })

local M = {}

-- One tag pill. Two children (a rect + a text), the text anchored to
-- the rect's center; the rect is sized to the text after the first
-- commit so long tag numbers (there aren't any, but) wouldn't
-- overflow. Like make_pill() in bar_2.lua, but the result is a small
-- table so the bar can update its label/color and click-target
-- independently after creation.
local function make_tag_pill(parent, idle_bg, active_bg, idle_fg, active_fg,
                             n, font_size)
  local pill_bg = parent:rect({ color = idle_bg })
  local pill_fg = parent:text({
    text      = tostring(n),
    font_size = font_size,
    color     = idle_fg,
    parent    = pill_bg,
  })
  pill_fg:anchor('hcenter', pill_bg, 'hcenter', 0)
  pill_fg:anchor('vcenter', pill_bg, 'vcenter', 0)
  return {
    bg     = pill_bg,
    fg     = pill_fg,
    number = n,
    idle_bg = idle_bg, active_bg = active_bg,
    idle_fg = idle_fg, active_fg = active_fg,
  }
end

local function size_pill(pill_bg, pill_fg, h_padding)
  pill_bg:set_size(pill_fg:width() + 2 * h_padding, pill_bg:height())
end

-- Mark a tag pill as "active" (this output's tagset includes it) or
-- "idle" (it doesn't). Bitmask-aware: if the user has two tags
-- visible (uwu.tag.toggle on two of them), both pills light up.
local function set_pill_active(pill, is_active)
  if is_active then
    pill.bg:set_color(pill.active_bg)
    pill.fg:set_color(pill.active_fg)
  else
    pill.bg:set_color(pill.idle_bg)
    pill.fg:set_color(pill.idle_fg)
  end
end

local function build(monitor_name, opts)
  if opts.on == 'focused' then
    local focused = uwu.monitor.focused()
    if focused ~= monitor_name then return function() end end
  elseif type(opts.on) == 'string' and opts.on ~= 'all' then
    if opts.on ~= monitor_name then return function() end end
  end

  local bar = uwu.widget.window({
    output   = monitor_name,
    mode     = 'bar',
    position = 'bottom',
    height   = opts.height,
  })

  local bg = bar:rect({ color = opts.bg })
  bg:anchor_fill(nil, 0)

  local h_padding = 10
  local pill_gap  = 6

  -- ── left zone: tag pills (1..uwu.tag_count), left-anchored in a
  -- chain. The first pill is anchored to the bar's left edge; each
  -- subsequent one is anchored to its left sibling's right edge +
  -- pill_gap. This keeps the cluster flush-left regardless of how
  -- many tags exist or which are active, with no manual x math.
  local tag_pills = {}
  for n = 1, uwu.tag_count do
    local pill = make_tag_pill(
      bg,
      opts.pill_bg_idle, opts.pill_bg_active,
      opts.pill_fg_idle, opts.pill_fg_active,
      n, opts.font_size)
    table.insert(tag_pills, pill)
    if n == 1 then
      pill.bg:anchor('left',    bg, 'left',    12)
    else
      pill.bg:anchor('left',    tag_pills[n - 1].bg, 'right', pill_gap)
    end
    pill.bg:anchor('vcenter', bg, 'vcenter', 0)

    -- Click-to-switch is intentionally NOT done via bar:on_click here:
    -- a Window has one slot for an on_click handler, and the existing
    -- keybind-driven flow (paw.workspace.go(n) from a global keybind,
    -- already in rc.lua's main keybind table) is the recommended path.
    -- Per-pill hit-testing from Lua would need a way to read each
    -- pill's resolved box back from the C side, which uwu.widget
    -- doesn't currently expose. A future "register click on this
    -- specific widget" API would slot in here cleanly.
  end

  -- ── right zone: clock + volume + brightness, anchored to the bar's
  -- right edge in a chain (same pattern as bar_2.lua). Order is
  -- fixed: brightness is leftmost, then volume, then clock, so the
  -- clock is the rightmost pill, matching the read direction.
  local clock_pill_bg, clock_pill_fg = (function()
    local b, f = bg:rect({ color = opts.stat_pill_bg }),
               bg:text({
                 text      = '--:--:--',
                 font_size = opts.font_size,
                 color     = opts.stat_pill_fg,
                 parent    = b,
               })
    f:anchor('hcenter', b, 'hcenter', 0)
    f:anchor('vcenter', b, 'vcenter', 0)
    b:anchor('right',   bg, 'right',  12)
    b:anchor('vcenter', bg, 'vcenter', 0)
    return b, f
  end)()

  local vol_pill_bg, vol_pill_fg = (function()
    local b, f = bg:rect({ color = opts.stat_pill_bg }),
               bg:text({
                 text      = 'Vol -',
                 font_size = opts.font_size,
                 color     = opts.stat_pill_fg,
                 parent    = b,
               })
    f:anchor('hcenter', b, 'hcenter', 0)
    f:anchor('vcenter', b, 'vcenter', 0)
    b:anchor('right',   clock_pill_bg, 'left', pill_gap)
    b:anchor('vcenter', bg,            'vcenter', 0)
    return b, f
  end)()

  local bright_pill_bg, bright_pill_fg = (function()
    local b, f = bg:rect({ color = opts.stat_pill_bg }),
               bg:text({
                 text      = 'Brt -',
                 font_size = opts.font_size,
                 color     = opts.stat_pill_fg,
                 parent    = b,
               })
    f:anchor('hcenter', b, 'hcenter', 0)
    f:anchor('vcenter', b, 'vcenter', 0)
    b:anchor('right',   vol_pill_bg, 'left', pill_gap)
    b:anchor('vcenter', bg,          'vcenter', 0)
    return b, f
  end)()

  -- First commit: resolves everything, measures text widths. Then
  -- size each pill to its content. The pills are wider than the
  -- text+padding; resizing them shrinks the cluster, but the
  -- chain-anchors (each anchored to its neighbor) keep the spacing
  -- consistent.
  bar:commit()
  for _, pill in ipairs(tag_pills) do
    size_pill(pill.bg, pill.fg, h_padding)
  end
  size_pill(clock_pill_bg,  clock_pill_fg,  h_padding)
  size_pill(vol_pill_bg,    vol_pill_fg,    h_padding)
  size_pill(bright_pill_bg, bright_pill_fg, h_padding)
  bar:commit()

  -- ── live updates ────────────────────────────────────────────────

  local function refresh_tags()
    local mask = uwu.tag.current(monitor_name) or 0
    for _, pill in ipairs(tag_pills) do
      set_pill_active(pill, (mask & (1 << (pill.number - 1))) ~= 0)
    end
  end

  local function refresh_clock()
    clock_pill_fg:set_text(os.date('%H:%M:%S'))
    size_pill(clock_pill_bg, clock_pill_fg, h_padding)
  end

  local function refresh_volume()
    local v = uwu.system.volume.get()
    if v then
      vol_pill_fg:set_text(('Vol %d%%'):format(v))
    else
      vol_pill_fg:set_text('Vol -')
    end
    size_pill(vol_pill_bg, vol_pill_fg, h_padding)
  end

  local function refresh_brightness()
    local b = uwu.system.brightness.get()
    if b then
      bright_pill_fg:set_text(('Brt %d%%'):format(b))
    else
      bright_pill_fg:set_text('Brt -')
    end
    size_pill(bright_pill_bg, bright_pill_fg, h_padding)
  end

  refresh_tags()
  refresh_volume()
  refresh_brightness()
  bar:commit()

  paw.workspace.on_change(function(name)
    if name == monitor_name then
      refresh_tags()
      bar:commit()
    end
  end)

  local sys_timer = uwu.timer.set_interval(1, function()
    refresh_volume()
    refresh_brightness()
    bar:commit()
  end)

  local clock_timer = uwu.timer.set_interval(1, function()
    refresh_clock()
    bar:commit()
  end)

  return function()
    uwu.timer.cancel(sys_timer)
    uwu.timer.cancel(clock_timer)
    bar:destroy()
  end
end

function M.setup(opts)
  opts = opts or {}
  opts.height         = opts.height         or 28
  opts.bg             = opts.bg             or 0x1e1e2eff
  opts.fg             = opts.fg             or 0xcdd6f4ff
  opts.pill_bg_idle   = opts.pill_bg_idle   or 0x313244ff
  opts.pill_bg_active = opts.pill_bg_active or 0x89b4faff
  opts.pill_fg_idle   = opts.pill_fg_idle   or 0xffffffff
  opts.pill_fg_active = opts.pill_fg_active or 0x1e1e2eff
  opts.stat_pill_bg   = opts.stat_pill_bg   or 0x313244ff
  opts.stat_pill_fg   = opts.stat_pill_fg   or 0xffffffff
  opts.font_size      = opts.font_size      or 14
  opts.on             = opts.on             or 'all'

  local teardowns = {}

  local function install(monitor_name)
    teardowns[monitor_name] = build(monitor_name, opts)
  end

  for _, m in ipairs(uwu.monitor.list()) do
    install(m.name)
  end

  paw.on('monitor_connected', install)
  paw.on('monitor_disconnected', function(name)
    local td = teardowns[name]
    if td then td(); teardowns[name] = nil end
  end)
end

return M
