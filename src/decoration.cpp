#include "decoration.hpp"

extern "C" {
#include <wlr/types/wlr_xdg_decoration_v1.h>
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

    // Assert server-side immediately, before the client's first commit --
    // well-behaved GTK/Qt clients that check the initial configure never
    // even flash a CSD titlebar this way.
    wlr_xdg_toplevel_decoration_v1_set_mode(
        decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

ToplevelDecoration::~ToplevelDecoration() = default;

void ToplevelDecoration::handleDestroy() {
    server.toplevel_decorations.remove_if(
        [this](const std::unique_ptr<ToplevelDecoration>& d) {
            return d.get() == this;
        });
}

void ToplevelDecoration::handleRequestMode() {
    // Ignore decoration->requested_mode entirely -- always re-assert
    // server-side. wlr_xdg_toplevel_decoration_v1_set_mode is a no-op
    // when the mode hasn't actually changed from what's already
    // scheduled, so this stays cheap even for a client that politely
    // re-requests SSD itself.
    wlr_xdg_toplevel_decoration_v1_set_mode(
        decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void decoration::newToplevelDecoration(Server&                         server,
                                       wlr_xdg_toplevel_decoration_v1* deco) {
    server.toplevel_decorations.push_back(
        std::make_unique<ToplevelDecoration>(server, deco));
}
