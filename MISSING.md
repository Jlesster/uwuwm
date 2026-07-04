# Missing for daily use

Checked against what's actually wired into `server.cpp` / the meson source list,
not just what's aspirational in comments. Grouped by how much it'll bite you.

## Will break your workflow immediately

- **No `rc.lua` ships in the repo.** `lua_config.cpp` loads
  `~/.config/uwuwm/rc.lua` but there's no default/example file anywhere in the
  tree (`find . -iname '*.lua'` turns up nothing but the C++ loader). Without
  one you have no keybinds, no monitor rules, no terminal/launcher set — you
  can't actually use the compositor yet, only build it.
- **No clipboard persistence across client death.** `wlr_data_device_manager` is
  created, but there's no `wlr-data-control-v1`, so no external clipboard
  manager (cliphist, etc.) can hook in, and copied text disappears once the
  source app closes/exits, which is the default wlroots behavior without a
  data-control-backed clipboard manager running.

## Will bite you on your gaming stack specifically

- **Pointer constraints are fetched but never used.** The build pulls
  `pointer-constraints-unstable-v1.xml` and the README even calls it out, but
  nothing in `input.cpp` binds `wlr_pointer_constraints_v1` or
  `wlr_relative_pointer_manager_v1`. That protocol pair is what most FPS/3D
  games under Proton need for mouselook (pointer lock + relative motion) — right
  now a Proton game grabbing the pointer likely won't get proper relative-motion
  behavior. This is probably the single most consequential gap for the GTA
  V/Proton use case specifically, more than XWayland itself (which is done).
- **No `wlr-output-power-management-v1` / DPMS control.** No way to blank/wake
  outputs from software (screen-off timeout, `wlropm`-style tools). Not
  gaming-specific but you'll notice it fastest when a game's fullscreen state
  interacts with your monitor's power state.
- **No screencopy / export-dmabuf.** `wlr_screencopy_manager_v1` isn't
  implemented, so no `grim`-style screenshots and no screen-share (Discord, OBS)
  of the compositor. If you stream or clip gameplay, or even just want a
  bug-report screenshot, there's currently no way to capture the screen at all.

## Quality-of-life you'll want within the first week

- **No idle-inhibit.** `wlr_idle_inhibit_manager_v1` isn't wired up, so
  fullscreen video/games can't tell an idle daemon (if you run one) to suppress
  screen-blank/lock. Compounds with the no-DPMS-control gap above.
- **No xdg-decoration negotiation.** Toplevels aren't told the compositor
  prefers server-side decoration, so some clients (mostly GTK apps) may draw
  their own CSD on top of uwuwm's thin border, giving you double chrome on those
  specific apps.
- **No virtual-keyboard/virtual-pointer protocols.** Screen-sharing tools and
  some remote-input/automation utilities rely on these to inject input; without
  them those tools won't work against uwuwm.
- **No config hot-reload.** Every `rc.lua` tweak costs a full compositor
  restart, which under a real DRM/KMS session means your whole graphical session
  bounces, not just a config reload. Fine occasionally, annoying if you're
  actively tuning gaps/colors/keybinds.

## Explicitly out of scope, not gaps

Per the README's own scope cuts, these are deliberate and probably fine to leave
alone: presentation-time consumer code, direct-scanout profiling tooling, and
any IPC beyond the autostart script (no waybar-style status protocol). None of
these block daily use the way the items above do.

## Implemented, but not yet verified against a real build

- **Session lock** (`src/session_lock.{hpp,cpp}`). Written to the same design as
  the rest of the codebase — see that file's header comments for the full
  lifecycle — but generated without a wlroots-0.20 install to compile against,
  same caveat as the rest of the tree. The corners most worth checking first if
  something's off:
  - `wlr_session_lock_v1_destroy` (used to refuse a second concurrent lock
    request) is the symbol I recall from this API, but isn't something I could
    verify against a live 0.20 header.
  - Whether `wlr_scene_subsurface_tree_create`'s auto-cleanup-on-surface-
    destroy actually fires before or after `wlr_session_lock_surface_v1`'s own
    `destroy` event — the implementation assumes "before or simultaneous,"
    matching how `layershell.cpp` treats `wlr_scene_layer_surface_v1_create`.
  - What happens, exactly, to a `wlr_session_lock_surface_v1` when its output is
    unplugged mid-lock — flagged inline in `output.cpp`'s destructor as the
    first place to look if that ever crashes.
  - The "client crashed without unlocking" fail-safe path (stay locked, leak the
    `SessionLock` wrapper, accept a fresh lock client afterward) is exercised by
    nothing but reasoning — it needs an actual `kill -9` against swaylock to
    confirm the backdrop really survives.

## Suggested order of attack

1. Write a working `rc.lua` (keybinds, terminal/launcher, at least one monitor
   rule) — nothing else matters until you can actually drive it.
2. First real build + smoke test of session lock above, including the crash path
   — confirm it before trusting it on a main machine.
3. Pointer constraints + relative pointer — directly serves the Proton/gaming
   use case that's the whole point of the XWayland work.
4. Screencopy (`grim` support at minimum) — cheap, high value, and you'll want
   it the first time something breaks and you want to show someone.
5. Data-control (clipboard manager support), idle-inhibit, DPMS, xdg-
   decoration, virtual-keyboard — round these out as they annoy you.
