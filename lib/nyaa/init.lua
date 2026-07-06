-- nyaa -- uwuwm's theming module. Appearance only.
--
-- uwu.set(name, value) covers ten settings total (see kSettingSetters in
-- lua_config.cpp), and they split cleanly into two kinds: five are how
-- things *look* (gap, border_width, border_color_active,
-- border_color_inactive, cursor_size), and five are *behavior* --
-- master_factor (a tiling ratio), repeat_rate/repeat_delay (keyboard
-- timing), terminal/launcher (app-launch preferences). nyaa only ever
-- calls uwu.set()/uwu.get() for the first five. The other five are
-- paw.defaults()'s job (see lib/paw/init.lua) -- deliberately, so the two
-- modules partition uwu.set()'s field space instead of both being able to
-- reach the same setting. nyaa.wear() rejects the other five by name
-- (pointing at paw.defaults() instead) rather than quietly forwarding
-- them, so that boundary can't drift back open by accident.
--
-- nyaa.rule() is the one addition outside that: it's per-*client* border
-- theming (uwu.rule()'s apply.border_color_active/inactive), not a
-- global uwu.set() field at all, so it doesn't participate in the
-- VISUAL_FIELDS/PAW_OWNED_FIELDS partition above.

local nyaa = {}

-- Only the color pair a palette actually owns. Add more presets here
-- freely -- each is just border_color_active/inactive, nothing more,
-- since that's all uwuwm has a color setting for today.
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

-- The five visual fields nyaa owns.
local VISUAL_FIELDS = {
  gap = true,
  border_width = true,
  cursor_size = true,
  border_color_active = true,
  border_color_inactive = true,
}

-- The five behavior/app fields nyaa deliberately does NOT own -- named
-- here just so nyaa.wear() can say "use paw.defaults() for that" instead
-- of a bare "unknown setting".
local PAW_OWNED_FIELDS = {
  master_factor = true,
  repeat_rate = true,
  repeat_delay = true,
  terminal = true,
  launcher = true,
  focus_follows_mouse = true,
}

-- nyaa.wear({ preset = "catppuccin_mocha", gap = 8, border_width = 2 })
--
-- `preset` (optional) seeds the table from nyaa.presets[name]; every
-- other field overrides/extends it. Whatever's left is pushed straight
-- through uwu.set() field-by-field, and also handed back so callers can
-- read e.g. `nyaa.wear({...}).gap` without a second lookup.
function nyaa.wear(theme)
  theme = theme or {}
  local merged = {}

  if theme.preset then
    local preset = nyaa.presets[theme.preset]
    if not preset then
      local names = {}
      for name in pairs(nyaa.presets) do
        table.insert(names, name)
      end
      table.sort(names)
      error(
        "nyaa.wear: unknown preset '"
          .. tostring(theme.preset)
          .. "' (known: "
          .. table.concat(names, ', ')
          .. ')'
      )
    end
    for k, v in pairs(preset) do
      merged[k] = v
    end
  end

  for k, v in pairs(theme) do
    if k ~= 'preset' then
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

-- nyaa.worn() -- reads the five visual settings back out of uwu.get(),
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
-- nyaa.rule({ when = { app_id = "~steam_app_.*" }, border_color_active = "#f38ba8" })
--
-- Per-client border theming -- sugar over uwu.rule()'s
-- apply.border_color_active/border_color_inactive (see l_rule_hook in
-- lua_config.cpp), the same relationship nyaa.wear() has to the global
-- uwu.set("border_color_active", ...) pair. `preset` (optional) seeds
-- both colors from nyaa.presets[name]; explicit border_color_active/
-- inactive fields override/extend it, same merge order as nyaa.wear().
-- Only the two color fields are supported here -- floating/fullscreen/
-- tag/output rules are paw.rule()'s job (or raw uwu.rule()), since those
-- aren't appearance.
function nyaa.rule(spec)
  spec = spec or {}
  local merged = {}

  if spec.preset then
    local preset = nyaa.presets[spec.preset]
    if not preset then
      local names = {}
      for name in pairs(nyaa.presets) do
        table.insert(names, name)
      end
      table.sort(names)
      error(
        "nyaa.rule: unknown preset '"
          .. tostring(spec.preset)
          .. "' (known: "
          .. table.concat(names, ', ')
          .. ')'
      )
    end
    merged.border_color_active = preset.border_color_active
    merged.border_color_inactive = preset.border_color_inactive
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
