#include "layershell.hpp"

extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
}

#include "bar.hpp"
#include "qml_bar.hpp"
#include "layout.hpp"
#include "output.hpp"
#include "popup.hpp"
#include "server.hpp"

LayerSurface::LayerSurface(Server& server, wlr_layer_surface_v1* layer_surface)
    : server(server), layer_surface(layer_surface) {
    layer_surface->data = this;

    // A layer surface with no output specified picks the currently
    // focused one, same as every other compositor does.
    if(!layer_surface->output) {
        layer_surface->output =
            server.focused_output ? server.focused_output->wlr_output : nullptr;
    }

    if(!layer_surface->output) {
        wlr_log(WLR_ERROR, "layer surface with no output available, closing");
        wlr_layer_surface_v1_destroy(layer_surface);
        // `layer_surface` is freed as of the line above -- don't touch
        // it again. No listeners get connected below, and `valid` tells
        // the caller (server.cpp) this object must be discarded, not
        // stored in server.layer_surfaces where it would sit forever as
        // a wrapper around freed memory.
        valid = false;
        return;
    }

    output = static_cast<Output*>(layer_surface->output->data);

    wlr_scene_tree* parent_tree =
        server.layer_tree[layer_surface->pending.layer];
    scene_layer_surface =
        wlr_scene_layer_surface_v1_create(parent_tree, layer_surface);
    scene_layer_surface->tree->node.data = this;

    output->layers[layer_surface->pending.layer].push_back(this);

    map_listener.connect(&layer_surface->surface->events.map,
                         [this](void*) { handleMap(); });
    unmap.connect(&layer_surface->surface->events.unmap,
                  [this](void*) { handleUnmap(); });
    destroy.connect(&layer_surface->events.destroy,
                    [this](void*) { handleDestroy(); });
    commit.connect(&layer_surface->surface->events.commit,
                   [this](wlr_surface* surface) { handleCommit(surface); });
    new_popup.connect(&layer_surface->events.new_popup,
                      [this](wlr_xdg_popup* popup) { handleNewPopup(popup); });
    output_destroy.connect(&layer_surface->output->events.destroy,
                           [this](void*) { handleOutputDestroy(); });
}

LayerSurface::~LayerSurface() {
    if(output) {
        for(auto& list : output->layers) { list.remove(this); }
    }
}

void LayerSurface::handleMap() {
    mapped = true;
    if(output) { arrangeLayers(*output); }
}

void LayerSurface::handleUnmap() {
    mapped = false;
    if(output) { arrangeLayers(*output); }
}

void LayerSurface::handleDestroy() {
    server.layer_surfaces.remove_if(
        [this](const std::unique_ptr<LayerSurface>& ls) {
            return ls.get() == this;
        });
}

void LayerSurface::handleCommit(wlr_surface* /*surface*/) {
    if(!output) { return; }

    uint32_t        committed      = layer_surface->current.layer;
    wlr_scene_tree* desired_parent = server.layer_tree[committed];
    if(scene_layer_surface->tree->node.parent != desired_parent) {
        wlr_scene_node_reparent(&scene_layer_surface->tree->node,
                                desired_parent);
    }

    if(layer_surface->initial_commit || layer_surface->current.committed != 0) {
        arrangeLayers(*output);
    }
}

void LayerSurface::handleNewPopup(wlr_xdg_popup* popup) {
    auto child = std::make_unique<Popup>(
        server, popup, scene_layer_surface->tree, nullptr, nullptr, this);
    popups.push_back(std::move(child));
}

void LayerSurface::handleOutputDestroy() { output = nullptr; }

// ----------------------------------------------------------------------------

void arrangeLayers(Output& output) {
    wlr_box full   = output.layout_box;
    wlr_box usable = full;

    // Native uwu.bar.create()'d bars (bar.hpp) claim their space first,
    // flush against the true output edge -- before any layer-shell
    // client's exclusive zone below gets to stack outward from what's
    // left. Matches a `top`-layer bar (waybar's own default) being the
    // outermost thing on its edge in practice; a native bar and a
    // client layer-shell bar on the same edge still stack correctly
    // either way, it's specifically "which one sits flush against the
    // screen edge" that this ordering decides. reposition() doesn't
    // depend on `usable` (a bar always spans the full output width at
    // its own edge, not whatever's left after other exclusive zones),
    // so calling it here regardless of what shrinks `usable` next is
    // safe.
    for(Bar* bar : output.bars) {
        if(bar->height <= 0) { continue; }
        if(bar->position == BarPosition::Top) {
            usable.y += bar->height;
            usable.height -= bar->height;
        } else {
            usable.height -= bar->height;
        }
        bar->reposition();
    }

    // Same accounting, same reasoning, for uwu.qml.create()'d bars --
    // see the comment above this loop's Bar counterpart.
    for(QmlBar* bar : output.qml_bars) {
        if(bar->height <= 0) { continue; }
        if(bar->position == BarPosition::Top) {
            usable.y += bar->height;
            usable.height -= bar->height;
        } else {
            usable.height -= bar->height;
        }
        bar->reposition();
    }

    // Exclusive-zone accounting, bottom layer up to overlay, same order
    // dwl/sway process it in: each surface that reserves space shrinks
    // `usable` before the next one is positioned, so stacked bars on the
    // same edge stack outward correctly.
    for(int layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
        layer <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        layer++) {
        for(LayerSurface* ls : output.layers[layer]) {
            if(!ls->layer_surface->initialized) { continue; }

            wlr_layer_surface_v1_state& state = ls->layer_surface->current;
            wlr_box                     box{};

            bool stretch_h =
                (state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) &&
                (state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
            bool stretch_v =
                (state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
                (state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);

            box.width  = state.desired_width != 0
                             ? static_cast<int>(state.desired_width)
                             : (stretch_h ? usable.width : 0);
            box.height = state.desired_height != 0
                             ? static_cast<int>(state.desired_height)
                             : (stretch_v ? usable.height : 0);

            // Horizontal placement
            if(stretch_h) {
                box.x = usable.x;
            } else if(state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
                box.x = usable.x + state.margin.left;
            } else if(state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
                box.x =
                    usable.x + usable.width - box.width - state.margin.right;
            } else {
                box.x = usable.x + (usable.width - box.width) / 2;
            }

            // Vertical placement
            if(stretch_v) {
                box.y = usable.y;
            } else if(state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
                box.y = usable.y + state.margin.top;
            } else if(state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
                box.y =
                    usable.y + usable.height - box.height - state.margin.bottom;
            } else {
                box.y = usable.y + (usable.height - box.height) / 2;
            }

            if(box.width <= 0 || box.height <= 0) { continue; }

            wlr_scene_layer_surface_v1_configure(
                ls->scene_layer_surface, &full, &usable);

            if(state.exclusive_zone > 0) {
                int zone = state.exclusive_zone;
                if((state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
                   !stretch_v) {
                    usable.y += zone;
                    usable.height -= zone;
                } else if((state.anchor &
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) &&
                          !stretch_v) {
                    usable.height -= zone;
                } else if((state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) &&
                          !stretch_h) {
                    usable.x += zone;
                    usable.width -= zone;
                } else if((state.anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) &&
                          !stretch_h) {
                    usable.width -= zone;
                }
            }
        }
    }

    output.usable_box = usable;
    layout::arrange(output);
}
