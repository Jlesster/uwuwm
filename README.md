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
`protocols/wlr-layer-shell-unstable-v1.xml` (that protocol isn't in
wayland-protocols upstream; it ships with wlroots itself, same as dwl's
`protocols/` directory). `scripts/patch_wlroots_headers.py` patches the system
wlroots headers into a C++-compatible form before compiling against them — this
runs automatically as part of the meson build.

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

Config lives in `~/.config/uwuwm/rc.lua`, loaded once at startup (no hot-reload
— edit and restart). `~/.config/uwuwm/autostart.sh`, if present and executable,
is run once at startup for launching bars/wallpaper/etc. Neither ships a default
in this repo yet — see "Missing for daily use" below.

## What's actually implemented

- Output management (multi-monitor, preferred mode, `request_state` for
  nested-window resize, per-monitor rules for position/mode/scale/transform/
  adaptive-sync via `rc.lua`)
- xdg-shell windows + popups, mapped into the scene graph
- **XWayland**, lazily started (no X server forks until an X11 client connects);
  X11 windows are wrapped the same as native ones (`XWaylandView`)
- Master-stack tiling with tags (1-9), a floating-window escape hatch, and
  fullscreen
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
  the exact lifecycle, and `MISSING_FOR_DAILY_USE.md` for what's unverified
  about it

## What's not, and why that's the right scope cut

- **Explicit sync verification, presentation-time consumers, direct scanout
  profiling.** wlroots gives you these for free as long as you stay on the
  scene-graph commit path, which this codebase does everywhere — there was
  nothing to add. Direct scanout in particular is a property to measure, not a
  feature to write.
- **IPC beyond an autostart script.** dwl's stdin-pipe model is more than this
  needed for a first pass; status-bar integration is a clean, additive follow-up
  once you've picked a bar.
- **rc.lua hot-reload.** Config changes need a restart. Fine for a window
  manager you don't tweak minute-to-minute; annoying if you're iterating on
  config itself.

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
  layout.{hpp,cpp}      # master-stack tiling -- the actual policy layer
  input.{hpp,cpp}        # keyboard, cursor, keybind dispatch into Lua
  lua_config.{hpp,cpp}   # rc.lua loader, uwu.* Lua API surface, RuntimeConfig
  session_lock.{hpp,cpp} # ext-session-lock-v1: backdrop + per-output lock surfaces
  utils.hpp
  main.cpp
```
