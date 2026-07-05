# uwuwm

A tiling Wayland compositor in C++23, built on wlroots, in the dwl/dwm
tradition: tags instead of workspaces, master-stack tiling, thin borders instead
of client-side decoration, `rc.lua` for runtime config (AwesomeWM- style)
instead of a compile-time table.

This has grown past the original M0-M4 build-order milestones (gray screen →
first client → input → tiling policy → layer-shell) to also include XWayland
(M5) and several of the M6 gaming-readiness items. See "What's not, and why"
below for what's still deliberately out of scope.

## Building

```sh
sudo pacman -S wlroots0.20 wayland wayland-protocols libinput xkbcommon \
    pixman meson ninja pkgconf wayland-utils lua54
# or on a distro that hasn't packaged 0.20 yet, build wlroots 0.20 from
# source first (meson) and make sure its .pc file is on PKG_CONFIG_PATH.

meson setup build
ninja -C build
```

The build pulls `xdg-shell.xml` and `pointer-constraints-unstable-v1.xml` from
your system's `wayland-protocols` package via pkg-config, and uses the vendored
`protocols/wlr-layer-shell-unstable-v1.xml` and
`protocols/wlr-output-power-management-unstable-v1.xml` (neither is in
wayland-protocols upstream; they ship with wlroots itself, same as dwl's
`protocols/` directory). Tearing control (`wp-tearing-control-v1`) needs no
separate protocol generation here — `wlr_tearing_control_manager_v1_create` is
satisfied directly by libwlroots-0.20, confirmed by a clean link.
`scripts/patch_wlroots_headers.py` patches the system wlroots headers into a
C++-compatible form before compiling against them — this runs automatically as
part of the meson build.

## Installing

```sh
meson setup build
ninja -C build
# Display-manager visibility requires /usr -- the default /usr/local
# prefix lands the .desktop where GDM/SDDM don't scan. Configure with
# -Dprefix=/usr before install if you want uwuwm in your login screen.
meson configure build -Dprefix=/usr      # optional, for DM discovery
sudo meson install -C build             # → /usr/bin, /usr/share/...
```

Files installed under `$prefix`:

- `$bindir/uwuwm` — the binary
- `$datadir/wayland-sessions/uwuwm.desktop` — session entry the display manager
  discovers (GDM, SDDM, LightDM all scan this dir)
- `$datadir/uwuwm/rc.lua` — a reference copy of the example config; uwuwm never
  reads it at runtime (it only looks at `~/.config/uwuwm/rc.lua`), this is just
  a `cp` target for new users who'd rather not clone the repo to start

If you don't want uwuwm visible in your DM, just skip the
`meson configure -Dprefix=/usr` line and run `meson install` without sudo —
everything lands under `/usr/local/...` and you can launch uwuwm manually from a
TTY as in the "Running" section below.

## Running

From inside an existing Hyprland/Wayland/X11 session (development mode — you'll
live here for most of the build-out):

```sh
WAYLAND_DISPLAY=wayland-1 ./build/uwuwm    # or just ./build/uwuwm; it autodetects
```

From a TTY, for a real DRM/KMS session:

```sh
./build/uwuwm
```

Config lives in `~/.config/uwuwm/rc.lua`, loaded at startup and hot-reloadable
via `uwu.reload()` (bound to `mod+shift+r` in the shipped `rc.lua`) or
`pkill -HUP uwuwm` — a broken reload keeps the last-known-good config running
rather than crashing the session. A working example ships at the repo root; copy
it to `~/.config/uwuwm/rc.lua` to start. `~/.config/uwuwm/autostart.sh`, if
present and executable, is run once at startup for launching bars/
wallpaper/etc.; that one doesn't ship a default — see `MISSING.md`.

## What's actually implemented

- Output management (multi-monitor, preferred mode, `request_state` for
  nested-window resize, per-monitor rules for position/mode/scale/transform/
  adaptive-sync via `rc.lua`)
- xdg-shell windows + popups, mapped into the scene graph
- **XWayland**, lazily started (no X server forks until an X11 client connects);
  X11 windows are wrapped the same as native ones (`XWaylandView`)
- Master-stack tiling with tags (1-9), a floating-window escape hatch, and
  fullscreen
- **Dwindle**: a second, per-output-selectable tiling algorithm (Hyprland-style
  BSP tree) alongside master-stack, switched via
  `uwu.set_layout("master" | "dwindle")`. Supports split toggle/swap/rotate,
  split-ratio adjustment, and move-to-root, all exposed as `uwu.dwindle_*` Lua
  functions -- see `rc.lua` for bind examples and `dwindle.cpp`'s header comment
  for the tree-reconcile approach (there's no separate window-opened/closed
  tiling hook, so `arrange()` reconciles the persistent tree against the current
  tiled-view set every call)
- wlr-layer-shell-v1 with exclusive-zone accounting, for bars/launchers
- Keyboard (single shared xkb keymap across devices) + pointer (move/resize
  drag, focus-follows-click) with keybinds defined in `rc.lua` as arbitrary Lua
  closures, not a closed action enum
- Adaptive sync, toggled only while a fullscreen client owns the output
- Open/close and tag-switch scene-graph animations (position + opacity tween),
  togglable and tunable from `rc.lua`
- Autostart script (`~/.config/uwuwm/autostart.sh`)
- Session lock (`ext-session-lock-v1`): a lock daemon (swaylock, etc.) can blank
  every output and take exclusive input until it unlocks. Fails safe if the lock
  client crashes without unlocking — see `session_lock.hpp`'s header comment for
  the exact lifecycle, and `MISSING.md` for what's unverified about it
- Pointer constraints + relative pointer (`pointer-constraints-unstable-v1`,
  `relative-pointer-unstable-v1`): lock/confine plus relative-motion forwarding,
  what Proton/SDL games need for FPS-style mouselook
- Tearing control (`wp-tearing-control-v1`): a focused, fullscreen client
  (Proton/SDL games, mesa's swapchain internals) that's hinted its content is
  fine being shown with tearing gets an async page-flip instead of a normal
  vsync'd one, trading a torn frame for lower latency. Never applies to a
  windowed, backgrounded, or non-hinting client -- see `output.cpp`'s
  `wantsTearing()` for the exact per-frame check, and `MISSING.md` for what's
  unverified about it
- Screen capture (`wlr-screencopy-unstable-v1`): `grim`-style screenshots and
  portal-based screen share/recording (OBS, Discord, `wf-recorder`)
- Clipboard persistence (`wlr-data-control-unstable-v1`): clipboard managers
  (cliphist, etc.) can observe and set the selection independent of whichever
  app currently owns it, on top of the normal copy/paste path
- xdg-decoration negotiation: server-side decoration mode is forced on every
  client and re-asserted on every `request_mode`, so GTK/Qt apps never draw
  their own CSD over uwuwm's border
- foreign-toplevel-management (`wlr-foreign-toplevel-management-unstable-v1`):
  every mapped window is exposed with title/app_id/fullscreen/minimized/
  activated kept in sync both ways -- waybar's taskbar module, `wlrctl`, and
  similar tools work against this
- Keyboard-shortcuts-inhibit (`zwp-keyboard-shortcuts-inhibit-unstable-v1`):
  granted unconditionally, same as sway; a remote-desktop viewer, VM console, or
  game that wants raw key access gets it, with Lua keybind dispatch skipped
  entirely while an inhibitor is active on the focused surface. VT-switch stays
  exempt as a session-level escape hatch
- DPMS / output power control (`wlr-output-power-management-unstable-v1`): an
  idle daemon or power applet can blank/wake a specific output
- Idle-inhibit (`idle-inhibit-unstable-v1` + `ext-idle-notify-v1`): a client
  (mpv, a game, a video call) can suppress an idle daemon's (swayidle)
  timeout-driven actions for as long as its inhibited surface stays visible --
  mapped, not minimized, and on its output's active tagset. Recomputed on
  inhibitor creation/destruction as well as view map/unmap/minimize and
  tag-switch, since a surface can lose visibility without its inhibitor being
  destroyed -- see `idle.hpp`'s header comment
- xdg-activation (`xdg-activation-v1`): a launcher or a background task that
  just finished can hand another client's surface a focus token; resolved back
  to a managed `View` and routed through the same focus path a click or alt-tab
  uses, so `session_locked` and the rest of the normal guards apply
- Cursor-shape (`wp-cursor-shape-v1`): a client can just name a shape ("text",
  "grab", etc.) instead of loading its own xcursor theme, mapped onto the same
  theme the rest of uwuwm's cursor handling already uses
- Content-type (`wp-content-type-v1`): a client hinting `CONTENT_TYPE_GAME`
  keeps adaptive sync on even when it isn't the only visible window on its
  output -- a second, protocol-level path into VRR alongside the existing "alone
  and focused" heuristic
- rc.lua hot-reload: `uwu.reload()` (bound to `mod+shift+r`) or
  `pkill -HUP uwuwm` re-run rc.lua from scratch without restarting the
  compositor; a broken reload keeps the last-known-good config running

## What's not, and why that's the right scope cut

- **Explicit sync verification, presentation-time consumers, direct scanout
  profiling.** wlroots gives you these for free as long as you stay on the
  scene-graph commit path, which this codebase does everywhere — there was
  nothing to add. Direct scanout in particular is a property to measure, not a
  feature to write.
- **IPC beyond an autostart script.** dwl's stdin-pipe model is more than this
  needed for a first pass; status-bar integration is a clean, additive follow-up
  once you've picked a bar.

## A note on correctness

This was written against my knowledge of the wlroots 0.18/0.19-era scene-
graph/xdg-shell/layer-shell/xwayland APIs, believed source-compatible with 0.20
for everything used here, but generated without a wlroots-0.20 install to
actually compile and link against — treat the first build as the real test.

## Layout

```
meson.build
scripts/patch_wlroots_headers.py
protocols/
  wlr-layer-shell-unstable-v1.xml
config.hpp            # compile-time constants that rc.lua can't reach (tag count)
src/
  listener.hpp        # RAII wl_signal/wl_listener wrapper
  server.{hpp,cpp}     # owns every wlroots singleton, runs setup + event loop
  output.{hpp,cpp}     # per-monitor state, frame callback (hot path, raw notify)
  toplevel.{hpp,cpp}   # xdg_toplevel window wrapper, border rendering
  xwayland_view.{hpp,cpp} # X11 window wrapper, same View interface as toplevel
  view.{hpp,cpp}       # shared View base: geometry, animation, tag membership
  popup.{hpp,cpp}       # xdg_popup positioning
  layershell.{hpp,cpp}  # bars/launchers, exclusive-zone arrangement
  layout.{hpp,cpp}      # tiling dispatch + shared tiled-view filter
  dwindle.{hpp,cpp}      # BSP-tree tiling, the alternative to master-stack
  idle.{hpp,cpp}          # idle-inhibit-unstable-v1 + ext-idle-notify-v1
  input.{hpp,cpp}        # keyboard, cursor, keybind dispatch into Lua
  lua_config.{hpp,cpp}   # rc.lua loader, uwu.* Lua API surface, RuntimeConfig
  session_lock.{hpp,cpp} # ext-session-lock-v1: backdrop + per-output lock surfaces
  utils.hpp
  main.cpp
```
