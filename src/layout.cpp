#include "layout.hpp"

extern "C" {
#include <wlr/util/box.h>
}

#include "dwindle.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>
#include <vector>

namespace layout {

int masterCount(Output& /*output*/) { return 1; }

std::vector<View*> getTiledViews(Output& output) {
    std::vector<View*> tiled;
    for(auto& t : output.server.views) {
        if(t->mapped && !t->is_minimized && t->output == &output &&
           (t->tags & output.tagset) && !t->is_floating && !t->is_fullscreen) {
            tiled.push_back(t.get());
        }
    }
    return tiled;
}

int calcMasterColumnWidth(Output& output, int n_stack, int gap) {
    const wlr_box& area = output.usable_box;
    int master_width = n_stack > 0
                           ? static_cast<int>(area.width * output.master_factor)
                           : area.width;
    return master_width - gap - (n_stack > 0 ? 0 : gap);
}

void incMasterFactor(Output& output, double delta) {
    output.master_factor = std::clamp(output.master_factor + delta, 0.1, 0.9);
    arrange(output);
}

namespace {

void masterStackArrange(Output& output) {
    Server& server = output.server;

    auto tiled = getTiledViews(output);
    if(tiled.empty()) { return; }

    const wlr_box& area = output.usable_box;
    int            gap  = server.lua_cfg.settings.gap_px;
    int            n_master =
        std::min(masterCount(output), static_cast<int>(tiled.size()));
    int n_stack = static_cast<int>(tiled.size()) - n_master;

    int master_width = n_stack > 0
                           ? static_cast<int>(area.width * output.master_factor)
                           : area.width;

    if(n_master > 0) {
        int slot_h = (area.height - gap * (n_master + 1)) / n_master;
        int y      = area.y + gap;
        for(int i = 0; i < n_master; i++) {
            wlr_box box;
            box.x      = area.x + gap;
            box.y      = y;
            box.width  = calcMasterColumnWidth(output, n_stack, gap);
            box.height = slot_h;
            tiled[i]->setGeometry(box);
            y += slot_h + gap;
        }
    }

    if(n_stack > 0) {
        int slot_h  = (area.height - gap * (n_stack + 1)) / n_stack;
        int y       = area.y + gap;
        int stack_x = area.x + master_width + gap;
        int stack_w = area.x + area.width - stack_x - gap;
        for(int i = 0; i < n_stack; i++) {
            wlr_box box;
            box.x      = stack_x;
            box.y      = y;
            box.width  = stack_w;
            box.height = slot_h;
            tiled[n_master + i]->setGeometry(box);
            y += slot_h + gap;
        }
    }
}

}  // namespace

void arrange(Output& output) {
    Server& server = output.server;

    for(auto& t : server.views) {
        if(!t->mapped || t->output != &output) { continue; }
        wlr_scene_node_set_enabled(
            &t->scene_tree->node,
            !t->is_minimized && (t->tags & output.tagset) != 0);
    }

    output.updateAdaptiveSync();

    switch(output.layout_mode) {
        case LayoutMode::MasterStack:
            masterStackArrange(output);
            break;
        case LayoutMode::Dwindle:
            dwindle::arrange(output);
            break;
    }
}

wlr_box previewTileBox(Output& output) {
    Server& server = output.server;

    auto tiled = getTiledViews(output);
    int  count = tiled.size();

    int n_master = std::min(masterCount(output), count + 1);
    int n_stack  = count + 1 - n_master;

    const wlr_box& area = output.usable_box;
    int            gap  = server.lua_cfg.settings.gap_px;
    int master_width = n_stack > 0
                           ? static_cast<int>(area.width * output.master_factor)
                           : area.width;

    wlr_box box{};
    if(count < n_master) {
        int slot_h = (area.height - gap * (n_master + 1)) / n_master;
        box.x      = area.x + gap;
        box.y      = area.y + gap;
        box.width  = calcMasterColumnWidth(output, n_stack, gap);
        box.height = slot_h;
    } else {
        int slot_h  = (area.height - gap * (n_stack + 1)) / n_stack;
        int stack_x = area.x + master_width + gap;
        box.x       = stack_x;
        box.y       = area.y + gap;
        box.width   = area.x + area.width - stack_x - gap;
        box.height  = slot_h;
    }
    return box;
}

}  // namespace layout
