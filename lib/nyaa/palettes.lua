-- nyaa.palettes -- full semantic-role color tables, one level below
-- nyaa.presets (nyaa.presets is border_color_active/inactive only, the
-- two fields uwuwm itself can draw with; a "palette" here is the wider
-- set a *theme engine* needs -- accents, text/subtext, and a
-- surface/overlay ramp -- so nyaa.export.* and your own rc.lua both have
-- real names ("mauve", "surface0") to reach for instead of hunting hex
-- codes by hand.
--
-- Catppuccin's four flavors use its own canonical 26 role names and
-- values (upstream: github.com/catppuccin/catppuccin) -- these are exact,
-- not approximations. The non-Catppuccin palettes (gruvbox/nord/
-- everforest) don't have an official 26-role spec the way Catppuccin
-- does, so they're a best-effort mapping onto the same role names from
-- each project's own published palette, so every nyaa.palette(name) call
-- returns the same shape regardless of which flavor you picked -- flagged
-- per-table below.
--
-- Role shape (every palette below has exactly these keys):
--   accents:  rosewater, flamingo, pink, mauve, red, maroon, peach,
--             yellow, green, teal, sky, sapphire, blue, lavender
--   text:     text, subtext1, subtext0
--   surfaces: overlay2, overlay1, overlay0, surface2, surface1, surface0,
--             base, mantle, crust
-- ("base" is the nearest analog to a terminal/background color;
-- "crust" is the darkest surface, "text" the primary foreground.)

---@class nyaa.Palette
---A 26-role color table, one per `nyaa.palettes` flavor. Every field is
---a "#rrggbb" string (no alpha); the wider 26-role shape (accents + text
---ramp + surface ramp) is what makes the same table useful both as the
---palette a *theme engine* reads (nyaa.export.*, status bars, ...) and as
---the seed for uwuwm's own two border colors + background_color when
---`nyaa.wear({ flavor = ... })` is called. Field names match Catppuccin's
---own canonical role spec verbatim, even for the non-Catppuccin flavors
---(gruvbox/nord/everforest), which are best-effort mappings onto the
---same 26 names -- see the per-table notes below for which roles were
---hand-assigned vs. pulled from a project's own spec.
---@field rosewater string
---@field flamingo string
---@field pink string
---@field mauve string
---@field red string
---@field maroon string
---@field peach string
---@field yellow string
---@field green string
---@field teal string
---@field sky string
---@field sapphire string
---@field blue string
---@field lavender string
---@field text string
---@field subtext1 string
---@field subtext0 string
---@field overlay2 string
---@field overlay1 string
---@field overlay0 string
---@field surface2 string
---@field surface1 string
---@field surface0 string
---@field base string
---@field mantle string
---@field crust string

---@class nyaa.Palettes
---@field [string] nyaa.Palette  Indexed by flavor name ("catppuccin_mocha", "nord", ...).

local palettes = {}

-- ── Catppuccin (canonical, exact upstream values) ───────────────────────

palettes.catppuccin_mocha = {
  rosewater = '#f5e0dc',
  flamingo = '#f2cdcd',
  pink = '#f5c2e7',
  mauve = '#cba6f7',
  red = '#f38ba8',
  maroon = '#eba0ac',
  peach = '#fab387',
  yellow = '#f9e2af',
  green = '#a6e3a1',
  teal = '#94e2d5',
  sky = '#89dceb',
  sapphire = '#74c7ec',
  blue = '#89b4fa',
  lavender = '#b4befe',
  text = '#cdd6f4',
  subtext1 = '#bac2de',
  subtext0 = '#a6adc8',
  overlay2 = '#9399b2',
  overlay1 = '#7f849c',
  overlay0 = '#6c7086',
  surface2 = '#585b70',
  surface1 = '#45475a',
  surface0 = '#313244',
  base = '#1e1e2e',
  mantle = '#181825',
  crust = '#11111b',
}

palettes.catppuccin_latte = {
  rosewater = '#dc8a78',
  flamingo = '#dd7878',
  pink = '#ea76cb',
  mauve = '#8839ef',
  red = '#d20f39',
  maroon = '#e64553',
  peach = '#fe640b',
  yellow = '#df8e1d',
  green = '#40a02b',
  teal = '#179299',
  sky = '#04a5e5',
  sapphire = '#209fb5',
  blue = '#1e66f5',
  lavender = '#7287fd',
  text = '#4c4f69',
  subtext1 = '#5c5f77',
  subtext0 = '#6c6f85',
  overlay2 = '#7c7f93',
  overlay1 = '#8c8fa1',
  overlay0 = '#9ca0b0',
  surface2 = '#acb0be',
  surface1 = '#bcc0cc',
  surface0 = '#ccd0da',
  base = '#eff1f5',
  mantle = '#e6e9ef',
  crust = '#dce0e8',
}

palettes.catppuccin_frappe = {
  rosewater = '#f2d5cf',
  flamingo = '#eebebe',
  pink = '#f4b8e4',
  mauve = '#ca9ee6',
  red = '#e78284',
  maroon = '#ea999c',
  peach = '#ef9f76',
  yellow = '#e5c890',
  green = '#a6d189',
  teal = '#81c8be',
  sky = '#99d1db',
  sapphire = '#85c1dc',
  blue = '#8caaee',
  lavender = '#babbf1',
  text = '#c6d0f5',
  subtext1 = '#b5bfe2',
  subtext0 = '#a5adce',
  overlay2 = '#949cbb',
  overlay1 = '#838ba7',
  overlay0 = '#737994',
  surface2 = '#626880',
  surface1 = '#51576d',
  surface0 = '#414559',
  base = '#303446',
  mantle = '#292c3c',
  crust = '#232634',
}

palettes.catppuccin_macchiato = {
  rosewater = '#f4dbd6',
  flamingo = '#f0c6c6',
  pink = '#f5bde6',
  mauve = '#c6a0f6',
  red = '#ed8796',
  maroon = '#ee99a0',
  peach = '#f5a97f',
  yellow = '#eed49f',
  green = '#a6da95',
  teal = '#8bd5ca',
  sky = '#91d7e3',
  sapphire = '#7dc4e4',
  blue = '#8aadf4',
  lavender = '#b7bdf8',
  text = '#cad3f5',
  subtext1 = '#b8c0e0',
  subtext0 = '#a5adcb',
  overlay2 = '#939ab7',
  overlay1 = '#8087a2',
  overlay0 = '#6e738d',
  surface2 = '#5b6078',
  surface1 = '#494d64',
  surface0 = '#363a4f',
  base = '#24273a',
  mantle = '#1e2030',
  crust = '#181926',
}

-- ── Best-effort mappings onto the same 26 roles ─────────────────────────
-- None of these three projects ship an official role table shaped like
-- Catppuccin's -- every value below traces back to each project's own
-- published swatches, hand-assigned onto the nearest-fitting role name so
-- nyaa.palette() and nyaa.export.* work identically regardless of flavor.
-- If your eye disagrees with a specific mapping, this table (not
-- lua_config.cpp) is the only thing to edit.

palettes.gruvbox_dark = {
  rosewater = '#d3869b',
  flamingo = '#d3869b',
  pink = '#d3869b',
  mauve = '#d3869b',
  red = '#fb4934',
  maroon = '#cc241d',
  peach = '#fe8019',
  yellow = '#fabd2f',
  green = '#b8bb26',
  teal = '#8ec07c',
  sky = '#83a598',
  sapphire = '#458588',
  blue = '#83a598',
  lavender = '#d3869b',
  text = '#ebdbb2',
  subtext1 = '#d5c4a1',
  subtext0 = '#bdae93',
  overlay2 = '#a89984',
  overlay1 = '#928374',
  overlay0 = '#7c6f64',
  surface2 = '#665c54',
  surface1 = '#504945',
  surface0 = '#3c3836',
  base = '#282828',
  mantle = '#1d2021',
  crust = '#1d2021',
}

palettes.gruvbox_light = {
  rosewater = '#b16286',
  flamingo = '#b16286',
  pink = '#b16286',
  mauve = '#b16286',
  red = '#cc241d',
  maroon = '#9d0006',
  peach = '#d65d0e',
  yellow = '#d79921',
  green = '#98971a',
  teal = '#689d6a',
  sky = '#458588',
  sapphire = '#076678',
  blue = '#458588',
  lavender = '#b16286',
  text = '#3c3836',
  subtext1 = '#504945',
  subtext0 = '#665c54',
  overlay2 = '#7c6f64',
  overlay1 = '#928374',
  overlay0 = '#a89984',
  surface2 = '#bdae93',
  surface1 = '#d5c4a1',
  surface0 = '#ebdbb2',
  base = '#fbf1c7',
  mantle = '#f2e5bc',
  crust = '#f2e5bc',
}

palettes.nord = {
  rosewater = '#d08770',
  flamingo = '#d08770',
  pink = '#b48ead',
  mauve = '#b48ead',
  red = '#bf616a',
  maroon = '#bf616a',
  peach = '#d08770',
  yellow = '#ebcb8b',
  green = '#a3be8c',
  teal = '#8fbcbb',
  sky = '#88c0d0',
  sapphire = '#81a1c1',
  blue = '#5e81ac',
  lavender = '#b48ead',
  text = '#eceff4',
  subtext1 = '#e5e9f0',
  subtext0 = '#d8dee9',
  overlay2 = '#4c566a',
  overlay1 = '#434c5e',
  overlay0 = '#3b4252',
  surface2 = '#3b4252',
  surface1 = '#3b4252',
  surface0 = '#3b4252',
  base = '#2e3440',
  mantle = '#2e3440',
  crust = '#242933',
}

palettes.everforest = {
  rosewater = '#e69875',
  flamingo = '#e69875',
  pink = '#d699b6',
  mauve = '#d699b6',
  red = '#e67e80',
  maroon = '#e67e80',
  peach = '#e69875',
  yellow = '#dbbc7f',
  green = '#a7c080',
  teal = '#83c092',
  sky = '#7fbbb3',
  sapphire = '#7fbbb3',
  blue = '#7fbbb3',
  lavender = '#d699b6',
  text = '#d3c6aa',
  subtext1 = '#dfddca',
  subtext0 = '#e6e2cc',
  overlay2 = '#859289',
  overlay1 = '#9da9a0',
  overlay0 = '#7a8478',
  surface2 = '#543a48',
  surface1 = '#475258',
  surface0 = '#374247',
  base = '#2d353b',
  mantle = '#2b3339',
  crust = '#232a2e',
}

---@type nyaa.Palettes
return palettes
