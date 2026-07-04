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
- **Tearing control is wired (`wp-tearing-control-v1`).**
  `wlr_tearing_control_manager_v1_create(display, 1)` is now called in
  `Server::setup`, and `Output::handleFrame` asks
  `wlr_tearing_control_manager_v1_surface_hint_from_surface` every frame for
  whatever view is focused, fullscreen, and on that output, setting
  `wlr_output_state.tearing_page_flip` when that surface has hinted async is
  fine. Falls back to a normal page-flip on the same frame if the backend
  rejects the tearing commit specifically. Needs `wayland-protocols` >= 1.31
  (for `staging/tearing-control/`) to build -- see the README's build section.
  Uncapped-fps games get the latency win; nothing else on the output can tear
  the desktop out from under itself, since the check only ever passes for the
  sole focused fullscreen client.
- **xdg-decoration negotiation is wired**, contrary to the previous version of
  this file, which claimed it was missing. `decoration.{hpp,cpp}` +
  `xdg_decoration_manager` in `server.cpp` force server-side decoration mode on
  every client and re-assert it on every `request_mode`, so GTK/Qt apps drawing
  their own CSD over uwuwm's border was never actually a live gap.
- **foreign-toplevel-management is wired**, contrary to the previous version of
  this file, which claimed no window list/taskbar integration was possible.
  Every mapped `View` owns a `wlr_foreign_toplevel_handle_v1` (see
  `View::createForeignToplevel` / the request\_\*/destroy wiring in `view.cpp`)
  with title/app_id/fullscreen/minimized/activated kept in sync both ways.
  waybar's taskbar module, `wlrctl`, and similar tools work against this today.
- **Keyboard-shortcuts-inhibit is wired
  (`zwp-keyboard-shortcuts-inhibit-unstable-v1`).** `shortcuts_inhibit_manager`
  in `server.cpp` grants every inhibit request unconditionally (matching sway's
  behavior -- there's no good UI for a confirmation prompt from a chrome-less
  compositor), and `input::shortcutsInhibited` (input.cpp) skips Lua keybind
  dispatch entirely whenever the focused surface holds an active inhibitor, so a
  remote-desktop viewer, VM console, or a game that wants Alt+Tab for itself now
  gets it. VT-switch (Ctrl+Alt+Fn) is deliberately exempt -- it stays a
  session-level escape hatch even under an inhibitor, so a frozen/misbehaving
  client can't turn "switch to another TTY" into "reboot."
- **DPMS / output power control is wired
  (`wlr-output-power-management-unstable-v1`).** `output_power_manager` in
  `server.cpp` re-applies `enabled` as a plain output-state commit whenever a
  client (swayidle, a power applet) asks to blank/wake a specific output -- the
  same commit path `Output::applyRule` already used for `uwu.monitor.set()`.
  wlroots' own implementation listens to that output's commit signal and
  notifies bound clients of the resulting mode, so the signal handler is the
  entire integration.

## Quality-of-life you'll want within the first month

- **No idle-inhibit.** Fullscreen video/games can't suppress screen-blank if you
  run an idle daemon.
- **No virtual-keyboard/virtual-pointer protocols.** Screen-sharing/remote-
  input-injection tools rely on these; without them, they won't work against
  uwuwm.
- **No cursor-shape-v1.** Newer client toolkits increasingly assume this instead
  of drawing/loading their own xcursor theme; absence isn't fatal (xcursor
  fallback still works) but it's a growing assumption to be behind on.
- **No fractional-scale-v1.** Only integer output scale is available (see
  `Output::applyRule`); a HiDPI/lodpi mixed-monitor setup is stuck rounding to
  whole-number scale.
- **No content-type-v1.** No way for a client to hint "I'm a game" for
  scheduling/vsync decisions; not a blocker today since nothing in this codebase
  would act on it yet, but it's the hook most compositor-side game-mode
  heuristics build on.
- **No xdg-activation.** No cross-client "please focus me, I have a good reason"
  channel -- a launcher spawning a window can't ask for focus the way it could
  ask a compositor that implements this.

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
- **Tearing control**, now that it's wired -- unverified against a real hinting
  client. `wlr_output_state.tearing_page_flip` compiles and the per-frame check
  is straightforward, but whether the DRM backend actually accepts an async
  page-flip on your specific GPU/driver, and whether the fallback-to-vsync retry
  ever actually fires, is untested. A Proton game with `vblank_mode=0` is a
  reasonable smoke test once you're on a real DRM session --
  nested-inside-Hyprland won't exercise the tearing page-flip path at all, since
  that's a KMS/atomic-commit feature the nested Wayland backend doesn't have.
- **Keyboard-shortcuts-inhibit and DPMS**, just added -- not build-verified at
  all yet (no wlroots-0.20 dev environment was available where these were
  written; the API was checked by hand against wlroots' own headers/source, not
  compiled against). First build should be watched closely for these two
  specifically. Once it builds:
  - Shortcuts-inhibit: a client that requests
    `zwp_keyboard_shortcuts_inhibit_manager_v1` (a VNC/RDP client like `wlvncc`,
    or `wl-mirror`-adjacent tools; there's no simple CLI to poke this with)
    should get every key raw while focused, with Lua binds (including
    `mod`-anything) silently not firing. Confirm VT-switch still works while
    that same client is focused and inhibiting.
  - DPMS: `wlopm --off <output>` or `swaymsg`-style tooling against
    `zwlr_output_power_manager_v1` (anything speaking
    wlr-output-power-management-unstable-v1) should blank the output immediately
    and wake it back to its prior mode; confirm the output isn't left disabled
    in `wlr-randr`-equivalent output listings after re-enabling.

## Suggested order of attack

1. First real build + boot, nested inside Hyprland, then from a TTY -- pay
   particular attention to keyboard-shortcuts-inhibit and DPMS, the two newest
   additions, since neither has seen a compiler yet.
2. Smoke-test session lock, including the crash path, before trusting it on a
   main machine.
3. Smoke-test screencopy (`grim`), data-control (a clipboard manager),
   tearing-control (a Proton game with `vblank_mode=0`), keyboard-shortcuts-
   inhibit (a remote-desktop/VM client), and DPMS (`wlopm` or equivalent) -- all
   five are wired and unverified against a live client.
4. Idle-inhibit, virtual-keyboard/virtual-pointer, cursor-shape-v1,
   fractional-scale-v1, content-type-v1, xdg-activation -- round these out as
   they annoy you.
