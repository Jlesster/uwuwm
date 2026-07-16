#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
}

#include "bar.hpp"  // BarPosition -- shared with the immediate-mode Bar

#include <cstdint>
#include <memory>
#include <string>

class Server;
struct Output;
class QtRuntime;

// uwu.qml.create({output=, position="top"/"bottom", height=}) -- a
// retained-mode sibling of Bar (bar.hpp). Where Bar's rc.lua primitives
// (clear/fillRect/drawText) redraw the *entire* pixel buffer from
// scratch on every commit(), QmlBar's rect()/text() each instantiate a
// real QQuickItem exactly once; every later poke (setText/setColor/...)
// mutates that already-live item's Qt properties directly, and
// QtQuick's own scenegraph does the "only re-render what actually
// changed" work Bar has no equivalent of. Ends up at the same place
// Bar does either way, though: a fresh RGBA8 frame uploaded via
// wlr_scene_buffer_set_buffer (see commit() below, and bar.cpp's
// wrapBarPixels, which qml_bar.cpp's wrapQmlBarPixels duplicates for
// the same reason wallpaper.cpp's own near-identical buffer struct
// already does -- a small enough recipe that a shared helper isn't
// worth the coupling).
//
// Pimpl'd (Impl fully defined only in qml_bar.cpp), unlike Bar itself:
// Bar has nothing but wlroots/std types in its interface, but QmlBar's
// actual guts are QQuickRenderControl/QQuickWindow/QQmlEngine/
// QQuickItem -- and Qt's real headers bring `signals`/`slots`/`emit`
// macros with them, a real, documented source of collisions with
// unrelated code. Keeping those confined to qml_bar.cpp (and
// qt_runtime.cpp) means every other translation unit that just wants
// to create/poke a bar -- server.hpp, lua_config.cpp -- never has to
// see them.
class QmlBar {
public:
    QmlBar(Server& server, Output& output, BarPosition position, int height,
           QtRuntime& qt_runtime);
    ~QmlBar();

    QmlBar(const QmlBar&)            = delete;
    QmlBar& operator=(const QmlBar&) = delete;

    Server& server;
    // Same null-after-output-disconnect contract as Bar::output -- see
    // that field's comment in bar.hpp.
    Output*     output;
    BarPosition position;
    int         height;
    // Set once, right after construction, by whichever Lua binding
    // inserted this QmlBar into Server::qml_bars -- same 0-means-
    // not-registered-yet convention as Bar::id.
    int id = 0;

    int width = 0;

    wlr_scene_buffer* scene_node = nullptr;

    // uwu.qml.on_click(bar, fn) -- same contract as Bar::click_fn_ref
    // (bar.hpp), unset via -2/LUA_NOREF.
    int click_fn_ref = -2;

    // Each returns a new widget id, scoped to this bar (ids from two
    // different QmlBars are not comparable/interchangeable). Every
    // color is 0xRRGGBBAA, straight (non-premultiplied) alpha -- same
    // convention Bar::fillRect uses (see bar.hpp), so existing rc.lua
    // color literals need no translation between uwu.bar.* and
    // uwu.qml.*.
    //
    // These are the *only* two primitive kinds today -- adding a third
    // (say, an image widget) means one new QML snippet string plus one
    // new create*() method here and one new luaL_Reg entry in
    // lua_config.cpp, not a rendering-pipeline change; that's the
    // "primitives baked into the compositor easily" property this
    // whole redesign was for.
    int createRect(int x, int y, int w, int h, uint32_t color);
    int createText(int x, int y, const std::string& text, int pixel_size,
                    uint32_t color);

    // Plain property pokes against an already-live QQuickItem -- never
    // recreate anything. `widget_id` not found (already invalid, or
    // never existed) is silently a no-op rather than an error: matches
    // Bar's own leniency (draw calls on a width<=0 Bar just no-op too)
    // rather than making every uwu.timer() tick that updates a clock
    // widget defensively check the id first.
    void setText(int widget_id, const std::string& text);
    void setColor(int widget_id, uint32_t color);
    void setPos(int widget_id, int x, int y);
    void setSize(int widget_id, int w, int h);

    // Renders the current QQuickItem tree and uploads it as a fresh
    // scene buffer -- the QQuickRenderControl-driven analogue of
    // Bar::commit(). Unlike Bar::commit(), there's no separate
    // "draw into a CPU buffer, then commit uploads a copy" split --
    // rendering the scenegraph *is* what produces the pixels, so this
    // does both steps.
    void commit();

    // Same contract as Bar::reposition(): recomputes width and the
    // scene node's position/size from output->layout_box (also resizes
    // the offscreen FBO and root QQuickItem to match). Called from the
    // constructor and from arrangeLayers() (layershell.cpp).
    void reposition();

    // Same contract as Bar::detachFromOutput() -- see bar.hpp.
    void detachFromOutput();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    int                   next_widget_id = 1;
};
