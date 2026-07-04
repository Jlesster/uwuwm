#include "server.hpp"

#include <cstdio>
#include <cstring>

namespace {

void printUsage(const char *argv0) {
	std::fprintf(stderr,
		"Usage: %s [-h]\n"
		"\n"
		"uwuwm -- a minimal tiling Wayland compositor on wlroots.\n"
		"\n"
		"Run nested inside an existing Wayland or X11 session for development\n"
		"(set WAYLAND_DISPLAY or DISPLAY before launching), or from a TTY for\n"
		"a real DRM/KMS session.\n"
		"\n"
		"Configuration is compile-time: edit config.hpp and rebuild.\n"
		"Autostart: ~/.config/uwuwm/autostart.sh, if present and executable.\n",
		argv0);
}

} // namespace

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
			printUsage(argv[0]);
			return 0;
		}
	}

	Server server;
	return server.run(argc, argv);
}
