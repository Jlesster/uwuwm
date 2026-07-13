-- nyaa -- uwuwm's theming module. Appearance only.
--
-- uwu.visual.set(name, value)/uwu.visual.get(name) cover seven settings --
-- how things *look* (gap, border_width, border_color_active,
-- border_color_inactive, background_color, cursor_size, inactive_opacity).
-- Six more exist (master_factor, repeat_rate/repeat_delay, terminal/
-- launcher, dwindle_preserve_split) but those are *behavior*, and
-- uwu.visual.set/get refuse them outright at the C level (see
-- kSettingCategory in src/lua_config.cpp) -- not just "nyaa happens not to
-- call them for those." paw.defaults() (lib/paw/init.lua) is the module
-- that owns them, through the mirror-image uwu.behavior.set/get, which in
-- turn refuse every field in VISUAL_FIELDS below. Two separate namespaces
-- now, not two callers sharing one and promising not to collide.
--
-- nyaa.wear() still keeps its own copy of VISUAL_FIELDS/PAW_OWNED_FIELDS
-- below -- not for enforcement (uwu.visual.set already can't reach a
-- behavior field even if this table were wrong) but so a bad key gets a
-- readable Lua error ("use paw.defaults() instead") rather than a bare
-- C-side log line about an unknown setting.
--
-- nyaa.rule() is the one addition outside that: it's per-*client* border
-- theming (uwu.rule()'s apply.border_color_active/inactive), not a
-- uwu.visual field at all, so it doesn't participate in the
-- VISUAL_FIELDS/PAW_OWNED_FIELDS partition above.
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
nyaa.wallpaper = require('nyaa.wallpaper')

-- The raw table nyaa.palette(name) reads from -- exposed directly too
-- (not just through the function) so `for name in pairs(nyaa.palettes) do`
-- or `nyaa.palettes.nord.mauve` both work, matching every reference to
-- "nyaa.palettes" already made above and in palettes.lua/export.lua's own
-- header comments. nyaa.palette(name) stays the preferred call for a
-- single known-name lookup -- it validates the name and raises a readable
-- error listing every known flavor on a typo, which indexing the table
-- directly won't do -- but enumeration has nowhere else to live.
nyaa.palettes = palettes

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
  catppuccin_frappe = {
    border_color_active = '#ca9ee6', -- mauve
    border_color_inactive = '#414559', -- surface0
  },
  catppuccin_macchiato = {
    border_color_active = '#c6a0f6', -- mauve
    border_color_inactive = '#363a4f', -- surface0
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
-- nyaa.wear({ flavor = "catppuccin_mocha", wallpaper = "~/Pictures/wall.png",
--   wallpaper_opts = { mode = "fill" } }) -- sets background_color as
--   above *and* hands wallpaper/wallpaper_opts to nyaa.wallpaper.set()
--   once every uwu.set() field has landed, so the flavor's background
--   color is already the fallback underneath if swaybg is missing.
--
-- `preset`/`flavor` (mutually exclusive, optional) seed the table from
-- nyaa.presets[name]/nyaa.palette(name); every other field overrides/
-- extends it. Whatever's left is pushed straight through uwu.set()
-- field-by-field, and also handed back so callers can read e.g.
-- `nyaa.wear({...}).gap` without a second lookup. `wallpaper`/
-- `wallpaper_opts` are pulled out before that field-by-field pass (see
-- below) since neither is a real uwu.set() field.
function nyaa.wear(theme)
  theme = theme or {}
  if theme.preset and theme.flavor then
    error("nyaa.wear: pass 'preset' or 'flavor', not both")
  end
  -- Pulled out before the VISUAL_FIELDS loop below since neither is a
  -- uwu.set() field at all -- nyaa.wallpaper is a tracked external
  -- client, not compositor state (see lib/nyaa/wallpaper.lua's header
  -- comment). Applied last, after background_color lands, so the
  -- fallback color is already in place underneath if swaybg is missing
  -- or the image path is bad.
  local wallpaper_path = theme.wallpaper
  local wallpaper_opts = theme.wallpaper_opts
  theme.wallpaper = nil
  theme.wallpaper_opts = nil

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
      uwu.visual.set(name, merged[name])
    end
  end

  if wallpaper_path then
    merged.wallpaper = nyaa.wallpaper.set(wallpaper_path, wallpaper_opts)
  end

  return merged
end

-- nyaa.worn() -- reads the seven visual settings back out of
-- uwu.visual.get(), as one table. Handy for a status-bar module, or just
-- `print`ing to sanity-check what actually landed after nyaa.wear().
function nyaa.worn()
  local current = {}
  for name in pairs(VISUAL_FIELDS) do
    current[name] = uwu.visual.get(name)
  end
  return current
end

-- nyaa.rule({ when = { app_id = "mpv" }, preset = "nord" })
-- nyaa.rule({ when = { app_id = "~steam_app_.*" }, flavor = "nord" })
-- nyaa.rule({ when = { app_id = "~steam_app_.*" }, border_color_active = "#f38ba8" })
--
-- Per-client appearance -- sugar over uwu.rule()'s
-- apply.border_color_active/inactive/opacity (see l_rule_hook in
-- lua_config.cpp), the same relationship nyaa.wear() has to the global
-- uwu.set("border_color_active", ...) pair. `preset`/`flavor` (mutually
-- exclusive, optional) seed both border colors the same way nyaa.wear()
-- does; explicit border_color_active/inactive/opacity fields override/
-- extend it, same merge order as nyaa.wear(). floating/fullscreen/tag/
-- output stay paw.rule()'s job (or raw uwu.rule()), since those aren't
-- appearance -- background_color is still global-only (no per-client
-- equivalent exists at the uwu level, unlike border color and opacity).
--
-- nyaa.rule({ when = { app_id = "mpv" }, opacity = 0.85 }) -- e.g. a
-- picture-in-picture player that should stay slightly see-through
-- whenever it's not focused.
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
  if spec.opacity then
    merged.opacity = spec.opacity
  end

  return uwu.rule({
    match = spec.when or {},
    apply = merged,
  })
end

return nyaa
