#pragma once

extern "C" {
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
}

#include "listener.hpp"
#include <vector>
#include <memory>

class Server;
struct View;

// Popups (menus, tooltips, context menus) are short-lived xdg surfaces
// parented to a toplevel or to another popup. wlroots positions them for
// us (wlr_xdg_popup::scheduled geometry already accounts for the
// xdg_positioner constraints the client sent); our job is just to put the
// resulting scene node in the right place in the tree and clean it up.
struct Popup {
    Popup(Server&         server,
          wlr_xdg_popup*  xdg_popup,
          wlr_scene_tree* parent_tree,
          View*           parent_view = nullptr,
          Popup*          parent_popup = nullptr);
    ~Popup();

    Server&         server;
    wlr_xdg_popup*  xdg_popup;
    wlr_scene_tree* scene_tree = nullptr;

    View*           parent_view = nullptr;
    Popup*          parent_popup = nullptr;
    std::vector<std::unique_ptr<Popup>> children;

private:
    void handleCommit(wlr_surface* surface);
    void handleDestroy();
    void handleReposition();
    void handleNewPopup(wlr_xdg_popup* popup);

    Listener<wlr_surface>   commit;
    VoidListener            destroy;
    VoidListener            reposition;
    Listener<wlr_xdg_popup> new_popup;
};
