#include "decoration.hpp"

extern "C" {
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
}

#include "server.hpp"

#include <memory>

ToplevelDecoration::ToplevelDecoration(
    Server& server, wlr_xdg_toplevel_decoration_v1* decoration)
    : server(server), decoration(decoration) {
    destroy.connect(&decoration->events.destroy,
                    [this](void*) { handleDestroy(); });
    request_mode.connect(&decoration->events.request_mode,
                         [this](void*) { handleRequestMode(); });

    // Defer the actual set_mode() call until the underlying xdg_surface
    // has been initialized (the client's first commit). See the long
    // comment in decoration.hpp for why calling it here would assert.
    commit.connect(&decoration->toplevel->base->surface->events.commit,
                   [this](wlr_surface* surface) { handleCommit(surface); });
}

ToplevelDecoration::~ToplevelDecoration() = default;

void ToplevelDecoration::handleDestroy() {
    server.toplevel_decorations.remove_if(
        [this](const std::unique_ptr<ToplevelDecoration>& d) {
            return d.get() == this;
        });
}

void ToplevelDecoration::handleCommit(wlr_surface* /*surface*/) {
    if(!mode_set && decoration->toplevel->base->initialized) {
        mode_set = true;
        wlr_xdg_toplevel_decoration_v1_set_mode(
            decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
}

void ToplevelDecoration::handleRequestMode() {
    if(!mode_set || !decoration->toplevel->base->initialized) { return; }
    wlr_xdg_toplevel_decoration_v1_set_mode(
        decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void decoration::newToplevelDecoration(Server&                         server,
                                       wlr_xdg_toplevel_decoration_v1* deco) {
    server.toplevel_decorations.push_back(
        std::make_unique<ToplevelDecoration>(server, deco));
}
