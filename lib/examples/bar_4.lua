-- examples/bar_4.lua -- one fixed-size popup window (mode = "popup")
-- that auto-shows on volume or brightness changes, displays workspace
-- / clock / focused-window-title / active-stat pills, and auto-hides
-- after a short delay. The popup is created once at startup and
-- reused -- uwu.widget's show()/hide() are cheap scene-graph toggles,
-- not create/destroy pairs, so a single popup can fire dozens of
-- times a second when you drag a volume slider without churning the
-- scene graph.
--
-- opts:
--   width, height  fixed popup size, default 360 x 96
--   anchor         popup screen anchor, default "bottom"
--   margin         pixels from the screen edge, default 48
--   ttl            seconds the popup stays visible after the last
--                  change, default 1.5
--   bg             popup background, default 0x11111bff
--   pill_bg        stat pill background, default 0x313244ff
--   pill_fg        stat pill foreground, default 0xffffffff
--   font_size      default 14
--
-- setup() returns a function `show(kind, value)` that rc.lua can call
-- from any keybind -- typically one for "raise volume", one for
-- "lower volume", one for "raise brightness", one for "lower
-- brightness", each ending with show('volume', N) or
-- show('brightness', N) for the matching visual feedback. The
-- example in the rc.lua section of the README shows the pattern.
-- uwu doesn't currently emit a dedicated "volume::change" or
-- "brightness::change" event, so the auto-trigger pattern is to
-- call show() directly from the same keybind that calls
-- uwu.system.volume.set / uwu.system.brightness.set, rather than
-- relying on a hook.
--
-- Example (rc.lua):
--
--   local show_osd = require('examples.bar_4').setup({ width = 360 })
--   uwu.bind({ 'mod' }, 'F12', function()
--     show_osd('volume', uwu.system.volume.get() or 0)
--   end)

local M = {}

-- One pill: a small rect with a text child centered inside. Returns
-- the rect and text handles so the caller can :set_text() to update
-- and :set_size() to grow/shrink the rect to match the text width
-- (widget.cpp measures the text width automatically on every commit,
-- so resizing after a text change is the natural pairing).
local function make_pill(parent, bg, fg, text, font_size)
  local pill_bg = parent:rect({ color = bg })
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

local function size_pill(pill_bg, pill_fg, h_padding)
  pill_bg:set_size(pill_fg:width() + 2 * h_padding, pill_bg:height())
end

-- Two-row layout, each row a horizontal strip of pills:
--
--   [WS 1]                   [--:--:--]
--   [focused client title]            [Vol 75%]
--
-- Row 1 (top):    ws pill left-anchored,   clock pill right-anchored.
-- Row 2 (bottom): title pill takes the rest, stat pill right-anchored.
--
-- Rows are implemented as transparent rects that the pills anchor
-- inside. Without them, the top row's pills (anchored to bg) would
-- use bg's vcenter, which lands them in the visual middle, not the
-- top. The transparent-row approach is the same trick used for
-- nested layouts in any retained-mode UI toolkit: a parent rect with
-- no visual but a defined box, and children anchored to its edges.
local function build(opts)
  local osd = uwu.widget.window({
    mode    = 'popup',
    anchor  = opts.anchor,
    margin  = opts.margin,
    width   = opts.width,
    height  = opts.height,
  })

  local bg = osd:rect({ color = opts.bg })
  bg:anchor_fill(nil, 0)

  local h_padding = 10
  local pill_gap  = 8
  local row_gap   = 8

  -- Two rows: each is half the popup's height minus a small vertical
  -- margin. The rows themselves are non-visual (transparent) -- they
  -- exist only to give the pills a non-overlapping box to anchor to.
  local row_h = math.floor((opts.height - 2 * row_gap) / 2)

  local top_row = osd:rect({
    x = 0, y = 0,
    w = opts.width, h = row_h,
    color = 0x00000000,
    parent = bg,
  })
  top_row:anchor('top',     bg, 'top',    row_gap)
  top_row:anchor('hcenter', bg, 'hcenter', 0)

  local bot_row = osd:rect({
    x = 0, y = 0,
    w = opts.width, h = row_h,
    color = 0x00000000,
    parent = bg,
  })
  bot_row:anchor('bottom',  bg, 'bottom', row_gap)
  bot_row:anchor('hcenter', bg, 'hcenter', 0)

  -- Top row pills.
  local ws_pill_bg, ws_pill_fg = make_pill(
    top_row, opts.pill_bg, opts.pill_fg, 'WS 1', opts.font_size)
  ws_pill_bg:anchor('left',    top_row, 'left',  8)
  ws_pill_bg:anchor('vcenter', top_row, 'vcenter', 0)

  local clock_pill_bg, clock_pill_fg = make_pill(
    top_row, opts.pill_bg, opts.pill_fg, '--:--:--', opts.font_size)
  clock_pill_bg:anchor('right',   top_row, 'right',  8)
  clock_pill_bg:anchor('vcenter', top_row, 'vcenter', 0)

  -- Bottom row pills. The title pill takes whatever's left after the
  -- stat pill. We anchor the title to the row's left edge and the
  -- stat to the row's right edge, then give the title a fixed width
  -- (popup width minus the stat pill's measured width minus a gap)
  -- so they don't overlap.
  local title_pill_bg, title_pill_fg = make_pill(
    bot_row, opts.pill_bg, opts.pill_fg, 'uwuwm', opts.font_size)
  title_pill_bg:anchor('left',    bot_row, 'left',  8)
  title_pill_bg:anchor('vcenter', bot_row, 'vcenter', 0)

  local stat_pill_bg, stat_pill_fg = make_pill(
    bot_row, opts.pill_bg, opts.pill_fg, '', opts.font_size)
  stat_pill_bg:anchor('right',   bot_row, 'right', 8)
  stat_pill_bg:anchor('vcenter', bot_row, 'vcenter', 0)

  -- First commit: resolver measures all text widths. Then size each
  -- pill to its content and size the title pill to fill the
  -- remaining row width minus the stat pill.
  osd:commit()
  size_pill(ws_pill_bg,      ws_pill_fg,      h_padding)
  size_pill(clock_pill_bg,   clock_pill_fg,   h_padding)
  size_pill(stat_pill_bg,    stat_pill_fg,    h_padding)
  size_pill(title_pill_bg,   title_pill_fg,   h_padding)
  osd:commit()
  -- After this second commit, stat_pill_bg:width() is the actual
  -- measured width. Resize the title pill to be exactly
  -- (row_w - stat_w - gap), so the two pills sit at the row's
  -- edges with a clean gap between them. anchor the title pill to
  -- the row's left edge so its position is independent of the stat
  -- pill's width.
  local title_w = bot_row:width() - 16 - stat_pill_bg:width() - pill_gap
  if title_w < 32 then title_w = 32 end  -- floor so the pill stays visible
  title_pill_bg:set_size(title_w, title_pill_bg:height())
  osd:commit()

  -- ── live updates ────────────────────────────────────────────────

  local function refresh_workspace()
    local idx = paw.workspace.current_indices()
    ws_pill_fg:set_text(idx[1] and ('WS %d'):format(idx[1]) or 'WS -')
    size_pill(ws_pill_bg, ws_pill_fg, h_padding)
  end

  local function refresh_clock()
    clock_pill_fg:set_text(os.date('%H:%M:%S'))
    size_pill(clock_pill_bg, clock_pill_fg, h_padding)
  end

  local function refresh_title()
    local c = uwu.client.focused()
    title_pill_fg:set_text(
      c and c.title ~= '' and c.title or 'uwuwm')
    size_pill(title_pill_bg, title_pill_fg, h_padding)
  end

  -- Highlights whichever stat pill is currently active. Calling
  -- show('volume', 75) flips the stat pill to "Vol 75%"; calling
  -- show('brightness', 50) flips it to "Brt 50%". The pill keeps
  -- that content visible until the next show() call or until the OSD
  -- hides.
  local function refresh_stat(kind, value)
    if kind == 'volume' then
      stat_pill_fg:set_text(('Vol %d%%'):format(value or 0))
    elseif kind == 'brightness' then
      stat_pill_fg:set_text(('Brt %d%%'):format(value or 0))
    end
    size_pill(stat_pill_bg, stat_pill_fg, h_padding)
    -- The title pill's width was sized to (row_w - 16 - stat_w -
    -- gap) at startup. If the stat text grows wider on a later
    -- show() call (e.g. "Vol 100%" vs "Vol 5%"), the title would
    -- overlap the stat. Re-size the title pill every refresh so
    -- the layout always fits. Two of the three possible stat
    -- strings ("Vol N%" and "Brt N%") are the same length, so this
    -- is only a real problem for very-short values like 0-9%.
    local title_w = bot_row:width() - 16 - stat_pill_bg:width() - pill_gap
    if title_w < 32 then title_w = 32 end
    title_pill_bg:set_size(title_w, title_pill_bg:height())
  end

  refresh_workspace()
  refresh_title()
  refresh_clock()
  osd:commit()

  paw.workspace.on_change(function() refresh_workspace(); osd:commit() end)
  paw.on('focuses',  function() refresh_title(); osd:commit() end)
  paw.on('unfocuses', function() refresh_title(); osd:commit() end)
  paw.on('retitles', function() refresh_title(); osd:commit() end)

  local clock_timer = uwu.timer.set_interval(1, function()
    refresh_clock()
    osd:commit()
  end)

  -- ── show/hide state machine ────────────────────────────────────
  -- The popup starts hidden. show(kind, value) reveals it, refreshes
  -- the stat pill, and (re)starts the auto-hide timer. Each new
  -- show() call resets the timer, so a quick burst of volume
  -- changes just extends the visibility window rather than letting
  -- the popup flicker on/off.
  local hide_timer = nil
  local function show(kind, value)
    refresh_stat(kind, value)
    osd:commit()
    osd:show()
    if hide_timer then
      uwu.timer.cancel(hide_timer)
    end
    hide_timer = uwu.timer.set_timeout(opts.ttl, function()
      osd:hide()
      hide_timer = nil
    end)
  end

  return function()
    if hide_timer then
      uwu.timer.cancel(hide_timer)
      hide_timer = nil
    end
    uwu.timer.cancel(clock_timer)
    osd:destroy()
  end, show
end

function M.setup(opts)
  opts = opts or {}
  opts.width     = opts.width     or 360
  opts.height    = opts.height    or 96
  opts.anchor    = opts.anchor    or 'bottom'
  opts.margin    = opts.margin    or 48
  opts.ttl       = opts.ttl       or 1.5
  opts.bg        = opts.bg        or 0x11111bff
  opts.pill_bg   = opts.pill_bg   or 0x313244ff
  opts.pill_fg   = opts.pill_fg   or 0xffffffff
  opts.font_size = opts.font_size or 14

  local teardown, show = build(opts)
  return show, function() teardown() end
end

return M
