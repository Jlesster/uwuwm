-- examples/bar_1.lua -- minimal top bar: tag indicator on the left,
-- clock on the right. The simplest example, used as a baseline that the
-- other three build on. Reads `paw.workspace.current_indices(name)` for
-- the tag number (one of the few callers of the per-monitor tagset read
-- path -- most bars only ever show the focused monitor's tag, this one
-- shows each output's own) and `os.date('%H:%M:%S')` for the clock.
--
-- opts:
--   height  integer, pixels, default 28
--   on      "all" (default) | "focused" | "<output name>"
--           which monitor(s) the bar is created on. Hot-plugged outputs
--           are auto-detected via paw.on('monitor_connected', ...) so
--           this only needs to cover the initial set of outputs that
--           exist when rc.lua runs.
--   bg      bar background color, 0xRRGGBBAA, default 0x1e1e2eff
--   fg      foreground text color, default 0xffffffff
--   font_size  default 14
--
-- The return value is the table itself -- call `.setup({ ... })` once
-- from rc.lua and the file does the rest (initial output + future
-- hot-plug, including cleanup on disconnect).
--
-- Example (rc.lua):
--
--   require('examples.bar_1').setup({ height = 28 })

local M = {}

-- Internal: build a single bar instance for one named output. Holds the
-- bar handle, the current set of widgets, and the periodic timer that
-- drives the clock. Returns a teardown function for monitor_disconnected.
local function build(monitor_name, opts)
  -- Skip the initial "all" sweep when the user only wanted focused /
  -- a specific output. monitor_connected still uses the same filter
  -- below, so the contract is uniform.
  if opts.on == 'focused' then
    local focused = uwu.monitor.focused()
    if focused ~= monitor_name then return function() end end
  elseif type(opts.on) == 'string' and opts.on ~= 'all' then
    if opts.on ~= monitor_name then return function() end end
  end

  local bar = uwu.widget.window({
    output   = monitor_name,
    mode     = 'bar',
    position = 'top',
    height   = opts.height,
  })

  -- Background. anchor_fill to the bar root so it always tracks the
  -- bar's full size, even when the bar resizes on output geometry
  -- changes (laptop undocked, monitor resolution change, ...).
  local bg = bar:rect({ color = opts.bg })
  bg:anchor_fill(nil, 0)

  -- Tag indicator on the left. Shows the first visible tag number for
  -- this output (most setups only have one visible, but the
  -- bitmask-aware helpers already handle multi-tag, so we don't bother
  -- rendering "1+3" or similar here -- one number is enough for an
  -- at-a-glance indicator and the bar is small).
  local tag_label = bar:text({
    text      = '1',
    font_size = opts.font_size,
    color     = opts.fg,
    parent    = bg,
  })
  tag_label:anchor('left',   bg, 'left',   12)
  tag_label:anchor('vcenter', bg, 'vcenter', 0)

  -- Clock on the right.
  local clock = bar:text({
    text      = '--:--:--',
    font_size = opts.font_size,
    color     = opts.fg,
    parent    = bg,
  })
  clock:anchor('right',   bg, 'right',  12)
  clock:anchor('vcenter', bg, 'vcenter', 0)

  -- One commit after wiring everything; the hooks below drive later
  -- commits.
  bar:commit()

  -- ── live updates ────────────────────────────────────────────────────

  local function refresh_tag()
    local idx = paw.workspace.current_indices(monitor_name)
    tag_label:set_text(idx[1] and tostring(idx[1]) or '-')
    bar:commit()
  end
  refresh_tag()

  -- The tagset is per-output, so refresh only when this specific
  -- output's tagset changes (paw.workspace.on_change gives us the
  -- monitor_name as its first arg; that one already does the filter
  -- for us).
  paw.workspace.on_change(function(name)
    if name == monitor_name then refresh_tag() end
  end)

  -- Clock ticks once a second. Cancel the timer on disconnect so we
  -- don't keep firing into a destroyed bar.
  local timer = uwu.timer.set_interval(1, function()
    clock:set_text(os.date('%H:%M:%S'))
    bar:commit()
  end)

  return function()
    uwu.timer.cancel(timer)
    bar:destroy()
  end
end

function M.setup(opts)
  opts = opts or {}
  opts.height    = opts.height    or 28
  opts.bg        = opts.bg        or 0x1e1e2eff
  opts.fg        = opts.fg        or 0xffffffff
  opts.font_size = opts.font_size or 14
  opts.on        = opts.on        or 'all'

  -- Map of monitor_name -> teardown function, used by the disconnect
  -- handler to find and destroy the right bar.
  local teardowns = {}

  local function install(monitor_name)
    teardowns[monitor_name] = build(monitor_name, opts)
  end

  -- Initial outputs: build a bar on every connected monitor (or just
  -- the focused one / a named one, per opts.on). Safe to call
  -- immediately -- lua_cfg.load() runs after wlr_backend_start, so
  -- uwu.monitor.list() is non-empty by this point.
  for _, m in ipairs(uwu.monitor.list()) do
    install(m.name)
  end

  -- Hot-plug: a new monitor shows up, build a bar for it; an old one
  -- goes away, tear its bar down. The ipc subscribe mirror in the
  -- README describes the same event source, so external tools see the
  -- exact same plugin/ unplug.
  paw.on('monitor_connected', install)
  paw.on('monitor_disconnected', function(name)
    local td = teardowns[name]
    if td then
      td()
      teardowns[name] = nil
    end
  end)
end

return M
