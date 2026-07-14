#pragma once

#include <cstdint>
#include <string>

// HarfBuzz shaping + FreeType rasterization for uwu.bar.*'s :text()
// primitive (bar.hpp/bar.cpp) -- the two pieces a single-line status-bar
// label actually needs, without pulling in cairo/pango for full 2D
// vector graphics + text layout on top. Deliberately opaque: nothing in
// this header names a FreeType or HarfBuzz type, so bar.cpp and
// lua_config.cpp (both of which need to call this) don't need either
// library's headers in scope -- only text.cpp does.
//
// One TextRenderer instance lives on Server (see server.hpp) and is
// shared by every Bar; the (font path, pixel size) -> loaded face cache
// and the (face, size, glyph id) -> rasterized bitmap cache inside it
// are keyed globally for that reason, not per-bar.
class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    TextRenderer(const TextRenderer&)            = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Shapes `utf8` at `font_path`/`pixel_size` (loading and caching that
    // face the first time it's seen) and blits it into `pixels` -- RGBA8,
    // `stride` bytes/row, `buf_w`x`buf_h` total -- with its baseline at
    // (x, y). `color` is 0xRRGGBBAA; each glyph's FreeType coverage
    // (antialiased 8-bit grayscale) is used as a per-pixel alpha
    // multiplier when compositing over whatever's already in `pixels`,
    // same straight-alpha-over blend bar.cpp's fillRect uses. Glyphs (or
    // the whole call) that land outside buf_w/buf_h are clipped, not an
    // error. Returns the total horizontal advance in pixels -- 0 if
    // `font_path` couldn't be loaded (logged once per bad path, not
    // once per call) -- so a caller can chain text after this or
    // right/center-align by calling measureText() first.
    int drawText(uint8_t*           pixels,
                 int                stride,
                 int                buf_w,
                 int                buf_h,
                 int                x,
                 int                y,
                 const std::string& utf8,
                 const std::string& font_path,
                 int                pixel_size,
                 uint32_t           color);

    // Same shaping pass as drawText, without rasterizing or blitting
    // anything -- just the total horizontal advance, for alignment math
    // (center/right-anchoring a label before committing to an x).
    int measureText(const std::string& utf8,
                    const std::string& font_path,
                    int                pixel_size);

    // Recommended line height (ascender - descender + linegap, in
    // pixels) for `font_path` at `pixel_size` -- lets a widget vertically
    // center a label in a bar of known height without a hand-picked
    // fudge factor. 0 if the font couldn't be loaded.
    int lineHeight(const std::string& font_path, int pixel_size);

private:
    struct Impl;
    Impl* impl_;
};
