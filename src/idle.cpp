#include "idle.hpp"

extern "C" {
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
}

#include "output.hpp"
#include "server.hpp"
#include "view.hpp"

#include <memory>

IdleInhibitor::IdleInhibitor(Server& server, wlr_idle_inhibitor_v1* inhibitor)
    : server(server), inhibitor(inhibitor) {
    destroy.connect(&inhibitor->events.destroy,
                    [this](void*) { handleDestroy(); });
}

IdleInhibitor::~IdleInhibitor() = default;

void IdleInhibitor::handleDestroy() {
    // Same self-destruction-inside-your-own-destroy-handler idiom
    // ToplevelDecoration::handleDestroy uses -- safe because wl_signal's
    // emit walks its listener list with the "safe" variant, tolerant of
    // the current node being removed (which is exactly what ~IdleInhibitor
    // does when it tears down `destroy` below) mid-callback.
    Server& srv = server;
    srv.idle_inhibitors.remove_if(
        [this](const std::unique_ptr<IdleInhibitor>& i) {
            return i.get() == this;
        });
    idle::updateInhibitState(srv);
}

void idle::newIdleInhibitor(Server& server, wlr_idle_inhibitor_v1* inhibitor) {
    server.idle_inhibitors.push_back(
        std::make_unique<IdleInhibitor>(server, inhibitor));
    updateInhibitState(server);
}

void idle::updateInhibitState(Server& server) {
    if(!server.idle_notifier) { return; }

    // Deliberately walks our own owned wrapper list, not
    // idle_inhibit_manager->inhibitors directly -- avoids any assumption
    // about whether wlroots has already unlinked a given inhibitor from
    // its own list by the time our destroy handler above runs.
    bool any_visible = false;
    for(auto& wrapper : server.idle_inhibitors) {
        wlr_surface* target = wrapper->inhibitor->surface;
        for(auto& v : server.views) {
            if(v->wlrSurface() != target) { continue; }
            if(v->mapped && !v->is_minimized && v->output &&
               (v->tags & v->output->tagset) != 0) {
                any_visible = true;
            }
            break;
        }
        if(any_visible) { break; }
    }

    wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, any_visible);
}
