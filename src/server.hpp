#pragma once

extern "C" {
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
}

#include "listener.hpp"
#include "lua_config.hpp"

#include <cstdint>
#include <list>
#include <memory>
#include <string>

struct Output;
struct View;
struct Popup;
struct LayerSurface;
struct Keyboard;
struct wlr_layer_shell_v1;
struct wlr_xwayland;
struct wlr_xwayland_surface;

enum class CursorMode {
    Passthrough,
    Move,
    Resize,
};

class Server {
public:
    Server();
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    int run(int argc, char** argv);

    void requestQuit() {
        if(display) { wl_display_terminate(display); }
    }

    LuaConfig lua_cfg;

    wl_display*              display        = nullptr;
    wlr_backend*             backend        = nullptr;
    wlr_session*             session        = nullptr;
    bool                     session_active = true;
    wlr_renderer*            renderer       = nullptr;
    wlr_allocator*           allocator      = nullptr;
    wlr_compositor*          compositor     = nullptr;
    wlr_output_layout*       output_layout  = nullptr;
    wlr_scene*               scene          = nullptr;
    wlr_scene_output_layout* scene_layout   = nullptr;
    wlr_xdg_shell*           xdg_shell      = nullptr;
    wlr_layer_shell_v1*      layer_shell    = nullptr;
    wlr_seat*                seat           = nullptr;
    wlr_cursor*              cursor         = nullptr;
    wlr_xcursor_manager*     cursor_mgr     = nullptr;
    wlr_xwayland* xwayland = nullptr;

    wlr_scene_tree* layer_tree[4] = {};
    wlr_scene_tree* window_tree   = nullptr;

    std::list<std::unique_ptr<Output>>       outputs;
    std::list<std::unique_ptr<View>>         views;
    std::list<std::unique_ptr<LayerSurface>> layer_surfaces;
    std::list<std::unique_ptr<Keyboard>>     keyboards;

    Output* focused_output = nullptr;
    View*   focused_view   = nullptr;

    CursorMode cursor_mode  = CursorMode::Passthrough;
    View*      grabbed_view = nullptr;
    double     grab_x = 0, grab_y = 0;
    wlr_box    grab_geobox{};
    uint32_t   resize_edges = 0;

    void focusView(View* view);
    void closeOnTag(uint32_t tagmask);
    Output* outputAt(double lx, double ly);
    void    arrangeAllOutputs();
    Output* getFocusedOrDefaultOutput();
    void tickAnimations();
    void reloadConfig();

    // Cursor listeners (public so input::setupCursor can access them)
    Listener<wlr_pointer_motion_event>          cursor_motion;
    Listener<wlr_pointer_motion_absolute_event> cursor_motion_absolute;
    Listener<wlr_pointer_button_event>          cursor_button;
    Listener<wlr_pointer_axis_event>            cursor_axis;
    VoidListener                                cursor_frame;

private:
    bool setup();
    void spawnAutostart();

    Listener<wlr_output>                                new_output;
    Listener<wlr_xdg_toplevel>                          new_xdg_toplevel;
    Listener<wlr_layer_surface_v1>                      new_layer_surface;
    Listener<wlr_xwayland_surface>                      new_xwayland_surface;
    VoidListener                                        xwayland_ready;
    Listener<wlr_input_device>                          new_input;
    Listener<wlr_seat_pointer_request_set_cursor_event> request_set_cursor;
    Listener<wlr_seat_request_set_selection_event>      request_set_selection;
    VoidListener                                        session_active_listener;

    friend struct Output;
    friend struct View;
    friend struct XdgToplevel;
    friend struct XWaylandView;
    friend struct LayerSurface;
    friend struct Keyboard;
};
