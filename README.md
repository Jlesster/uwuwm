<div align="center">

# uwuwm

**A tiling Wayland compositor in C++, built on wlroots 0.18 -- with a Lua
configuration layer on top.**

[![License](https://img.shields.io/badge/license-unlicensed-lightgrey)](#license)
[![C++](https://img.shields.io/badge/C%2B%2B-20%2F23-00599C?logo=cplusplus&logoColor=white)](meson.build)
[![Lua](https://img.shields.io/badge/config-Lua%205.4%20%2F%20LuaJIT-2C2D72?logo=lua&logoColor=white)](rc.lua)
[![wlroots](https://img.shields.io/badge/wlroots-0.18-purple)](https://gitlab.freedesktop.org/wlroots/wlroots)
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

**In progress**

- Unix-socket IPC (line protocol: `query`, `command`, and `subscribe` modes) --
  the wire protocol is defined and window addressing is wired through
  `View::id`; `ipc.cpp` itself is still being written

## Building

```sh
sudo pacman -S wlroots0.18 wayland wayland-protocols libinput xkbcommon \
    pixman meson ninja pkgconf wayland-utils lua54
# wlroots 0.18 not packaged on your distro? Build it from source (meson)
# and make sure its .pc file is on PKG_CONFIG_PATH.

meson setup build
ninja -C build
```

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
uwu.monitor.set('DP-1', { position = { 0, 0 }, scale = 1.0 })
```

See [`rc.lua`](rc.lua) for a full working example.

## IPC

A Unix-socket line protocol is being added for external tooling -- three request
modes:

| Mode        | Purpose                                                                 |
| ----------- | ----------------------------------------------------------------------- |
| `query`     | one-shot read of compositor state (e.g. window list, active tag)        |
| `command`   | trigger an action (focus, move, tag switch) from outside the compositor |
| `subscribe` | long-lived connection streaming state-change events                     |

Windows are addressed by the stable `View::id` assigned to each view, so
external tools can target a specific window across tag/output changes. Status:
protocol defined, wiring in progress -- not yet exposed by a released binary.

## What's not, and why

- **A wibox-equivalent (status bar / widget toolkit).** Not planned as part of
  the compositor itself -- IPC is the intended integration point for bars once
  it's finished, rather than uwuwm drawing its own widgets.
- **Gaming-readiness surface (tearing control, adaptive sync, pointer
  constraints).** Not yet implemented; current focus is core window management,
  rules, and IPC.

## Project layout

```
meson.build
config.hpp              # compile-time constants rc.lua can't reach
rc.lua                  # working example config -- copy to ~/.config/uwuwm/
src/
  server.{hpp,cpp}       # owns wlroots singletons, event loop
  output.{hpp,cpp}       # per-monitor state, frame callback
  toplevel.{hpp,cpp}     # xdg_toplevel window wrapper, border rendering
  xwayland_view.{hpp,cpp}# X11 window wrapper, same View interface
  view.{hpp,cpp}         # shared View base: geometry, tag membership
  popup.{hpp,cpp}        # xdg_popup positioning
  layout.{hpp,cpp}       # tiling dispatch
  decoration.{hpp,cpp}   # xdg-decoration negotiation
  input.{hpp,cpp}        # keyboard, cursor, keybind dispatch into Lua
  lua_config.{hpp,cpp}   # rc.lua loader, uwu.* Lua API surface
  ipc.{hpp,cpp}          # Unix-socket IPC (in progress)
  utils.hpp
  main.cpp
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
