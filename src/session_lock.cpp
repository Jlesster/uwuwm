#include "session_lock.hpp"

extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>
}

#include "output.hpp"
#include "server.hpp"

// ----------------------------------------------------------------------------
// SessionLockSurface
// ----------------------------------------------------------------------------

SessionLockSurface::SessionLockSurface(SessionLock&                 lock,
                                       Output&                      output,
                                       wlr_session_lock_surface_v1* surface)
    : lock(lock), output(output), surface(surface) {
    surface->data = this;

    // A plain wlr_surface, not an xdg_surface -- wlr_scene_subsurface_tree_
    // create (not wlr_scene_xdg_surface_create) is the right helper, same
    // as popup.hpp would use for a bare surface if popups didn't ride
    // along on xdg_surface's own scene helper. wlroots ties this tree's
    // lifetime to the underlying wlr_surface's own destruction, same as
    // LayerSurface's scene_layer_surface -- we don't explicitly destroy it
    // ourselves below, for the same reason layershell.cpp doesn't.
    scene_tree = wlr_scene_subsurface_tree_create(lock.tree, surface->surface);
    wlr_scene_node_set_position(
        &scene_tree->node, output.layout_box.x, output.layout_box.y);

    output.lock_surface = this;

    // Full output size, not usable_box -- a lock surface has to cover
    // whatever a layer-shell bar would otherwise be reserving space for
    // too. There is no "exclusive zone" concept for a security boundary.
    wlr_session_lock_surface_v1_configure(
        surface,
        static_cast<uint32_t>(output.layout_box.width),
        static_cast<uint32_t>(output.layout_box.height));

    map_listener.connect(&surface->events.map, [this](void*) { handleMap(); });
    destroy.connect(&surface->events.destroy,
                    [this](void*) { handleDestroy(); });
}

SessionLockSurface::~SessionLockSurface() {
    if(output.lock_surface == this) { output.lock_surface = nullptr; }
}

void SessionLockSurface::handleMap() {
    mapped = true;

    // First mapped lock surface across the whole lock claims keyboard
    // focus if nothing already has it. SessionLock's constructor clears
    // focus before any of these exist, so this only ever fires once per
    // lock in the common case (one output, or a client that maps all its
    // surfaces close together) -- if a client maps several outputs'
    // surfaces at different times, later ones deliberately leave focus
    // alone rather than snatching it from whichever one the user is
    // already typing into.
    if(!lock.server.seat->keyboard_state.focused_surface) {
        wlr_seat_keyboard_notify_enter(
            lock.server.seat, surface->surface, nullptr, 0, nullptr);
    }
}

void SessionLockSurface::handleDestroy() {
    lock.surfaces.remove_if(
        [this](const std::unique_ptr<SessionLockSurface>& s) {
            return s.get() == this;
        });
}

// ----------------------------------------------------------------------------
// SessionLock
// ----------------------------------------------------------------------------

SessionLock::SessionLock(Server& server, wlr_session_lock_v1* lock)
    : server(server), lock(lock) {
    // Always above every layer-shell layer, including OVERLAY. A bar that
    // asks for OVERLAY is still just a bar; a lock screen is a security
    // boundary and has to win against literally everything else in the
    // scene, unconditionally, for as long as this object exists.
    tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_scene_node_raise_to_top(&tree->node);

    for(auto& out : server.outputs) { addOutput(*out); }

    // The security-relevant half of "locked" -- nothing pre-lock visible,
    // input no longer reaching any pre-lock client -- is already true the
    // instant the backdrops above are up and Server::session_locked (set
    // by the caller in the new_session_lock handler, immediately before
    // this constructor runs) starts gating focusView and Lua keybinds.
    // That's independent of whether the lock client has drawn anything
    // yet, so `locked` is sent right here instead of waiting on the first
    // new_surface/map -- a lock client that's slow to paint its password
    // prompt still means a genuinely blanked, input-safe screen in the
    // meantime, not a gap.
    wlr_session_lock_v1_send_locked(lock);

    // Nothing pre-lock should still hold keyboard focus once the
    // backdrops are up; the first SessionLockSurface to map claims it
    // from here (see SessionLockSurface::handleMap).
    wlr_seat_keyboard_clear_focus(server.seat);

    new_surface.connect(
        &lock->events.new_surface,
        [this](wlr_session_lock_surface_v1* s) { handleNewSurface(s); });
    unlock.connect(&lock->events.unlock, [this](void*) { handleUnlock(); });
    destroy.connect(&lock->events.destroy, [this](void*) { handleDestroy(); });
}

SessionLock::~SessionLock() {
    // Children first, then the parent tree -- each SessionLockSurface's
    // own scene node is torn down (via the underlying wlr_surface, per
    // its constructor's comment) while `tree` is still alive to be its
    // parent. Destroying `tree` first would leave those objects holding
    // pointers into an already-freed subtree.
    surfaces.clear();

    for(auto& out : server.outputs) {
        out->lock_backdrop = nullptr;  // freed by the tree destroy below
        out->lock_surface  = nullptr;  // already null; redundant safety
    }

    if(tree) { wlr_scene_node_destroy(&tree->node); }
}

void SessionLock::addOutput(Output& output) {
    static constexpr float kOpaqueBlack[4] = {0.f, 0.f, 0.f, 1.f};

    output.lock_backdrop = wlr_scene_rect_create(
        tree, output.layout_box.width, output.layout_box.height, kOpaqueBlack);
    wlr_scene_node_set_position(
        &output.lock_backdrop->node, output.layout_box.x, output.layout_box.y);
    wlr_scene_node_raise_to_top(&output.lock_backdrop->node);
}

void SessionLock::handleNewSurface(wlr_session_lock_surface_v1* surface) {
    // Same ->data recovery convention as LayerSurface's output resolution
    // (layershell.cpp) -- set once in Output's constructor.
    auto* output = static_cast<Output*>(surface->output->data);
    if(!output) {
        wlr_log(WLR_ERROR,
                "session lock surface for an output uwuwm isn't tracking, "
                "ignoring");
        return;
    }

    if(output->lock_surface) {
        wlr_log(WLR_ERROR,
                "output %s already has a session lock surface, replacing it",
                output->wlr_output->name);
    }

    surfaces.push_back(
        std::make_unique<SessionLockSurface>(*this, *output, surface));
}

void SessionLock::handleUnlock() {
    // Clean shutdown: the lock client is satisfied (correct password,
    // etc.) and is releasing the session. Server::session_locked flips
    // back off *before* this object goes away so nothing in
    // ~SessionLock's cleanup (or anything it triggers, like a scene
    // node destroy re-entering event handling) can be mistaken for
    // still-locked state.
    server.session_locked = false;

    // Restore whatever had keyboard focus before the lock, the same way
    // reloadConfig() re-asserts current state after a config change --
    // there's no separate "pre-lock focus" stash, focusView already knows
    // how to re-enter a surface from focused_view.
    if(server.focused_view) {
        server.focused_view->setFocused(true);
        wlr_surface* s = server.focused_view->wlrSurface();
        if(s) {
            wlr_seat_keyboard_notify_enter(server.seat, s, nullptr, 0, nullptr);
        }
    }

    // Drops this object. Nothing below this line may touch `this`,
    // `lock`, `tree`, or `surfaces` again -- see Server::session_lock's
    // declaration comment for why erasing it here (rather than, say,
    // deferring to an idle callback) is safe even though we're inside a
    // callback owned by the object being destroyed: the destroy signal
    // this handler is connected to has already fully fired by the time
    // wlroots calls us, so there's nothing left downstream of `lock` for
    // ~SessionLock to race against.
    server.session_lock.reset();
}

void SessionLock::handleDestroy() {
    // The lock object is going away. If handleUnlock already ran, this is
    // just the normal end-of-life destroy notification for an object
    // Server::session_lock no longer owns (handleUnlock already reset it)
    // -- nothing to do.
    //
    // If handleUnlock did NOT already run, the lock client died (crashed,
    // was killed, lost its Wayland connection) without ever unlocking.
    // Per the protocol's fail-safe intent, that must NOT be treated as an
    // unlock: session_locked stays true and the black backdrops stay up
    // forever (or until a fresh lock client successfully replaces them --
    // see the "already locked" check in server.cpp's new_session_lock
    // handler, which only looks at session_lock, not session_locked, so a
    // new client can still come in and cover the now-orphaned backdrops
    // with a working prompt). We only drop the now-invalid
    // wlr_session_lock_v1* itself.
    if(server.session_lock.get() == this) {
        wlr_log(WLR_ERROR,
                "session lock client exited without unlocking -- staying "
                "locked");

        // Deliberately leak `this`: release() detaches it from
        // server.session_lock WITHOUT invoking ~SessionLock, because
        // ~SessionLock's whole job is tearing down tree/surfaces/
        // backdrops, which is exactly what must NOT happen here -- the
        // backdrops are the only thing keeping the screen from reverting
        // to whatever was underneath. This is the one intentional
        // exception to every other handleDestroy in this codebase, which
        // clean up unconditionally; a one-time, one-object leak per
        // lock-client crash is a deliberate trade against the far worse
        // failure mode of a lock screen that un-blanks itself when its
        // process dies. `this` (and therefore `surfaces`) staying alive
        // in memory also means SessionLockSurface::handleDestroy below
        // is still safe to call for any individual surface that gets
        // destroyed later -- lock.surfaces.remove_if still operates on a
        // live list, it just never shrinks to zero and gets cleaned up.
        //
        // A subsequent, well-behaved lock client can still supersede
        // this: server.session_lock is nullptr again after release(), so
        // server.cpp's new_session_lock "already locked" check (which
        // keys off session_lock, not session_locked) will accept it, and
        // its SessionLock will raise a fresh set of backdrops on top of
        // these orphaned ones -- covering them rather than needing to
        // reclaim them.
        server.session_lock.release();
    }
}
