#include "server.hpp"

extern "C" {
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
}

#include "decoration.hpp"
#include "input.hpp"
#include "layershell.hpp"
#include "layout.hpp"
#include "output.hpp"
#include "popup.hpp"
#include "session_lock.hpp"
#include "toplevel.hpp"
#include "view.hpp"
#include "xwayland_view.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

Server::Server() = default;

Server::~Server() {
    if(session) { wlr_session_destroy(session); }
    if(display) {
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
    }
}

// ----------------------------------------------------------------------------
// §2.2 setup sequence. Comment numbers below match the doc's numbered list
// so this function can be read directly against it.
// ----------------------------------------------------------------------------
bool Server::setup() {
    // 0. rc.lua, before anything else touches wlroots. This is uwuwm's
    // equivalent of AwesomeWM reading rc.lua before it does anything else
    // -- keybinds, gap/border/color settings, and terminal/launcher all
    // come from here now instead of config.hpp. A syntax/runtime error in
    // a user's script is fatal, same as a config.hpp that failed to
    // compile would have been.
    lua_cfg.init(*this);
    if(!lua_cfg.load()) {
        wlr_log(WLR_ERROR, "failed to load configuration");
        return false;
    }

    // 1. core Wayland server object, owns the event loop
    display             = wl_display_create();
    wl_event_loop* loop = wl_display_get_event_loop(display);

    // 1b. graceful shutdown on SIGINT/SIGTERM
    auto onSignal = [](int sig, void* data) -> int {
        (void)sig;
        wl_display_terminate(static_cast<wl_display*>(data));
        return 0;
    };
    wl_event_loop_add_signal(loop, SIGINT, onSignal, display);
    wl_event_loop_add_signal(loop, SIGTERM, onSignal, display);

    // 1b-ii. SIGHUP hot-reloads rc.lua, same idea as nginx/sway/etc. This
    // fires from the event loop, not from inside a Lua callback, so it's
    // free to call reloadConfig() (and therefore LuaConfig::reload(),
    // which closes/replaces `L`) directly and synchronously -- unlike
    // uwu.reload(), which has to defer (see LuaConfig::requestReload()).
    auto onHup = [](int /*sig*/, void* data) -> int {
        static_cast<Server*>(data)->reloadConfig();
        return 0;
    };
    wl_event_loop_add_signal(loop, SIGHUP, onHup, this);

    // 2. abstracts over DRM/KMS vs nested Wayland/X11 window. Reads
    // WAYLAND_DISPLAY / DISPLAY itself to decide nested-vs-real, same as
    // every other wlroots compositor.
    backend = wlr_backend_autocreate(loop, &session);
    if(!backend) {
        wlr_log(WLR_ERROR, "wlr_backend_autocreate failed");
        return false;
    }

    // 3. renderer (GLES2/Vulkan, autodetected)
    renderer = wlr_renderer_autocreate(backend);
    if(!renderer) {
        wlr_log(WLR_ERROR, "wlr_renderer_autocreate failed");
        return false;
    }
    wlr_renderer_init_wl_display(renderer, display);

    // 4. buffer allocator, tied to renderer+backend
    allocator = wlr_allocator_autocreate(backend, renderer);
    if(!allocator) {
        wlr_log(WLR_ERROR, "wlr_allocator_autocreate failed");
        return false;
    }

    // 5. core wl_compositor (surfaces) + subcompositor + data device manager
    compositor = wlr_compositor_create(display, 5, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);

    // wlr-screencopy-unstable-v1: grim-style screenshots and OBS/Discord/
    // wf-recorder-style screen capture. Entirely self-contained -- it
    // resolves the wl_output a client names against the wl_output global
    // that Output already exposes, and pulls pixels from whatever's
    // already in the scene-graph's output buffer, so there's nothing else
    // for us to feed it or listen to here; creating the global is the
    // whole integration.
    wlr_screencopy_manager_v1_create(display);

    // wlr-data-control-unstable-v1: lets a privileged client (cliphist,
    // wl-clip-persist, etc.) observe and set the seat's selection out of
    // band from the normal copy/paste path -- this is what lets clipboard
    // history survive the source app closing, on top of the plain
    // wlr_data_device_manager clipboard above. Same as screencopy, this is
    // a bare global: it discovers wlr_seat itself per-client via the
    // wl_seat the client already has bound, so it doesn't need `seat` to
    // exist yet at this point in setup.
    wlr_data_control_manager_v1_create(display);

    // wp-tearing-control-v1: lets a client hint that a surface is fine
    // being shown with tearing (async page-flips) in exchange for lower
    // latency -- what Proton/SDL/mesa's swapchain internals reach for on
    // uncapped-fps games. Like screencopy/data-control above this is a
    // bare global with nothing to feed it at setup time: we don't listen
    // to new_object/set_hint here, we just ask
    // wlr_tearing_control_manager_v1_surface_hint_from_surface for a
    // given surface's current hint at commit time. See
    // Output::handleFrame.
    tearing_control_manager = wlr_tearing_control_manager_v1_create(display, 1);

    output_layout = wlr_output_layout_create(display);

    new_output.connect(
        &backend->events.new_output, [this](wlr_output* wlr_output) {
            outputs.push_back(std::make_unique<Output>(*this, wlr_output));
            // A monitor plugged in mid-lock must come up covered in
            // black, not showing whatever the tiling layout would put
            // there -- session_lock is null in the fail-safe "client
            // crashed" state (see session_lock.hpp), so this
            // intentionally does nothing in that case; the orphaned
            // backdrops from the crashed lock don't extend to a brand
            // new output either, which is a known gap, not a decision.
            if(session_locked && session_lock) {
                session_lock->addOutput(*outputs.back());
            }
        });

    // 6. scene graph. wlroots owns damage tracking, occlusion culling, and
    // opportunistic direct scanout from here on -- we never touch a
    // renderer or framebuffer directly anywhere else in this codebase.
    scene        = wlr_scene_create();
    scene_layout = wlr_scene_attach_output_layout(scene, output_layout);

    // Z-order, bottom to top: background/bottom layer-shell, windows,
    // top layer-shell, overlay layer-shell. wlr_scene node order *is*
    // z-order, so this one set of wlr_scene_tree_create calls is the
    // entirety of our stacking-order policy for non-floating content.
    layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
        wlr_scene_tree_create(&scene->tree);
    layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
        wlr_scene_tree_create(&scene->tree);
    window_tree = wlr_scene_tree_create(&scene->tree);
    layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_TOP] =
        wlr_scene_tree_create(&scene->tree);
    layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] =
        wlr_scene_tree_create(&scene->tree);

    // 7. shell protocols
    xdg_shell = wlr_xdg_shell_create(display, 6);
    new_xdg_toplevel.connect(
        &xdg_shell->events.new_toplevel, [this](wlr_xdg_toplevel* t) {
            views.push_back(std::make_unique<XdgToplevel>(*this, t));
        });

    // xdg-decoration-unstable-v1: lets a client ask whether it should
    // draw its own titlebar (CSD) or let us draw ours (SSD). We always
    // force SSD -- see decoration.hpp for the rationale -- so all this
    // needs is the global plus one signal hookup; ToplevelDecoration
    // (decoration.{hpp,cpp}) owns the actual per-window negotiation and
    // is what re-asserts SSD on every request_mode.
    xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(display);
    new_toplevel_decoration.connect(
        &xdg_decoration_manager->events.new_toplevel_decoration,
        [this](wlr_xdg_toplevel_decoration_v1* deco) {
            decoration::newToplevelDecoration(*this, deco);
        });

    // wlr-foreign-toplevel-management-v1: what a taskbar/dock/app-
    // switcher (waybar's taskbar module, wlrctl, etc.) uses to see the
    // window list and ask to activate/minimize/maximize/close/fullscreen
    // a window it doesn't own. Just the global here -- there's no
    // client-facing "new toplevel" signal on the manager to wire up;
    // every wlr_foreign_toplevel_handle_v1 is one we create ourselves,
    // from View::createForeignToplevel() as each window maps (see
    // toplevel.cpp/xwayland_view.cpp's handleMap, and view.cpp for the
    // request_*/destroy wiring).
    foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(display);

    layer_shell = wlr_layer_shell_v1_create(display, 4);
    new_layer_surface.connect(
        &layer_shell->events.new_surface, [this](wlr_layer_surface_v1* ls) {
            // LayerSurface's constructor can fail to resolve an output (no
            // output specified and none focused yet) and destroys the
            // underlying wlr_layer_surface_v1 itself in that case. Only keep
            // the wrapper around -- and only then does it have live
            // listeners connected -- if that succeeded; otherwise `ls` is
            // already-freed memory and this LayerSurface must not be stored
            // anywhere, or it becomes a zombie holding a dangling pointer.
            auto surface = std::make_unique<LayerSurface>(*this, ls);
            if(surface->valid) { layer_surfaces.push_back(std::move(surface)); }
        });

    // 7b. session lock (ext-session-lock-v1, wrapped by wlroots as
    // wlr_session_lock_manager_v1/wlr_session_lock_v1): a dedicated lock
    // daemon (swaylock and friends) asks to blank and own every output
    // until it says otherwise. The actual "make it locked" work -- per-
    // output backdrops, per-output client surfaces, keyboard-focus
    // handoff, the crash-vs-clean-unlock distinction -- lives in its own
    // SessionLock type (session_lock.{hpp,cpp}) for the same reason
    // LayerSurface/Popup aren't inlined into Server: enough state that
    // inlining it here would bloat this file for a feature only that one
    // type needs to know the internals of.
    session_lock_manager = wlr_session_lock_manager_v1_create(display);
    new_session_lock.connect(
        &session_lock_manager->events.new_lock,
        [this](wlr_session_lock_v1* lock) {
            if(session_lock) {
                // Already locked -- a second lock client (two instances
                // of swaylock racing each other, a buggy autostart that
                // launches one twice) doesn't get to pile on top of the
                // first one's UI. Refuse outright rather than silently
                // replacing it, which would be a strange thing to do to
                // something this security-sensitive. Note this checks
                // session_lock, not session_locked -- see session_lock.hpp
                // for why a lock client that crashed without unlocking
                // (session_locked still true, session_lock already null)
                // must still be replaceable by a new one.
                wlr_log(WLR_ERROR,
                        "session already locked, refusing new lock request");
                wlr_session_lock_v1_destroy(lock);
                return;
            }
            session_locked = true;
            session_lock   = std::make_unique<SessionLock>(*this, lock);
        });

    // 8. input: seat + per-device handling as new_input fires
    seat = wlr_seat_create(display, "seat0");
    input::setupCursor(*this);

    // Pointer lock/confine + the relative-motion channel that rides
    // alongside it -- what Proton/SDL games need for FPS-style mouselook.
    // Must come after setupCursor (which creates server.cursor, read by
    // the clamp logic these route into) but has no other ordering
    // requirement, so it lives right next to the rest of seat/cursor setup.
    pointer_constraints = wlr_pointer_constraints_v1_create(display);
    new_pointer_constraint.connect(
        &pointer_constraints->events.new_constraint,
        [this](wlr_pointer_constraint_v1* constraint) {
            input::newPointerConstraint(*this, constraint);
        });

    relative_pointer_manager = wlr_relative_pointer_manager_v1_create(display);

    new_input.connect(&backend->events.new_input,
                      [this](wlr_input_device* device) {
                          input::newInputDevice(*this, device);
                      });

    request_set_cursor.connect(
        &seat->events.request_set_cursor,
        [this](wlr_seat_pointer_request_set_cursor_event* event) {
            // Only honor the request if it comes from the client that
            // currently has pointer focus -- otherwise an unfocused
            // client could fight over cursor image.
            if(event->seat_client == seat->pointer_state.focused_client) {
                wlr_cursor_set_surface(
                    cursor, event->surface, event->hotspot_x, event->hotspot_y);
            }
        });

    request_set_selection.connect(
        &seat->events.request_set_selection,
        [this](wlr_seat_request_set_selection_event* event) {
            wlr_seat_set_selection(seat, event->source, event->serial);
        });

    // 8b. XWayland, lazily started: no X server process forks until an
    // X11 client actually connects, so this costs nothing for anyone who
    // never runs one. Every X11 window becomes a View (xwayland_view.hpp)
    // exactly like an XdgToplevel -- tiling/focus/animation code never
    // has to know the difference.
    xwayland = wlr_xwayland_create(display, compositor, /*lazy=*/true);
    if(xwayland) {
        new_xwayland_surface.connect(
            &xwayland->events.new_surface, [this](wlr_xwayland_surface* xs) {
                views.push_back(std::make_unique<XWaylandView>(*this, xs));
            });

        // Fires once the lazy X server has actually started (i.e. the
        // first time it's needed). Seat + cursor image can only be set
        // once we're at this point -- setting them any earlier is a
        // no-op at best on most wlroots versions.
        xwayland_ready.connect(&xwayland->events.ready, [this](void*) {
            wlr_xwayland_set_seat(xwayland, seat);

            wlr_xcursor_manager_load(cursor_mgr, 1);
            wlr_xcursor* xcursor =
                wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1);
            if(xcursor && xcursor->image_count > 0) {
                wlr_xcursor_image* image = xcursor->images[0];
                wlr_xwayland_set_cursor(xwayland,
                                        wlr_xcursor_image_get_buffer(image),
                                        image->hotspot_x,
                                        image->hotspot_y);
            }
        });

        setenv("DISPLAY", xwayland->display_name, true);
    } else {
        wlr_log(
            WLR_ERROR,
            "wlr_xwayland_create failed -- continuing without X11 app support");
    }

    // 9. listen on a free Wayland socket, start the backend, run.
    const char* socket = wl_display_add_socket_auto(display);
    if(!socket) {
        wlr_log(WLR_ERROR, "wl_display_add_socket_auto failed");
        return false;
    }

    if(!wlr_backend_start(backend)) {
        wlr_log(WLR_ERROR, "wlr_backend_start failed");
        return false;
    }

    // 9b. wire up session active/inactive now that outputs exist
    if(session) {
        session_active_listener.connect(&session->events.active, [this](void*) {
            session_active = session->active;
            if(session_active) {
                for(auto& out : outputs) {
                    wlr_output_schedule_frame(out->wlr_output);
                }
            }
        });
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    wlr_log(WLR_INFO, "uwuwm running on WAYLAND_DISPLAY=%s", socket);

    return true;
}

void Server::spawnAutostart() {
    // dwl's model: pipe layout/status info to a -s startup command's stdin.
    // uwuwm keeps this even simpler for v1 -- just fork+exec a fixed
    // autostart script if present, same as most minimal compositors do
    // before they grow real IPC. Swap in your bar/wallpaper/etc here.
    const char* home = getenv("HOME");
    if(!home) { return; }
    std::string script = std::string(home) + "/.config/uwuwm/autostart.sh";
    if(access(script.c_str(), X_OK) != 0) { return; }

    pid_t pid = fork();
    if(pid == 0) {
        setsid();
        execl(script.c_str(), script.c_str(), nullptr);
        _exit(127);
    }
}

int Server::run(int argc, char** argv) {
    (void)argc;
    (void)argv;

    wlr_log_init(WLR_DEBUG, nullptr);

    if(!setup()) { return 1; }

    spawnAutostart();

    wl_display_run(display);

    wl_display_destroy_clients(display);
    return 0;
}

// ----------------------------------------------------------------------------
// Focus / multi-output helpers used by input.cpp and layout.cpp
// ----------------------------------------------------------------------------

void Server::focusView(View* view) {
    // Toplevel/XWaylandView call this unconditionally on map and on
    // unmap-picks-next (toplevel.cpp, xwayland_view.cpp) -- neither of
    // those call sites knows or should need to know about session locks.
    // Centralizing the refusal here, once, is what actually closes that
    // gap for all of them (plus the click handler in input.cpp and the
    // alt-tab-style bind in lua_config.cpp) at the same time, rather than
    // needing every call site to remember to check. SessionLockSurface::
    // handleMap is the only thing allowed to grant keyboard focus while
    // session_locked is set.
    if(session_locked) { return; }

    if(focused_view == view) { return; }
    Output* prev_output = focused_view ? focused_view->output : nullptr;

    if(focused_view) { focused_view->setFocused(false); }

    focused_view = view;

    if(!view) {
        wlr_seat_keyboard_clear_focus(seat);
        if(prev_output) { prev_output->updateAdaptiveSync(); }
        return;
    }

    view->setFocused(true);
    if(view->output) { focused_output = view->output; }

    // Raise to top of its layer in stacking order.
    wlr_scene_node_raise_to_top(&view->scene_tree->node);

    wlr_surface* surface = view->wlrSurface();
    if(surface) {
        wlr_keyboard* kb = wlr_seat_get_keyboard(seat);
        if(kb) {
            wlr_seat_keyboard_notify_enter(
                seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
        } else {
            wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
        }
    }
    if(prev_output && prev_output != view->output) {
        prev_output->updateAdaptiveSync();
    }
    if(view->output) { view->output->updateAdaptiveSync(); }
}

Output* Server::outputAt(double lx, double ly) {
    wlr_output* wlr_out = wlr_output_layout_output_at(output_layout, lx, ly);
    if(!wlr_out) { return nullptr; }
    for(auto& out : outputs) {
        if(out->wlr_output == wlr_out) { return out.get(); }
    }
    return nullptr;
}

Output* Server::getFocusedOrDefaultOutput() {
    if(focused_output) { return focused_output; }
    return outputs.empty() ? nullptr : outputs.front().get();
}

void Server::arrangeAllOutputs() {
    for(auto& out : outputs) {
        arrangeLayers(*out);
        layout::arrange(*out);
    }
}

void Server::tickAnimations() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    for(auto& v : views) { v->tickAnimation(now); }
}

void Server::closeOnTag(uint32_t /*tagmask*/) {
    // Placeholder hook for tag-scoped bulk actions (e.g. "close all
    // windows on tag N"); not bound to a key by default. Left as a clean
    // extension point rather than wired up, since closeOnTag-by-default
    // is a destructive action that shouldn't ship bound in config.hpp.
}

void Server::reloadConfig() {
    wlr_log(WLR_INFO, "reloading rc.lua");

    if(!lua_cfg.reload()) {
        // LuaConfig::reload() already logged specifics. A broken script
        // shouldn't take down an already-running compositor the way it's
        // fatal at cold start (Server::setup() bails out entirely) --
        // just keep going with whatever partially applied before the
        // error, same as the old dofile-on-the-same-VM behaviour would
        // have left in place for anything that ran before a mid-script
        // error. gap_px/border_px etc. below still get re-synced from
        // whatever `settings` ended up holding either way.
    }

    // Output-level state that was set once during construction (not read
    // live from lua_cfg.settings each frame) must be refreshed here.
    for(auto& out : outputs) {
        if(const MonitorRule* rule =
               findMonitorRule(*this, out->wlr_output->name)) {
            out->applyRule(*rule);
        }
        out->updateBackground();
    }
    arrangeAllOutputs();

    // Keyboard::Keyboard() only reads repeat_rate/repeat_delay once, at
    // construction -- without this, a repeat-rate change in rc.lua would
    // silently not apply to any keyboard that was already plugged in
    // when you reloaded.
    for(auto& kb : keyboards) {
        wlr_keyboard_set_repeat_info(kb->wlr_keyboard,
                                     lua_cfg.settings.repeat_rate_hz,
                                     lua_cfg.settings.repeat_delay_ms);
    }

    // updateBorderColor() (protected, called via setFocused()) only runs
    // on a focus change, so a border_color_active/inactive edit in rc.lua
    // wouldn't otherwise show up on already-mapped windows until the
    // next time focus moves. Re-asserting each view's current focus
    // state is a cheap way to force that refresh without duplicating
    // setFocused()'s color logic here.
    for(auto& v : views) { v->setFocused(focused_view == v.get()); }
}
