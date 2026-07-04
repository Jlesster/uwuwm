#pragma once

extern "C" {
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
}

#include "listener.hpp"

class Server;
struct wlr_pointer_constraint_v1;

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

}  // namespace input
