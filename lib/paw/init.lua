-- paw -- sugar over the raw `uwu` C API (see MISSING.md / lua_config.cpp
-- for the full primitive-level reference; paw never does anything a
-- couple of lines of raw uwu.* calls couldn't, it just gives the common
-- shapes -- a list of keybinds, a client rule, a short-named hook, the
-- five behavior/app settings -- names and call shapes of their own
-- instead of copying awful's).

local paw = {}

-- ── behavior / app defaults ──────────────────────────────────────────────
-- uwu.set(name, value) covers thirteen settings total (see
-- kSettingSetters in lua_config.cpp): seven are visual (nyaa.wear()'s job
-- -- gap, border_width, border_color_active, border_color_inactive,
-- background_color, cursor_size, inactive_opacity) and six are
-- behavior/app preferences, which paw.defaults() owns instead:
-- master_factor (initial tiling ratio -- paw.layout.inc_master only ever
-- nudges it by a delta, this sets the absolute starting value),
-- repeat_rate/repeat_delay (keyboard timing), terminal/launcher (the two
-- app strings your own keybinds will want to reference), and
-- dwindle_preserve_split (see dwindle.hpp -- whether a split's
-- orientation survives a resize instead of being recomputed from the
-- box's aspect ratio every arrange). Same partition as nyaa.wear(),
-- mirrored: paw.defaults() rejects nyaa's seven fields by name instead of
-- quietly forwarding them, so the boundary between the two can't drift
-- back open by accident.
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
-- Pushes each field straight through uwu.set(), and hands the table back
-- so callers can do e.g. `local terminal = paw.defaults({...}).terminal`
-- without a second lookup.
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
    uwu.set(k, v)
  end

  return opts
end

-- ── keybinds ─────────────────────────────────────────────────────────────
-- paw.keys({
--   { {"mod"}, "Return", function() paw.spawn(terminal) end, "open a terminal" },
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
