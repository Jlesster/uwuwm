-- nyaa -- uwuwm's theming module. Appearance only.
--
-- uwu.set(name, value) covers thirteen settings total (see
-- kSettingSetters in lua_config.cpp), and they split cleanly into two
-- kinds: seven are how things *look* (gap, border_width,
-- border_color_active, border_color_inactive, background_color,
-- cursor_size, inactive_opacity), and six are *behavior*
-- (master_factor -- a tiling ratio, repeat_rate/repeat_delay -- keyboard
-- timing, terminal/launcher -- app-launch preferences,
-- dwindle_preserve_split). nyaa only ever calls uwu.set()/uwu.get() for
-- the first seven. The other six are paw.defaults()'s job (see
-- lib/paw/init.lua) -- deliberately, so the two modules partition
-- uwu.set()'s field space instead of both being able to reach the same
-- setting. nyaa.wear() rejects the other six by name (pointing at
-- paw.defaults() instead) rather than quietly forwarding them, so that
-- boundary can't drift back open by accident.
--
-- nyaa.rule() is the one addition outside that: it's per-*client* border
-- theming (uwu.rule()'s apply.border_color_active/inactive), not a
-- global uwu.set() field at all, so it doesn't participate in the
-- VISUAL_FIELDS/PAW_OWNED_FIELDS partition above.
--
-- Two layers, not one:
--   nyaa.presets   -- the original, small shape: just the two border
--                     colors uwuwm itself can draw with. Still what
--                     `preset = "..."` on nyaa.wear()/nyaa.rule() reads
--                     from -- unchanged, so existing rc.lua files keep
--                     working exactly as before.
--   nyaa.palettes  -- (lib/nyaa/palettes.lua) the wider, awesome-
--                     `beautiful`-shaped layer: a full semantic role
--                     table (26 named colors -- accents, text/subtext,
--                     a surface/overlay ramp) per flavor, for anything
--                     beyond the two things uwuwm's own border draws
--                     with -- nyaa.export.* (lib/nyaa/export.lua) and
--                     your own rc.lua both read these by role name.
--                     `flavor = "..."` on nyaa.wear() reads from here
--                     instead, deriving border colors + background_color
--                     from the same table nyaa.palette() and
--                     nyaa.export.render() use, so the compositor's own
--                     colors and everything nyaa.export.* generates for
--                     the rest of your desktop can never drift apart.

local palettes = require('nyaa.palettes')

local nyaa = {}

nyaa.export = require('nyaa.export')

-- Only the color pair a palette actually owns. Add more presets here
-- freely -- each is just border_color_active/inactive, nothing more,
-- since that's all uwuwm has a color setting for today. Kept exactly as
-- it was before nyaa.palettes existed -- `preset = "..."` still means
-- "just these two colors."
nyaa.presets = {
  catppuccin_mocha = {
    border_color_active = '#cba6f7', -- mauve
    border_color_inactive = '#313244', -- surface0
  },
  catppuccin_latte = {
    border_color_active = '#8839ef', -- mauve
    border_color_inactive = '#ccd0da', -- surface0
  },
  gruvbox_dark = {
    border_color_active = '#fabd2f',
    border_color_inactive = '#3c3836',
  },
  gruvbox_light = {
    border_color_active = '#b57614',
    border_color_inactive = '#d5c4a1',
  },
  nord = {
    border_color_active = '#88c0d0',
    border_color_inactive = '#3b4252',
  },
  everforest = {
    border_color_active = '#a7c080',
    border_color_inactive = '#374247',
  },
}

-- Which palette role nyaa.wear({flavor = ...})/nyaa.rule({flavor = ...})
-- draws border_color_active from -- matches each project's own accent
-- choice above so switching a preset call to the equivalent flavor call
-- reproduces the same two border colors, plus everything else the fuller
-- palette adds (background_color, and every role nyaa.palette()/
-- nyaa.export.* can see). border_color_inactive is always the flavor's
-- own `surface0` -- true for every palette in palettes.lua.
local FLAVOR_ACCENT = {
  catppuccin_mocha = 'mauve',
  catppuccin_latte = 'mauve',
  catppuccin_frappe = 'mauve',
  catppuccin_macchiato = 'mauve',
  gruvbox_dark = 'yellow',
  gruvbox_light = 'yellow',
  nord = 'sky',
  everforest = 'green',
}

-- The seven visual fields nyaa owns.
local VISUAL_FIELDS = {
  gap = true,
  border_width = true,
  cursor_size = true,
  border_color_active = true,
  border_color_inactive = true,
  background_color = true,
  inactive_opacity = true,
}

-- The six behavior/app fields nyaa deliberately does NOT own -- named
-- here just so nyaa.wear() can say "use paw.defaults() for that" instead
-- of a bare "unknown setting".
local PAW_OWNED_FIELDS = {
  master_factor = true,
  repeat_rate = true,
  repeat_delay = true,
  terminal = true,
  launcher = true,
  focus_follows_mouse = true,
  dwindle_preserve_split = true,
}

local function unknown_preset_error(name, prefix)
  local names = {}
  for preset_name in pairs(nyaa.presets) do
    table.insert(names, preset_name)
  end
  table.sort(names)
  error(
    prefix
      .. ": unknown preset '"
      .. tostring(name)
      .. "' (known: "
      .. table.concat(names, ', ')
      .. ')'
  )
end

local function unknown_flavor_error(name, prefix)
  local names = {}
  for flavor_name in pairs(palettes) do
    table.insert(names, flavor_name)
  end
  table.sort(names)
  error(
    prefix
      .. ": unknown flavor '"
      .. tostring(name)
      .. "' (known: "
      .. table.concat(names, ', ')
      .. ')'
  )
end

-- nyaa.palette("catppuccin_mocha") -- the full 26-role table
-- (lib/nyaa/palettes.lua) for a flavor, for anything that wants more than
-- the two colors a preset gives you: nyaa.export.render(), a status-bar
-- module, or just picking a role by name in your own rc.lua
-- (nyaa.palette("catppuccin_mocha").teal, say).
function nyaa.palette(name)
  local p = palettes[name]
  if not p then
    unknown_flavor_error(name, 'nyaa.palette')
  end
  return p
end

-- nyaa.wear({ preset = "catppuccin_mocha", gap = 8, border_width = 2 })
-- nyaa.wear({ flavor = "catppuccin_mocha", gap = 8 }) -- also sets
--   background_color from the flavor's `base`, not just the two border
--   colors a preset gives you.
--
-- `preset`/`flavor` (mutually exclusive, optional) seed the table from
-- nyaa.presets[name]/nyaa.palette(name); every other field overrides/
-- extends it. Whatever's left is pushed straight through uwu.set()
-- field-by-field, and also handed back so callers can read e.g.
-- `nyaa.wear({...}).gap` without a second lookup.
function nyaa.wear(theme)
  theme = theme or {}
  if theme.preset and theme.flavor then
    error("nyaa.wear: pass 'preset' or 'flavor', not both")
  end
  local merged = {}

  if theme.preset then
    local preset = nyaa.presets[theme.preset]
    if not preset then
      unknown_preset_error(theme.preset, 'nyaa.wear')
    end
    for k, v in pairs(preset) do
      merged[k] = v
    end
  elseif theme.flavor then
    local palette = palettes[theme.flavor]
    if not palette then
      unknown_flavor_error(theme.flavor, 'nyaa.wear')
    end
    local accent = FLAVOR_ACCENT[theme.flavor] or 'mauve'
    merged.border_color_active = palette[accent]
    merged.border_color_inactive = palette.surface0
    merged.background_color = palette.base
  end

  for k, v in pairs(theme) do
    if k ~= 'preset' and k ~= 'flavor' then
      if PAW_OWNED_FIELDS[k] then
        error(
          "nyaa.wear: '"
            .. k
            .. "' is a behavior setting -- use paw.defaults() instead"
        )
      end
      if not VISUAL_FIELDS[k] then
        error("nyaa.wear: '" .. tostring(k) .. "' isn't a real uwuwm setting")
      end
      merged[k] = v
    end
  end

  for name in pairs(VISUAL_FIELDS) do
    if merged[name] ~= nil then
      uwu.set(name, merged[name])
    end
  end

  return merged
end

-- nyaa.worn() -- reads the seven visual settings back out of uwu.get(),
-- as one table. Handy for a status-bar module, or just `print`ing to
-- sanity-check what actually landed after nyaa.wear().
function nyaa.worn()
  local current = {}
  for name in pairs(VISUAL_FIELDS) do
    current[name] = uwu.get(name)
  end
  return current
end

-- nyaa.rule({ when = { app_id = "mpv" }, preset = "nord" })
-- nyaa.rule({ when = { app_id = "~steam_app_.*" }, flavor = "nord" })
-- nyaa.rule({ when = { app_id = "~steam_app_.*" }, border_color_active = "#f38ba8" })
--
-- Per-client border theming -- sugar over uwu.rule()'s
-- apply.border_color_active/inactive (see l_rule_hook in lua_config.cpp),
-- the same relationship nyaa.wear() has to the global
-- uwu.set("border_color_active", ...) pair. `preset`/`flavor` (mutually
-- exclusive, optional) seed both colors the same way nyaa.wear() does;
-- explicit border_color_active/inactive fields override/extend it, same
-- merge order as nyaa.wear(). Only the two color fields are supported
-- here -- floating/fullscreen/tag/output rules are paw.rule()'s job (or
-- raw uwu.rule()), since those aren't appearance, and per-client
-- background/opacity aren't uwu.rule()-applicable settings at all today
-- (both are global-only -- see uwu.RuleApply in lib/meta/uwu.lua).
function nyaa.rule(spec)
  spec = spec or {}
  if spec.preset and spec.flavor then
    error("nyaa.rule: pass 'preset' or 'flavor', not both")
  end
  local merged = {}

  if spec.preset then
    local preset = nyaa.presets[spec.preset]
    if not preset then
      unknown_preset_error(spec.preset, 'nyaa.rule')
    end
    merged.border_color_active = preset.border_color_active
    merged.border_color_inactive = preset.border_color_inactive
  elseif spec.flavor then
    local palette = palettes[spec.flavor]
    if not palette then
      unknown_flavor_error(spec.flavor, 'nyaa.rule')
    end
    local accent = FLAVOR_ACCENT[spec.flavor] or 'mauve'
    merged.border_color_active = palette[accent]
    merged.border_color_inactive = palette.surface0
  end

  if spec.border_color_active then
    merged.border_color_active = spec.border_color_active
  end
  if spec.border_color_inactive then
    merged.border_color_inactive = spec.border_color_inactive
  end

  return uwu.rule({
    match = spec.when or {},
    apply = merged,
  })
end

return nyaa
