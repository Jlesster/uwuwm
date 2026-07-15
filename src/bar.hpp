#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
}

#include <cstdint>
#include <string>
#include <vector>

class Server;
struct Output;

enum class BarPosition { Top, Bottom };

// uwu.bar.create({output=, position="top"/"bottom", height=}) -- a
// compositor-owned, compositor-drawn status bar: not a Wayland client
// (unlike waybar/polybar, which are layer-shell clients uwuwm merely
// hosts), so there's no external process, no IPC round-trip to another
// binary, and nothing to have installed separately. rc.lua draws into it
// directly with the primitives below (clear/fillRect/drawText),
// immediate-mode -- every commit() redraws the whole buffer from
// scratch rather than diffing a retained widget tree, which is the
// right tradeoff for something redrawn a handful of times a second at
// most (driven by uwu.timer/uwu.hook), not once a frame the way View
// rendering is.
struct Bar {
    Bar(Server& server, Output& output, BarPosition position, int height);
    ~Bar();

    Bar(const Bar&)            = delete;
    Bar& operator=(const Bar&) = delete;

    Server& server;
    // Non-null exactly as long as the output this bar was created on is
    // still connected -- detachFromOutput() (called from Output's
    // teardown) clears it. Every draw primitive still works with a null
    // output (it's only writing into `pixels`, which stays allocated);
    // reposition()/commit(), which touch the scene graph, no-op instead.
    Output*     output;
    BarPosition position;
    int         height;
    // Set once, right after construction, by whichever Lua binding
    // inserted this Bar into Server::bars -- 0 is never a valid id
    // (LuaConfig-style ids all start at 1), so 0 here means "not
    // registered yet" transiently during construction.
    int id = 0;

    // RGBA8, tightly packed (stride == width*4). Every draw primitive
    // below writes directly into this; nothing reaches the screen until
    // commit() uploads a copy of it. Sized (and zeroed) by reposition().
    std::vector<uint8_t> pixels;
    int                  width = 0;

    wlr_scene_buffer* scene_node = nullptr;

    // uwu.bar.on_click(bar, fn) -- fn(x, y, button), x/y relative to
    // this bar's own top-left corner, button a BTN_LEFT/BTN_RIGHT/...
    // evdev code. -2 (LUA_NOREF, spelled out -- see LuaTimer's identical
    // comment in lua_config.hpp) means unset: a click just falls through
    // to whatever's underneath instead of being consumed (see
    // input.cpp's barAt()/cursor_button handler).
    int click_fn_ref = -2;

    // color is 0xRRGGBBAA, straight (non-premultiplied) alpha. Bars are
    // drawn opaque -- see the blend-onto-255-alpha note in text.cpp,
    // which fillRect follows too -- so semi-transparent colors blend
    // into whatever's already in `pixels`, they don't punch a
    // see-through hole in the bar itself.
    void clear(uint32_t color);
    void fillRect(int x, int y, int w, int h, uint32_t color);
    int  drawText(int                x,
                  int                y,
                  const std::string& utf8,
                  const std::string& font_path,
                  int                pixel_size,
                  uint32_t           color);
    int  textWidth(const std::string& utf8,
                   const std::string& font_path,
                   int                pixel_size);
    int  lineHeight(const std::string& font_path, int pixel_size);

    // Uploads a copy of the current `pixels` as a fresh scene buffer.
    // No-op if output is null or width/height aren't positive yet.
    void commit();

    // Recomputes `width`/`pixels`' size and the scene node's
    // position/size from output->layout_box, and zero-fills `pixels`.
    // Called from the constructor and from arrangeLayers() (layershell.cpp)
    // whenever this output's geometry might have changed (resize, mode
    // change, or another bar/layer-shell surface's exclusive zone
    // changing -- a bar always spans the *entire* output width at its
    // own true edge, so it doesn't actually depend on other exclusive
    // zones, but arrangeLayers calls this unconditionally alongside its
    // own recompute for simplicity). Does not commit(); the next draw+
    // commit (or an explicit uwu.bar->commit() with unchanged pixel
    // content) is responsible for that -- reposition() alone leaves the
    // *previous* frame's pixels on screen at the *new* position/size
    // until something redraws, same as a plain resize would.
    void reposition();

    // See Output's own destructor -- detaches this bar's scene node and
    // clears `output`, but the Bar object itself survives (Server::bars
    // still owns it): a still-valid id shouldn't dangle across a
    // monitor unplug/replug, only uwu.bar.destroy() should actually
    // free it.
    void detachFromOutput();
};
