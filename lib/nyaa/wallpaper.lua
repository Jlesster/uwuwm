-- nyaa.wallpaper -- image-backed wallpapers, layered on top of nyaa's
-- solid background_color.
--
-- uwuwm draws no wallpaper of its own: Output::background_rect (see
-- src/output.cpp) is a plain wlr_scene_rect painting background_color,
-- nothing else -- there's no texture/image path in the compositor at
-- all. That's deliberate, same reason there's no wibox-equivalent (see
-- the README's "What's not, and why"): on Wayland, the wallpaper is
-- conventionally a separate layer-shell client (swaybg, in the sway/
-- wlroots ecosystem) sitting on the exact same background layer uwuwm's
-- own background_rect already occupies, not something the compositor
-- renders itself -- awesome's beautiful.wallpaper doesn't have this
-- indirection because X11 has a single shared root-window pixmap to
-- paint onto, and Wayland deliberately has no such concept.
--
-- nyaa.wallpaper doesn't change that -- it just launches/tracks that
-- external client so rc.lua gets a beautiful.wallpaper-shaped call
-- (nyaa.wallpaper.set(path, opts) / nyaa.wallpaper.clear()) instead of a
-- raw uwu.spawn("swaybg ...") one-liner you'd have to also remember to
-- pkill yourself on every reload.
--
-- Requires `swaybg` on PATH (pacman -S swaybg / apt install swaybg).
-- nyaa.wallpaper.set() logs a warning and falls back to leaving
-- background_color as-is if it isn't found -- never a hard error, same
-- philosophy as uwu.set()/uwu.hook() treating an unknown name as a
-- log-and-continue instead of aborting rc.lua.

---@class nyaa.WallpaperEntry
---One tracked wallpaper, as last applied via `nyaa.wallpaper.set()`. The
---fields here are exactly the inputs that were used to spawn the
---swaybg process -- `nyaa.wallpaper.current()` does NOT verify the
---process is still alive, so reading this back is "what nyaa last told
---it to do" rather than a liveness probe. See the header comment on
---`nyaa.wallpaper` for the why.
---@field path string   Image path (a leading "~" is expanded to $HOME before spawn).
---@field mode "fill"|"fit"|"stretch"|"center"|"tile"
---@field output string  Output name, or "*" for the fallback default.

local wallpaper = {}

-- swaybg's own -m values -- see `man swaybg`. "solid_color" is
-- deliberately not exposed here: that's just background_color, already
-- nyaa.wear()'s job.
local MODES = {
  fill = true,
  fit = true,
  stretch = true,
  center = true,
  tile = true,
}

-- One tracked entry per output selector ("*" = every output, or an
-- exact uwu.monitor.list() name). Keyed this way (not a single global)
-- so per-output wallpapers don't stomp each other's pkill on the next
-- nyaa.wallpaper.set() call -- see kill_marker() below.
local active = {}

local function has_swaybg()
  -- os.execute's return shape differs across Lua 5.1/5.4 (boolean vs
  -- true/exit-code/reason triple) -- accept either "truthy" or an exit
  -- code of 0, same defensive check style as everywhere else in this
  -- codebase that shells out and can't fully trust the runtime's Lua
  -- version.
  local ok = os.execute('command -v swaybg >/dev/null 2>&1')
  return ok == true or ok == 0
end

-- A marker baked into the spawned command line via an inert env var, so
-- a later nyaa.wallpaper.set()/clear() for the *same* output selector
-- can pkill -f its own prior instance precisely -- without this, two
-- calls for different outputs (or a reload re-running the same
-- nyaa.wallpaper.set() line) would either stack swaybg processes or,
-- with a broader pkill pattern, kill wallpapers on unrelated outputs.
local function kill_marker(output)
  return 'NYAA_WALLPAPER_TAG=' .. output:gsub('%W', '_')
end

-- string.format's %q double-quotes its argument, which suppresses the
-- shell's own tilde-expansion (unlike an *unquoted* ~/... at word
-- start) -- so "~/Pictures/wall.png" would otherwise reach swaybg
-- completely literally and fail to open. Expanded here instead, same
-- os.getenv('HOME') the shipped rc.lua already uses for this.
local function expand_home(path)
  if path:sub(1, 1) == '~' then
    local home = os.getenv('HOME')
    if home then
      return home .. path:sub(2)
    end
  end
  return path
end

-- nyaa.wallpaper.set("~/Pictures/wall.png")
-- nyaa.wallpaper.set("/path/to/wall.png", { mode = "fill", output = "DP-1" })
--
---`mode` (default "fill") is one of MODES above. `output` (default "*")
---is an exact uwu.monitor.list() name, or "*" for every currently
---connected output (swaybg's own default when passed no -o). Killing +
---relaunching is intentional on every call -- reload-safety, same reason
---nyaa.wear() is idempotent -- and only ever targets the previous
---wallpaper client for this *same* output selector (see kill_marker()).
---@param path string
---@param opts? { mode?: "fill"|"fit"|"stretch"|"center"|"tile", output?: string }
---@return nyaa.WallpaperEntry?  The entry that was stored, or nil if swaybg is missing on PATH.
function wallpaper.set(path, opts)
  opts = opts or {}
  if not path or path == '' then
    error('nyaa.wallpaper.set: path is required')
  end
  local mode = opts.mode or 'fill'
  if not MODES[mode] then
    local names = {}
    for name in pairs(MODES) do
      table.insert(names, name)
    end
    table.sort(names)
    error(
      "nyaa.wallpaper.set: unknown mode '"
        .. tostring(mode)
        .. "' (known: "
        .. table.concat(names, ', ')
        .. ')'
    )
  end
  local output = opts.output or '*'

  if not has_swaybg() then
    print(
      'nyaa.wallpaper.set: swaybg not found on PATH -- leaving '
        .. 'background_color as-is (pacman -S swaybg / apt install swaybg)'
    )
    return nil
  end

  local marker = kill_marker(output)
  local output_flag = (output ~= '*') and ('-o ' .. output .. ' ') or ''
  local cmd = string.format(
    'pkill -f %q 2>/dev/null; env %s swaybg %s-i %q -m %s >/dev/null 2>&1',
    marker,
    marker,
    output_flag,
    expand_home(path),
    mode
  )

  uwu.spawn(cmd)

  active[output] = { path = path, mode = mode, output = output }
  return active[output]
end

-- nyaa.wallpaper.clear() -- every output.
-- nyaa.wallpaper.clear("DP-1") -- just that one.
--
-- Kills the tracked swaybg instance for `output` (default "*") and
-- drops it from nyaa.wallpaper.current(), uncovering whatever
-- background_color is set underneath. No-op, not an error, if nothing
-- was ever set for that selector.
---@param output? string  Output name, or "*" for the fallback default. Defaults to "*".
function wallpaper.clear(output)
  output = output or '*'
  if not active[output] then
    return
  end
  uwu.spawn(string.format('pkill -f %q 2>/dev/null', kill_marker(output)))
  active[output] = nil
end

-- nyaa.wallpaper.current() -- every tracked {path, mode, output}, keyed
-- by output selector, as last passed to nyaa.wallpaper.set(). Doesn't
-- verify the swaybg process is still alive (e.g. survived a crash) --
-- purely "what nyaa last told it to do", same "read back what nyaa
-- itself applied" contract as nyaa.worn().
---@return table<string, nyaa.WallpaperEntry>  Snapshot keyed by output selector ("*" or an exact name).
function wallpaper.current()
  local snapshot = {}
  for output, entry in pairs(active) do
    snapshot[output] = entry
  end
  return snapshot
end

return wallpaper
