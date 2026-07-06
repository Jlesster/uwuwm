#include "popup.hpp"

#include "layershell.hpp"
#include "view.hpp"

extern "C" {
#include <wlr/types/wlr_compositor.h>
}

#include "server.hpp"

#include <algorithm>

Popup::Popup(Server&         server,
             wlr_xdg_popup*  xdg_popup,
             wlr_scene_tree* parent_tree,
             View*           parent_view,
             Popup*          parent_popup,
             LayerSurface*   parent_layer)
    : server(server), xdg_popup(xdg_popup), parent_view(parent_view),
      parent_popup(parent_popup), parent_layer(parent_layer) {
    scene_tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    xdg_popup->base->data = scene_tree;

    commit.connect(&xdg_popup->base->surface->events.commit,
                   [this](wlr_surface* surface) { handleCommit(surface); });
    destroy.connect(&xdg_popup->events.destroy,
                    [this](void*) { handleDestroy(); });
    reposition.connect(&xdg_popup->events.reposition,
                       [this](void*) { handleReposition(); });
    new_popup.connect(&xdg_popup->base->events.new_popup,
                      [this](wlr_xdg_popup* popup) { handleNewPopup(popup); });
}

Popup::~Popup() = default;

void Popup::handleCommit(wlr_surface* /*surface*/) {
    if(xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(xdg_popup->base);
    }
}

void Popup::handleReposition() {}

void Popup::handleNewPopup(wlr_xdg_popup* popup) {
    auto child =
        std::make_unique<Popup>(server, popup, scene_tree, nullptr, this);
    children.push_back(std::move(child));
}

void Popup::handleDestroy() {
    if(parent_view) {
        parent_view->popups.erase(
            std::remove_if(parent_view->popups.begin(),
                           parent_view->popups.end(),
                           [this](const auto& p) { return p.get() == this; }),
            parent_view->popups.end());
    } else if(parent_popup) {
        parent_popup->children.erase(
            std::remove_if(parent_popup->children.begin(),
                           parent_popup->children.end(),
                           [this](const auto& p) { return p.get() == this; }),
            parent_popup->children.end());
    } else if(parent_layer) {
        parent_layer->popups.erase(
            std::remove_if(parent_layer->popups.begin(),
                           parent_layer->popups.end(),
                           [this](const auto& p) { return p.get() == this; }),
            parent_layer->popups.end());
    }
}
