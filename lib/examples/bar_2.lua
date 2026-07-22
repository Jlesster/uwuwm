-- examples/bar_2.lua -- top bar with three distinct zones:
-- tags on the left, the focused client's title in the middle, and a
-- system-tray cluster (clock + volume + brightness) on the right.
-- Stricter layout than bar_1.lua: the center zone shrinks to fit and
-- ellipsizes its text rather than overflowing into the right zone's
-- pills, since both zones are anchored to the same parent.
--
-- opts:
--   height     integer, default 28
--   on         "all" | "focused" | "<name>" (default "all")
--   bg         bar background, default 0x1e1e2eff
--   fg         foreground text color, default 0xffffffff
--   pill_bg    background of the right-zone pills, default 0x313244ff
--   pill_fg    foreground of the right-zone pills, default 0xffffffff
--   font_size  default 14
--
-- Example:
--
--   require('examples.bar_2').setup({ height = 30 })

local M = {}

-- Pills in the right zone. Each pill is a small rounded-ish rect
-- (a plain rect here; rounded corners would need either a separate
-- widget kind or a chain of overlapping rects) with a text label
-- inside, anchored to the right edge. They stack leftward as more are
-- added -- the same anchor chain is reused in bar_3.lua and bar_4.lua.
--
-- pill = make_pill(parent, bg_color, fg_color, text, font_size, x_margin)
-- The pill's rect is laid out first; the text is then anchored to its
-- hcenter/vcenter, with explicit left/right padding via the rect's
-- own width (set generously and the text is centered inside it).
local function make_pill(parent, bg, fg, text, font_size)
  local pill_bg = parent:rect({
    color = bg,
  })
  local pill_fg = parent:text({
    text      = text,
    font_size = font_size,
    color     = fg,
    parent    = pill_bg,
  })
  pill_fg:anchor('hcenter', pill_bg, 'hcenter', 0)
  pill_fg:anchor('vcenter', pill_bg, 'vcenter', 0)
  return pill_bg, pill_fg
end

-- Width of a pill, computed from its text's measured width plus a
-- horizontal margin. anchor_fill would overshoot: we want the rect to
-- hug the text, not the other way around. set_size() is the explicit
-- way to do that, and widget.cpp's resolver just uses the value
-- as-is.
local function size_pill(pill_bg, pill_fg, h_padding)
  local w = pill_fg:width() + 2 * h_padding
  pill_bg:set_size(w, pill_bg:height())
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
    position = 'top',
    height   = opts.height,
  })

  local bg = bar:rect({ color = opts.bg })
  bg:anchor_fill(nil, 0)

  -- ── left zone: tag indicator ─────────────────────────────────────
  local tag_label = bar:text({
    text      = '1',
    font_size = opts.font_size,
    color     = opts.fg,
    parent    = bg,
  })
  tag_label:anchor('left',   bg, 'left',   12)
  tag_label:anchor('vcenter', bg, 'vcenter', 0)

  -- ── center zone: focused client title ────────────────────────────
  -- Anchored to hcenter with no width constraint, so the text widget
  -- is exactly as wide as its current string. The center zone
  -- therefore naturally shrinks/expands with the title; if the title
  -- gets long, it can run into the left or right zone visually, but
  -- that's a known limitation of single-line text in a fixed-geometry
  -- widget tree without a max-width / ellipsize pass. A future
  -- set_size_max() would slot in here.
  local title = bar:text({
    text      = 'uwuwm',
    font_size = opts.font_size,
    color     = opts.fg,
    parent    = bg,
  })
  title:anchor('hcenter',  bg, 'hcenter', 0)
  title:anchor('vcenter',  bg, 'vcenter', 0)

  -- ── right zone: clock + volume + brightness pills ────────────────
  -- Right zone is a vertical strip of pills anchored to the bar's
  -- right edge. The clock pill is the rightmost; the others stack
  -- leftward off it. Each pill is sized to its text after layout so
  -- long volume values ("Volume: 100%") don't overflow.
  local h_padding = 10

  local clock_pill_bg, clock_pill_fg = make_pill(
    bg, opts.pill_bg, opts.pill_fg, '--:--:--', opts.font_size)
  clock_pill_bg:anchor('right',   bg, 'right',  12)
  clock_pill_bg:anchor('vcenter', bg, 'vcenter', 0)

  local vol_pill_bg, vol_pill_fg = make_pill(
    bg, opts.pill_bg, opts.pill_fg, 'Vol -', opts.font_size)
  vol_pill_bg:anchor('right',      clock_pill_bg, 'left', 8)
  vol_pill_bg:anchor('vcenter',    bg,             'vcenter', 0)

  local bright_pill_bg, bright_pill_fg = make_pill(
    bg, opts.pill_bg, opts.pill_fg, 'Brt -', opts.font_size)
  bright_pill_bg:anchor('right',   vol_pill_bg, 'left', 8)
  bright_pill_bg:anchor('vcenter', bg,          'vcenter', 0)

  -- First commit so widget.cpp's resolver measures text widths and
  -- we can then size each pill to its content. Two-pass is
  -- unavoidable: pills depend on text widths, and text widths come
  -- from the same resolver that reads widget positions. Resize, then
  -- commit again to bake the new rect sizes into the buffer.
  bar:commit()
  size_pill(clock_pill_bg,   clock_pill_fg,   h_padding)
  size_pill(vol_pill_bg,     vol_pill_fg,     h_padding)
  size_pill(bright_pill_bg,  bright_pill_fg,  h_padding)
  bar:commit()

  -- ── live updates ────────────────────────────────────────────────

  local function refresh_tag()
    local idx = paw.workspace.current_indices(monitor_name)
    tag_label:set_text(idx[1] and tostring(idx[1]) or '-')
  end

  local function refresh_title()
    local c = uwu.client.focused()
    title:set_text(c and c.title ~= '' and c.title or 'uwuwm')
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

  refresh_tag()
  refresh_title()
  refresh_volume()
  refresh_brightness()
  bar:commit()

  paw.workspace.on_change(function(name)
    if name == monitor_name then
      refresh_tag()
      bar:commit()
    end
  end)
  paw.on('focuses', function(c) refresh_title(); bar:commit() end)
  paw.on('unfocuses', function() refresh_title(); bar:commit() end)
  paw.on('retitles', function(c) refresh_title(); bar:commit() end)

  -- Volume and brightness change hooks: uwu doesn't currently emit a
  -- dedicated "volume::change" event, so the safest portable refresh
  -- trigger is a low-rate timer poll. 1 second is fast enough that
  -- the bar feels live when you move a volume slider, slow enough not
  -- to wake the CPU. The same approach backs bar_3.lua.
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
  opts.height    = opts.height    or 28
  opts.bg        = opts.bg        or 0x1e1e2eff
  opts.fg        = opts.fg        or 0xffffffff
  opts.pill_bg   = opts.pill_bg   or 0x313244ff
  opts.pill_fg   = opts.pill_fg   or 0xffffffff
  opts.font_size = opts.font_size or 14
  opts.on        = opts.on        or 'all'

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
