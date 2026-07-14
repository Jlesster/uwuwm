# Top-level Makefile for uwuwm.
#
# Wraps the meson/ninja build and handles the install step manually so
# we can chown /usr/share/uwuwm/ to the invoking user. `meson install`
# would write files owned by the user running it, which is useless when
# the prefix is /usr (you'd need sudo, and the result is root-owned).
# A Makefile can run `sudo` once, do the file copies, and then chown
# the share tree to $SUDO_USER -- meson has no equivalent knob.
#
# Targets:
#   make            - configure (meson setup) and build (ninja)
#   make build      - same as `make`
#   make deps       - install uwuwm's build- and runtime-time deps via
#                     scripts/install-deps.sh (one shot across Arch,
#                     Debian/Ubuntu, Fedora, Void, Alpine, openSUSE,
#                     Gentoo; NixOS gets a nix-shell hint instead)
#   make clean      - ninja clean in builddir/
#   make install    - build, then `sudo` the install + chown
#   make -n install - print the install commands without running them

BUILD_DIR    := builddir
PREFIX       := /usr
BINDIR       := $(PREFIX)/bin
SHARE_DIR    := $(PREFIX)/share/uwuwm
SESSIONS_DIR := $(PREFIX)/share/wayland-sessions

# Directories to create on install. The four lib/ subdirs are
# explicit (not implicit) so the install doesn't depend on any of
# them already existing on the target system.
INSTALL_DIRS := \
    $(BINDIR) \
    $(SHARE_DIR) \
    $(SHARE_DIR)/lib/meta \
    $(SHARE_DIR)/lib/nyaa \
    $(SHARE_DIR)/lib/paw \
    $(SESSIONS_DIR)

# Files to install. The `install.list` file at the repo root is a
# plain text manifest with one (src dst mode) triple per line. The
# install target reads it inside the privileged shell via stdin
# redirection and iterates with `while read`. This keeps make's
# expansion simple (no `$(call)` gymnastics -- make's $(call) does
# not re-split its argument on whitespace, so any triple-packed
# form silently passes the whole triple as $(1) and $(2)/$(3)
# come out empty) and lets the shell do the per-line token split.
# `install.list` is checked in alongside the Makefile -- it's the
# source of truth for the install layout, in the same way the old
# `install_data`/`install_subdir` calls were in meson.build.
INSTALL_LIST := install.list

.PHONY: all build deps clean install

all: build

build:
	meson setup --reconfigure $(BUILD_DIR)
	ninja -C $(BUILD_DIR)

# install-deps.sh uses sudo internally; if sudo isn't on PATH the
# script will tell the user, but we still gate the target here so
# `make deps` on a system without sudo (running as root already,
# e.g. inside a container) doesn't blow up with a confusing
# "command not found" instead of skipping sudo. The script
# itself probes for sudo and falls back to running unprivileged
# for distros where deps can be installed without it (Alpine in
# some configs, NixOS via nix-shell) -- see the script body.
deps:
	@command -v sudo >/dev/null 2>&1 || { \
		echo "uwuwm: 'sudo' is required for 'make deps' (the install needs root for most distros)" >&2; \
		echo "uwuwm: install sudo, or run scripts/install-deps.sh as root yourself" >&2; \
		exit 1; \
	}
	./scripts/install-deps.sh

clean:
	ninja -C $(BUILD_DIR) -t clean

# `install` runs the build first so a stale binary never gets shipped.
# The whole install body is wrapped in a single `sudo bash -c` so the
# user gets one password prompt and `set -e` covers every step.
#
# Why sudo and not "must be root": because the chown line below needs
# $SUDO_USER, which sudo sets itself. When the user is already root,
# `sudo -E` is a no-op, $SUDO_USER is empty, and we skip the chown
# (root already owns the whole tree -- correct outcome).
#
# The install list is piped into the privileged shell via stdin
# redirection (`< $(INSTALL_LIST)` on the rule line) and consumed
# by a `while read` loop. This sidesteps both the make `$(call)`
# problem (no arg-splitting across whitespace) and the
# recipe-parsing problem of inlining a multi-line here-doc body
# in a make recipe.
install: build $(INSTALL_LIST)
	@command -v sudo >/dev/null 2>&1 || { \
		echo "uwuwm: 'sudo' is required for 'make install' (to chown /usr/share/uwuwm to $$USER)" >&2; \
		echo "uwuwm: install sudo, or run 'sudo make install' yourself" >&2; \
		exit 1; \
	}
	sudo -E bash -c 'set -e; \
		$$([ -n "$$SUDO_USER" ] && OWNER="$$SUDO_USER" || OWNER=""); \
		echo "uwuwm: installing to $(PREFIX) (share tree owned by $${OWNER:-root})"; \
		install -d $(INSTALL_DIRS); \
		while IFS=" " read -r src dst mode; do \
			[ -z "$$src" ] && continue; \
			case "$$src" in \#*) continue ;; esac; \
			install -D -m "$$mode" "$$src" "$$dst"; \
		done; \
		if [ -n "$$OWNER" ]; then \
			chown -R "$$OWNER" $(SHARE_DIR); \
			echo "uwuwm: chowned $(SHARE_DIR) to $$OWNER"; \
		fi' < $(INSTALL_LIST)
