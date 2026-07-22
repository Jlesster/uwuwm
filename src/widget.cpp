#include "widget.hpp"

#include "layershell.hpp"
#include "output.hpp"
#include "server.hpp"

extern "C" {
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
}

#include <unistd.h>

#include <algorithm>

namespace {

// Same recipe as bar.cpp's BarPixelBuffer/wrapBarPixels -- duplicated
// rather than shared (each holds its own owned copy since the source
// keeps getting mutated by the next commit()'s repaint as soon as this
// one returns).
struct WidgetPixelBuffer {
    wlr_buffer           base;
    std::vector<uint8_t> pixels;
    int                  width, height;
};

void widgetBufferDestroy(wlr_buffer* buffer) {
    WidgetPixelBuffer* self = wl_container_of(buffer, self, base);
    delete self;
}

bool widgetBufferBeginDataPtrAccess(wlr_buffer* buffer,
                                    uint32_t /*flags*/,
                                    void**    data,
                                    uint32_t* format,
                                    size_t*   stride) {
    WidgetPixelBuffer* self = wl_container_of(buffer, self, base);
    *data                   = self->pixels.data();
    *format                 = DRM_FORMAT_ABGR8888;
    *stride                 = static_cast<size_t>(self->width) * 4;
    return true;
}

void widgetBufferEndDataPtrAccess(wlr_buffer* /*buffer*/) {}

const wlr_buffer_impl kWidgetBufferImpl = {
    .destroy               = widgetBufferDestroy,
    .get_dmabuf            = nullptr,
    .get_shm               = nullptr,
    .begin_data_ptr_access = widgetBufferBeginDataPtrAccess,
    .end_data_ptr_access   = widgetBufferEndDataPtrAccess,
};

wlr_buffer*
wrapWidgetPixels(std::vector<uint8_t> pixels, int width, int height) {
    auto* buf   = new WidgetPixelBuffer{};
    buf->pixels = std::move(pixels);
    buf->width  = width;
    buf->height = height;
    wlr_buffer_init(&buf->base, &kWidgetBufferImpl, width, height);
    return &buf->base;
}

inline void blendPixel(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, float a) {
    dst[0] = static_cast<uint8_t>(dst[0] * (1.0f - a) + r * a);
    dst[1] = static_cast<uint8_t>(dst[1] * (1.0f - a) + g * a);
    dst[2] = static_cast<uint8_t>(dst[2] * (1.0f - a) + b * a);
    dst[3] = 255;
}

// text() widgets take no font_path -- TextRenderer needs a real file
// path (no fontconfig fallback of its own), so this resolves a
// reasonable system default once and caches it. Covers essentially
// every distro, including a minimal Raspberry Pi OS image (ships
// DejaVu by default). If none exist, TextRenderer::drawText's own
// "log once, draw nothing" leniency applies.
const std::string& defaultFontPath() {
    static const std::string path = [] {
        static const char* kCandidates[] = {
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular."
            "ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular."
            "ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        };
        for(const char* candidate : kCandidates) {
            if(access(candidate, R_OK) == 0) { return std::string(candidate); }
        }
        return std::string();
    }();
    return path;
}

// ---------------------------------------------------------------------
// Anchor solver -- see widget.hpp's class comment for the geometry
// model. Runs once per commit(), fresh: no state is cached between
// calls, so a widget whose anchor target moved (or whose anchor
// changed since the last commit()) always sees the current layout.
// ---------------------------------------------------------------------

enum class VisitState : uint8_t { Unvisited, InProgress, Done };

struct Box {
    int x, y, w, h;
};

int edgeCoord(const Box& b, WidgetWindow::AnchorEdge edge) {
    switch(edge) {
        case WidgetWindow::AnchorEdge::Left:
            return b.x;
        case WidgetWindow::AnchorEdge::Right:
            return b.x + b.w;
        case WidgetWindow::AnchorEdge::Top:
            return b.y;
        case WidgetWindow::AnchorEdge::Bottom:
            return b.y + b.h;
        case WidgetWindow::AnchorEdge::HCenter:
            return b.x + b.w / 2;
        case WidgetWindow::AnchorEdge::VCenter:
            return b.y + b.h / 2;
        default:
            return 0;
    }
}

// Resolves (and returns) widget `id`'s box. id == 0 (or out of range)
// means the window root -- (0, 0, win.width, win.height) -- which is
// always already "resolved", never recurses, and is therefore always
// a safe base case no matter how a bad parent_id/target_id got there.
//
// Cycle handling: a widget midway through resolution is marked
// InProgress; hitting an InProgress widget again means its own
// resolution transitively depends on itself. Rather than recurse
// forever, that widget is reported at whatever resolved_x/resolved_y
// it last had (0,0 the very first layout) and the cycle is logged
// once -- "safely wrong once" beats a compositor-hanging infinite
// recursion for a mistake that's already visible on screen (the
// widget just won't move) and easy to fix in rc.lua once logged.
Box resolveBox(WidgetWindow& win, int id, std::vector<VisitState>& state) {
    if(id <= 0 || static_cast<size_t>(id) > win.widgets.size()) {
        return Box{0, 0, win.width, win.height};
    }
    size_t idx = static_cast<size_t>(id) - 1;

    if(state[idx] == VisitState::Done) {
        auto& w = win.widgets[idx];
        return Box{w.resolved_x, w.resolved_y, w.w, w.h};
    }
    if(state[idx] == VisitState::InProgress) {
        static bool warned = false;
        if(!warned) {
            wlr_log(WLR_ERROR,
                    "uwuwm: uwu.widget anchor cycle detected (widget id "
                    "%d) -- layout for it left unresolved this commit()",
                    id);
            warned = true;
        }
        auto& w = win.widgets[idx];
        return Box{w.resolved_x, w.resolved_y, w.w, w.h};
    }

    state[idx]                  = VisitState::InProgress;
    WidgetWindow::WidgetNode& w = win.widgets[idx];

    Box parent_box = resolveBox(win, w.parent_id, state);

    if(w.fill) {
        Box target = resolveBox(
            win, w.fill_target_id ? w.fill_target_id : w.parent_id, state);
        w.resolved_x = target.x + w.fill_margin;
        w.resolved_y = target.y + w.fill_margin;
        w.w          = std::max(0, target.w - 2 * w.fill_margin);
        w.h          = std::max(0, target.h - 2 * w.fill_margin);
    } else {
        int rx = parent_box.x + w.x;
        int ry = parent_box.y + w.y;

        using Edge = WidgetWindow::AnchorEdge;
        if(w.anchor_left.edge != Edge::None) {
            Box t = resolveBox(win, w.anchor_left.target_id, state);
            rx    = edgeCoord(t, w.anchor_left.edge) + w.anchor_left.margin;
        } else if(w.anchor_right.edge != Edge::None) {
            Box t = resolveBox(win, w.anchor_right.target_id, state);
            // Inset convention: a positive rightMargin/bottomMargin
            // moves the widget *inward* from the target's right/bottom
            // edge, mirroring how a positive left/topMargin already
            // moves inward from the left/top edge -- this is what
            // every anchor-based UI toolkit (QML included) means by
            // "margin", and the one this class got backwards in an
            // earlier draft: adding margin here pushed anchored
            // widgets outward past the target's edge instead, which is
            // how a clock text anchored to a bar's right edge ended up
            // rendered entirely off the right side of the bar buffer
            // (silently clipped by TextRenderer::drawText, not
            // crashing -- just invisible).
            rx =
                edgeCoord(t, w.anchor_right.edge) - w.anchor_right.margin - w.w;
        } else if(w.anchor_hcenter.edge != Edge::None) {
            Box t = resolveBox(win, w.anchor_hcenter.target_id, state);
            rx = edgeCoord(t, w.anchor_hcenter.edge) + w.anchor_hcenter.margin -
                 w.w / 2;
        }

        if(w.anchor_top.edge != Edge::None) {
            Box t = resolveBox(win, w.anchor_top.target_id, state);
            ry    = edgeCoord(t, w.anchor_top.edge) + w.anchor_top.margin;
        } else if(w.anchor_bottom.edge != Edge::None) {
            Box t = resolveBox(win, w.anchor_bottom.target_id, state);
            // Same inset fix as anchor_right above.
            ry = edgeCoord(t, w.anchor_bottom.edge) - w.anchor_bottom.margin -
                 w.h;
        } else if(w.anchor_vcenter.edge != Edge::None) {
            Box t = resolveBox(win, w.anchor_vcenter.target_id, state);
            ry = edgeCoord(t, w.anchor_vcenter.edge) + w.anchor_vcenter.margin -
                 w.h / 2;
        }

        w.resolved_x = rx;
        w.resolved_y = ry;
    }

    state[idx] = VisitState::Done;
    return Box{w.resolved_x, w.resolved_y, w.w, w.h};
}

}  // namespace

WidgetWindow::WidgetWindow(Server&     server_,
                           Output&     output_,
                           BarPosition position,
                           int         height_)
    : mode(Mode::Bar), server(server_), output(&output_),
      bar_position(position) {
    height = height_;
    reposition();
    commit();
}

WidgetWindow::WidgetWindow(Server&     server_,
                           Output&     output_,
                           PopupAnchor anchor,
                           int         margin,
                           int         width_,
                           int         height_)
    : mode(Mode::Popup), server(server_), output(&output_),
      popup_anchor(anchor), popup_margin(margin) {
    width   = width_;
    height  = height_;
    visible = false;  // popups start hidden -- see class comment
    reposition();
    commit();
}

WidgetWindow::~WidgetWindow() {
    if(scene_node) { wlr_scene_node_destroy(&scene_node->node); }
    if(output) {
        auto& v = output->widget_windows;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }
}

void WidgetWindow::detachFromOutput() {
    if(scene_node) {
        wlr_scene_node_destroy(&scene_node->node);
        scene_node = nullptr;
    }
    output = nullptr;
}

void WidgetWindow::show() {
    visible = true;
    if(scene_node) { wlr_scene_node_set_enabled(&scene_node->node, true); }
}

void WidgetWindow::hide() {
    visible = false;
    if(scene_node) { wlr_scene_node_set_enabled(&scene_node->node, false); }
}

void WidgetWindow::reposition() {
    if(!output) { return; }

    if(mode == Mode::Bar) { width = output->layout_box.width; }
    if(width <= 0 || height <= 0) { return; }

    pixels.assign(static_cast<size_t>(width) * height * 4, 0);

    int layer = (mode == Mode::Bar) ? ZWLR_LAYER_SHELL_V1_LAYER_TOP
                                    : ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;

    int node_x, node_y;
    if(mode == Mode::Bar) {
        node_x = output->layout_box.x;
        node_y =
            (bar_position == BarPosition::Top)
                ? output->layout_box.y
                : output->layout_box.y + output->layout_box.height - height;
    } else {
        int ox = output->layout_box.x, oy = output->layout_box.y;
        int ow = output->layout_box.width, oh = output->layout_box.height;
        switch(popup_anchor) {
            case PopupAnchor::Center:
                node_x = ox + (ow - width) / 2;
                node_y = oy + (oh - height) / 2;
                break;
            case PopupAnchor::Top:
                node_x = ox + (ow - width) / 2;
                node_y = oy + popup_margin;
                break;
            case PopupAnchor::Bottom:
                node_x = ox + (ow - width) / 2;
                node_y = oy + oh - height - popup_margin;
                break;
            case PopupAnchor::Left:
                node_x = ox + popup_margin;
                node_y = oy + (oh - height) / 2;
                break;
            case PopupAnchor::Right:
                node_x = ox + ow - width - popup_margin;
                node_y = oy + (oh - height) / 2;
                break;
            case PopupAnchor::TopLeft:
                node_x = ox + popup_margin;
                node_y = oy + popup_margin;
                break;
            case PopupAnchor::TopRight:
                node_x = ox + ow - width - popup_margin;
                node_y = oy + popup_margin;
                break;
            case PopupAnchor::BottomLeft:
                node_x = ox + popup_margin;
                node_y = oy + oh - height - popup_margin;
                break;
            case PopupAnchor::BottomRight:
            default:
                node_x = ox + ow - width - popup_margin;
                node_y = oy + oh - height - popup_margin;
                break;
        }
    }

    if(!scene_node) {
        std::vector<uint8_t> empty(static_cast<size_t>(width) * height * 4, 0);
        wlr_buffer* buf = wrapWidgetPixels(std::move(empty), width, height);
        scene_node = wlr_scene_buffer_create(server.layer_tree[layer], buf);
        wlr_buffer_drop(buf);
        scene_node->node.data = this;
        if(!visible) { wlr_scene_node_set_enabled(&scene_node->node, false); }
    } else {
        wlr_scene_buffer_set_dest_size(scene_node, width, height);
    }
    wlr_scene_node_set_position(&scene_node->node, node_x, node_y);
}

int WidgetWindow::createRect(
    int parent_id, int x, int y, int w, int h, uint32_t color) {
    WidgetNode node;
    node.kind      = WidgetKind::Rect;
    node.parent_id = parent_id;
    node.x         = x;
    node.y         = y;
    node.w         = w;
    node.h         = h;
    node.color     = color;
    widgets.push_back(node);
    return static_cast<int>(widgets.size());
}

int WidgetWindow::createText(int                parent_id,
                             int                x,
                             int                y,
                             const std::string& text,
                             int                pixel_size,
                             uint32_t           color) {
    WidgetNode node;
    node.kind       = WidgetKind::Text;
    node.parent_id  = parent_id;
    node.x          = x;
    node.y          = y;
    node.text       = text;
    node.pixel_size = pixel_size;
    node.color      = color;
    node.w =
        server.text_renderer.measureText(text, defaultFontPath(), pixel_size);
    node.h = server.text_renderer.lineHeight(defaultFontPath(), pixel_size);
    widgets.push_back(node);
    return static_cast<int>(widgets.size());
}

void WidgetWindow::setText(int widget_id, const std::string& text) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    auto& w = widgets[static_cast<size_t>(widget_id) - 1];
    w.text  = text;
    if(w.kind == WidgetKind::Text) {
        w.w = server.text_renderer.measureText(
            text, defaultFontPath(), w.pixel_size);
        // height depends only on font/pixel_size, not on `text` itself
        // -- already correct from createText(), no need to redo it.
    }
}

void WidgetWindow::setColor(int widget_id, uint32_t color) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    widgets[static_cast<size_t>(widget_id) - 1].color = color;
}

void WidgetWindow::setPos(int widget_id, int x, int y) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    auto& w = widgets[static_cast<size_t>(widget_id) - 1];
    w.x     = x;
    w.y     = y;
}

void WidgetWindow::setSize(int widget_id, int w, int h) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    auto& node = widgets[static_cast<size_t>(widget_id) - 1];
    node.w     = w;
    node.h     = h;
}

void WidgetWindow::setAnchor(int        widget_id,
                             AnchorEdge my_edge,
                             int        target_id,
                             AnchorEdge target_edge,
                             int        margin) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    auto&      w = widgets[static_cast<size_t>(widget_id) - 1];
    AnchorLine line{target_edge, target_id, margin};
    switch(my_edge) {
        case AnchorEdge::Left:
            w.anchor_left = line;
            break;
        case AnchorEdge::Right:
            w.anchor_right = line;
            break;
        case AnchorEdge::Top:
            w.anchor_top = line;
            break;
        case AnchorEdge::Bottom:
            w.anchor_bottom = line;
            break;
        case AnchorEdge::HCenter:
            w.anchor_hcenter = line;
            break;
        case AnchorEdge::VCenter:
            w.anchor_vcenter = line;
            break;
        default:
            break;  // None -- not a valid `my_edge`, silently ignored
    }
}

void WidgetWindow::clearAnchor(int widget_id, AnchorEdge my_edge) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    auto& w = widgets[static_cast<size_t>(widget_id) - 1];
    switch(my_edge) {
        case AnchorEdge::Left:
            w.anchor_left = AnchorLine{};
            break;
        case AnchorEdge::Right:
            w.anchor_right = AnchorLine{};
            break;
        case AnchorEdge::Top:
            w.anchor_top = AnchorLine{};
            break;
        case AnchorEdge::Bottom:
            w.anchor_bottom = AnchorLine{};
            break;
        case AnchorEdge::HCenter:
            w.anchor_hcenter = AnchorLine{};
            break;
        case AnchorEdge::VCenter:
            w.anchor_vcenter = AnchorLine{};
            break;
        default:
            break;
    }
}

void WidgetWindow::clearAnchors(int widget_id) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    auto& w       = widgets[static_cast<size_t>(widget_id) - 1];
    w.anchor_left = w.anchor_right = w.anchor_hcenter = AnchorLine{};
    w.anchor_top = w.anchor_bottom = w.anchor_vcenter = AnchorLine{};
}

void WidgetWindow::setFill(int widget_id, int target_id, int margin) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    auto& w          = widgets[static_cast<size_t>(widget_id) - 1];
    w.fill           = true;
    w.fill_target_id = target_id;
    w.fill_margin    = margin;
}

void WidgetWindow::clearFill(int widget_id) {
    if(widget_id < 1 || static_cast<size_t>(widget_id) > widgets.size()) {
        return;
    }
    widgets[static_cast<size_t>(widget_id) - 1].fill = false;
}

void WidgetWindow::commit() {
    if(!output || !scene_node || width <= 0 || height <= 0) { return; }

    std::vector<VisitState> state(widgets.size(), VisitState::Unvisited);
    for(size_t i = 0; i < widgets.size(); i++) {
        resolveBox(*this, static_cast<int>(i + 1), state);
    }

    for(size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i]     = 0;
        pixels[i + 1] = 0;
        pixels[i + 2] = 0;
        pixels[i + 3] = 255;
    }

    for(const WidgetNode& node : widgets) {
        if(node.kind == WidgetKind::Rect) {
            uint8_t r  = static_cast<uint8_t>((node.color >> 24) & 0xff);
            uint8_t g  = static_cast<uint8_t>((node.color >> 16) & 0xff);
            uint8_t b  = static_cast<uint8_t>((node.color >> 8) & 0xff);
            uint8_t a8 = static_cast<uint8_t>(node.color & 0xff);
            float   a  = a8 / 255.0f;

            int x0 = std::max(0, node.resolved_x);
            int y0 = std::max(0, node.resolved_y);
            int x1 = std::min(width, node.resolved_x + node.w);
            int y1 = std::min(height, node.resolved_y + node.h);
            for(int py = y0; py < y1; py++) {
                for(int px = x0; px < x1; px++) {
                    blendPixel(
                        &pixels[static_cast<size_t>(py) * width * 4 + px * 4],
                        r,
                        g,
                        b,
                        a);
                }
            }
        } else {
            // node.resolved_x/resolved_y are this widget's top-left box
            // corner -- same convention every other widget uses, so
            // anchors targeting a Text widget's top/bottom/vcenter
            // edge get sane geometry (node.w/h are real, measured
            // values -- see createText()/setText()). drawText itself
            // wants a baseline, not a top-left corner, so convert right
            // here, once, rather than making every anchor computation
            // upstream reason about baselines.
            const std::string& font = defaultFontPath();
            int ascent = server.text_renderer.ascent(font, node.pixel_size);
            server.text_renderer.drawText(pixels.data(),
                                          width * 4,
                                          width,
                                          height,
                                          node.resolved_x,
                                          node.resolved_y + ascent,
                                          node.text,
                                          font,
                                          node.pixel_size,
                                          node.color);
        }
    }

    wlr_buffer* buf = wrapWidgetPixels(pixels, width, height);
    wlr_scene_buffer_set_buffer(scene_node, buf);
    wlr_buffer_drop(buf);
}
