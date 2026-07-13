-- paw -- sugar over the raw `uwu` C API (see MISSING.md / lua_config.cpp
-- for the full primitive-level reference; paw never does anything a
-- couple of lines of raw uwu.* calls couldn't, it just gives the common
-- shapes -- a list of keybinds, a client rule, a short-named hook, the
-- tiling primitives, the behavior/app settings -- names and call shapes
-- of their own instead of copying awful's).

local paw = {}

-- ── behavior / app defaults ──────────────────────────────────────────────
-- uwu.behavior.set(name, value)/uwu.behavior.get(name) cover seven
-- settings -- app/behavior preferences, not appearance: master_factor
-- (initial tiling ratio -- paw.layout.inc_master only ever nudges it by a
-- delta, this sets the absolute starting value), repeat_rate/repeat_delay
-- (keyboard timing), terminal/launcher (the two app strings your own
-- keybinds will want to reference), focus_follows_mouse, and
-- dwindle_preserve_split (see dwindle.hpp -- whether a split's
-- orientation survives a resize instead of being recomputed from the
-- box's aspect ratio every arrange). nyaa's seven visual fields
-- (gap, border_width, ...) live on the mirror-image uwu.visual.set/get
-- instead -- see nyaa's header comment. uwu.behavior.set already can't
-- reach any of nyaa's fields at the C level (see kSettingCategory in
-- src/lua_config.cpp), so BEHAVIOR_FIELDS/NYAA_OWNED_FIELDS below exist
-- only to turn a misrouted call into a readable Lua error instead of a
-- bare C-side log line.
local BEHAVIOR_FIELDS = {
  master_factor = true,
  repeat_rate = true,
  repeat_delay = true,
  terminal = true,
  launcher = true,
  focus_follows_mouse = true,
  dwindle_preserve_split = true,
}

local NYAA_OWNED_FIELDS = {
  gap = true,
  border_width = true,
  cursor_size = true,
  border_color_active = true,
  border_color_inactive = true,
  background_color = true,
  inactive_opacity = true,
}

-- paw.defaults({ terminal = "wezterm", launcher = "fuzzel", master_factor = 0.55 })
--
-- Pushes each field straight through uwu.behavior.set(), and hands the
-- table back so callers can do e.g.
-- `local terminal = paw.defaults({...}).terminal` without a second lookup.
function paw.defaults(opts)
  opts = opts or {}

  for k, v in pairs(opts) do
    if NYAA_OWNED_FIELDS[k] then
      error(
        "paw.defaults: '"
          .. k
          .. "' is a visual setting -- use nyaa.wear() instead"
      )
    end
    if not BEHAVIOR_FIELDS[k] then
      error("paw.defaults: '" .. tostring(k) .. "' isn't a real uwuwm setting")
    end
    uwu.behavior.set(k, v)
  end

  return opts
end

-- ── layout / tiling ───────────────────────────────────────────────────────
-- paw.layout -- AwesomeWM's `awful.layout` module is the parity target
-- here: this is the layer that actually calls uwu.layout.* (see
-- src/lua_config.cpp), the same relationship awful.layout has to the
-- core client/tag objects it arranges. Every function here used to be
-- called as a bare uwu.set_layout()/uwu.inc_master()/uwu.dwindle_*() --
-- raw primitive calls sitting directly in rc.lua with no paw layer
-- between them and a keybind, despite this module's own comments always
-- having assumed paw.layout existed. It didn't; this is that module,
-- finally, wrapping uwu.layout.* (nested uwu.layout.dwindle.* for the
-- dwindle-specific actions) instead of the deprecated flat aliases.
paw.layout = {
  -- paw.layout.set("dwindle" | "master") -- per current output, same as
  -- uwu.layout.set's own semantics (see its comment in lua_config.cpp
  -- for why it's per-output rather than global).
  set = function(name)
    return uwu.layout.set(name)
  end,

  -- paw.layout.inc_master(delta) -- nudges master_factor by `delta`;
  -- paw.defaults({ master_factor = ... }) sets the absolute starting
  -- value this nudges away from.
  inc_master = function(delta)
    return uwu.layout.inc_master(delta)
  end,

  dwindle = {
    toggle_split = function()
      return uwu.layout.dwindle.toggle_split()
    end,
    swap_split = function()
      return uwu.layout.dwindle.swap_split()
    end,
    rotate_split = function(degrees)
      return uwu.layout.dwindle.rotate_split(degrees)
    end,
    splitratio = function(ratio)
      return uwu.layout.dwindle.splitratio(ratio)
    end,
    -- move_to_root(stable) -- `stable` defaults to true, matching
    -- uwu.layout.dwindle.move_to_root's own default.
    move_to_root = function(stable)
      return uwu.layout.dwindle.move_to_root(stable)
    end,
  },
}

-- ── client actions ───────────────────────────────────────────────────────
-- paw.client -- AwesomeWM parity target: `awful.client` (focus-by-index,
-- floating/fullscreen toggles). uwuwm's client *lifecycle* (list/focused/
-- kill) stays on uwu.client -- those are core-object accessors, the same
-- role client.focus/client.get() play in awesomewm itself. What moves
-- here is client *workflow*: which client focus lands on next depends on
-- tiling order, exactly the kind of layout-aware navigation awful.client
-- wraps rather than the core `client` object exposing directly.
paw.client = {
  focus_next = function()
    return uwu.focus_next()
  end,
  focus_prev = function()
    return uwu.focus_prev()
  end,
  toggle_floating = function()
    return uwu.toggle_floating()
  end,
  toggle_fullscreen = function()
    return uwu.toggle_fullscreen()
  end,
  kill = function()
    return uwu.kill()
  end,
}

-- ── keybinds ─────────────────────────────────────────────────────────────
-- paw.keys({
--   { {"mod"}, "Return", function() uwu.spawn(terminal) end, "open a terminal" },
--   ...
-- })
--
-- One flat call, one flat list -- no gears.table.join()-ing separate
-- awful.key() objects together before handing them to root.keys(), since
-- uwu.bind() already registers a binding the moment it's called; there's
-- nothing a separate "commit the whole list" step would add here. The
-- 4th element (a description) isn't used by uwuwm today, but is kept on
-- paw.keymap so a future which-key-style overlay (or just `for _, k in
-- ipairs(paw.keymap) do print(...) end` from a keybind of your own) has
-- something to read.
--
-- Note this example calls uwu.spawn directly, not a paw-wrapped version --
-- spawning a command isn't a window-management *action* the way
-- paw.client/paw.layout are (there's no shape to give it beyond the one
-- raw call), so it deliberately stays on `uwu` rather than getting a
-- pointless paw.spawn wrapper. See rc.lua's own header comment: "all
-- three styles compose freely in the same rc.lua" -- uwu.spawn sitting
-- next to paw.keys() in the same table is expected, not a layering leak.
paw.keymap = {}

function paw.keys(specs)
  for _, spec in ipairs(specs) do
    local mods, key, fn, desc = spec[1], spec[2], spec[3], spec[4]
    uwu.bind(mods, key, fn)
    table.insert(paw.keymap, { mods = mods, key = key, desc = desc })
  end
end

-- ── tags ───────────────────────────────────────────────────────────────
-- paw.tags(fn) -- calls fn(i) once per tag, 1..uwu.tag_count. Mainly for
-- building the mod+1..9 / mod+shift+1..9 keybind blocks in a for-loop
-- instead of by hand, e.g.:
--
--   local keys = { ... }
--   paw.tags(function(i)
--     table.insert(keys, { mod, tostring(i), function() uwu.tag.view(i) end })
--   end)
--   paw.keys(keys)
function paw.tags(fn)
  for i = 1, uwu.tag_count do
    fn(i)
  end
end

-- ── client rules ─────────────────────────────────────────────────────────
-- paw.rule({
--   when = { app_id = "mpv" },
--   set  = { floating = true, tag = 3 },
-- })
--
-- `when`/`set` instead of AwesomeWM's `rule`/`properties` -- mostly so
-- match-only fields (when.floating: "only clients the compositor already
-- auto-detected as floating") and apply-only fields (set.floating: "make
-- this client floating") can't get confused for the same field just
-- because they share a name, which `rule = properties` sharing one flat
-- field namespace invites. Passes straight through to uwu.rule()'s own
-- match/apply table underneath -- see l_rule_hook in lua_config.cpp for
-- exactly what each field does.
function paw.rule(spec)
  spec = spec or {}
  local when, set = spec.when or {}, spec.set or {}
  return uwu.rule({
    match = {
      app_id = when.app_id,
      title = when.title,
      floating = when.floating,
    },
    apply = {
      floating = set.floating,
      fullscreen = set.fullscreen,
      tag = set.tag,
      output = set.output,
    },
  })
end

-- paw.like("steam_app_.*") -- wraps uwuwm's own "~" Lua-pattern-match
-- prefix convention (see `matches` in l_rule_hook) so a rule reads as
-- "match like this pattern" instead of a bare string with a leading
-- sigil someone has to already know about.
function paw.like(pattern)
  return '~' .. pattern
end

-- ── hooks ──────────────────────────────────────────────────────────────
-- Short, readable verb names for the raw "namespace::event" strings
-- uwu.hook()/uwu.unhook() use -- these are genuinely uwuwm's own event
-- set (see the comment on l_hook in lua_config.cpp), just given names
-- that don't require remembering the "client::" prefix. Any raw event
-- string not in this table (or a future one uwuwm adds before paw
-- catches up) is passed straight through unchanged, so paw.on() never
-- blocks access to a real uwu.hook() event.
local EVENT_ALIASES = {
  opens = 'client::manage',
  closes = 'client::unmanage',
  focuses = 'client::focus',
  unfocuses = 'client::unfocus',
  fullscreens = 'client::fullscreen',
  tags_changed = 'tag::change',
  monitor_connected = 'output::connect',
  monitor_disconnected = 'output::disconnect',
}

function paw.on(event, fn)
  return uwu.hook(EVENT_ALIASES[event] or event, fn)
end
paw.off = uwu.unhook

return paw
