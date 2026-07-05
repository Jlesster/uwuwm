#pragma once

extern "C" {
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
}

#include "listener.hpp"

class Server;
struct wlr_pointer_constraint_v1;
struct InputRule;  // lua_config.hpp

// One physical keyboard (or keyboard-capable virtual device). wlroots 0.18+
// lets multiple keyboards exist simultaneously with independent state
// (useful for per-device layouts); we keep it simple and give every
// keyboard the same xkb keymap from the server's locale.
struct Keyboard {
    Keyboard(Server& server, wlr_input_device* device);
    ~Keyboard();

    Server&           server;
    wlr_input_device* device;
    wlr_keyboard*     wlr_keyboard;

private:
    // Returns true if the event was consumed as a compositor keybind (and
    // should NOT be forwarded to the focused client).
    bool handleKeybind(uint32_t keycode, uint32_t modifiers);

    std::vector<xkb_keysym_t> getKeysyms(uint32_t keycode);

    void handleKey(wlr_keyboard_key_event* event);
    void handleModifiers();
    void handleDestroy();

    Listener<wlr_keyboard_key_event> key;
    VoidListener                     modifiers;
    VoidListener                     destroy;
};

// One client-requested pointer lock or confine (zwp_locked_pointer_v1 /
// zwp_confined_pointer_v1), wrapping the wlroots-side
// wlr_pointer_constraint_v1. Exists for as long as the client keeps that object
// open; whether it's currently the seat's *active* constraint (see
// Server::active_constraint) is tracked separately, since a client is free to
// request a lock/confine on a surface well before -- or after -- that surface
// actually has pointer focus.
struct PointerConstraint {
    PointerConstraint(Server&                    server,
                      wlr_pointer_constraint_v1* wlr_constraint);
    ~PointerConstraint();

    Server&                    server;
    wlr_pointer_constraint_v1* wlr_constraint;

private:
    void handleDestroy();
    void handleSetRegion();

    VoidListener destroy;
    VoidListener set_region;
};

// Lightweight tracker for one connected pointer input device (mouse,
// touchpad, trackball -- anything WLR_INPUT_DEVICE_POINTER). wlr_cursor
// already aggregates all of them for motion, so this exists purely so
// uwu.input.set()/uwu.input.list() have a list of live devices to walk
// (server.input_devices) and so a rule can be re-applied the instant it's
// set, not just at plug-in time. Owns nothing beyond the destroy
// listener; the device itself is owned by wlroots' backend.
struct InputDevice {
    InputDevice(Server& server, wlr_input_device* device);

    Server&           server;
    wlr_input_device* device;

private:
    void handleDestroy();

    VoidListener destroy;
};

// Pointer/cursor handling is not one persistent object the way Keyboard is
// -- wlr_cursor already aggregates every pointer device into one logical
// cursor for us (that's the point of wlr_cursor). These are the
// Server-level handlers wired up once in Server::setup().
namespace input {

void setupCursor(Server& server);
void processCursorMotion(Server& server, uint32_t time_msec);
void resetCursorMode(Server& server);
void newInputDevice(Server& server, wlr_input_device* device);

// Recomputes and applies wl_seat capabilities from the current device
// set. Called both when a device is added (newInputDevice) and when the
// last keyboard is removed (Keyboard::handleDestroy) -- previously only
// the add path updated capabilities, so unplugging the only keyboard left
// the seat incorrectly advertising WL_SEAT_CAPABILITY_KEYBOARD forever.
void updateSeatCapabilities(Server& server);

// Wraps a newly-created wlr_pointer_constraint_v1 in a PointerConstraint,
// owned by server.pointer_constraint_wrappers, and activates it
// immediately if its surface already has pointer focus. Called from the
// pointer_constraints manager's new_constraint signal, wired up in
// Server::setup().
void newPointerConstraint(Server&                    server,
                          wlr_pointer_constraint_v1* constraint);

// True if the surface currently holding keyboard focus has an active
// zwp_keyboard_shortcuts_inhibitor_v1 -- i.e. some client has asked for
// its keys to bypass compositor keybind handling and we've granted that
// (see Server::setup, which activates every inhibitor unconditionally).
// Walks server.shortcuts_inhibit_manager's own inhibitor list rather
// than a wrapper of our own, since wlroots already tracks surface +
// active-state for us. Checked from Keyboard::handleKey before Lua
// keybind dispatch.
bool shortcutsInhibited(Server& server);

// Finds the best-matching InputRule for `device` per InputRule's doc
// comment (exact name > type selector > wildcard), or nullptr if no rule
// matches. `is_touchpad` distinguishes the "type:touchpad" vs "type:mouse"
// selector -- see isTouchpad() below for how it's determined.
const InputRule*
findInputRule(Server& server, wlr_input_device* device, bool is_touchpad);

// True if `device` is a libinput touchpad (has a nonzero tap
// finger-count capability), false for anything else including
// non-libinput-backed pointer devices (nested backend, etc.) -- see
// applyInputRule for why a non-libinput device is simply a no-op rather
// than an error.
bool isTouchpad(wlr_input_device* device);

// Applies every has_*-guarded field of `rule` to `device`'s underlying
// libinput_device, skipping (with a WLR_DEBUG log) any field the device
// doesn't support and no-op'ing entirely if `device` isn't
// libinput-backed at all. Called both from newInputDevice (device
// plugged in after the rule already existed) and from uwu.input.set()
// (rule set/changed while the device is already connected).
void applyInputRule(wlr_input_device* device, const InputRule& rule);

}  // namespace input
