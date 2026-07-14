#include "input.hpp"

extern "C" {
#include <libinput.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
}

#include "lua_config.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t kBindableMods = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
                                   WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;

// ext-idle-notify-v1: resets every bound client's idle timer. Called from
// every real input event below (key press/release, all four cursor
// signals) regardless of whether the event went on to do anything else --
// an idle daemon shouldn't care whether a keypress hit a compositor bind
// or got forwarded to a client, only that *something* happened. No-op
// pre-Server::setup-completing or if idle_notifier was never created.
void notifyIdleActivity(Server& server) {
    if(server.idle_notifier) {
        wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
    }
}
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

// Finds the View wrapping a given wlr_surface, or nullptr if it isn't one
// (layer-shell surface, unmapped, etc.) -- same "walk server.views and
// compare" approach desktopViewAt uses to distinguish View from
// LayerSurface, just keyed by surface identity instead of scene node.
View* viewForSurface(Server& server, wlr_surface* surface) {
    if(!surface) { return nullptr; }
    for(auto& v : server.views) {
        if(v->wlrSurface() == surface) { return v.get(); }
    }
    return nullptr;
}

// True exactly when wl_pointer.motion delivery and cursor warping must
// both be suppressed for ordinary (non-grab) pointer motion -- i.e. a
// LOCKED constraint is active and we're not in the middle of a
// compositor-initiated move/resize grab. Confine (CONFINED) does *not*
// count: the cursor still moves and motion is still sent, just clamped
// into the constraint's region -- see clampCursorToConstraint below.
bool pointerLockActive(Server& server) {
    return server.active_constraint &&
           server.cursor_mode == CursorMode::Passthrough &&
           server.active_constraint->wlr_constraint->type ==
               WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

// If a CONFINED constraint is active, clamps server.cursor->{x,y} back
// into its region. No-op for LOCKED (the cursor doesn't move at all while
// locked, so there's nothing to clamp) and for an empty region (a client
// asking to confine to nothing is nonsensical; treat as unconfined rather
// than freezing the cursor at whatever point it last happened to be).
void clampCursorToConstraint(Server& server) {
    if(!server.active_constraint) { return; }
    wlr_pointer_constraint_v1* c = server.active_constraint->wlr_constraint;
    if(c->type != WLR_POINTER_CONSTRAINT_V1_CONFINED) { return; }
    if(!pixman_region32_not_empty(&c->region)) { return; }

    // The region is in surface-local coordinates; server.cursor->{x,y}
    // are layout (global) coordinates. content_tree is positioned at
    // exactly the surface's on-screen origin (border offset already
    // baked in -- see view.hpp), so wlr_scene_node_coords on it gives the
    // exact conversion between the two spaces.
    int   origin_x = 0, origin_y = 0;
    View* v = viewForSurface(server, c->surface);
    if(v) {
        wlr_scene_node_coords(&v->content_tree->node, &origin_x, &origin_y);
    }

    double sx = server.cursor->x - origin_x;
    double sy = server.cursor->y - origin_y;

    if(pixman_region32_contains_point(
           &c->region, static_cast<int>(sx), static_cast<int>(sy), nullptr)) {
        return;
    }

    // Outside the region: clamp to its bounding box. Exact for the
    // overwhelmingly common case of a single rectangular region (which is
    // what every known game/toolkit actually requests); an approximation
    // for a disjoint or concave region, since pixman doesn't expose a
    // "nearest point in region" query and that shape is vanishingly rare
    // in practice for this protocol.
    const pixman_box32_t& b = c->region.extents;
    sx                      = std::clamp(
        sx, static_cast<double>(b.x1), static_cast<double>(b.x2 - 1));
    sy = std::clamp(
        sy, static_cast<double>(b.y1), static_cast<double>(b.y2 - 1));

    wlr_cursor_warp(server.cursor, nullptr, origin_x + sx, origin_y + sy);
}

void deactivateActiveConstraint(Server& server) {
    if(!server.active_constraint) { return; }
    wlr_pointer_constraint_v1* wlr_constraint =
        server.active_constraint->wlr_constraint;
    // Clear first: wlr_pointer_constraint_v1_send_deactivated may destroy
    // `wlr_constraint` outright for a oneshot-lifetime constraint, which
    // would otherwise leave server.active_constraint dangling for the
    // instant before its own destroy handler gets a chance to clear it.
    server.active_constraint = nullptr;
    wlr_pointer_constraint_v1_send_deactivated(wlr_constraint);
}

void activateConstraint(Server& server, PointerConstraint* pc) {
    if(server.active_constraint == pc) { return; }
    if(server.active_constraint) { deactivateActiveConstraint(server); }

    server.active_constraint = pc;
    wlr_pointer_constraint_v1_send_activated(pc->wlr_constraint);

    // Locking guarantees (per the protocol spec) the pointer is already
    // within bounds at activation; confining only recommends warping if
    // it's outside. Doing it unconditionally here is a no-op for the
    // common already-inside case and avoids a client-visible
    // jump-then-clamp on the very next motion event otherwise.
    clampCursorToConstraint(server);
}

}  // namespace

// ---------------------------------------------------------------------------
// PointerConstraint
// ---------------------------------------------------------------------------

PointerConstraint::PointerConstraint(Server&                    server,
                                     wlr_pointer_constraint_v1* wlr_constraint)
    : server(server), wlr_constraint(wlr_constraint) {
    wlr_constraint->data = this;

    destroy.connect(&wlr_constraint->events.destroy,
                    [this](void*) { handleDestroy(); });
    set_region.connect(&wlr_constraint->events.set_region,
                       [this](void*) { handleSetRegion(); });
}

PointerConstraint::~PointerConstraint() {
    if(server.active_constraint == this) { server.active_constraint = nullptr; }
}

void PointerConstraint::handleDestroy() {
    Server& srv = server;  // copy the reference out before `this` goes away
    srv.pointer_constraint_wrappers.remove_if(
        [this](const std::unique_ptr<PointerConstraint>& p) {
            return p.get() == this;
        });
}

void PointerConstraint::handleSetRegion() {
    // Nothing to cache: clampCursorToConstraint reads wlr_constraint->region
    // directly at clamp time, so an updated region takes effect on the very
    // next motion event with nothing to invalidate here. Kept as an
    // explicit (empty) handler rather than not listening at all, so
    // future region-dependent behavior -- e.g. immediately re-clamping if
    // the cursor is left outside a newly-shrunk region -- has an obvious
    // place to go.
}

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
    xkb_layout_index_t layout =
        xkb_state_key_get_layout(wlr_keyboard->xkb_state, keycode + 8);
    const xkb_keysym_t* syms;
    int                 n = xkb_keymap_key_get_syms_by_level(
        keymap, keycode + 8, layout, /*level=*/0, &syms);

    if(n <= 0) return {};
    return std::vector<xkb_keysym_t>(syms, syms + n);
}

bool Keyboard::handleKeybind(uint32_t keycode, uint32_t modifiers_state) {
    auto     syms          = getKeysyms(keycode);
    uint32_t relevant_mods = modifiers_state & kBindableMods;

    for(auto sym : syms) {
        auto range =
            server.lua_cfg.keybinds.equal_range(static_cast<uint32_t>(sym));
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
    notifyIdleActivity(server);

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
        // A client holding an active keyboard-shortcuts-inhibitor for
        // whichever surface currently has focus (remote-desktop viewer,
        // VM console, a game that wants Alt+Tab for itself) gets every
        // key raw -- skip Lua keybind dispatch entirely rather than
        // stealing binds out from under it. VT-switch above is
        // deliberately exempt: it's a session-level escape hatch, not a
        // compositor keybind the inhibiting client could plausibly want
        // routed to itself, and losing it under a frozen/misbehaving
        // client would turn "reach for another TTY" into "reboot".
        if(!handled && !input::shortcutsInhibited(server)) {
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
// InputDevice (pointer device tracking for uwu.input.*)
// ---------------------------------------------------------------------------

InputDevice::InputDevice(Server& server, wlr_input_device* device)
    : server(server), device(device) {
    destroy.connect(&device->events.destroy,
                    [this](void*) { handleDestroy(); });
}

void InputDevice::handleDestroy() {
    Server& srv = server;  // copy the reference out before `this` goes away
    srv.input_devices.remove_if([this](const std::unique_ptr<InputDevice>& d) {
        return d.get() == this;
    });
}

// ---------------------------------------------------------------------------
// Cursor / pointer
// ---------------------------------------------------------------------------

namespace input {

// True if `device`'s libinput backing reports a nonzero tap
// finger-count -- the standard way to distinguish a touchpad from a
// mouse/trackball, since libinput doesn't expose a direct "is this a
// touchpad" query. Non-libinput-backed devices (nested Wayland backend)
// report false here; they fall through to the "type:mouse"/wildcard
// selectors below rather than "type:touchpad", which is the safer
// default for something we can't actually identify.
bool isTouchpad(wlr_input_device* device) {
    if(!wlr_input_device_is_libinput(device)) { return false; }
    libinput_device* dev = wlr_libinput_get_device_handle(device);
    return libinput_device_config_tap_get_finger_count(dev) > 0;
}

const InputRule*
findInputRule(Server& server, wlr_input_device* device, bool is_touchpad) {
    const char* type_selector = is_touchpad ? "type:touchpad" : "type:mouse";
    const InputRule* exact    = nullptr;
    const InputRule* by_type  = nullptr;
    const InputRule* wildcard = nullptr;

    for(const auto& rule : server.lua_cfg.input_rules) {
        if(rule.match == device->name) {
            exact = &rule;
        } else if(rule.match == type_selector) {
            by_type = &rule;
        } else if(rule.match == "*") {
            wildcard = &rule;
        }
    }
    return exact ? exact : (by_type ? by_type : wildcard);
}

namespace {

// Every setter below follows the same shape: skip entirely if the rule
// didn't set this field, then skip (with a debug log, not an error --
// "this touchpad has no scroll wheel to configure natural-scroll on" is
// routine, not a misconfiguration) if libinput itself reports the device
// doesn't support the knob at all.
void applyTap(libinput_device* dev, const InputRule& r) {
    if(!r.has_tap) { return; }
    if(libinput_device_config_tap_get_finger_count(dev) == 0) {
        wlr_log(WLR_DEBUG, "input device has no tap support, ignoring tap=");
        return;
    }
    libinput_device_config_tap_set_enabled(
        dev,
        r.tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
}

void applyTapDrag(libinput_device* dev, const InputRule& r) {
    if(!r.has_tap_drag) { return; }
    libinput_device_config_tap_set_drag_enabled(
        dev,
        r.tap_drag ? LIBINPUT_CONFIG_DRAG_ENABLED
                   : LIBINPUT_CONFIG_DRAG_DISABLED);
}

void applyTapDragLock(libinput_device* dev, const InputRule& r) {
    if(!r.has_tap_drag_lock) { return; }
    libinput_device_config_tap_set_drag_lock_enabled(
        dev,
        r.tap_drag_lock ? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED
                        : LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
}

void applyNaturalScroll(libinput_device* dev, const InputRule& r) {
    if(!r.has_natural_scroll) { return; }
    if(!libinput_device_config_scroll_has_natural_scroll(dev)) {
        wlr_log(WLR_DEBUG,
                "input device has no natural-scroll support, ignoring "
                "natural_scroll=");
        return;
    }
    libinput_device_config_scroll_set_natural_scroll_enabled(dev,
                                                             r.natural_scroll);
}

void applyDwt(libinput_device* dev, const InputRule& r) {
    if(!r.has_dwt) { return; }
    if(!libinput_device_config_dwt_is_available(dev)) {
        wlr_log(WLR_DEBUG,
                "input device has no disable-while-typing support, "
                "ignoring dwt=");
        return;
    }
    libinput_device_config_dwt_set_enabled(
        dev,
        r.dwt ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
}

void applyLeftHanded(libinput_device* dev, const InputRule& r) {
    if(!r.has_left_handed) { return; }
    if(!libinput_device_config_left_handed_is_available(dev)) {
        wlr_log(WLR_DEBUG,
                "input device has no left-handed support, ignoring "
                "left_handed=");
        return;
    }
    libinput_device_config_left_handed_set(dev, r.left_handed);
}

void applyMiddleEmulation(libinput_device* dev, const InputRule& r) {
    if(!r.has_middle_emulation) { return; }
    if(!libinput_device_config_middle_emulation_is_available(dev)) {
        wlr_log(WLR_DEBUG,
                "input device has no middle-emulation support, ignoring "
                "middle_emulation=");
        return;
    }
    libinput_device_config_middle_emulation_set_enabled(
        dev,
        r.middle_emulation ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
                           : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
}

void applyAccelSpeed(libinput_device* dev, const InputRule& r) {
    if(!r.has_accel_speed) { return; }
    if(!libinput_device_config_accel_is_available(dev)) {
        wlr_log(WLR_DEBUG,
                "input device has no pointer-accel support, ignoring "
                "accel_speed=");
        return;
    }
    libinput_device_config_accel_set_speed(dev, r.accel_speed);
}

void applyAccelProfile(libinput_device* dev, const InputRule& r) {
    if(!r.has_accel_profile) { return; }
    auto profile = static_cast<libinput_config_accel_profile>(r.accel_profile);
    if(!(libinput_device_config_accel_get_profiles(dev) & profile)) {
        wlr_log(WLR_DEBUG,
                "input device doesn't support requested accel_profile, "
                "ignoring");
        return;
    }
    libinput_device_config_accel_set_profile(dev, profile);
}

void applyScrollMethod(libinput_device* dev, const InputRule& r) {
    if(!r.has_scroll_method) { return; }
    auto method = static_cast<libinput_config_scroll_method>(r.scroll_method);
    if(!(libinput_device_config_scroll_get_methods(dev) & method)) {
        wlr_log(WLR_DEBUG,
                "input device doesn't support requested scroll_method, "
                "ignoring");
        return;
    }
    libinput_device_config_scroll_set_method(dev, method);
    // on-button-down scrolling needs a button to hold; scroll_button is
    // applied right after so a single uwu.input.set() call with both
    // fields set works in one shot instead of needing a second call.
    if(method == LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN && r.has_scroll_button) {
        libinput_device_config_scroll_set_button(dev, r.scroll_button);
    }
}

void applyClickMethod(libinput_device* dev, const InputRule& r) {
    if(!r.has_click_method) { return; }
    auto method = static_cast<libinput_config_click_method>(r.click_method);
    if(!(libinput_device_config_click_get_methods(dev) & method)) {
        wlr_log(WLR_DEBUG,
                "input device doesn't support requested click_method, "
                "ignoring");
        return;
    }
    libinput_device_config_click_set_method(dev, method);
}

}  // namespace

void applyInputRule(wlr_input_device* device, const InputRule& rule) {
    if(!wlr_input_device_is_libinput(device)) {
        // Not libinput-backed (nested Wayland backend, virtual pointer,
        // etc.) -- there's no config surface to apply to, and that's
        // expected/routine while developing nested, not an error.
        wlr_log(WLR_DEBUG,
                "input device '%s' has no libinput backing, ignoring rule",
                device->name);
        return;
    }
    libinput_device* dev = wlr_libinput_get_device_handle(device);

    applyTap(dev, rule);
    applyTapDrag(dev, rule);
    applyTapDragLock(dev, rule);
    applyNaturalScroll(dev, rule);
    applyDwt(dev, rule);
    applyLeftHanded(dev, rule);
    applyMiddleEmulation(dev, rule);
    applyAccelSpeed(dev, rule);
    applyAccelProfile(dev, rule);
    applyScrollMethod(dev, rule);
    applyClickMethod(dev, rule);
}

void setupCursor(Server& server) {
    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    // nullptr (cursor_theme unset) falls through to
    // wlr_xcursor_manager_create's own default, which itself reads
    // XCURSOR_THEME from the environment if present -- so this is purely
    // additive: uwu.set("cursor_theme", "Qogir") in rc.lua now works
    // whether or not the session that launched uwuwm ever exported
    // XCURSOR_THEME itself (many display managers/session scripts don't).
    const std::string& cursor_theme = server.lua_cfg.settings.cursor_theme;
    server.cursor_mgr               = wlr_xcursor_manager_create(
        cursor_theme.empty() ? nullptr : cursor_theme.c_str(),
        static_cast<uint32_t>(server.lua_cfg.settings.cursor_size));

    // All pointer motion ultimately funnels through wlr_cursor's signals
    // regardless of which physical device generated it, so we only ever
    // need to hook these once here, not per-device.
    server.cursor_motion.connect(
        &server.cursor->events.motion,
        [&server](wlr_pointer_motion_event* event) {
            notifyIdleActivity(server);

            // While a LOCKED constraint is active, the
            // on-screen cursor must not move at all --
            // only the relative-motion channel below
            // carries movement to the client. A
            // CONFINED constraint still moves the
            // cursor, just clamped afterwards.
            if(!pointerLockActive(server)) {
                wlr_cursor_move(server.cursor,
                                &event->pointer->base,
                                event->delta_x,
                                event->delta_y);
                clampCursorToConstraint(server);
            }

            // Sent unconditionally: independent of
            // any lock/confine, this is a no-op
            // unless some client has actually bound
            // zwp_relative_pointer_v1 for the
            // currently-focused pointer resource --
            // wlroots handles that lookup internally.
            if(server.relative_pointer_manager) {
                wlr_relative_pointer_manager_v1_send_relative_motion(
                    server.relative_pointer_manager,
                    server.seat,
                    static_cast<uint64_t>(event->time_msec) * 1000,
                    event->delta_x,
                    event->delta_y,
                    event->unaccel_dx,
                    event->unaccel_dy);
            }

            processCursorMotion(server, event->time_msec);
        });

    server.cursor_motion_absolute.connect(
        &server.cursor->events.motion_absolute,
        [&server](wlr_pointer_motion_absolute_event* event) {
            notifyIdleActivity(server);

            if(!pointerLockActive(server)) {
                wlr_cursor_warp_absolute(
                    server.cursor, &event->pointer->base, event->x, event->y);
                clampCursorToConstraint(server);
            }
            processCursorMotion(server, event->time_msec);
        });

    server.cursor_button.connect(
        &server.cursor->events.button,
        [&server](wlr_pointer_button_event* event) {
            notifyIdleActivity(server);

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

    server.cursor_axis.connect(
        &server.cursor->events.axis, [&server](wlr_pointer_axis_event* event) {
            notifyIdleActivity(server);

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
        View*   t   = server.grabbed_view;
        double  dx  = server.cursor->x - server.grab_x;
        double  dy  = server.cursor->y - server.grab_y;
        wlr_box box = server.grab_geobox;

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

    // uwu.set("focus_follows_mouse", true) -- off by default (uwuwm is
    // click-to-focus otherwise, same as the cursor_button handler above).
    // Deliberately checked unconditionally on every motion event rather
    // than only on a surface change: focusView() already no-ops the
    // moment `v == server.focused_view` (the overwhelmingly common case
    // -- most motion events happen *within* the already-focused window),
    // so there's no real cost to skip by special-casing "did the
    // hovered view change" ourselves here too. Unmanaged surfaces (menus,
    // tooltips, DND icons) are excluded, same as the click-to-focus path
    // -- hovering a dropdown shouldn't steal focus from the window
    // behind it. Hovering bare desktop (v == nullptr, gaps/wallpaper)
    // intentionally leaves whatever was last focused alone, matching
    // dwm/Awesome's sloppy-focus behavior, rather than clearing focus
    // to none.
    if(server.lua_cfg.settings.focus_follows_mouse && v && !v->unmanaged) {
        server.focusView(v);
    }

    if(!surface) {
        if(server.active_constraint) { deactivateActiveConstraint(server); }
        wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server.seat);
        return;
    }

    // Constraint (de)activation tracks pointer focus, checked only on an
    // actual focus change rather than every motion event -- cheap, and
    // the common case (already-focused surface, no constraint churn)
    // does zero extra work per motion.
    if(server.pointer_constraints &&
       surface != server.seat->pointer_state.focused_surface) {
        if(wlr_pointer_constraint_v1* c =
               wlr_pointer_constraints_v1_constraint_for_surface(
                   server.pointer_constraints, surface, server.seat)) {
            activateConstraint(server,
                               static_cast<PointerConstraint*>(c->data));
        } else if(server.active_constraint &&
                  server.active_constraint->wlr_constraint->surface !=
                      surface) {
            deactivateActiveConstraint(server);
        }
    }

    wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);

    // Per the pointer-constraints spec: "While the lock ... is active, the
    // wl_pointer objects of the associated seat will not emit any
    // wl_pointer.motion events." Enter is still fine (nothing about focus
    // changed), just not motion.
    if(!pointerLockActive(server)) {
        wlr_seat_pointer_notify_motion(server.seat, time_msec, sx, sy);
    }
}

void resetCursorMode(Server& server) {
    server.cursor_mode  = CursorMode::Passthrough;
    server.grabbed_view = nullptr;
}

bool shortcutsInhibited(Server& server) {
    if(!server.shortcuts_inhibit_manager) { return false; }

    wlr_surface* focused = server.seat->keyboard_state.focused_surface;
    if(!focused) { return false; }

    // wlroots keeps every live inhibitor -- regardless of surface or
    // active-state -- on the manager's own `inhibitors` list, so we walk
    // it directly instead of tracking a parallel wrapper list the way
    // PointerConstraint does below. This only runs once per keypress on
    // the compositor-keybind path (not the hot per-motion pointer path),
    // and the list is realistically one or two entries long, so a linear
    // scan costs nothing worth optimizing.
    wlr_keyboard_shortcuts_inhibitor_v1* inhibitor;
    wl_list_for_each(
        inhibitor, &server.shortcuts_inhibit_manager->inhibitors, link) {
        if(inhibitor->surface == focused && inhibitor->active) { return true; }
    }
    return false;
}

void newPointerConstraint(Server&                    server,
                          wlr_pointer_constraint_v1* wlr_constraint) {
    server.pointer_constraint_wrappers.push_back(
        std::make_unique<PointerConstraint>(server, wlr_constraint));
    PointerConstraint* pc = server.pointer_constraint_wrappers.back().get();

    // Activate immediately if its surface already has pointer focus --
    // the common case, since a client almost always requests the
    // lock/confine right after it already has pointer focus on its own
    // surface (e.g. reacting to a click). If it doesn't have focus yet,
    // activation happens later from processCursorMotion's passthrough
    // path once focus actually lands on this surface.
    if(server.seat->pointer_state.focused_surface == wlr_constraint->surface) {
        activateConstraint(server, pc);
    }
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
        case WLR_INPUT_DEVICE_POINTER: {
            wlr_cursor_attach_input_device(server.cursor, device);
            server.input_devices.push_back(
                std::make_unique<InputDevice>(server, device));
            bool touchpad = isTouchpad(device);
            if(const InputRule* rule =
                   findInputRule(server, device, touchpad)) {
                applyInputRule(device, *rule);
            }
            break;
        }
        case WLR_INPUT_DEVICE_TOUCH:
            wlr_cursor_attach_input_device(server.cursor, device);
            break;
        default:
            break;
    }

    updateSeatCapabilities(server);
}

}  // namespace input
