project(
    'uwuwm',
    ['cpp', 'c'],
    version: '0.1.0',
    default_options: [
        'warning_level=3',
        'cpp_std=c++23',
        'buildtype=debugoptimized',
        'prefix=/usr',
    ],
)

cpp = meson.get_compiler('cpp')

add_project_arguments('-DWLR_USE_UNSTABLE', language: ['c', 'cpp'])

# Lets lua_config.cpp's extendPackagePath() find the paw/nyaa Lua
# libraries both when running straight out of the build directory
# (dev/test, e.g. Jlessters's own `./builddir/uwuwm` workflow --
# UWUWM_SRCDIR) and from a system install (UWUWM_DATADIR, matching
# the share tree the Makefile lands at /usr/share/uwuwm/). Quoted
# so the resulting C++ string literal is exactly the path, not the
# path parsed as more preprocessor tokens.
add_project_arguments(
    '-DUWUWM_SRCDIR="' + meson.current_source_dir() + '"',
    '-DUWUWM_DATADIR="' + get_option('prefix') / get_option('datadir') / 'uwuwm' + '"',
    language: ['cpp'],
)

# ── Dependencies ─────────────────────────────────────────────────────────
pkgconfig = find_program('pkg-config')

wlroots_pkg_name = 'wlroots-0.20'
wlroots_dep = dependency(wlroots_pkg_name, required: false)
if not wlroots_dep.found()
    wlroots_pkg_name = 'wlroots'
    wlroots_dep = dependency(wlroots_pkg_name)
endif

wayland_server_dep = dependency('wayland-server')
wayland_protocols_dep = dependency('wayland-protocols')
xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput')
pixman_dep = dependency('pixman-1')

# xcb_intern_atom/xcb_intern_atom_reply for XWaylandView's
# _NET_WM_WINDOW_TYPE_* dialog detection (xwayland_view.cpp) -- wlr/
# xwayland.h already declares xcb_size_hints_t et al (transitively
# pulling in the xcb-icccm headers), but linking libxcb itself isn't
# implied by linking wlroots, so it needs to be requested explicitly
# here or xcb_intern_atom is left an undefined symbol at link time.
xcb_dep = dependency('xcb')

lua_dep = dependency('lua5.4', required: false)
if not lua_dep.found()
    lua_dep = dependency('lua-5.4', required: false)
endif
if not lua_dep.found()
    lua_dep = dependency('lua', required: true)
endif

# Text shaping (HarfBuzz) + rasterization (FreeType) for uwu.bar.* (see
# src/text.hpp/bar.hpp) -- deliberately not cairo+pango: those pull in a
# full 2D vector-graphics stack and a text-layout engine on top of it for
# what a single-line status-bar label needs, which is shaping + glyph
# bitmaps and nothing else. freetype2/harfbuzz's pkg-config module names
# are exactly that, on every distro this has ever needed patching for.
freetype_dep = dependency('freetype2')
harfbuzz_dep = dependency('harfbuzz')

deps = [
    wlroots_dep,
    wayland_server_dep,
    xkbcommon_dep,
    libinput_dep,
    pixman_dep,
    xcb_dep,
    lua_dep,
    freetype_dep,
    harfbuzz_dep,
]

# ── C++-incompatible wlroots headers ────────────────────────────────────
python3 = find_program('python3')

cflags_result = run_command(
    pkgconfig,
    ['--cflags', wlroots_pkg_name],
    check: true,
)
wlr_include_dirs = []
foreach tok : cflags_result.stdout().strip().split(' ')
    if tok.startswith('-I')
        wlr_include_dirs += tok.substring(2)
    endif
endforeach

if wlr_include_dirs.length() == 0
    error(
        'pkg-config --cflags @0@ returned no -I flags to patch headers from'.format(
            wlroots_pkg_name,
        ),
    )
endif

patched_include_dir = meson.current_build_dir() / 'patched_include'

run_command(
    python3,
    [
        meson.current_source_dir() / 'scripts/patch_wlroots_headers.py',
        patched_include_dir,
    ] + wlr_include_dirs,
    check: true,
)

patched_inc_args = ['-I' + patched_include_dir]

# ── Wayland protocol code generation ────────────────────────────────────
wayland_scanner = find_program('wayland-scanner')
wl_protocol_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')

wl_protocol_gen = generator(
    wayland_scanner,
    output: '@BASENAME@-protocol.h',
    arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

protocols_src = wl_protocol_gen.process(
    wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
    wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
    'protocols/wlr-layer-shell-unstable-v1.xml',
    'protocols/wlr-output-power-management-unstable-v1.xml',
)

# ── Sources ──────────────────────────────────────────────────────────────
src = files(
    'src/main.cpp',
    'src/server.cpp',
    'src/output.cpp',
    'src/dwindle.cpp',
    'src/idle.cpp',
    'src/toplevel.cpp',
    'src/wallpaper.cpp',
    'src/xwayland_view.cpp',
    'src/view.cpp',
    'src/session_lock.cpp',
    'src/decoration.cpp',
    'src/popup.cpp',
    'src/input.cpp',
    'src/ipc.cpp',
    'src/layershell.cpp',
    'src/layout.cpp',
    'src/text.cpp',
    'src/bar.cpp',
    'src/widget.cpp',
    'src/lua_config.cpp',
)

inc = include_directories('src')

# ── Executable ───────────────────────────────────────────────────────────
# The Makefile installs the built binary -- not `meson install`. See
# the install section below for the full rationale.
executable(
    'uwuwm',
    src,
    protocols_src,
    include_directories: inc,
    dependencies: deps,
    cpp_args: patched_inc_args + ['-Wno-keyword-macro'],
    c_args: patched_inc_args,
    link_args: ['-Wl,--as-needed'],
    install: false,
)

# ── Install layout (Standard FHS) ───────────────────────────────────────
# Install is handled by the top-level Makefile, not by `meson install`.
# See `install.list` at the repo root for the (src dst mode) triples
# and `make install` for the install + chown logic.
#
# The -DUWUWM_DATADIR=... definition at the top of this file still
# embeds the absolute datadir path in the compiled binary -- that's
# build-time data the runtime reads in extendPackagePath() (see
# src/lua_config.cpp), not an install rule. Removing it would break
# require("paw")/require("nyaa") for any user running the installed
# binary, so it stays regardless of who runs the install.
