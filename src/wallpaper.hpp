#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
}

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Server;
struct Output;

// Native, compositor-drawn wallpapers -- the image-backed counterpart to
// RuntimeConfig::background_color's plain wlr_scene_rect. There's no
// external process involved anywhere in this file: an image is decoded
// once (stb_image, vendored under src/vendor/ -- see WallpaperImage
// below), wrapped in a minimal custom wlr_buffer (WallpaperImageBuffer,
// in wallpaper.cpp), and handed to wlr_scene_buffer_create() on the
// exact same background layer Output::background_rect already sits on.
// `uwu.wallpaper` (lua_config.cpp) is the only way to reach any of this
// from rc.lua -- nyaa.wallpaper (lib/nyaa/wallpaper.lua) is sugar over
// that, the same relationship every other nyaa.* function has to its
// underlying uwu.* call.

enum class WallpaperMode {
    Fill,     // crop to the output's aspect ratio, then scale to cover it
              // exactly -- no letterboxing, no overflow past output bounds.
    Fit,      // scale to fit entirely inside the output, no cropping --
              // background_color shows through any letterbox/pillarbox gap.
    Stretch,  // scale to the output's exact size, ignoring aspect ratio.
    Center,   // no scaling; centered at native resolution, cropped to the
              // output's bounds if the image is larger than the output.
    Tile,     // repeated at native resolution from the output's origin;
              // edge tiles are cropped, never overflow past output bounds.
};

// One uwu.wallpaper.set(name, {...}) rule. `name` matches wlr_output->name
// exactly, or is the literal wildcard "*" -- same precedence contract as
// MonitorRule/findMonitorRule: exact name wins, "*" is the fallback
// applied to any output without its own rule. See findWallpaperRule.
struct WallpaperRule {
    std::string name;
    std::string path;  // as given to uwu.wallpaper.set -- ~ already
                       // expanded by lua_config.cpp before this is built.
    WallpaperMode mode = WallpaperMode::Fill;
};

// A decoded image, cached by absolute path (see wallpaper.cpp's
// decodeCache) so setting the same wallpaper on several outputs -- or
// tiling one across a single output -- decodes the file exactly once and
// shares the pixel data via shared_ptr, not once per consumer. RGBA8,
// top-to-bottom row order, 4-byte-aligned rows (stb_image's own default
// packing) -- see wallpaper.cpp's WallpaperImageBuffer for how this
// becomes an actual wlr_buffer the scene graph can render.
struct WallpaperImage {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> pixels;  // width * height * 4 bytes, RGBA8
};

// Looks up the wallpaper rule that applies to an output named `name`:
// exact match wins, otherwise the last "*" rule (if any). Returns nullptr
// if rc.lua never called uwu.wallpaper.set for this output or "*" --
// callers should leave background_color as the only thing visible in
// that case. Mirrors findMonitorRule's shape exactly (output.cpp).
const WallpaperRule* findWallpaperRule(Server& server, const char* name);

// Loads (or reuses a cached decode of) the image at `path`. Returns
// nullptr -- logging exactly one WLR_ERROR line -- if the file doesn't
// exist or stb_image can't decode it; never throws, so a bad path from
// rc.lua degrades to "no wallpaper" instead of taking the compositor
// down. The cache is keyed on the literal path string given, so editing
// a wallpaper file in place and re-running uwu.wallpaper.set with the
// same path won't pick up the change until uwu.wallpaper.forget(path) or
// a full uwu.reload() clears the cache -- see wallpaper.cpp.
std::shared_ptr<WallpaperImage> loadWallpaperImage(const std::string& path);

// Drops `path` from the decode cache, if present. Called by
// uwu.wallpaper.set() itself right before re-decoding (so re-setting the
// same path always reflects the file's current contents on disk, not a
// stale cache entry) -- exposed here mainly so tests/tools can force a
// re-decode without going through Lua.
void forgetWallpaperImage(const std::string& path);

// (Re)builds `out`'s wallpaper scene node(s) from whatever rule
// findWallpaperRule() currently resolves for it -- one node for
// fill/fit/stretch/center, several (a full grid) for Tile. Destroys any
// previously-built nodes first, so this is always safe to call again:
// after a resize/transform change (Output::updateLayoutBox), after
// uwu.wallpaper.set()/clear() touches a rule that could affect this
// output, and once from the constructor for a newly-connected monitor.
// If no rule applies, this just clears the wallpaper nodes and leaves
// background_rect as the only thing visible -- exactly the pre-wallpaper
// behavior.
void applyWallpaper(Output& out);

// Destroys `out`'s wallpaper scene node(s) without consulting any rule.
// Called by applyWallpaper() itself before rebuilding, and by
// Output::~Output() so a wallpaper node never outlives the tree it's
// parented under going away mid-teardown.
void clearWallpaperNodes(Output& out);
