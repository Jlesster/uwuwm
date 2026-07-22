#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
}

#include "bar.hpp"  // BarPosition -- reused for Bar-mode edge anchoring

#include <cstdint>
#include <string>
#include <vector>

class Server;
struct Output;

// uwu.widget.window({...}) -- a retained-mode widget surface. Two
// flavors, picked by the `mode` field from Lua:
//
//   * Bar   -- anchored to an output edge, full output width, always
//              in the layer-shell TOP layer. What this class used to
//              be entirely, before popups existed.
//   * Popup -- a fixed-size box positioned by screen anchor + margin
//              (corner/edge/center, not tied to any output edge), in
//              the OVERLAY layer (drawn above everything, including
//              fullscreen views and bars) -- for OSDs, notifications,
//              small dashboards. Starts hidden; call show()/hide()
//              (a wlr_scene_node_set_enabled toggle, no create/destroy
//              churn -- pair with uwu.timer.set_timeout for auto-hide).
//
// Both flavors share one widget tree: rect()/text() each record one
// retained widget exactly once ("record once, poke forever after" --
// see set_text/set_color/etc below). Widgets can nest (`parent` field
// on rect()/text()) and position themselves against their parent or
// any sibling via anchor() -- see the Anchors section below -- instead
// of only ever taking raw window-local x/y.
class WidgetWindow {
public:
    // Bar mode.
    WidgetWindow(Server&     server,
                 Output&     output,
                 BarPosition position,
                 int         height);

    enum class PopupAnchor {
        Center,
        Top,
        Bottom,
        Left,
        Right,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };
    // Popup mode. `width`/`height` are fixed for the window's whole
    // lifetime (unlike a Bar's width, which tracks its output) --
    // there's no natural "full width" default for a floating popup,
    // so both are required from Lua (see lua_config.cpp's
    // l_widget_window_create).
    WidgetWindow(Server&     server,
                 Output&     output,
                 PopupAnchor anchor,
                 int         margin,
                 int         width,
                 int         height);

    ~WidgetWindow();

    WidgetWindow(const WidgetWindow&)            = delete;
    WidgetWindow& operator=(const WidgetWindow&) = delete;

    enum class Mode { Bar, Popup };
    Mode mode;

    Server& server;
    // Same null-after-output-disconnect contract as Bar::output.
    Output* output;

    // Bar mode only.
    BarPosition bar_position = BarPosition::Top;
    // Popup mode only.
    PopupAnchor popup_anchor = PopupAnchor::Center;
    int         popup_margin = 0;

    // Set once, right after construction, by whichever Lua binding
    // inserted this window into Server::widget_windows.
    int id = 0;

    int width = 0, height = 0;

    wlr_scene_buffer* scene_node = nullptr;

    // Bars start visible immediately (matches the always-on-when-
    // created status-bar contract from before popups existed). Popups
    // start hidden -- create once at startup, then show()/hide() from
    // Lua (typically paired with uwu.timer.set_timeout for auto-hide)
    // rather than construct/destroy per OSD event.
    bool visible = true;
    void show();
    void hide();

    int click_fn_ref = -2;  // uwu.widget.on_click(), -2 == LUA_NOREF

    std::vector<uint8_t> pixels;  // RGBA8, same layout as Bar::pixels

    enum class WidgetKind { Rect, Text };
    enum class AnchorEdge { None, Left, Right, Top, Bottom, HCenter, VCenter };

    // One anchor constraint: "my `edge` (the field name this AnchorLine
    // is stored under, not stored here) sits at target's `edge` +
    // `margin`". `target_id == 0` means the anchor is against this
    // widget's own parent (or the window root, if parent_id is also 0)
    // -- never a fixed global "the parent", so a deeply nested widget
    // can still say "my top = my own container's top" correctly no
    // matter how deep it's nested.
    struct AnchorLine {
        AnchorEdge edge      = AnchorEdge::None;  // None == unset
        int        target_id = 0;
        int        margin    = 0;
    };

    // One retained widget.
    //
    // Geometry model (deliberately simple): anchors only ever move a
    // widget's (x, y); they never resize it. `w`/`h` are always
    // whatever set_size()/the constructor gave them for a Rect; for
    // Text they're measured automatically from `text`/`pixel_size`
    // (see createText()/setText() in widget.cpp) rather than
    // user-set, so anchoring *to* a text label's right/bottom/vcenter
    // edge gets real geometry instead of always 0. The one deliberate
    // size exception either way is `fill`, below, which is a
    // distinct, explicit operation that sets all four at once -- not
    // a side effect of combining left+right anchors the way QML's own
    // anchors would. This means the anchor solver only ever has to
    // resolve positions, which are always fully determined by
    // already-known widths/heights -- no "solve for width and
    // position simultaneously" case, and therefore no way for two
    // anchors to disagree about a size.
    //
    // resolved_x/resolved_y (below) are a top-left box corner for
    // every widget kind, Text included -- uniform with Rect, and with
    // every other widget's anchor math, on purpose. TextRenderer's own
    // drawText() wants a baseline instead; that conversion (+ ascent)
    // happens once, in commit()'s Text paint case, rather than leaking
    // baseline semantics into the anchor solver itself.
    struct WidgetNode {
        WidgetKind kind;
        // 0 == this widget's parent is the window root (bounds
        // (0, 0, width, height)); otherwise another widget's id.
        // Verified to exist at layout time, not at set-time -- an
        // invalid/cyclic parent_id is a safe fallback, never an error
        // (see widget.cpp's resolveBox()).
        int parent_id = 0;

        // x/y: parent-relative offset, used as-is when no anchor line
        // below is set for that axis (so a plain, non-anchored nested
        // widget behaves exactly like every other UI toolkit's
        // parent-relative Item.x/Item.y -- and a widget with
        // parent_id == 0 and no anchors behaves exactly like the old
        // flat qml_bar.cpp model, window-local x/y, unchanged).
        // w/h: explicit for Rect (set_size()); measured automatically
        // for Text (see createText()/setText() in widget.cpp).
        int x = 0, y = 0, w = 0, h = 0;

        // Resolved (computed) position, filled in by commit()'s layout
        // pass each call -- what actually gets painted. Always this
        // widget's top-left box corner, every kind, including Text
        // (converted to a baseline only at paint time -- see
        // commit()'s Text case in widget.cpp).
        int resolved_x = 0, resolved_y = 0;

        // At most one of left/right/hcenter can meaningfully apply to
        // the x axis (same for top/bottom/vcenter on y) -- if more
        // than one is set, priority is left > right > hcenter (top >
        // bottom > vcenter), documented and deterministic rather than
        // "last write wins" or an error. `margin` is always an inset:
        // a positive left/topMargin moves inward from the target's
        // left/top edge, and a positive right/bottomMargin moves
        // inward from the target's right/bottom edge too -- the same
        // convention as CSS padding or QML's own anchors, not "always
        // added to the raw edge coordinate" (which would push a
        // right/bottom anchor outward, off the target, instead).
        AnchorLine anchor_left, anchor_right, anchor_hcenter;
        AnchorLine anchor_top, anchor_bottom, anchor_vcenter;

        // uwu.Widget:anchor_fill(target, margin) -- sets resolved_x/y
        // and w/h directly from `target`'s box (0 == this widget's own
        // parent) each layout pass, inset by `margin` on all four
        // sides. The one widget "shape" allowed to resize itself via
        // anchoring -- see the class comment above for why this is a
        // dedicated flag rather than inferred from left+right both
        // being set.
        bool fill           = false;
        int  fill_target_id = 0;
        int  fill_margin    = 0;

        // Rect
        uint32_t color = 0x000000ffu;
        // Text
        std::string text;
        int         pixel_size = 14;
    };

    // Insertion-ordered, indexed by (widget_id - 1) -- there is still
    // no per-widget destroy, only whole-window destroy, so ids never
    // need reuse or a separate counter.
    std::vector<WidgetNode> widgets;

    int createRect(int parent_id, int x, int y, int w, int h, uint32_t color);
    int createText(int                parent_id,
                   int                x,
                   int                y,
                   const std::string& text,
                   int                pixel_size,
                   uint32_t           color);

    void setText(int widget_id, const std::string& text);
    void setColor(int widget_id, uint32_t color);
    void setPos(int widget_id, int x, int y);
    void setSize(int widget_id, int w, int h);

    // my_edge selects which of the six AnchorLine fields on
    // WidgetNode this writes; target_id == 0 means "this widget's
    // parent" (see AnchorLine's own comment). A stale/out-of-range
    // widget_id or target_id is a silent no-op / safe fallback at
    // layout time -- consistent with this whole class's existing
    // leniency for a stale widget_id.
    void setAnchor(int        widget_id,
                   AnchorEdge my_edge,
                   int        target_id,
                   AnchorEdge target_edge,
                   int        margin);
    void clearAnchor(int widget_id, AnchorEdge my_edge);
    void clearAnchors(int widget_id);  // all six at once
    void setFill(int widget_id, int target_id, int margin);
    void clearFill(int widget_id);

    // Resolves every widget's position (anchor solver, cycle-safe --
    // see widget.cpp's resolveBox()), then repaints `pixels` from
    // scratch in creation order and uploads a fresh scene buffer. Same
    // full-repaint-every-call cost this class always had.
    void commit();

    // Same contract as Bar::reposition(): recomputes geometry and the
    // scene node's position/size from output->layout_box. For Bar mode
    // this is the output's full width, same as before; for Popup mode
    // width/height stay fixed and only the on-screen position is
    // recomputed (output resize can still move a corner/edge-anchored
    // popup, e.g. if the output shrinks).
    void reposition();

    void detachFromOutput();
};
