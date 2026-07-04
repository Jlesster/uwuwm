# uwuwm

A minimal tiling Wayland compositor in C++20, built on wlroots, in the dwl/dwm
tradition: tags instead of workspaces, master-stack tiling, no animations, no
client-side decoration beyond a thin border, compile-time config.

This covers milestones **M0-M4** from the build-order in
`writing-a-wayland-compositor.md`: gray screen → first client → input → tiling
policy → layer-shell (bars/launchers). It also includes the adaptive-sync toggle
from §5.2 as a freebie, since it's ~15 lines once fullscreen tracking exists.
**XWayland (M5) and the rest of gaming-readiness (M6) are deliberately not
implemented** — they're real, separately-scoped follow-on work, not something to
half-do inside an already-large first pass.

## Building

```sh
sudo pacman -S wlroots0.20 wayland wayland-protocols libinput xkbcommon pixman cmake pkgconf wayland-utils
# or on a distro that hasn't packaged 0.20 yet, build wlroots 0.20 from
# source first (meson) and make sure its .pc file is on PKG_CONFIG_PATH.

cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

The build pulls `xdg-shell.xml` and `pointer-constraints-unstable-v1.xml` from
your system's `wayland-protocols` package via
`pkg-config --variable=pkgdatadir`, and uses the vendored
`protocols/wlr-layer-shell-unstable-v1.xml` (that protocol isn't in
wayland-protocols upstream; it ships with wlroots itself, same as dwl's
`protocols/` directory).

## Running

From inside an existing Hyprland/Wayland/X11 session (development mode — you'll
live here for most of the build-out, per the doc's §2.2 step 2):

```sh
WAYLAND_DISPLAY=wayland-1 ./build/uwuwm    # or just ./build/uwuwm; it autodetects
```

From a TTY, for a real DRM/KMS session:

```sh
./build/uwuwm
```

`Mod+Return` opens a terminal, `Mod+d` opens a launcher — both are just shell
commands in `config.hpp`, so this only does anything once `wezterm` and `fuzzel`
(or whatever you swap them for) are installed.

## What's actually implemented

- Output management (multi-monitor, preferred mode, `request_state` for
  nested-window resize)
- xdg-shell windows + popups, mapped into the scene graph
- Master-stack tiling with tags (1-9), a floating-window escape hatch, and
  fullscreen
- wlr-layer-shell-v1 with exclusive-zone accounting, for bars/launchers
- Keyboard (single shared xkb keymap across devices) + pointer (move/resize
  drag, focus-follows-click) + a flat compile-time keybind table
- Adaptive sync, toggled only while a fullscreen client owns the output

## What's not, and why that's the right scope cut

- **XWayland.** Real and needed for your GTA V/Proton stack, but it's its own
  milestone with its own surface-type abstraction (the doc's §2.3 `client.h`
  problem) — bolting it on half-finished here would make both halves worse.
- **Explicit sync verification, presentation-time consumers, direct scanout
  profiling.** Per §5.1/§5.3/§5.5: wlroots gives you these for free _as long as
  you stay on the scene-graph commit path_, which this codebase does everywhere
  — there was nothing to add. Direct scanout in particular is a property to
  measure, not a feature to write.
- **IPC beyond an autostart script.** dwl's stdin-pipe model is more than this
  needed for a first pass; status-bar integration is a clean, additive follow-up
  once you've picked a bar.

## A note on correctness

This was written against my knowledge of the wlroots 0.18/0.19-era scene-
graph/xdg-shell/layer-shell APIs, which 0.20 is believed to be source-
compatible with for everything used here — but I generated this without a
wlroots-0.20 install to actually compile and link against, so treat the first
build as the real test. The likeliest failure points if something doesn't
compile: `wlr_backend_autocreate`'s exact parameter list, whether
`wlr_output_layout_create` takes a `wl_display*` in your installed version, and
the `wlr_layer_surface_v1_state` field names in `layershell.cpp` — those three
have moved between point releases historically. Everything else (the listener
pattern, scene-tree structure, tiling math) is independent of those specifics
and shouldn't need surgery if one of them is off.

## Layout

```
CMakeLists.txt
config.hpp           # keybinds, tags, colors, layout constants -- edit & rebuild
protocols/
  wlr-layer-shell-unstable-v1.xml
src/
  listener.hpp        # RAII wl_signal/wl_listener wrapper (the §2.1 idiom)
  server.{hpp,cpp}     # owns every wlroots singleton, runs setup + event loop
  output.{hpp,cpp}     # per-monitor state, frame callback (hot path, raw notify)
  toplevel.{hpp,cpp}   # xdg_toplevel window wrapper, border rendering
  popup.{hpp,cpp}      # xdg_popup positioning
  layershell.{hpp,cpp} # bars/launchers, exclusive-zone arrangement
  layout.{hpp,cpp}     # master-stack tiling -- the actual policy layer
  input.{hpp,cpp}      # keyboard, cursor, keybind dispatch
  main.cpp
```
