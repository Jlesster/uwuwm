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
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;
struct SessionLock;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_constraint_v1;
struct wlr_relative_pointer_manager_v1;
struct PointerConstraint;
struct wlr_tearing_control_manager_v1;
struct wlr_xdg_decoration_manager_v1;
struct wlr_xdg_toplevel_decoration_v1;
struct ToplevelDecoration;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_keyboard_shortcuts_inhibit_manager_v1;
struct wlr_keyboard_shortcuts_inhibitor_v1;
struct wlr_output_power_manager_v1;
struct wlr_output_power_v1_set_mode_event;
struct wlr_idle_notifier_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_inhibitor_v1;
struct IdleInhibitor;
struct wlr_xdg_activation_v1;
struct wlr_xdg_activation_v1_request_activate_event;
struct wlr_cursor_shape_manager_v1;
struct wlr_cursor_shape_manager_v1_request_set_shape_event;
struct wlr_content_type_manager_v1;

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

    wl_display*                  display              = nullptr;
    wlr_backend*                 backend              = nullptr;
    wlr_session*                 session              = nullptr;
    bool                         session_active       = true;
    wlr_renderer*                renderer             = nullptr;
    wlr_allocator*               allocator            = nullptr;
    wlr_compositor*              compositor           = nullptr;
    wlr_output_layout*           output_layout        = nullptr;
    wlr_scene*                   scene                = nullptr;
    wlr_scene_output_layout*     scene_layout         = nullptr;
    wlr_xdg_shell*               xdg_shell            = nullptr;
    wlr_layer_shell_v1*          layer_shell          = nullptr;
    wlr_seat*                    seat                 = nullptr;
    wlr_cursor*                  cursor               = nullptr;
    wlr_xcursor_manager*         cursor_mgr           = nullptr;
    wlr_xwayland*                xwayland             = nullptr;
    wlr_session_lock_manager_v1* session_lock_manager = nullptr;

    // Pointer lock/confine (games' mouselook) + the relative-motion channel
    // that rides alongside it. See input.hpp/input.cpp for the
    // PointerConstraint wrapper and the activation/clamping logic that uses
    // these.
    wlr_pointer_constraints_v1*      pointer_constraints      = nullptr;
    wlr_relative_pointer_manager_v1* relative_pointer_manager = nullptr;

    // wp_tearing_control_v1: lets a client (Proton/SDL/mesa's swapchain
    // internals) hint that a surface's content is fine being shown with
    // tearing in exchange for lower latency. We only ever query this
    // per-output at commit time via
    // wlr_tearing_control_manager_v1_surface_hint_from_surface -- see
    // Output::handleFrame -- so there are no signals to connect here,
    // unlike pointer_constraints above.
    wlr_tearing_control_manager_v1* tearing_control_manager = nullptr;

    // xdg-decoration-unstable-v1: lets a client ask whether it should draw
    // its own titlebar (CSD) or let us draw ours (SSD). We always force
    // SSD -- see decoration.hpp for why -- via the per-decoration
    // ToplevelDecoration wrapper owned by toplevel_decorations below.
    wlr_xdg_decoration_manager_v1* xdg_decoration_manager = nullptr;

    // wlr-foreign-toplevel-management-v1: the protocol a taskbar/dock/
    // app-switcher (waybar's taskbar module, etc.) uses to see the window
    // list and ask to activate/minimize/maximize/close/fullscreen a
    // window it doesn't itself own. Unlike screencopy/data-control/
    // tearing-control above, this isn't a bare global -- we own one
    // wlr_foreign_toplevel_handle_v1 per mapped, managed View, created
    // and destroyed by View itself (see view.hpp's createForeignToplevel/
    // destroyForeignToplevel). The manager has no client-facing "new
    // toplevel" signal to wire here; toplevel handles only ever come from
    // our own wlr_foreign_toplevel_handle_v1_create() calls.
    wlr_foreign_toplevel_manager_v1* foreign_toplevel_manager = nullptr;

    // zwp-keyboard-shortcuts-inhibit-unstable-v1: lets a client (remote-
    // desktop viewer, VM console, or a game that wants Alt+Tab routed to
    // itself instead of the compositor) ask that its key events bypass
    // compositor keybind handling entirely while it holds keyboard focus.
    // We grant every request unconditionally in Server::setup -- see the
    // comment there -- so this is a bare global with nothing else to
    // track on the Server side; enforcement reads the manager's own
    // `inhibitors` list directly via input::shortcutsInhibited, checked
    // from Keyboard::handleKey.
    wlr_keyboard_shortcuts_inhibit_manager_v1* shortcuts_inhibit_manager =
        nullptr;

    // wlr-output-power-management-unstable-v1: DPMS control from
    // software -- what an idle daemon (swayidle) or a power applet uses
    // to blank/wake a specific output on demand. Like screencopy/
    // data-control/tearing-control, this is a bare global; the only
    // thing we feed it is the set_mode signal, wired in Server::setup,
    // which just re-applies `enabled` as an output-state commit -- the
    // same mechanism Output::applyRule uses for uwu.monitor.set().
    wlr_output_power_manager_v1* output_power_manager = nullptr;

    // ext-idle-notify-v1: what an idle daemon (swayidle) binds to find
    // out "no input for N seconds" at all -- fed from every real input
    // event via wlr_idle_notifier_v1_notify_activity, called from
    // Keyboard::handleKey and all four cursor signals in input.cpp.
    // idle-inhibit-unstable-v1 (the pair below) is what temporarily mutes
    // that timer while a client that shouldn't be interrupted -- a video,
    // a game -- is visible; neither protocol is useful daily-driven
    // without the other, so they're wired up together. See idle.hpp for
    // the actual inhibit-state bookkeeping.
    wlr_idle_notifier_v1*        idle_notifier        = nullptr;
    wlr_idle_inhibit_manager_v1* idle_inhibit_manager = nullptr;

    // One entry per live zwp_idle_inhibitor_v1 a client currently holds
    // open, regardless of whether its surface is presently visible --
    // see idle.hpp's IdleInhibitor / idle::updateInhibitState.
    std::list<std::unique_ptr<IdleInhibitor>> idle_inhibitors;

    // xdg-activation-v1: lets one client (a launcher, a completed
    // background task, an xdg-desktop-portal window-activation request)
    // ask to focus a *different* client's surface, with wlroots handling
    // the token issue/verify dance itself -- request_activate only ever
    // fires once a client presents a token it was actually handed. We
    // just resolve event->surface to a managed View and route it through
    // the same Server::focusView every other focus path already uses
    // (session_locked, etc. all still apply). See Server::setup.
    wlr_xdg_activation_v1* xdg_activation = nullptr;

    // wp-cursor-shape-v1: lets a client name a cursor shape ("text",
    // "grab", "wait", ...) instead of drawing/loading its own xcursor
    // theme -- newer GTK4/Qt6-era toolkits increasingly assume this is
    // available. wlr_cursor_shape_v1_name() maps straight onto the same
    // xcursor names cursor_mgr already knows how to load, so this reuses
    // the exact wlr_cursor_set_xcursor call path request_set_cursor
    // above uses; no new cursor-loading logic needed.
    wlr_cursor_shape_manager_v1* cursor_shape_manager = nullptr;

    // wp-content-type-v1: lets a client hint what kind of content a
    // surface holds (game/video/photo/none). Bare global, nothing to
    // listen for here -- like tearing-control, we only ever query it
    // per-surface on demand (wlr_surface_get_content_type_v1), from
    // Output::updateAdaptiveSync, so a focused client's own "I'm a game"
    // hint can pull in adaptive sync the same way the existing
    // exactly-one-visible-window heuristic already does.
    wlr_content_type_manager_v1* content_type_manager = nullptr;

    wlr_scene_tree* layer_tree[4] = {};
    wlr_scene_tree* window_tree   = nullptr;

    std::list<std::unique_ptr<Output>>       outputs;
    std::list<std::unique_ptr<View>>         views;
    std::list<std::unique_ptr<LayerSurface>> layer_surfaces;
    std::list<std::unique_ptr<Keyboard>>     keyboards;

    // True from the moment a lock is granted (SessionLock's constructor
    // runs) until a clean unlock -- unlike `bool(session_lock)`, this
    // does NOT flip back off if the lock client dies without unlocking;
    // see session_lock.hpp's header comment for why that split exists.
    // Keyboard::handleKey and Server::focusView both gate on this, not on
    // session_lock, so every input path stays blocked through that
    // fail-safe case too.
    bool session_locked = false;

    // Declared after outputs/views/layer_surfaces/keyboards above
    // deliberately: members destruct in reverse declaration order, and
    // ~SessionLock() walks `outputs` to clear per-output lock_backdrop/
    // lock_surface pointers. Declaring session_lock any earlier would let
    // `outputs` be destroyed first during ~Server(), leaving ~SessionLock
    // to walk a list of already-freed Output objects.
    std::unique_ptr<SessionLock> session_lock;

    // Every constraint a client currently has open (lock or confine
    // requests are 1:1 with a PointerConstraint for as long as the
    // client's zwp_locked_pointer_v1/zwp_confined_pointer_v1 object
    // lives), most of which are inactive at any given moment -- only the
    // one matching whatever surface currently has pointer focus, if any,
    // is "active". See input.cpp's PointerConstraint/activateConstraint.
    std::list<std::unique_ptr<PointerConstraint>> pointer_constraint_wrappers;

    // Non-owning; points into pointer_constraint_wrappers, or nullptr.
    // Only one constraint may be active across the whole seat at a time
    // per the protocol spec (locking/confining an already-locked/confined
    // pointer is a protocol error on the client's part, not something we
    // need to arbitrate here).
    PointerConstraint* active_constraint = nullptr;

    // One entry per live zxdg_toplevel_decoration_v1 object a client has
    // open, regardless of which View (or no View yet, if the client
    // hasn't attached a role surface) it's associated with -- see
    // decoration.hpp.
    std::list<std::unique_ptr<ToplevelDecoration>> toplevel_decorations;

    Output* focused_output = nullptr;
    View*   focused_view   = nullptr;

    CursorMode cursor_mode  = CursorMode::Passthrough;
    View*      grabbed_view = nullptr;
    double     grab_x = 0, grab_y = 0;
    wlr_box    grab_geobox{};
    uint32_t   resize_edges = 0;

    void    focusView(View* view);
    void    closeOnTag(uint32_t tagmask);
    Output* outputAt(double lx, double ly);
    void    arrangeAllOutputs();
    Output* getFocusedOrDefaultOutput();
    void    tickAnimations();
    void    reloadConfig();

    // Cursor listeners (public so input::setupCursor can access them)
    Listener<wlr_pointer_motion_event>          cursor_motion;
    Listener<wlr_pointer_motion_absolute_event> cursor_motion_absolute;
    Listener<wlr_pointer_button_event>          cursor_button;
    Listener<wlr_pointer_axis_event>            cursor_axis;
    VoidListener                                cursor_frame;

private:
    bool setup();
    void spawnAutostart();

    Listener<wlr_output>                          new_output;
    Listener<wlr_xdg_toplevel>                    new_xdg_toplevel;
    Listener<wlr_layer_surface_v1>                new_layer_surface;
    Listener<wlr_xwayland_surface>                new_xwayland_surface;
    VoidListener                                  xwayland_ready;
    Listener<wlr_session_lock_v1>                 new_session_lock;
    Listener<wlr_pointer_constraint_v1>           new_pointer_constraint;
    Listener<wlr_xdg_toplevel_decoration_v1>      new_toplevel_decoration;
    Listener<wlr_keyboard_shortcuts_inhibitor_v1> new_shortcuts_inhibitor;
    Listener<wlr_output_power_v1_set_mode_event>  output_power_set_mode;
    Listener<wlr_idle_inhibitor_v1>               new_idle_inhibitor;
    Listener<wlr_xdg_activation_v1_request_activate_event>
        new_xdg_activation_request;
    Listener<wlr_cursor_shape_manager_v1_request_set_shape_event>
                               request_set_cursor_shape;
    Listener<wlr_input_device> new_input;
    Listener<wlr_seat_pointer_request_set_cursor_event> request_set_cursor;
    Listener<wlr_seat_request_set_selection_event>      request_set_selection;
    VoidListener                                        session_active_listener;

    friend struct Output;
    friend struct View;
    friend struct XdgToplevel;
    friend struct XWaylandView;
    friend struct LayerSurface;
    friend struct Keyboard;
    friend struct SessionLock;
    friend struct PointerConstraint;
};
