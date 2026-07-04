#!/usr/bin/env python3
"""Copy wlroots' render/color.h and types/wlr_scene.h into a build-local
include dir with C99 `[static N]` array-parameter syntax stripped, since
Clang rejects that syntax in C++ mode. Mirrors CMakeLists.txt's
patch_wlroots_header() function.

Searches every candidate include dir (the real wlroots -I paths from
pkg-config Cflags) for each header, since which one is "the" wlroots root
varies by distro (e.g. Arch's versioned /usr/include/wlroots0.20/).
"""
import re
import sys
from pathlib import Path

HEADERS = [
    "wlr/render/color.h",
    "wlr/types/wlr_scene.h",
]


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <output-dir> <include-dir>...", file=sys.stderr)
        return 1

    output_dir = Path(sys.argv[1])
    include_dirs = [Path(p) for p in sys.argv[2:]]

    for rel in HEADERS:
        src = next((d / rel for d in include_dirs if (d / rel).is_file()), None)
        if src is None:
            searched = "\n  ".join(str(d) for d in include_dirs)
            print(f"error: {rel} not found in any of:\n  {searched}", file=sys.stderr)
            return 1

        dst = output_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        content = src.read_text()
        patched = re.sub(r"\[static ", "[", content)
        dst.write_text(patched)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
