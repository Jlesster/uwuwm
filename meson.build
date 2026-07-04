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
)

# ── Sources ──────────────────────────────────────────────────────────────
src = files(
    'src/main.cpp',
    'src/server.cpp',
    'src/output.cpp',
    'src/toplevel.cpp',
    'src/xwayland_view.cpp',
    'src/view.cpp',
    'src/session_lock.cpp',
    'src/popup.cpp',
    'src/input.cpp',
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
    install: false,
)
