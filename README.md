<div align="center">

# uwuwm

**A tiling Wayland compositor in C++, built on wlroots 0.20 -- with a Lua
configuration layer on top.**

[![License](https://img.shields.io/badge/license-unlicensed-lightgrey)](#license)
[![C++](https://img.shields.io/badge/C%2B%2B-20%2F23-00599C?logo=cplusplus&logoColor=white)](meson.build)
[![Lua](https://img.shields.io/badge/config-Lua%205.4%20%2F%20LuaJIT-2C2D72?logo=lua&logoColor=white)](rc.lua)
[![wlroots](https://img.shields.io/badge/wlroots-0.20-purple)](https://gitlab.freedesktop.org/wlroots/wlroots)
[![Wayland](https://img.shields.io/badge/Wayland-native-informational?logo=wayland&logoColor=white)](https://wayland.freedesktop.org/)
[![Status](https://img.shields.io/badge/status-pre--alpha-orange)](#whats-not-and-why)
[![Issues](https://img.shields.io/github/issues/Jlesster/uwuwm)](https://github.com/Jlesster/uwuwm/issues)
[![Last commit](https://img.shields.io/github/last-commit/Jlesster/uwuwm)](https://github.com/Jlesster/uwuwm/commits)

---

A tag-based, XDG + XWayland-aware tiling compositor built directly on wlroots,
with all runtime behavior -- keybinds, window rules, theming -- driven from a Lua
config instead of a compile-time table. Server-drawn borders, no client-side
decoration required, and a growing Unix-socket IPC surface for scripting and
status-bar integration.

## Table of contents

- [Features](#features)
- [Building](#building)
- [Running](#running)
- [Configuring](#configuring)
- [IPC](#ipc)
- [What's not, and why](#whats-not-and-why)
- [Project layout](#project-layout)
- [Contributing](#contributing)
- [License](#license)

## Features

**Window management**

- Tag-based workspace model with per-view tag membership, stable `View::id` for
  addressing windows externally, and `closeOnTag` behavior
- XDG-shell windows + popups, and XWayland, unified behind a shared `View`
  interface
- Floating window detection heuristics for both XDG and XWayland clients
  (fixed-size and dialog-type windows float automatically)
- Window rules engine: match on app-id/class or `match.floating`, apply output
  placement via `apply.output`, plus shared `centeredFloatBox()` /
  `setFloating()` helpers for consistent float placement
- `wlr-foreign-toplevel-management-v1` -- taskbars, `wlrctl`, and similar tools
  get a live, two-way-synced window list
- `xdg-decoration-unstable-v1` support

**Configuration**

- Lua config layer (`rc.lua`) with a `uwu.monitor` API for per-output
  configuration
- Hot-reload (`uwu.reload()`) with snapshot-and-rollback semantics -- a broken
  config reload restores the last-known-good state instead of crashing the
  compositor

**Protocol coverage**

Most wlroots protocol managers that a tiling compositor reaches for are
wired: `wlr-screencopy-unstable-v1`, `wlr-data-control-unstable-v1`,
`ext-session-lock-v1`, `wlr-foreign-toplevel-management-v1`,
`idle-inhibit-unstable-v1` + `ext-idle-notify-v1`,
`zwp-keyboard-shortcuts-inhibit-unstable-v1`, `wlr-output-power-management-unstable-v1`,
`xdg-activation-v1`, `wp-cursor-shape-v1`, `wp-content-type-v1`,
`wlr-pointer-constraints-unstable-v1` + `wlr-relative-pointer-unstable-v1`,
`wp-tearing-control-v1`, and `xdg-decoration-unstable-v1` (forced to
server-side decoration). For what's deliberately *not* wired and the
known runtime gaps in what is wired, see [MISSING.md](MISSING.md).

## Building

uwuwm's `Makefile` is a thin wrapper over meson/ninja; the dep install
is the only non-obvious step. Start there:

```sh
make deps    # install wlroots 0.20, wayland-protocols, lua5.4, ...
meson setup build
ninja -C build
```

`make deps` is a one-shot installer for uwuwm's build- and runtime-time
deps (`scripts/install-deps.sh` underneath) that detects the distro
from `/etc/os-release` and dispatches to the right package manager
across Arch (and derivatives), Debian/Ubuntu, Fedora/RHEL, Void, Alpine,
openSUSE, and Gentoo. NixOS gets a `nix-shell` one-liner printed
instead. Re-running is safe -- already-installed packages are skipped.
`make deps --check` (or `./scripts/install-deps.sh --check`) prints
what *would* be installed without invoking the package manager, useful
for reviewing a list before committing.

The build itself is just `meson setup` + `ninja`; the only quirk is
that meson.build's `wlroots-0.20` lookup falls back to `wlroots` if
your distro hasn't versioned the slot, and `scripts/patch_wlroots_headers.py`
strips the C99 `[static N]` syntax from `wlr/render/color.h` and
`wlr/types/wlr_scene.h` because Clang rejects it in C++ -- both
visible in the meson log on a successful build.

### If `make deps` doesn't recognize your distro

`scripts/install-deps.sh` prints the manual install list as a fallback.
The short version, in case the script itself isn't available:

| Purpose | Packages |
| --- | --- |
| Build tools | `meson`, `ninja`, `pkgconf` (or `pkg-config`), `python3`, `gcc/g++` |
| wlroots | **`wlroots 0.20` is mandatory** -- 0.19 / 0.18 will not build uwuwm. Look for `wlroots0.20`, `wlroots-0.20`, `libwlroots-0.20-dev`, or `wlroots-devel` depending on distro. Source build (meson) works too -- put the resulting `.pc` file on `PKG_CONFIG_PATH`. |
| Wayland stack | `wayland-server`, `wayland-protocols` (>= 1.31, for `wp-tearing-control-v1`), `libinput`, `libxkbcommon`, `pixman`, `xcb` |
| Lua | `lua 5.4` (headers + library) |
| Runtime | `swaybg` -- only if you want `nyaa.wallpaper.set()` to actually launch something; uwuwm itself works fine without it, the helper just logs a warning and leaves `background_color` as-is. |

## Running

From inside an existing Wayland/X11 session (development mode):

```sh
WAYLAND_DISPLAY=wayland-1 ./build/uwuwm
```

From a TTY, for a real DRM/KMS session:

```sh
./build/uwuwm
```

Config lives at `~/.config/uwuwm/rc.lua`, loaded at startup and hot-reloadable
via `uwu.reload()`.

## Configuring

```lua
-- window rules
uwu.rule({
  match = { app_id = 'mpv' },
  apply = { floating = true, tag = 3 },
})

uwu.rule({
  match = { floating = true },
  apply = { output = 'DP-1' },
})

-- monitor config
uwu.monitor.set('DP-1', { x = 0, y = 0, scale = 1.0 })
```

See [`rc.lua`](rc.lua) for a full working example.

## IPC

A Unix-socket line protocol is available for external tooling -- a
newline-delimited UTF-8 text socket at
`$XDG_RUNTIME_DIR/uwuwm-<WAYLAND_DISPLAY>.sock` (falling back to
`/tmp/uwuwm-<WAYLAND_DISPLAY>.sock`). One request per line in, one
response per line out: a JSON value for queries, or a bare
`ok` / `error: <reason>` for commands. Windows are addressed by the
stable `View::id` assigned to each view, so external tools can target a
specific window across tag/output changes.

| Command | Purpose |
| --- | --- |
| `get_outputs` | JSON array of output snapshots |
| `get_clients` | JSON array of client snapshots |
| `get_tags <output>` | `{"tagset": N}` for the named output |
| `focus_tag <n> [output]` | Switch an output to showing only tag `n` |
| `set_tag <client_id> <n>` | Move client to tag `n` |
| `close <client_id>` | Close one client |
| `close_all_on_tag <n>` | Close every client on tag `n` |
| `reload` | Hot-reload `rc.lua` |
| `subscribe <events>` | Stream events: `client`, `output`, `tag`, or `*` (comma-separated) |

The full wire spec lives in `src/ipc.hpp`'s leading comment. Subscribers
receive the same `client::*` / `output::*` / `tag::change` events that
`uwu.hook()` Lua callbacks do -- `IpcServer::onClientEvent` /
`onOutputEvent` / `onTagChange` are wired into the exact same call
sites `LuaConfig::fireClientEvent` etc. fire from, so a bar module and
a Lua hook can never see divergent events.

## What's not, and why

For a current list of what isn't yet implemented and the known runtime
gaps in what is wired, see [MISSING.md](MISSING.md). This section used
to enumerate the same gaps but drifted out of sync with the source --
MISSING.md is the source of truth, re-verified against `src/` rather
than written from memory.

The two pieces that are explicitly *not planned* in uwuwm itself
(captured in MISSING.md for completeness):

- **A wibox-equivalent (status bar / widget toolkit).** IPC is the
  intended integration point for bars, not a compositor-drawn widget
  surface.
- **Direct-scanout profiling and presentation-time consumer code.**
  Outside the scope of a daily-driver window manager.

## Project layout

```
meson.build
config.hpp              # compile-time constants rc.lua can't reach
rc.lua                  # working example config -- copy to ~/.config/uwuwm/
src/
  main.cpp
  server.{hpp,cpp}       # owns wlroots singletons, event loop, all protocol managers
  output.{hpp,cpp}       # per-monitor state, frame callback, adaptive-sync gate
  toplevel.{hpp,cpp}     # xdg_toplevel window wrapper, border rendering
  xwayland_view.{hpp,cpp}# X11 window wrapper, same View interface
  view.{hpp,cpp}         # shared View base: geometry, tag membership
  popup.{hpp,cpp}        # xdg_popup positioning
  layout.{hpp,cpp}       # master-stack tiling dispatch
  dwindle.{hpp,cpp}      # Hyprland-style BSP tiling engine
  decoration.{hpp,cpp}   # xdg-decoration negotiation
  wallpaper.{hpp,cpp}    # native wallpaper decoding (uwu.wallpaper.*)
  idle.{hpp,cpp}         # idle-inhibit + idle-notify
  session_lock.{hpp,cpp} # ext-session-lock-v1
  layershell.{hpp,cpp}   # wlr-layer-shell integration
  input.{hpp,cpp}        # keyboard, cursor, keybind dispatch into Lua
  ipc.{hpp,cpp}          # Unix-socket IPC
  lua_config.{hpp,cpp}   # rc.lua loader, uwu.* Lua API surface
  utils.hpp
lib/
  meta/uwu.lua           # LuaLS type annotations for the uwu global
  paw/init.lua           # sugar over uwu.* (paw.client, paw.layout, paw.keys, ...)
  nyaa/init.lua          # theming layer (nyaa.wear, nyaa.rule, ...)
  nyaa/palettes.lua      # 26-role color palettes (catppuccin, gruvbox, nord, ...)
  nyaa/export.lua        # render a palette into GTK/kitty/foot/waybar/dunst/...
  nyaa/wallpaper.lua     # swaybg-backed wallpaper helper (orthogonal to uwu.wallpaper)
```

## Contributing

This is a daily-driver compositor developed in the open, not currently
soliciting outside contributions -- issues and forks are welcome, PRs may sit
unreviewed for a while.

## License

No `LICENSE` file exists in this repository yet -- until one is added, all rights
are reserved by default and the usual open-source permissions (copying,
modification, redistribution) don't apply. If you intend to reuse this code,
open an issue first.
