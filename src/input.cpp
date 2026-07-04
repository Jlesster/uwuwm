#include "input.hpp"

extern "C" {
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
}

#include "output.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t kBindableMods = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
                                   WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
// Hit-tests the scene graph at layout coordinates (lx, ly). On a hit,
// fills `surface` and surface-local coordinates, and returns the owning
// View if the hit surface belongs to one -- an XdgToplevel or an
// XWaylandView, indistinguishable here and treated identically -- or
// nullptr for layer-shell surfaces, which don't have a View.
View* desktopViewAt(Server&       server,
                    double        lx,
                    double        ly,
                    wlr_surface** surface,
                    double*       sx,
                    double*       sy) {
    wlr_scene_node* node =
        wlr_scene_node_at(&server.scene->tree.node, lx, ly, sx, sy);
    if(!node || node->type != WLR_SCENE_NODE_BUFFER) { return nullptr; }

    wlr_scene_buffer*  scene_buffer = wlr_scene_buffer_from_node(node);
    wlr_scene_surface* scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if(!scene_surface) { return nullptr; }

    *surface = scene_surface->surface;

    // Walk up the tree until we hit a node carrying owner data -- set on
    // every View's scene_tree and on LayerSurface's scene_layer_surface
    // tree. Layer surfaces' data is a LayerSurface*, not a View*, so this
    // returns nullptr for them deliberately; layer-shell surfaces are not
    // focusable/movable the way views are.
    wlr_scene_tree* tree = node->parent;
    while(tree && !tree->node.data) { tree = tree->node.parent; }
    if(!tree) { return nullptr; }

    // Distinguish View* from LayerSurface* by checking which list owns
    // the scene_tree -- cheaper and safer than relying on layout, since we
    // don't have RTTI-friendly polymorphism here by design (see view.hpp).
    for(auto& t : server.views) {
        if(t->scene_tree == tree) { return t.get(); }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

Keyboard::Keyboard(Server& server, wlr_input_device* device)
    : server(server), device(device),
      wlr_keyboard(wlr_keyboard_from_input_device(device)) {
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap*  keymap  = xkb_keymap_new_from_names(
        context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    wlr_keyboard_set_repeat_info(wlr_keyboard,
                                 server.lua_cfg.settings.repeat_rate_hz,
                                 server.lua_cfg.settings.repeat_delay_ms);

    key.connect(&wlr_keyboard->events.key,
                [this](wlr_keyboard_key_event* event) { handleKey(event); });
    modifiers.connect(&wlr_keyboard->events.modifiers,
                      [this](void*) { handleModifiers(); });
    destroy.connect(&device->events.destroy,
                    [this](void*) { handleDestroy(); });

    wlr_seat_set_keyboard(server.seat, wlr_keyboard);
}

Keyboard::~Keyboard() = default;

void Keyboard::handleDestroy() {
    Server& srv = server;  // copy the reference out before `this` goes away
    srv.keyboards.remove_if(
        [this](const std::unique_ptr<Keyboard>& k) { return k.get() == this; });
    input::updateSeatCapabilities(srv);
}

void Keyboard::handleModifiers() {
    wlr_seat_set_keyboard(server.seat, wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(server.seat, &wlr_keyboard->modifiers);
}

std::vector<xkb_keysym_t> Keyboard::getKeysyms(uint32_t keycode) {
    xkb_keymap*        keymap = xkb_state_get_keymap(wlr_keyboard->xkb_state);
    xkb_layout_index_t layout = xkb_state_key_get_layout(wlr_keyboard->xkb_state, keycode + 8);
    const xkb_keysym_t* syms;
    int                 n = xkb_keymap_key_get_syms_by_level(
        keymap, keycode + 8, layout, /*level=*/0, &syms);

    if(n <= 0) return {};
    return std::vector<xkb_keysym_t>(syms, syms + n);
}

bool Keyboard::handleKeybind(uint32_t keycode, uint32_t modifiers_state) {
    auto syms = getKeysyms(keycode);
    uint32_t relevant_mods = modifiers_state & kBindableMods;

    for(auto sym : syms) {
        auto range = server.lua_cfg.keybinds.equal_range(static_cast<uint32_t>(sym));
        for(auto it = range.first; it != range.second; ++it) {
            const auto& bind = it->second;
            if(bind.mods == relevant_mods) {
                server.lua_cfg.invoke(bind.fn_ref);
                return true;
            }
        }
    }

    if(!syms.empty()) {
        wlr_log(WLR_DEBUG,
                "no keybind for keysym 0x%x mods 0x%x (%zu bound)",
                syms[0],
                relevant_mods,
                server.lua_cfg.keybinds.size());
    }
    return false;
}

void Keyboard::handleKey(wlr_keyboard_key_event* event) {
    uint32_t modifiers_state = wlr_keyboard_get_modifiers(wlr_keyboard);
    bool     handled         = false;

    if(event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // VT switching: Ctrl+Alt+F1-F12 is always handled by the
        // compositor before any Lua keybinds, matching the standard
        // Linux console / Xorg behaviour. Nested sessions (no DRM)
        // skip this because server.session == nullptr.
        if(server.session &&
           (modifiers_state & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) ==
               (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
            for(auto sym : getKeysyms(event->keycode)) {
                if(sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) {
                    unsigned vt = static_cast<unsigned>(sym - XKB_KEY_F1 + 1);
                    if(wlr_session_change_vt(server.session, vt)) {
                        handled = true;
                    } else {
                        wlr_log(WLR_ERROR, "VT switch to tty%u failed", vt);
                    }
                    break;
                }
            }
        }
        if(!handled) {
            handled = handleKeybind(event->keycode, modifiers_state);
        }
    }

    if(!handled) {
        wlr_seat_set_keyboard(server.seat, wlr_keyboard);
        wlr_seat_keyboard_notify_key(
            server.seat, event->time_msec, event->keycode, event->state);
    }
}

// ---------------------------------------------------------------------------
// Cursor / pointer
// ---------------------------------------------------------------------------

namespace input {

void setupCursor(Server& server) {
    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    server.cursor_mgr = wlr_xcursor_manager_create(
        nullptr, static_cast<uint32_t>(server.lua_cfg.settings.cursor_size));

    // All pointer motion ultimately funnels through wlr_cursor's signals
    // regardless of which physical device generated it, so we only ever
    // need to hook these once here, not per-device.
    server.cursor_motion.connect(&server.cursor->events.motion,
                                [&server](wlr_pointer_motion_event* event) {
                                    wlr_cursor_move(server.cursor,
                                                &event->pointer->base,
                                                event->delta_x,
                                                event->delta_y);
                                    processCursorMotion(server, event->time_msec);
                                });

    server.cursor_motion_absolute.connect(
        &server.cursor->events.motion_absolute,
        [&server](wlr_pointer_motion_absolute_event* event) {
            wlr_cursor_warp_absolute(
                server.cursor, &event->pointer->base, event->x, event->y);
            processCursorMotion(server, event->time_msec);
        });

    server.cursor_button.connect(
        &server.cursor->events.button,
        [&server](wlr_pointer_button_event* event) {
            wlr_seat_pointer_notify_button(
                server.seat, event->time_msec, event->button, event->state);

            if(event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
                if(server.cursor_mode != CursorMode::Passthrough) {
                    resetCursorMode(server);
                }
                return;
            }

            wlr_surface* surface = nullptr;
            double       sx, sy;
            View*        v = desktopViewAt(
                server, server.cursor->x, server.cursor->y, &surface, &sx, &sy);
            // Unmanaged (X11 override-redirect) windows -- menus,
            // tooltips, DND icons -- don't take keyboard focus on click,
            // same as every other compositor.
            if(v && !v->unmanaged) { server.focusView(v); }
        });

    server.cursor_axis.connect(&server.cursor->events.axis,
                             [&server](wlr_pointer_axis_event* event) {
                                 wlr_seat_pointer_notify_axis(server.seat,
                                                               event->time_msec,
                                                               event->orientation,
                                                               event->delta,
                                                               event->delta_discrete,
                                                               event->source,
                                                               event->relative_direction);
                             });

    server.cursor_frame.connect(&server.cursor->events.frame, [&server](void*) {
        wlr_seat_pointer_notify_frame(server.seat);
    });
    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
}

void processCursorMotion(Server& server, uint32_t time_msec) {
    if(server.cursor_mode == CursorMode::Move && server.grabbed_view) {
        View*   t   = server.grabbed_view;
        wlr_box box = t->geo;
        box.x       = static_cast<int>(server.cursor->x - server.grab_x);
        box.y       = static_cast<int>(server.cursor->y - server.grab_y);
        t->setGeometry(box);
        return;
    }

    if(server.cursor_mode == CursorMode::Resize && server.grabbed_view) {
        View*  t   = server.grabbed_view;
        double dx  = server.cursor->x - server.grab_x;
        double    dy  = server.cursor->y - server.grab_y;
        wlr_box   box = server.grab_geobox;

        if(server.resize_edges & WLR_EDGE_TOP) {
            box.y += static_cast<int>(dy);
            box.height -= static_cast<int>(dy);
        } else if(server.resize_edges & WLR_EDGE_BOTTOM) {
            box.height += static_cast<int>(dy);
        }
        if(server.resize_edges & WLR_EDGE_LEFT) {
            box.x += static_cast<int>(dx);
            box.width -= static_cast<int>(dx);
        } else if(server.resize_edges & WLR_EDGE_RIGHT) {
            box.width += static_cast<int>(dx);
        }

        int min_dim = 1 + 2 * server.lua_cfg.settings.border_px;
        box.width   = std::max(box.width, min_dim);
        box.height  = std::max(box.height, min_dim);
        t->setGeometry(box);
        return;
    }

    // Passthrough: forward enter/motion to whatever's under the cursor.
    wlr_surface* surface = nullptr;
    double       sx = 0, sy = 0;
    View*        v = desktopViewAt(
        server, server.cursor->x, server.cursor->y, &surface, &sx, &sy);

    if(!surface) {
        wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server.seat);
        return;
    }

    (void)v;
    wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(server.seat, time_msec, sx, sy);
}

void resetCursorMode(Server& server) {
    server.cursor_mode  = CursorMode::Passthrough;
    server.grabbed_view = nullptr;
}

void updateSeatCapabilities(Server& server) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if(!server.keyboards.empty()) { caps |= WL_SEAT_CAPABILITY_KEYBOARD; }
    wlr_seat_set_capabilities(server.seat, caps);
}

void newInputDevice(Server& server, wlr_input_device* device) {
    switch(device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            server.keyboards.push_back(
                std::make_unique<Keyboard>(server, device));
            break;
        case WLR_INPUT_DEVICE_POINTER:
        case WLR_INPUT_DEVICE_TOUCH:
            wlr_cursor_attach_input_device(server.cursor, device);
            break;
        default:
            break;
    }

    updateSeatCapabilities(server);
}

}  // namespace input
