project(
    'uwuwm',
    ['cpp', 'c'],
    version: '0.1.0',
    default_options: [
        'warning_level=3',
        'cpp_std=c++23',
        'buildtype=debugoptimized',
    ],
)

cpp = meson.get_compiler('cpp')

add_project_arguments('-DWLR_USE_UNSTABLE', language: ['c', 'cpp'])

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

deps = [
    wlroots_dep,
    wayland_server_dep,
    xkbcommon_dep,
    libinput_dep,
    pixman_dep,
    xcb_dep,
    lua_dep,
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
    'src/xwayland_view.cpp',
    'src/view.cpp',
    'src/session_lock.cpp',
    'src/decoration.cpp',
    'src/popup.cpp',
    'src/input.cpp',
    'src/ipc.cpp',
    'src/layershell.cpp',
    'src/layout.cpp',
    'src/lua_config.cpp',
)

inc = include_directories('src')

# ── Executable ───────────────────────────────────────────────────────────
exe = executable(
    'uwuwm',
    src,
    protocols_src,
    include_directories: inc,
    dependencies: deps,
    cpp_args: patched_inc_args,
    c_args: patched_inc_args,
    link_args: ['-Wl,--as-needed'],
    install: true,
)

# ── Install layout (Standard FHS) ───────────────────────────────────────
# Default Meson prefix is /usr/local; `meson install` is what writes to
# the prefix. The expected user flow is:
#
#   meson setup build
#   ninja -C build
#   sudo meson install        # copies to /usr/bin, /usr/share/...
#
# (or, without sudo: `meson configure build -Dprefix=/usr` first; the
# default /usr/local is for personal installs that don't go through a
# package manager, but the .desktop file there is invisible to GDM/SDDM
# which only scan /usr/share/wayland-sessions).
#
# Files installed:
#   /usr/bin/uwuwm                       the binary (executable.install: true)
#   /usr/share/wayland-sessions/uwuwm.desktop  DM-visible session entry
#   /usr/share/uwuwm/rc.lua              example config the user can copy
#                                         to ~/.config/uwuwm/rc.lua; never
#                                         auto-loaded (the runtime looks at
#                                         ~/.config/uwuwm/rc.lua first and
#                                         the embedded kDefaultConfig only
#                                         if that doesn't exist).
#
# NOT installed: the autostart script, which is per-user and not part
# of the shipped tree (the user creates ~/.config/uwuwm/autostart.sh).

install_data(
    'wayland-sessions/uwuwm.desktop',
    install_dir: get_option('datadir') / 'wayland-sessions',
)

# Copy the example rc.lua to /usr/share/uwuwm/ as a *reference*, not as
# something the runtime reads -- uwuwm never looks in /usr/share/uwuwm/
# at startup. The point is just so a fresh user has a copy they can
# `cp /usr/share/uwuwm/rc.lua ~/.config/uwuwm/` to start editing from,
# without having to clone the repo. `install_mode` preserves the source
# file's mode bits (444) so the user can't accidentally edit the
# /usr copy and wonder why their changes vanish on the next update.
install_data(
    'rc.lua',
    install_dir: get_option('datadir') / 'uwuwm',
    install_mode: 'rw-r--r--',
)
