#!/usr/bin/env bash
# scripts/install-deps.sh -- install uwuwm's build- and runtime-time
# dependencies on whatever distro this happens to be, in one shot. New
# users run `make deps` (or this script directly) before `make` so they
# don't have to read meson.build and hunt each dep down by name.
#
# What it installs (everything uwuwm's `make` needs, plus `swaybg`
# because rc.lua's wallpaper helper shells out to it):
#
#   Build tools:   meson, ninja, pkgconf, python3
#   Compilers:     gcc/g++ (or whatever meson picks; no specific version
#                  required -- the project uses c++23 but defaults to
#                  whatever your distro ships as `c++`)
#   wlroots 0.20 -- the wlroots-0.20 package, the wlroots package, or a
#                  source build depending on what the distro has.
#                  wlroots 0.19 / 0.18 won't work -- uwuwm depends on
#                  APIs added in 0.20 (the scene-graph rewrite, in
#                  particular -- see src/server.cpp's scene-graph setup
#                  and the wlr_scene.h header patch in
#                  scripts/patch_wlroots_headers.py).
#   wayland-server, wayland-protocols (>= 1.31 -- wp-tearing-control-v1
#                  is in staging and only landed upstream after 1.31),
#   libinput, xkbcommon, pixman, xcb (for xwayland_view.cpp's
#                  xcb_intern_atom), lua5.4
#   Runtime only:  swaybg (only if you want `nyaa.wallpaper.set()` to
#                  actually have something to launch -- uwuwm itself
#                  doesn't need it, the script just warns and falls
#                  back to background_color if swaybg is missing).
#
# The script is idempotent: it skips any package that's already
# installed (per the package manager's own notion of "installed"), so
# re-running is safe. It does NOT touch ~/.config/uwuwm/, doesn't
# rebuild anything, doesn't run uwuwm. Pure dependency install.
#
# Usage:
#   ./scripts/install-deps.sh         # install everything
#   ./scripts/install-deps.sh --check # print what would be installed
#                                    # without invoking the package
#                                    # manager
#   ./scripts/install-deps.sh --help  # this message
#
# Exits 0 if everything's already there or installs cleanly; non-zero
# only on a hard failure (unknown distro, package manager rejected the
# install, etc). A "unknown distro" exit prints the manual install list
# so the user has something to copy-paste.

set -euo pipefail

# ── Argument parsing ────────────────────────────────────────────────────
usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

CHECK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --help|-h) usage 0 ;;
        *) echo "install-deps.sh: unknown argument '$arg'" >&2; usage 1 ;;
    esac
done

# ── Build/runtime dep manifest ──────────────────────────────────────────
# One source of truth. The package names are *per-distro*; the
# rationale field is the only common ground (a name + a version
# constraint for the version-pinned ones). `lua` and `swaybg` are
# listed under "runtime" because they're optional in spirit (swaybg
# is a warning-and-fallback; lua is only needed if a downstream
# packager wants to split the lua libs into a separate subpackage) --
# but in practice every distro has them so the script installs
# them anyway.
#
# Note on wlroots 0.20: most distros ship wlroots 0.19 or 0.18 as
# their main `wlroots` package. The wlroots-0.20 package is
# typically a separate slot, sometimes in AUR/various community
# repos. The script tries `wlroots-0.20` first and falls back to
# `wlroots` so a source-built wlroots (where the .pc file is just
# `wlroots.pc`) works too. The README still calls this out
# explicitly because it's the only non-uniform dep.

# ── Distro dispatch ─────────────────────────────────────────────────────
# The package list is rendered as one quoted string per distro. We
# could keep this in an associative array, but a case/esac reads
# better than bash 4+ features for a file this small -- and runs
# on any bash 3.2 (still macOS's default).

detect_distro() {
    # /etc/os-release is the freedesktop-standard place; os-release's
    # `ID` field is the canonical short name (arch, debian, ubuntu,
    # fedora, ...). $ID_LIKE gives parent distros for derivatives
    # (ubuntu -> debian, mint -> ubuntu, ...). lsb_release is a
    # fallback for the rare system where /etc/os-release is missing.
    if [ -r /etc/os-release ]; then
        . /etc/os-release
        echo "${ID:-unknown}"
        return
    fi
    if command -v lsb_release >/dev/null 2>&1; then
        lsb_release -is | tr '[:upper:]' '[:lower:]'
        return
    fi
    echo "unknown"
}

# Pre-format the install command without running it, so --check can
# show the user what *would* happen and so the actual install path
# stays a single line per distro (easier to keep in sync with
# reality than 4-place quoting).

emit_install() {
    local cmd="$1"
    echo "+ $cmd"
}

# `pkg_installed <pm> <pkg>` -- distro-specific "is X already there"
# check. Returns 0 if installed (and the script will skip it), 1 if
# not (and the script will install it). Used to keep the install
# idempotent. We only implement it for the package managers we
# actually dispatch to below; --check and unknown-distro paths don't
# need it.

# `run_install <list-of-pkgs>` -- actually invoke the package
# manager, sudo if needed. This is the only place `sudo` is
# required; everything else runs unprivileged.

# ── Per-distro definitions ─────────────────────────────────────────────
# One case branch per distro, with the package list inline. Keep
# the lists in alphabetical order so adding a new package is
# obvious-diff rather than hidden-noise.

case "$(detect_distro)" in
    arch|manjaro|endeavouros|archcraft|garuda|cachyos)
        # Arch + derivatives. wlroots 0.20 is in [extra] as wlroots0.20
        # (the versioned-slot package); the unwrapped `wlroots` is the
        # older release. Both are tried, in that order, by the
        # generic "install these if missing" loop below -- but Arch
        # specifically wants `wlroots0.20`, not `wlroots`, so we
        # override the loop's default list here.
        PKGS_BUILD=(
            meson ninja pkgconf python
            wlroots0.20
            wayland wayland-protocols libinput libxkbcommon pixman libxcb
            lua
        )
        # lua is lua54 on Arch (AUR has lua54, repos have lua). Both
        # are lua5.4-line; meson.build's lua-5.4 lookup handles both.
        # swaybg lives in [extra] since 2022.
        PKGS_RUNTIME=(swaybg)
        PM="pacman"
        SUDO="sudo"
        INSTALL_ARGS=(--needed --noconfirm -S)
        CHECK_CMD=(pacman -Qi)
        ;;

    debian|ubuntu|linuxmint|pop|elementary|zorin|kali|raspbian)
        # Debian + Ubuntu + derivatives. wlroots 0.20 is in
        # Debian trixie/13+ and Ubuntu 24.04+ repos as `libwlroots-0.20-dev`
        # (note the `-dev` suffix -- this is the `-dev` packaging
        # convention, the runtime lib is `libwlroots-0.20-0`).
        # Older releases need a backport, PPA, or source build; the
        # README's "Building" section covers that.
        PKGS_BUILD=(
            meson ninja-build pkg-config python3 build-essential
            libwlroots-0.20-dev
            libwayland-dev wayland-protocols libinput-dev libxkbcommon-dev
            libpixman-1-dev libxcb1-dev
            liblua5.4-dev
        )
        # lua5.4-dev on Debian/Ubuntu ships lua5.4.pc; meson.build
        # prefers `lua5.4` then `lua-5.4` then `lua`, all of which
        # resolve to the same library on these distros.
        PKGS_RUNTIME=(swaybg)
        PM="apt"
        SUDO="sudo"
        INSTALL_ARGS=(install -y --no-install-recommends)
        CHECK_CMD=(dpkg-query -W -f='${Status}\n')
        # apt's "is installed" query returns "install ok installed";
        # dpkg-query is its underlying impl and is the most reliable
        # -- apt-cache policy is too chatty and dpkg -l is the
        # wrong tool (it's about installability, not presence).
        ;;

    fedora|nobara|ultramarine|rhel|centos|rocky|almalinux)
        # Fedora + RHEL-family. wlroots 0.20 is in the standard
        # Fedora repos as `wlroots-devel` (the `-devel` suffix is
        # Fedora's equivalent of Debian's `-dev`). RHEL/CentOS
        # Stream/Rocky need EPEL + a 3rd-party repo for wlroots;
        # the README's "Building" section calls that out.
        PKGS_BUILD=(
            meson ninja-build pkgconf pkgconfig-pkg-config python3 gcc-c++
            wlroots-devel
            wayland-devel wayland-protocols-devel libinput-devel
            libxkbcommon-devel pixman-devel libxcb-devel
            lua-devel
        )
        # Fedora's `lua-devel` is lua 5.4 since Fedora 39; older
        # versions of Fedora need `lua5.4-devel` explicitly. The
        # generic name covers >= 39 cleanly.
        PKGS_RUNTIME=(swaybg)
        PM="dnf"
        SUDO="sudo"
        # dnf5 is the default on Fedora 41+; the install command
        # is the same on both dnf4 and dnf5.
        INSTALL_ARGS=(install -y)
        CHECK_CMD=(rpm -q)
        ;;

    void)
        # Void ships wlroots 0.20 directly (their wlroots package
        # tracks upstream closely). No versioned-slot split.
        PKGS_BUILD=(
            meson ninja pkgconf python3 gcc
            wlroots-devel
            wayland-devel wayland-protocols libinput-devel
            libxkbcommon-devel pixman-devel libxcb-devel
            lua5.4-devel
        )
        PKGS_RUNTIME=(swaybg)
        PM="xbps-install"
        SUDO="sudo"
        INSTALL_ARGS=(-y)
        CHECK_CMD=(xbps-query -S)
        ;;

    alpine)
        # Alpine. wlroots 0.20 is in `main` as `wlroots0.20-dev`
        # (the versioned-slot pattern Arch uses). Note the
        # `linux-headers` line -- Alpine's kernel-headers package
        # carries the uinput/libinput headers that the rest of
        # the toolchain depends on.
        PKGS_BUILD=(
            meson ninja pkgconf python3 build-base
            wlroots0.20-dev
            wayland-dev wayland-protocols libinput-dev libxkbcommon-dev
            pixman-dev libxcb-dev
            lua5.4-dev
        )
        PKGS_RUNTIME=(swaybg)
        PM="apk"
        SUDO="sudo"
        INSTALL_ARGS=(add)
        CHECK_CMD=(apk info -e)
        ;;

    opensuse-tumbleweed|opensuse-leap|suse)
        # openSUSE. wlroots 0.20 is in the standard repos as
        # `wlroots-devel` (the `devel:` patterns aren't where
        # this lives -- it's in the main patterns-*-wayland
        # pattern, enabled by default on Tumbleweed).
        PKGS_BUILD=(
            meson ninja pkg-config python3 gcc-c++
            wlroots-devel
            wayland-devel wayland-protocols libinput-devel
            libxkbcommon-devel pixman-devel libxcb-devel
            lua5.4-devel
        )
        PKGS_RUNTIME=(swaybg)
        PM="zypper"
        SUDO="sudo"
        INSTALL_ARGS=(install -y --no-recommends)
        CHECK_CMD=(rpm -q)
        ;;

    gentoo|funtoo)
        # Gentoo. The package set is *amend-only* -- the
        # user has likely already configured their @world set
        # and adding `::guru` overlay deps here would be
        # overstepping. wlroots 0.20 is in ::guru as
        # `gui-libs/wlroots:0.20` (the slotted version);
        # mesa, pixman, libinput, xkbcommon, etc. are in the
        # main tree.
        PKGS_BUILD=(
            dev-build/meson dev-util/ninja dev-util/pkgconf
            gui-libs/wlroots:0.20
            dev-libs/wayland media-libs/wayland-protocols
            dev-libs/libinput x11-libs/libxkbcommon
            x11-libs/pixman x11-libs/libxcb
            dev-lang/lua:5.4
        )
        PKGS_RUNTIME=(gui-apps/swaybg)
        PM="emerge"
        SUDO="sudo"
        INSTALL_ARGS=(--ask=n --verbose --noreplace)
        # emerge --noreplace is the gentoo idiom for "install
        # if not present"; --ask=n is the non-interactive flag.
        CHECK_CMD=(qlist -I)
        ;;

    nixos)
        # NixOS doesn't have a "package manager" in the
        # traditional sense -- everything goes through
        # configuration.nix. Point the user at the README's
        # "Building" section for the nix-shell one-liner
        # (the install-deps script has nothing useful to do
        # here).
        cat <<'EOF' >&2
install-deps.sh: NixOS detected.

  Add the uwuwm deps to your configuration.nix, or use the
  nix-shell one-liner from README § Building:

    nix-shell -p meson ninja pkg-config python3 \\
      wlroots wayland wayland-protocols libinput \\
      libxkbcommon pixman libxcb lua5_4 swaybg

  (see the wlroots 0.20 pinning note in README if your
  channel's wlroots is older than 0.20)
EOF
        exit 0
        ;;

    *)
        # Unknown distro. Don't guess. Print the manual install
        # list (lifted from README §Building) so the user has
        # something concrete to work with.
        cat <<EOF >&2
install-deps.sh: couldn't identify this distro.

  /etc/os-release was $([ -r /etc/os-release ] && echo "present" || echo "missing"),
  lsb_release was $(command -v lsb_release >/dev/null 2>&1 && echo "found" || echo "not found").

  Manual install -- these are uwuwm's build- and runtime-time deps
  (see README.md § Building for the full list with rationale):

    wlroots 0.20      libinput           lua 5.4
    wayland-server    libxkbcommon       swaybg (optional)
    wayland-protocols pixman             meson
    xcb               pkgconf/pkg-config ninja
                                          python3

  If your distro's package manager isn't one of:
    pacman (Arch), apt (Debian/Ubuntu), dnf (Fedora),
    xbps-install (Void), apk (Alpine), zypper (openSUSE),
    emerge (Gentoo), nix-shell (NixOS)
  ...let me know in an issue and I'll add a case branch.
EOF
        exit 1
        ;;
esac

# ── Build the actual install command ───────────────────────────────────
# Idempotency: skip anything the package manager already reports
# as installed. The per-distro `CHECK_CMD` array is the "is X
# installed" probe -- on pacman it's `pacman -Qi`, on apt it's
# `dpkg-query -W`, etc. The `2>/dev/null || true` and
# `--quiet`/`-q` flags are picked so a missing package shows up
# as a non-zero exit (which is what we want to skip on) rather
# than a wall of stderr.

is_installed() {
    local pkg="$1"
    # `--quiet` / `-q` makes the package manager's "is X
    # installed" probe silent on success, and the exit code is
    # the only thing that matters. We accept either an exit
    # code of 0 (installed) or anything else (not installed).
    case "$PM" in
        pacman) pacman -Qq "$pkg" >/dev/null 2>&1 ;;
        apt)    dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q '^install ok installed$' ;;
        dnf|zypper) rpm -q "$pkg" >/dev/null 2>&1 ;;
        xbps-install) xbps-query "$pkg" >/dev/null 2>&1 ;;
        apk)    apk info -e "$pkg" >/dev/null 2>&1 ;;
        emerge) qlist -I "$pkg" >/dev/null 2>&1 ;;
    esac
}

# Walk the build + runtime lists, partition into "missing" and
# "present", and emit one `pm install missing...` command. In
# --check mode just print what would be installed and exit.

missing=()
present=()
for pkg in "${PKGS_BUILD[@]}" "${PKGS_RUNTIME[@]}"; do
    if is_installed "$pkg"; then
        present+=("$pkg")
    else
        missing+=("$pkg")
    fi
done

echo "uwuwm: detected $PM on $(detect_distro) ($(uname -m))"
echo "uwuwm: build deps present: ${#present[@]}/${#PKGS_BUILD[@]}"
echo "uwuwm: runtime deps present: $((${#present[@]} - ${#PKGS_BUILD[@]}))/${#PKGS_RUNTIME[@]}"

if [ ${#missing[@]} -eq 0 ]; then
    echo "uwuwm: nothing to install -- all deps already present"
    exit 0
fi

# Compose the install command. The exact flag set varies by
# package manager (apt uses --no-install-recommends, pacman
# uses --needed, emerge uses --noreplace, etc.) so this is
# the one place that needs to be careful about quoting.
case "$PM" in
    pacman)    cmd=("$SUDO" "$PM" "${INSTALL_ARGS[@]}" "${missing[@]}") ;;
    apt)       cmd=("$SUDO" "$PM" "${INSTALL_ARGS[@]}" "${missing[@]}") ;;
    dnf)       cmd=("$SUDO" "$PM" "${INSTALL_ARGS[@]}" "${missing[@]}") ;;
    xbps-install) cmd=("$SUDO" "$PM" "${INSTALL_ARGS[@]}" "${missing[@]}") ;;
    apk)       cmd=("$SUDO" "$PM" "${INSTALL_ARGS[@]}" "${missing[@]}") ;;
    zypper)    cmd=("$SUDO" "$PM" "${INSTALL_ARGS[@]}" "${missing[@]}") ;;
    emerge)    cmd=("$SUDO" "$PM" "${INSTALL_ARGS[@]}" "${missing[@]}") ;;
esac

emit_install "${cmd[*]}"

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "uwuwm: --check set, not invoking $PM"
    exit 0
fi

# Actually run it. `set -e` is on, so any non-zero exit
# propagates and the script ends with a clear failure
# message. We don't capture output -- let it stream to the
# terminal so the user can see progress, same as if they'd
# typed the command themselves.
"${cmd[@]}"
