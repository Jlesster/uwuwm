#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
}

#include "listener.hpp"

#include <list>
#include <memory>

class Server;
struct Output;
struct SessionLock;
struct wlr_session_lock_v1;
struct wlr_session_lock_surface_v1;

// One output's worth of client-drawn lock UI -- swaylock's password prompt
// for that monitor, or whatever else a lock client renders. Deliberately
// thin, same spirit as Popup: all this owns is the scene plumbing and
// configure/map bookkeeping. It does not decide *when* to take keyboard
// focus beyond "nothing else has it yet" -- see handleMap -- and it does
// not decide what happens on unlock; that's SessionLock's job.
struct SessionLockSurface {
    SessionLockSurface(SessionLock&                 lock,
                       Output&                      output,
                       wlr_session_lock_surface_v1* surface);
    ~SessionLockSurface();

    SessionLock&                 lock;
    Output&                      output;
    wlr_session_lock_surface_v1* surface;
    wlr_scene_tree*              scene_tree = nullptr;
    bool                         mapped     = false;

private:
    void handleMap();
    void handleDestroy();

    VoidListener map_listener;
    VoidListener destroy;
};

// One in-progress or active session lock (wlroots' wrapper around
// ext-session-lock-v1). Owns the always-on-top black backdrop that makes
// the lock real *before* any client pixel is on screen -- see the "why
// locked fires immediately" comment in session_lock.cpp -- and tracks the
// per-output client lock surfaces that draw on top of that backdrop once
// (and if) the lock client gets around to it.
//
// Lifetime: created in Server's new_lock handler and stored in
// Server::session_lock. Destroyed on a clean unlock (handleUnlock erases
// Server::session_lock, which is what actually runs ~SessionLock).
// Server::session_locked is a *separate* bool from "does a SessionLock
// object exist" specifically so that if the lock client dies without
// unlocking (handleDestroy), the compositor can drop this object -- its
// wlr_session_lock_v1* is no longer valid to touch -- while leaving
// session_locked true and the black backdrops up. That fail-safe is the
// entire point of the protocol: a crashing lock screen must not be
// equivalent to no lock screen. A fresh lock request (new SessionLock) is
// still accepted afterward, since session_locked alone (not "session_lock
// != nullptr") is what Server::focusView and Keyboard::handleKey check --
// see server.cpp's new_session_lock handler for the corresponding "already
// locked" refusal, which keys off session_lock, not session_locked.
struct SessionLock {
    SessionLock(Server& server, wlr_session_lock_v1* lock);
    ~SessionLock();

    SessionLock(const SessionLock&)            = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    Server&              server;
    wlr_session_lock_v1* lock;

    // Parent of every per-output black rect and lock surface. Raised to
    // the top of the whole scene graph once at construction and never
    // touched again -- nothing ever needs to out-rank a lock screen, so
    // unlike View's per-window raise-on-focus there's no ongoing
    // maintenance here.
    wlr_scene_tree* tree = nullptr;

    // Owning. Populated by handleNewSurface as the lock client claims
    // outputs one get_lock_surface call at a time; a client is free to
    // never claim some of them (bug, or a monitor that appeared mid-lock
    // faster than the client's output-added handling), which is exactly
    // why the black backdrop in addOutput() exists independently of this
    // list -- an unclaimed output still reads as opaque black, never as
    // "whatever was underneath before the lock."
    std::list<std::unique_ptr<SessionLockSurface>> surfaces;

    // Adds the black backdrop for an output that's present when the lock
    // starts (called once per output from the constructor) or that's
    // plugged in mid-lock (called from Server's new_output handler when
    // session_locked is already true). A monitor hotplugged during a lock
    // must come up black, not showing whatever the tiling layout would
    // otherwise have put there.
    void addOutput(Output& output);

private:
    void handleNewSurface(wlr_session_lock_surface_v1* surface);
    void handleUnlock();
    void handleDestroy();

    Listener<wlr_session_lock_surface_v1> new_surface;
    VoidListener                          unlock;
    VoidListener                          destroy;
};
