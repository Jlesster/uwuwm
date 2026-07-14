-- paw -- sugar over the raw `uwu` C API (see MISSING.md / lua_config.cpp
-- for the full primitive-level reference; paw never does anything a
-- couple of lines of raw uwu.* calls couldn't, it just gives the common
-- shapes -- a list of keybinds, a client rule, a short-named hook, the
-- tiling primitives, the behavior/app settings -- names and call shapes
-- of their own instead of copying awful's).
--
-- ── data shapes ─────────────────────────────────────────────────────────

---@class paw.KeySpec
---A single `paw.keys()` entry. Positional: `[1]` is the modifier list,
---`[2]` is the xkbcommon key name, `[3]` is the callback, `[4]` is an
---optional human description (kept on `paw.keymap` for which-key-style
---overlays; uwuwm itself doesn't read it).
---@field [1] string[]
---@field [2] string
---@field [3] fun()
---@field [4] string?

---@class paw.KeymapEntry
---Row stored in `paw.keymap` -- the read-back view of what was last
---registered. Same as `paw.KeySpec` minus the callback.
---@field mods string[]
---@field key string
---@field desc? string

---@class paw.RuleWhen
---Per-client match shape for `paw.rule({ when = ... })`. Only the three
---fields `paw.rule` actually surfaces -- `uwu.rule()`'s own `match` is
---slightly wider. `app_id`/`title` may be exact strings or a `paw.like`
---pattern (uwuwm's own "~" prefix convention); `floating` matches
---against the state uwuwm already auto-detected *before* this rule
---runs, so a catch-all `{ floating = true }` rule parks dialogs/
---file-pickers on the scratch tag without needing every app_id by hand.
---@field app_id? string
---@field title? string
---@field floating? boolean

---@class paw.RuleSet
---Per-client apply shape for `paw.rule({ set = ... })`. Layout/state
---fields only -- not the appearance fields `nyaa.RuleSet` covers. Pass
---straight through to `uwu.rule()`'s `apply` underneath.
---@field floating? boolean
---@field fullscreen? boolean
---@field tag? integer   1-based tag index.
---@field output? string  Output name (see `uwu.monitor.list()`).

---@class paw.SpecialWorkspaceDef
---The table `paw.specialworkspace.set(name, opts)` takes. Any field
---can be omitted: `match`-without-`spawn` is still legal (an unprompted
---newly-mapped matching window is silently claimed-and-hidden rather
---than revealed), and `size` defaults to half the focused output.
---@field spawn? string  Shell command to launch if no instance is running yet.
---@field match? { app_id?: string, title?: string }  How to recognize the resulting window.
---@field size? { width: number, height: number }  Fractional (0-1) of the focused output's dimensions.

---@class paw.layout
---Sugar over `uwu.layout.*` -- the per-output tiling primitives your
---keybinds call. All these used to be flat `uwu.set_layout()`/
---`uwu.inc_master()`/`uwu.dwindle_*()` calls sitting directly in rc.lua
---with no paw layer between them and a keybind; this is the paw layer.
---@field set fun(name: "master"|"masterstack"|"master_stack"|"dwindle")
---@field inc_master fun(delta: number)
---@field dwindle paw.layout.dwindle

---@class paw.layout.dwindle
---The five dwindle-specific actions. No-op on an output currently in
---master-stack mode.
---@field toggle_split fun()
---@field swap_split fun()
---@field rotate_split fun(angle?: integer)
---@field splitratio fun(ratio: number)
---@field move_to_root fun(stable?: boolean)

---@class paw.client
---Sugar over the focused-client actions uwuwm exposes at the `uwu.*`
---level (`uwu.focus_next`, `uwu.kill`, `uwu.toggle_floating`, etc.),
---plus relative-delta `move`/`resize` shaped for Hyprland-style binds.
---uwuwm's own client *lifecycle* (list/focused/kill) stays on
---`uwu.client` -- paw.client is the workflow-shaped layer.
---@field focus_next fun()
---@field focus_prev fun()
---@field toggle_floating fun()
---@field toggle_fullscreen fun()
---@field kill fun()
---@field move fun(dx: integer, dy: integer)  No-op (not an error) with no focused client.
---@field resize fun(dw: integer, dh: integer)  No-op (not an error) with no focused client.

---@class paw.monitor
---Workflow-shaped sugar over `uwu.monitor.*`: `focused` for the
---currently-focused output's name, `each(fn)` for iterating every
---connected output (the two things an rc.lua actually reaches for when
---*reacting* to monitor state, as opposed to *configuring* it).
---@field focused fun(): string?
---@field each fun(fn: fun(m: uwu.MonitorInfo))

---@class paw.workspace
---Friendlier name for what the C side calls a "tag" (one flat, 9-bit
---set of tags shared across every output -- Awesome/dwm-style, not a
---per-monitor workspace list). Every action takes either a 1-based
---integer or a name previously registered via `paw.workspace.names()`.
---@field go fun(n: integer|string)
---@field toggle fun(n: integer|string)
---@field move_focused_to fun(n: integer|string)
---@field close_all fun(n: integer|string)
---@field current fun(output_name?: string): integer?  Raw tagset bitmask.
---@field current_indices fun(output_name?: string): integer[]  Decoded 1-based list.
---@field each fun(fn: fun(i: integer))  Alias of `paw.tags(fn)`.
---@field on_change fun(fn: fun(monitor_name: string, new_tagset: integer)): integer  Hook id.

---@class paw.specialworkspace
---Hyprland-style named scratchpads: a floating window that toggles in
---and out of view on top of whatever tag you're currently on, instead
---of living on a fixed tag. Built on `client.minimized` +
---`client:set_tags_mask(uwu.tag.current())` -- see the long header on
---`lib/paw/init.lua` for the design.
---@field set fun(name: string, opts?: paw.SpecialWorkspaceDef)
---@field toggle fun(name: string)
---@field show fun(name: string)
---@field hide fun(name: string)
---@field move_focused fun(name: string)

---@class paw
---Sugar over the raw `uwu` C API. Every function here is a thin
---pass-through that gives the common shapes (a list of keybinds, a
---client rule, a short-named hook, the tiling primitives, the
---behavior/app settings) names and call shapes of their own instead
---of copying awful's. paw never does anything a couple of lines of raw
---uwu.* calls couldn't -- it's an ergonomics layer, not a different
---compositor.
---@field layout paw.layout
---@field client paw.client
---@field monitor paw.monitor
---@field workspace paw.workspace
---@field specialworkspace paw.specialworkspace
---@field keymap paw.KeymapEntry[]  Read-back of what `paw.keys()` last registered.

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
---@param opts? { master_factor?: number, repeat_rate?: integer, repeat_delay?: integer, terminal?: string, launcher?: string, focus_follows_mouse?: boolean, dwindle_preserve_split?: boolean }
---@return { [string]: any }  The merged table that was pushed into `uwu.behavior.set`.
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
-- wrapping uwu.layout.* (nested uwu.layout.dwindle.* for the
-- dwindle-specific actions). The old flat aliases are gone entirely now
-- (not just deprecated) -- uwu.layout.*/paw.layout.* is the only path in.
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

  -- paw.client.move(dx, dy) / paw.client.resize(dw, dh) -- relative nudges
  -- on the *focused* client, over the raw c:move(x, y)/c:resize(w, h)
  -- (which take absolute coordinates and only work on a floating client --
  -- see their comment in lua_config.cpp). This is the Hyprland-bind-style
  -- shape (dwindlesize/movewindow taking a delta, not a target rect) --
  -- e.g. `uwu.bind({"mod"}, "l", function() paw.client.move(20, 0) end)`.
  -- No-op (not an error) with no focused client, since a keybind calling
  -- this doesn't know in advance whether one exists; a focused-but-tiled
  -- client still errors, same as the raw method, since silently ignoring
  -- that would hide a genuinely wrong keybind (probably meant to float
  -- first) instead of surfacing it.
  move = function(dx, dy)
    local c = uwu.client.focused()
    if not c then
      return
    end
    c:move(c.geo.x + dx, c.geo.y + dy)
  end,
  resize = function(dw, dh)
    local c = uwu.client.focused()
    if not c then
      return
    end
    c:resize(c.geo.width + dw, c.geo.height + dh)
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

---@param specs paw.KeySpec[]
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
---@param fn fun(i: integer)  Called once per tag, 1..`uwu.tag_count`.
function paw.tags(fn)
  for i = 1, uwu.tag_count do
    fn(i)
  end
end

-- ── monitors ─────────────────────────────────────────────────────────────
-- paw.monitor -- workflow-shaped sugar over uwu.monitor.* the same way
-- paw.client wraps uwu.client.*: uwu.monitor.set()/list() stay the
-- primitive-level API (configuring an output, reading every output's
-- state), what's missing at that level is "just tell me which one" and
-- "run this for each" -- the two shapes an rc.lua actually reaches for
-- when it's reacting to monitor state rather than configuring it.
paw.monitor = {
  focused = function()
    return uwu.monitor.focused()
  end,

  -- paw.monitor.each(function(m) ... end) -- m is one uwu.MonitorInfo
  -- table per currently connected output (see uwu.monitor.list()).
  each = function(fn)
    for _, m in ipairs(uwu.monitor.list()) do
      fn(m)
    end
  end,
}

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
---@param spec? { when?: paw.RuleWhen, set?: paw.RuleSet }
---@return integer  Hook id (same as `uwu.hook()`'s return; pass to `uwu.unhook()` to remove).
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
---@param pattern string  A Lua pattern (no leading "~").
---@return string  `"~" .. pattern` -- pass directly to `paw.rule({ when = { app_id = ... } })`.
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
  retitles = 'client::title_changed',
  tags_changed = 'tag::change',
  monitor_connected = 'output::connect',
  monitor_disconnected = 'output::disconnect',
}

---@param event string  A short verb name (see `EVENT_ALIASES`) or a raw `"namespace::event"` string.
---@param fn fun(...)  Callback; receives whatever the underlying event sends (a `uwu.Client` for client events, `(monitor_name, new_tagset)` for `tag::change`, etc).
---@return integer  Hook id.
function paw.on(event, fn)
  return uwu.hook(EVENT_ALIASES[event] or event, fn)
end
---@param id integer  Hook id returned by `paw.on`/`paw.rule`/`uwu.hook`.
paw.off = uwu.unhook

-- ── workspaces ───────────────────────────────────────────────────────────
-- paw.workspace -- a friendlier name for what the C side calls a "tag"
-- (see output.hpp's comment on Output::tagset: one flat, 9-bit set of
-- tags shared across every output -- Awesome/dwm-style, not a separate
-- per-monitor workspace list). paw.tags(fn) above already loops
-- 1..uwu.tag_count for building keybind blocks; paw.workspace wraps the
-- actual uwu.tag.* actions the same way paw.layout wraps uwu.layout.*,
-- plus optional human names for anyone who'd rather bind mod+w than
-- remember "tag 4 is always chat".
paw.workspace = {
  -- paw.workspace.go(n) / .toggle(n) / .move_focused_to(n) -- 1-based,
  -- straight passthroughs to uwu.tag.view/toggle/move_client_here. `n`
  -- may also be a name previously registered via paw.workspace.names().
  go = function(n)
    return uwu.tag.view(paw.workspace.resolve(n))
  end,
  toggle = function(n)
    return uwu.tag.toggle(paw.workspace.resolve(n))
  end,
  move_focused_to = function(n)
    return uwu.tag.move_client_here(paw.workspace.resolve(n))
  end,
  close_all = function(n)
    return uwu.tag.close_all(paw.workspace.resolve(n))
  end,

  -- paw.workspace.current([output_name]) -> raw tagset bitmask (see
  -- uwu.tag.current -- the read-side counterpart to view/toggle, which
  -- only ever wrote a tagset before now). Defaults to the focused
  -- output when `output_name` is omitted.
  current = function(output_name)
    return uwu.tag.current(output_name)
  end,

  -- paw.workspace.current_indices([output_name]) -> array of 1-based
  -- tag numbers currently visible, decoded from current()'s bitmask.
  -- Most setups only ever show one at a time, but uwu.tag.toggle can
  -- put an output on several simultaneously, so this is a list rather
  -- than a single number.
  current_indices = function(output_name)
    local mask = uwu.tag.current(output_name)
    local out = {}
    if not mask then
      return out
    end
    for i = 1, uwu.tag_count do
      if (mask & (1 << (i - 1))) ~= 0 then
        table.insert(out, i)
      end
    end
    return out
  end,

  -- paw.workspace.each(fn) -- alias of paw.tags(fn); kept as its own
  -- name under paw.workspace so everything workspace-shaped lives under
  -- one table instead of half living on bare paw.
  each = paw.tags,

  -- paw.workspace.on_change(fn) -- alias of paw.on("tags_changed", fn):
  -- fn receives (monitor_name, new_tagset) whenever any output's
  -- visible tagset changes, from any source (a keybind, a rule's
  -- apply.tag, monitor unplug migration -- see setTagset's own comment
  -- in output.cpp for why every path funnels through one place).
  on_change = function(fn)
    return paw.on('tags_changed', fn)
  end,
}

-- paw.workspace.names({ [1] = "web", [2] = "code", [3] = "chat" }) --
-- optional; lets go/toggle/move_focused_to/close_all above take a name
-- instead of remembering raw tag numbers. Purely a Lua-side lookup
-- table -- the C side and uwu.tag.* never see anything but the
-- resolved 1-based integer, so this is safe to skip entirely if you're
-- happy with bare numbers (paw.workspace.resolve(n) is a no-op for
-- anything that's already a number).
local workspace_names = {}

---@param map table<string, integer>  Map of human-readable name -> 1-based tag index. Merged into the existing table.
function paw.workspace.names(map)
  for name, n in pairs(map or {}) do
    workspace_names[name] = n
  end
end

---@param n integer|string  A 1-based tag index, or a name previously registered via `paw.workspace.names()`.
---@return integer  Resolved 1-based tag index.
function paw.workspace.resolve(n)
  if type(n) == 'number' then
    return n
  end
  local resolved = workspace_names[n]
  if not resolved then
    error("paw.workspace: no such workspace name '" .. tostring(n) .. "'")
  end
  return resolved
end

-- ── special workspaces (scratchpads) ────────────────────────────────────
-- paw.specialworkspace -- Hyprland-style named scratchpads. There's no
-- spare "special" tag at the C level to dedicate to this (all 9 tag
-- bits are ordinary, user-visible tags -- see the paw.workspace comment
-- above), so a special workspace is built entirely out of primitives
-- that already exist for other reasons:
--
--   * client.minimized (View::setMinimized, view.cpp) -- scene-disables
--     the client and drops it out of tiling/focus order while hidden,
--     without touching its output/tags/geo, so hiding and re-showing a
--     scratchpad is cheap and keeps whatever state it had.
--   * client.floating -- a scratchpad is always a floating overlay, never
--     tiled into the layout.
--   * client:set_tags_mask(mask) + uwu.tag.current() -- every time a
--     scratchpad is *shown*, it's re-tagged onto the exact live tagset of
--     the focused output first. That's what makes it follow you: toggle
--     it on from tag 2, it's on tag 2; switch to tag 5 and toggle it on
--     again, it's now on tag 5 too -- always an overlay on whatever
--     you're actually looking at, never a fixed "home" tag you have to
--     go find it on.
--
-- Usage:
--   paw.specialworkspace.set("term", {
--     spawn = "wezterm start --class scratch_term",
--     match = { app_id = "scratch_term" },
--     size  = { width = 0.6, height = 0.5 }, -- fraction of output size
--   })
--   uwu.bind({"mod"}, "grave", function()
--     paw.specialworkspace.toggle("term")
--   end)
--
-- `spawn`/`match` are optional -- paw.specialworkspace.move_focused(name)
-- claims whatever client is currently focused into a named scratchpad
-- with no pre-registered command at all, for one-off ad hoc use.
paw.specialworkspace = {}

-- name -> { spawn, match, size, clients = {[id]=true}, spawning }
local sw_defs = {}
-- client id -> name, so hooks/cleanup don't have to scan every def
local sw_owner = {}
-- Set to true once the client::manage / client::unmanage hooks below
-- have been registered. Lazy on purpose, and registered from the
-- *runtime* entry points (.toggle, .show, .hide) rather than from
-- .set(), so the hooks sit at the *end* of the registration list --
-- any paw.rule({ when = { floating = true }, ... }) the user has
-- registered in rc.lua fires first, sees the freshly-mapped client
-- as not-yet-floating, and (correctly) doesn't match. Without this
-- ordering, a catch-all "park every floater on the scratch tag" rule
-- re-tags the scratchpad the moment sw_reveal marks it floating, so
-- the scratchpad ends up on tag 9 instead of overlaying the focused
-- tag. .set() is data registration (called at file-load time, often
-- alongside .rule() calls); .toggle/.show/.hide are the runtime calls
-- that fire on keybind presses, which by definition happen after
-- rc.lua has fully loaded -- so the hooks naturally end up last in
-- the registration list regardless of where .set() sits in the file.
local sw_hooks_registered = false
-- Forward-declared here so the runtime entry points defined just below
-- (toggle, show, hide) can call into the function whose body sits
-- further down the file. Without the forward declaration, their
-- bodies resolve sw_register_hooks as a free variable (nil) at call
-- time, since the local doesn't exist in their lexical scope.
local sw_register_hooks

local function sw_def(name)
  sw_defs[name] = sw_defs[name] or { clients = {} }
  return sw_defs[name]
end

-- paw.specialworkspace.set(name, { spawn, match, size }) -- registers or
-- updates a scratchpad definition. Calling it again with a running
-- instance already claimed doesn't disturb that instance; it only
-- changes what a *future* spawn/match/reveal uses.
--
-- Pure data registration -- does NOT install the manage/unmanage hooks
-- (those are installed lazily on the first runtime call to .toggle,
-- .show, or .hide, so they sit at the end of the registration list
-- and run *after* any paw.rule() the user has already registered --
-- see the long comment above sw_hooks_registered for the reasoning).
---@param name string  Scratchpad name -- used as the lookup key for `.toggle`/`.show`/`.hide`.
---@param opts? paw.SpecialWorkspaceDef
function paw.specialworkspace.set(name, opts)
  opts = opts or {}
  local def = sw_def(name)
  def.spawn = opts.spawn
  def.match = opts.match
  def.size = opts.size
end

local function sw_matches(c, match)
  if not match then
    return false
  end
  if match.app_id and c.app_id ~= match.app_id then
    return false
  end
  if match.title and not string.find(c.title, match.title) then
    return false
  end
  return true
end

-- Finds the live Client belonging to `name`, if its window is still
-- open. def.clients may still list ids for windows that have since
-- closed (cleaned up lazily by the closes-hook below, not synchronously
-- here) -- iterating uwu.client.list() rather than trusting the id
-- table directly means a stale id just silently matches nothing instead
-- of needing its own liveness check.
local function sw_find(name)
  local def = sw_defs[name]
  if not def then
    return nil
  end
  for _, c in ipairs(uwu.client.list()) do
    if def.clients[c.id] then
      return c
    end
  end
  return nil
end

local function sw_focused_monitor()
  local out = nil
  paw.monitor.each(function(m)
    if m.focused then
      out = m
    end
  end)
  return out
end

-- Claims `c` into scratchpad `name` and makes it visible: floats it,
-- applies def.size (if any) relative to the focused output, re-tags it
-- onto that output's exact live tagset, un-minimizes, and focuses it.
-- This is the one place that actually shows a scratchpad -- toggle/show
-- below both funnel through here.
local function sw_reveal(c, name)
  local def = sw_def(name)
  sw_owner[c.id] = name
  def.clients[c.id] = true

  if not c.floating then
    c.floating = true
  end

  if def.size then
    local out = sw_focused_monitor()
    if out then
      local w = math.floor(out.width * (def.size.width or 0.5))
      local h = math.floor(out.height * (def.size.height or 0.5))
      c:resize(w, h)
      c:move(
        out.x + math.floor((out.width - w) / 2),
        out.y + math.floor((out.height - h) / 2)
      )
    end
  end

  local mask = uwu.tag.current()
  if mask then
    c:set_tags_mask(mask)
  end

  c.minimized = false
  c:focus()
end

-- Catches every newly-mapped client, not just scratchpad ones -- most
-- fall through both checks below and cost one pairs() scan over
-- sw_defs, which stays tiny (a handful of named scratchpads at most).
-- Two cases:
--   1. def.spawning is true -- paw.specialworkspace.toggle() just ran
--      uwu.spawn(def.spawn) and is waiting for the resulting window to
--      map. Claim it and reveal it immediately.
--   2. Not spawning, but it matches def.match anyway (e.g. the app was
--      already running and just opened a second window, or it was
--      launched some other way entirely). Claim it but leave it hidden
--      -- an unprompted new scratchpad window popping up unminimized
--      would be surprising; the next explicit toggle()/show() reveals
--      it same as any other claimed instance.
--
-- Registered lazily from paw.specialworkspace.set() rather than at
-- module top so the hook sits at the *end* of the registration list
-- and runs after any paw.rule({ when = { floating = true }, ... }) the
-- user has registered -- see that function's comment for the reasoning.
sw_register_hooks = function()
  sw_hooks_registered = true

  uwu.hook('client::manage', function(c)
    for name, def in pairs(sw_defs) do
      if not sw_owner[c.id] and def.match and sw_matches(c, def.match) then
        sw_owner[c.id] = name
        def.clients[c.id] = true
        if def.spawning then
          def.spawning = false
          sw_reveal(c, name)
        else
          c.floating = true
          c.minimized = true
        end
        break
      end
    end
  end)

  -- Lazily forgets a closed client's id instead of leaving it in
  -- def.clients forever -- harmless correctness-wise (sw_find only ever
  -- matches against currently-open clients), but this keeps the table
  -- from growing without bound across a long uptime with lots of
  -- ephemeral scratchpad instances (e.g. a scratchpad terminal that gets
  -- closed and respawned many times in a session).
  uwu.hook('client::unmanage', function(c)
    local name = sw_owner[c.id]
    if name and sw_defs[name] then
      sw_defs[name].clients[c.id] = nil
    end
    sw_owner[c.id] = nil
  end)
end

-- paw.specialworkspace.toggle(name) -- the main entry point. No running
-- instance and no def.spawn -- errors if `name` was never registered
-- via .set() (a typo'd name should fail loudly, same reasoning as
-- l_client_newindex's rejection of unknown client properties); a
-- registered-but-spawnless def with no running instance is a silent
-- no-op, since there's nothing to reveal and nothing to launch.
---@param name string
function paw.specialworkspace.toggle(name)
  if not sw_hooks_registered then sw_register_hooks() end
  local def = sw_defs[name]
  if not def then
    error(
      "paw.specialworkspace: no definition for '"
        .. tostring(name)
        .. "' -- call paw.specialworkspace.set() first"
    )
  end

  local c = sw_find(name)
  if c then
    if c.minimized then
      sw_reveal(c, name)
    else
      c.minimized = true
    end
    return
  end

  if def.spawn then
    def.spawning = true
    uwu.spawn(def.spawn)
  end
end

-- paw.specialworkspace.show(name) / .hide(name) -- non-toggling
-- versions, for binds that want "always bring to front" / "always
-- dismiss" rather than a flip-flop (e.g. a hook that shows a scratchpad
-- on some external event, where re-triggering it mid-session shouldn't
-- hide an already-visible one).
---@param name string
function paw.specialworkspace.show(name)
  if not sw_hooks_registered then sw_register_hooks() end
  local def = sw_defs[name]
  if not def then
    error(
      "paw.specialworkspace: no definition for '"
        .. tostring(name)
        .. "' -- call paw.specialworkspace.set() first"
    )
  end
  local c = sw_find(name)
  if c then
    sw_reveal(c, name)
  elseif def.spawn then
    def.spawning = true
    uwu.spawn(def.spawn)
  end
end

---@param name string
function paw.specialworkspace.hide(name)
  if not sw_hooks_registered then sw_register_hooks() end
  local c = sw_find(name)
  if c then
    c.minimized = true
  end
end

-- paw.specialworkspace.move_focused(name) -- ad hoc scratchpad use with
-- no paw.specialworkspace.set() definition at all: grabs whatever
-- client is currently focused and claims/reveals it under `name` on the
-- spot. A later paw.specialworkspace.toggle(name)/hide(name) then works
-- on it same as a spawn-configured one, just without a `spawn` to fall
-- back on if it gets closed.
---@param name string
function paw.specialworkspace.move_focused(name)
  local c = uwu.client.focused()
  if not c then
    return
  end
  sw_reveal(c, name)
end

return paw
