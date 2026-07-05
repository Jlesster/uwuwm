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
- **Idle-inhibit is wired (`idle-inhibit-unstable-v1` + `ext-idle-notify-v1`),
  contrary to the previous version of this file, which claimed neither
  existed.** `idle.{hpp,cpp}` wraps each live `zwp_idle_inhibitor_v1` and
  recomputes `wlr_idle_notifier_v1_set_inhibited` from scratch whenever
  visibility could have changed -- inhibitor create/destroy, but also view
  map/unmap/minimize and tag-switch, since a video paused on a tag you've
  switched away from must not keep blocking idle actions forever. See
  `idle.hpp`'s header comment for why a stable recompute was needed instead of
  just deriving lifetime from wlroots' own inhibitor list.
- **Dwindle (Hyprland-style BSP tiling) landed as a second layout engine,** not
  just master-stack. `dwindle.{hpp,cpp}` is a per-output tree, selected via
  `uwu.set_layout("dwindle")`, reconciled against the current tiled-view set on
  every `arrange()` call (there's no separate window-opened/closed tiling hook
  to hang tree mutation off of).
  `uwu.dwindle_toggle_split/swap_split/ rotate_split/splitratio/move_to_root`
  are all live -- see the commented examples already in `rc.lua`. Only
  compile-verified so far, not smoke-tested against a real session -- see below.
- **xdg-activation-v1 is wired**, contrary to the previous version of this file,
  which claimed no cross-client focus-request channel existed. `xdg_activation`
  in `server.cpp` resolves a `request_activate` event's surface back to a
  managed `View` and routes it through the same `Server::focusView` every
  click/alt-tab bind uses, so `session_locked` and the rest of the normal focus
  guards apply for free. Unmanaged (X11 override-redirect) surfaces are
  excluded, same as the click-to-focus guard. A launcher or `xdg-desktop-portal`
  window-activation request can now ask to focus a window on another client's
  behalf.
- **wp-cursor-shape-v1 is wired**, contrary to the previous version of this
  file. `cursor_shape_manager` in `server.cpp` maps a client's named-shape
  request straight onto the same xcursor theme lookup the rest of the cursor
  code already used, so newer toolkits that assume this protocol instead of
  loading their own xcursor theme are covered.
- **wp-content-type-v1 is wired**, contrary to the previous version of this
  file, which claimed nothing would act on it. `content_type_manager` in
  `server.cpp` creates the global, and `output.cpp`'s `focusedWantsGameSync()`
  queries `wlr_surface_get_content_type_v1` as a second, protocol-level path
  into `Output::updateAdaptiveSync` alongside the existing "exactly one visible
  window, and it's focused" heuristic -- so a windowed game sharing a tag with
  e.g. a chat client no longer loses VRR just because it isn't alone on screen.

## Quality-of-life you'll want within the first month

- **No virtual-keyboard/virtual-pointer protocols.** Screen-sharing/remote-
  input-injection tools rely on these; without them, they won't work against
  uwuwm.
- **No fractional-scale-v1.** Only integer output scale is available (see
  `Output::applyRule`); a HiDPI/lodpi mixed-monitor setup is stuck rounding to
  whole-number scale.

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
- **Dwindle**, now that it builds -- reconcile logic (`dwindle::arrange`) hasn't
  run against a real, changing window set yet. Worth checking specifically:
  split orientation flips correctly as windows are added/removed out of
  insertion order, `uwu.dwindle_move_to_root` behaves with `stable` true/false,
  and switching `uwu.set_layout()` back and forth between `"master"` and
  `"dwindle"` mid-session doesn't leave a stale tree around on that output.
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
- **xdg-activation, cursor-shape, and content-type** -- all three compile clean
  against real headers but none has run against a live client yet:
  - xdg-activation: something that actually issues a token and hands it to a
    second client (a launcher spawning a window, or a completed background task
    asking to be focused) should steal focus the same way a click would, and
    respect `session_locked` the same way. `wlrctl toplevel focus` doesn't
    exercise this path -- it's foreign-toplevel, a separate protocol.
  - Cursor-shape: a client that names a shape instead of loading its own xcursor
    theme (recent GTK4 apps are a good test) should render uwuwm's theme cursor
    for that shape, not fall back to a default arrow.
  - Content-type: `mpv --content-type=game` (or any client that hints
    `CONTENT_TYPE_GAME`) sharing a tag with another visible window should keep
    VRR on via `focusedWantsGameSync()`, where it would otherwise drop the
    moment it's not the only visible window.

## Suggested order of attack

1. First real build + boot, nested inside Hyprland, then from a TTY -- pay
   particular attention to keyboard-shortcuts-inhibit and DPMS, the two newest
   additions, since neither has seen a compiler yet.
2. Smoke-test session lock, including the crash path, before trusting it on a
   main machine.
3. Smoke-test screencopy (`grim`), data-control (a clipboard manager),
   tearing-control (a Proton game with `vblank_mode=0`), keyboard-shortcuts-
   inhibit (a remote-desktop/VM client), DPMS (`wlopm` or equivalent), idle-
   inhibit (an idle daemon + `mpv`/a game), dwindle (switch layouts mid-session,
   add/remove windows out of order), xdg-activation (a launcher or completed
   background task requesting focus), cursor-shape (a GTK4 app), and
   content-type (`mpv --content-type=game`) -- all ten are wired and unverified
   against a live client.
4. Virtual-keyboard/virtual-pointer and fractional-scale-v1 -- the two remaining
   real gaps, round out as they annoy you.
