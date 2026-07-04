# Missing for daily use

Checked against what's actually wired into `server.cpp`/`input.cpp`/the meson
source list, not just what's aspirational in comments or in the README. This
file has drifted out of sync with the code before -- treat it as a snapshot, not
a spec, and re-verify against `src/` before trusting an item here for long.

## Fixed since the last pass

- **`rc.lua` ships now.** A full example (appearance, keybinds, tag binds,
  monitor-rule examples) lives at the repo root.
- **Hot-reload is implemented**, contrary to what the README's "what's not"
  section still claims. `uwu.reload()` (bound to `mod+shift+r`) and
  `pkill -HUP uwuwm` both call `LuaConfig::reload()`; a broken rc.lua keeps the
  last-known-good config running instead of crashing or going dark.
- **Pointer constraints + relative pointer are wired**, contrary to the previous
  version of this file. `input.cpp` fully implements
  `wlr_pointer_constraints_v1` (lock/confine) and forwards relative motion via
  `wlr_relative_pointer_manager_v1` -- the FPS/Proton mouselook gap is closed.
- **Session lock now builds.** It was generated against pre-0.20 API shapes and
  failed to compile (`wlr_session_lock_surface_v1` has no `map` event of its own
  in 0.20 -- that moved to the underlying `wlr_surface` as part of the unified
  surface-role model; `session_lock.cpp`/`output.cpp` were also missing two
  cross-includes for types they dereference). Fixed; still only
  compile-verified, not runtime-tested -- see "needs a real smoke test" below.
- **Screencopy is wired.** `wlr_screencopy_manager_v1_create(display)` is now
  called in `Server::setup`. `grim`, `wf-recorder`, and portal-based screen
  share/capture (OBS, Discord) should all work against it. It's a self-contained
  wlroots manager -- no per-frame plumbing needed on our side beyond creating
  the global.
- **Clipboard persistence is wired.**
  `wlr_data_control_manager_v1_create(display)` is now called alongside it, so
  cliphist/wl-clip-persist-style clipboard managers can observe and set the
  selection independent of whichever app currently owns it.

## Will bite you on your gaming stack specifically

- **No tearing-control (`wp_tearing_control_v1`).** Uncapped-fps older/lighter
  games will fight vsync/stutter without it. Pointer constraints solved the aim
  problem; this is the remaining perf-adjacent gap.
- **No keyboard-shortcuts-inhibit.** A game or remote-input tool that wants to
  grab all keys (e.g. capturing Alt+Tab) has no protocol to ask for that.
- **No `wlr-output-power-management-v1` / DPMS control.** No way to blank/wake
  outputs from software.

## Quality-of-life you'll want within the first month

- **No idle-inhibit.** Fullscreen video/games can't suppress screen-blank if you
  run an idle daemon.
- **No xdg-decoration negotiation.** Some GTK apps may draw their own CSD over
  uwuwm's thin border.
- **No virtual-keyboard/virtual-pointer protocols.** Screen-sharing/remote-
  input-injection tools rely on these; without them, they won't work against
  uwuwm.
- **No foreign-toplevel-management.** No window list/app-switcher/waybar- style
  taskbar integration is possible yet -- nothing exposes toplevel state outside
  the compositor.

## Explicitly out of scope, not gaps

Per the README's own scope cuts: presentation-time consumer code, direct-
scanout profiling tooling, and any IPC beyond the autostart script (no
waybar-style status protocol). None of these block daily use the way the items
above do.

## Needs a real smoke test, not just a clean compile

- **Session lock** (`src/session_lock.{hpp,cpp}`), now that it builds. Worth
  checking specifically:
  - The map-event fix (now listening on `surface->surface->events.map` instead
    of the nonexistent `surface->events.map`) -- confirm focus handoff to the
    lock surface actually fires on first real swaylock run.
  - The "client crashed without unlocking" fail-safe path -- needs an actual
    `kill -9` against swaylock to confirm the backdrop survives and a second
    lock client can still supersede it.
  - What happens to a `wlr_session_lock_surface_v1` when its output is unplugged
    mid-lock -- flagged inline in `output.cpp`'s destructor.
- **Screencopy and data-control**, now that they're wired -- neither has run
  against a real client (`grim`, `wl-paste -w`/cliphist) yet. Confirm with:
  `grim -o <output> test.png` and `wl-copy hello && cliphist list` (or
  equivalent) after a real session.

## Suggested order of attack

1. First real build + boot, nested inside Hyprland, then from a TTY.
2. Smoke-test session lock, including the crash path, before trusting it on a
   main machine.
3. Smoke-test screencopy (`grim`) and data-control (a clipboard manager) -- both
   are newly wired and unverified against a live client.
4. Tearing-control + keyboard-shortcuts-inhibit -- the remaining gaming-specific
   gaps.
5. DPMS, idle-inhibit, xdg-decoration, foreign-toplevel-management -- round
   these out as they annoy you.
