#include "text.hpp"

extern "C" {
#include <wlr/util/log.h>
}

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

#include <cstring>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// A rasterized glyph, copied out of FT_GlyphSlot's bitmap into a tightly
// packed (no pitch padding) 8-bit coverage buffer, cached forever keyed
// by (face, size, glyph index) -- a status bar redraws the same handful
// of glyphs (digits, a few dozen letters) over and over, most commonly
// every second off a clock timer, so re-rasterizing on every commit()
// would be pure waste. Assumes FT_LOAD_RENDER's default antialiased
// FT_PIXEL_MODE_GRAY output (8-bit coverage, non-negative pitch) --
// uwuwm never asks FreeType for mono/LCD rendering, so this doesn't
// handle those.
// ---------------------------------------------------------------------------
struct CachedGlyph {
    int                  width = 0, rows = 0;
    int                  left = 0, top = 0;  // offset from pen to bitmap origin
    std::vector<uint8_t> coverage;           // width*rows bytes, row-major
};

struct FaceEntry {
    FT_Face    face    = nullptr;
    hb_font_t* hb_font = nullptr;
    bool       valid   = false;
};

struct GlyphKey {
    std::string  face_key;
    unsigned int glyph_id;
    bool operator==(const GlyphKey& o) const {
        return face_key == o.face_key && glyph_id == o.glyph_id;
    }
};
struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const {
        return std::hash<std::string>()(k.face_key) ^
               (std::hash<unsigned int>()(k.glyph_id) << 1);
    }
};

struct TextRenderer::Impl {
    FT_Library ft     = nullptr;
    bool       ft_ok  = false;

    // Keyed on "path:pixel_size" -- a distinct FT_Face per size actually
    // requested, never resized in place, so hb_font (bound to this exact
    // face via hb_ft_font_create) never goes stale.
    std::unordered_map<std::string, FaceEntry>                faces;
    std::unordered_map<GlyphKey, CachedGlyph, GlyphKeyHash> glyph_cache;

    static std::string faceKey(const std::string& path, int pixel_size) {
        return path + ":" + std::to_string(pixel_size);
    }

    FaceEntry* getFace(const std::string& path, int pixel_size) {
        if(!ft_ok) { return nullptr; }
        std::string key = faceKey(path, pixel_size);
        if(auto it = faces.find(key); it != faces.end()) {
            return it->second.valid ? &it->second : nullptr;
        }

        FaceEntry entry;
        if(FT_New_Face(ft, path.c_str(), 0, &entry.face)) {
            wlr_log(WLR_ERROR, "uwu.bar: couldn't load font '%s'", path.c_str());
            faces.emplace(key, entry);  // cache the failure too -- one log
                                        // line per bad path, not one per
                                        // redraw.
            return nullptr;
        }
        FT_Set_Pixel_Sizes(entry.face, 0, static_cast<FT_UInt>(pixel_size));
        entry.hb_font = hb_ft_font_create(entry.face, nullptr);
        entry.valid   = true;
        return &faces.emplace(key, entry).first->second;
    }

    const CachedGlyph*
    getGlyph(const std::string& face_key, FT_Face face, unsigned int glyph_id) {
        GlyphKey key{face_key, glyph_id};
        if(auto it = glyph_cache.find(key); it != glyph_cache.end()) {
            return &it->second;
        }
        if(FT_Load_Glyph(face, glyph_id, FT_LOAD_RENDER)) { return nullptr; }

        FT_GlyphSlot slot = face->glyph;
        CachedGlyph  cg;
        cg.width = static_cast<int>(slot->bitmap.width);
        cg.rows  = static_cast<int>(slot->bitmap.rows);
        cg.left  = slot->bitmap_left;
        cg.top   = slot->bitmap_top;
        cg.coverage.resize(static_cast<size_t>(cg.width) * cg.rows);
        for(int row = 0; row < cg.rows; row++) {
            std::memcpy(cg.coverage.data() + static_cast<size_t>(row) * cg.width,
                        slot->bitmap.buffer +
                            static_cast<size_t>(row) * slot->bitmap.pitch,
                        static_cast<size_t>(cg.width));
        }
        return &glyph_cache.emplace(key, std::move(cg)).first->second;
    }

    ~Impl() {
        for(auto& [key, entry] : faces) {
            if(entry.hb_font) { hb_font_destroy(entry.hb_font); }
            if(entry.face) { FT_Done_Face(entry.face); }
        }
        if(ft_ok) { FT_Done_FreeType(ft); }
    }
};

TextRenderer::TextRenderer() : impl_(new Impl()) {
    impl_->ft_ok = (FT_Init_FreeType(&impl_->ft) == 0);
    if(!impl_->ft_ok) {
        wlr_log(WLR_ERROR,
                "uwu.bar: FT_Init_FreeType failed -- text rendering disabled "
                "for this session");
    }
}

TextRenderer::~TextRenderer() { delete impl_; }

namespace {
// bars are drawn opaque (see Bar::clear()/fillRect() in bar.cpp) -- text
// blends its color into whatever RGB is already there using glyph
// coverage * color alpha as the blend factor, and always leaves the
// destination alpha at 255 rather than doing full Porter-Duff "over"
// compositing on the alpha channel too. A translucent whole-bar (glass
// panel over the desktop) isn't a v1 goal; every real status bar this
// was modeled on (waybar, polybar) is opaque in practice anyway.
inline void blendPixel(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, float a) {
    dst[0] = static_cast<uint8_t>(dst[0] * (1.0f - a) + r * a);
    dst[1] = static_cast<uint8_t>(dst[1] * (1.0f - a) + g * a);
    dst[2] = static_cast<uint8_t>(dst[2] * (1.0f - a) + b * a);
    dst[3] = 255;
}
}  // namespace

int TextRenderer::drawText(uint8_t*           pixels,
                           int                stride,
                           int                buf_w,
                           int                buf_h,
                           int                x,
                           int                y,
                           const std::string& utf8,
                           const std::string& font_path,
                           int                pixel_size,
                           uint32_t           color) {
    FaceEntry* fe = impl_->getFace(font_path, pixel_size);
    if(!fe) { return 0; }

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, utf8.c_str(), -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(fe->hb_font, buf, nullptr, 0);

    unsigned int          glyph_count = 0;
    hb_glyph_info_t*      infos       = hb_buffer_get_glyph_infos(buf, &glyph_count);
    hb_glyph_position_t*  positions =
        hb_buffer_get_glyph_positions(buf, &glyph_count);

    std::string face_key = Impl::faceKey(font_path, pixel_size);

    uint8_t cr = static_cast<uint8_t>((color >> 24) & 0xff);
    uint8_t cg = static_cast<uint8_t>((color >> 16) & 0xff);
    uint8_t cb = static_cast<uint8_t>((color >> 8) & 0xff);
    uint8_t ca = static_cast<uint8_t>(color & 0xff);

    double pen_x = 0, pen_y = 0;
    for(unsigned int i = 0; i < glyph_count; i++) {
        const CachedGlyph* g =
            impl_->getGlyph(face_key, fe->face, infos[i].codepoint);
        if(g && g->width > 0 && g->rows > 0) {
            int gx = x +
                     static_cast<int>(pen_x + positions[i].x_offset / 64.0) +
                     g->left;
            int gy = y -
                     static_cast<int>(pen_y + positions[i].y_offset / 64.0) -
                     g->top;
            for(int row = 0; row < g->rows; row++) {
                int py = gy + row;
                if(py < 0 || py >= buf_h) { continue; }
                for(int col = 0; col < g->width; col++) {
                    int px = gx + col;
                    if(px < 0 || px >= buf_w) { continue; }
                    uint8_t coverage = g->coverage[static_cast<size_t>(row) *
                                                        g->width +
                                                    col];
                    if(coverage == 0) { continue; }
                    float a = (coverage / 255.0f) * (ca / 255.0f);
                    blendPixel(pixels + py * stride + px * 4, cr, cg, cb, a);
                }
            }
        }
        pen_x += positions[i].x_advance / 64.0;
        pen_y += positions[i].y_advance / 64.0;
    }

    hb_buffer_destroy(buf);
    return static_cast<int>(pen_x + 0.5);
}

int TextRenderer::measureText(const std::string& utf8,
                              const std::string& font_path,
                              int                pixel_size) {
    FaceEntry* fe = impl_->getFace(font_path, pixel_size);
    if(!fe) { return 0; }

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, utf8.c_str(), -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(fe->hb_font, buf, nullptr, 0);

    unsigned int         glyph_count = 0;
    hb_glyph_position_t* positions =
        hb_buffer_get_glyph_positions(buf, &glyph_count);

    double advance = 0;
    for(unsigned int i = 0; i < glyph_count; i++) {
        advance += positions[i].x_advance / 64.0;
    }
    hb_buffer_destroy(buf);
    return static_cast<int>(advance + 0.5);
}

int TextRenderer::lineHeight(const std::string& font_path, int pixel_size) {
    FaceEntry* fe = impl_->getFace(font_path, pixel_size);
    if(!fe) { return 0; }
    // face->size->metrics.height is 26.6 fixed-point pixels once
    // FT_Set_Pixel_Sizes has been called (getFace always does, on load).
    return static_cast<int>(fe->face->size->metrics.height >> 6);
}
