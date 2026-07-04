#pragma once

#include "listener.hpp"

class Server;
struct wlr_idle_inhibitor_v1;

// One live zwp_idle_inhibitor_v1 object (idle-inhibit-unstable-v1),
// wrapping the wlroots-side wlr_idle_inhibitor_v1. A client (mpv, a game,
// a video call) holds one of these open for as long as it wants idle
// actions -- whatever an ext-idle-notify-v1 client like swayidle is
// configured to do on timeout -- suppressed.
//
// Unlike input::shortcutsInhibited, which just walks
// wlr_keyboard_shortcuts_inhibit_manager_v1's own inhibitors list on
// every keypress, idle-inhibit needs a stable recompute
// (idle::updateInhibitState) triggered from several different places:
// inhibitor creation/destruction, but also view map/unmap/minimize and
// tag-switch, since a surface can lose visibility without its inhibitor
// ever being destroyed. An owned wrapper list per inhibitor (rather than
// re-deriving lifetime from wlroots' own list) is what gives the destroy
// path above a clean, single place to hang that recompute off of.
struct IdleInhibitor {
    IdleInhibitor(Server& server, wlr_idle_inhibitor_v1* inhibitor);
    ~IdleInhibitor();

    Server&                server;
    wlr_idle_inhibitor_v1* inhibitor;

private:
    void handleDestroy();

    VoidListener destroy;
};

namespace idle {

// Wraps a newly-created wlr_idle_inhibitor_v1 in an IdleInhibitor, owned
// by server.idle_inhibitors, and immediately recomputes inhibit state.
// Called from the idle-inhibit manager's new_inhibitor signal, wired up
// in Server::setup().
void newIdleInhibitor(Server& server, wlr_idle_inhibitor_v1* inhibitor);

// Recomputes whether any live idle-inhibitor's surface currently belongs
// to a visible View -- mapped, not minimized, on its output's active
// tagset, the same test layout::getTiledViews / Output::updateAdaptiveSync
// use -- and pushes the result to wlr_idle_notifier_v1_set_inhibited. A
// surface losing visibility without its inhibitor being destroyed (a
// video paused on a tag you've since switched away from) must not keep
// blocking idle actions forever, which is why this is re-run from every
// place visibility can change, not just from inhibitor
// creation/destruction -- see the call sites in toplevel.cpp /
// xwayland_view.cpp (handleMap/handleUnmap), view.cpp (setMinimized), and
// output.cpp (setTagset). No-op if server.idle_notifier is null.
void updateInhibitState(Server& server);

}  // namespace idle
