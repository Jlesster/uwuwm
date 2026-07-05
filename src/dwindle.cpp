#include "dwindle.hpp"

#include "layout.hpp"
#include "lua_config.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"

#include <algorithm>
#include <vector>

namespace dwindle {

namespace {

DwindleNode* findLeaf(DwindleNode* n, View* v) {
    if(!n) { return nullptr; }
    if(n->view == v) { return n; }
    if(auto* r = findLeaf(n->children[0].get(), v)) { return r; }
    return findLeaf(n->children[1].get(), v);
}

DwindleNode* firstLeaf(DwindleNode* n) {
    if(!n) { return nullptr; }
    if(!n->children[0] && !n->children[1]) { return n; }
    if(auto* r = firstLeaf(n->children[0].get())) { return r; }
    return firstLeaf(n->children[1].get());
}

void collectLeaves(DwindleNode* n, std::vector<DwindleNode*>& out) {
    if(!n) { return; }
    if(!n->children[0] && !n->children[1]) {
        out.push_back(n);
        return;
    }
    collectLeaves(n->children[0].get(), out);
    collectLeaves(n->children[1].get(), out);
}

// The unique_ptr slot in the tree that currently owns `node` -- either
// the output's root, or one of its parent's two children. Needed
// whenever we're about to detach/re-home a node: unique_ptr ownership
// has to move through an actual owning slot, not just the raw pointer.
std::unique_ptr<DwindleNode>& owningSlot(Output& output, DwindleNode* node) {
    if(!node->parent) { return output.dwindle_root; }
    DwindleNode& parent = *node->parent;
    return (parent.children[0].get() == node) ? parent.children[0]
                                              : parent.children[1];
}

// SDwindleNodeData::recalcSizePosRecursive, minus the animation/force
// params (uwuwm's View owns its own tween, driven by setGeometry) and
// the smart_split/precise_mouse_move config checks that gate whether
// split_top gets recomputed here at all -- folded into the single
// `preserve_split` bool (RuntimeConfig::dwindle_preserve_split).
void recalc(DwindleNode* n, bool preserve_split, int gap) {
    if(!n) { return; }

    if(n->children[0]) {
        if(!preserve_split) { n->split_top = n->box.height > n->box.width; }

        if(!n->split_top) {
            // side by side
            int first_w =
                static_cast<int>(n->box.width / 2.0f * n->split_ratio);
            n->children[0]->box = {n->box.x, n->box.y, first_w, n->box.height};
            n->children[1]->box = {n->box.x + first_w,
                                   n->box.y,
                                   n->box.width - first_w,
                                   n->box.height};
        } else {
            // top/bottom
            int first_h =
                static_cast<int>(n->box.height / 2.0f * n->split_ratio);
            n->children[0]->box = {n->box.x, n->box.y, n->box.width, first_h};
            n->children[1]->box = {n->box.x,
                                   n->box.y + first_h,
                                   n->box.width,
                                   n->box.height - first_h};
        }

        recalc(n->children[0].get(), preserve_split, gap);
        recalc(n->children[1].get(), preserve_split, gap);
        return;
    }

    if(!n->view) { return; }

    // Split-per-window gap (bspwm-style: each leaf's box shrinks by `gap`
    // on every side), rather than master-stack's single shared inset --
    // there's no natural "column" to hang a once-per-column gap off of
    // in a BSP tree the way layout.cpp's arrange() does for the master
    // column/stack boundary.
    wlr_box box = n->box;
    box.x += gap;
    box.y += gap;
    box.width  = std::max(1, box.width - gap * 2);
    box.height = std::max(1, box.height - gap * 2);
    n->view->setGeometry(box);
}

// CDwindleAlgorithm::addTarget, default branch only (see the file
// comment above for exactly which config-gated branches got dropped).
void insertLeaf(Output& output, View* view) {
    auto new_leaf  = std::make_unique<DwindleNode>();
    new_leaf->view = view;

    if(!output.dwindle_root) {
        new_leaf->box       = output.usable_box;
        output.dwindle_root = std::move(new_leaf);
        return;
    }

    Server&      server = view->server;
    DwindleNode* opening_on =
        (server.focused_view && server.focused_view != view &&
         server.focused_view->output == &output)
            ? findLeaf(output.dwindle_root.get(), server.focused_view)
            : nullptr;
    if(!opening_on) { opening_on = firstLeaf(output.dwindle_root.get()); }
    if(!opening_on) {
        // Root exists but somehow has no leaves -- shouldn't happen, but
        // don't crash a compositor over a tiling edge case.
        new_leaf->box       = output.usable_box;
        output.dwindle_root = std::move(new_leaf);
        return;
    }

    DwindleNode* old_parent = opening_on->parent;
    // Hold onto the *reference* to the slot that used to own opening_on
    // (output.dwindle_root, or old_parent->children[0 or 1]) rather than
    // just the raw pointer -- we move the new subtree back into this
    // same reference below instead of re-deriving which child index it
    // was, which would be comparing against an already-emptied slot.
    std::unique_ptr<DwindleNode>& opening_on_slot =
        owningSlot(output, opening_on);
    std::unique_ptr<DwindleNode> opening_on_owned = std::move(opening_on_slot);

    auto         new_parent     = std::make_unique<DwindleNode>();
    DwindleNode* new_parent_ptr = new_parent.get();
    new_parent->box             = opening_on->box;
    new_parent->parent          = old_parent;
    new_parent->split_ratio     = 1.0f;

    const bool side_by_side = new_parent->box.width > new_parent->box.height;
    new_parent->split_top   = !side_by_side;

    double cx = server.cursor ? server.cursor->x
                              : new_parent->box.x + new_parent->box.width / 2.0;
    double cy = server.cursor
                    ? server.cursor->y
                    : new_parent->box.y + new_parent->box.height / 2.0;
    bool   on_first_half =
        side_by_side ? cx < new_parent->box.x + new_parent->box.width / 2.0
                     : cy < new_parent->box.y + new_parent->box.height / 2.0;

    if(on_first_half) {
        new_parent->children[0] = std::move(new_leaf);
        new_parent->children[1] = std::move(opening_on_owned);
    } else {
        new_parent->children[0] = std::move(opening_on_owned);
        new_parent->children[1] = std::move(new_leaf);
    }
    new_parent->children[0]->parent = new_parent_ptr;
    new_parent->children[1]->parent = new_parent_ptr;

    // Re-use the same slot reference we emptied above -- this is
    // output.dwindle_root itself when opening_on had no parent, or
    // old_parent->children[0 or 1] otherwise. Re-deriving that index by
    // comparing pointers here (as opposed to reusing the reference)
    // would be comparing against a slot we already moved out of, which
    // is always empty by this point -- see the comment above.
    opening_on_slot = std::move(new_parent);

    recalc(new_parent_ptr,
           output.server.lua_cfg.settings.dwindle_preserve_split,
           output.server.lua_cfg.settings.gap_px);
}

// CDwindleAlgorithm::removeTarget, minus the fullscreen-clearing call
// (View::setFullscreen already yanks a view out of the tiled set before
// this ever runs, via getTiledViews' is_fullscreen check, so there's
// nothing left tiling-side to clear here).
void removeLeaf(Output& output, View* view) {
    DwindleNode* leaf = findLeaf(output.dwindle_root.get(), view);
    if(!leaf) { return; }

    DwindleNode* parent = leaf->parent;
    if(!parent) {
        output.dwindle_root.reset();
        return;
    }

    int sib_idx = (parent->children[0].get() == leaf) ? 1 : 0;

    DwindleNode*                 grandparent = parent->parent;
    std::unique_ptr<DwindleNode> sibling = std::move(parent->children[sib_idx]);
    sibling->parent                      = grandparent;
    sibling->box             = parent->box;  // inherit the freed space outright
    DwindleNode* sibling_ptr = sibling.get();

    if(grandparent) {
        int parent_idx = (grandparent->children[0].get() == parent) ? 0 : 1;
        // Overwriting this slot destroys the unique_ptr that used to own
        // `parent`, which recursively destroys parent's remaining child
        // (`leaf`, still sitting in parent->children[1 - sib_idx]) along
        // with it -- no manual cleanup needed for either.
        grandparent->children[parent_idx] = std::move(sibling);
    } else {
        output.dwindle_root = std::move(sibling);
    }

    recalc(sibling_ptr,
           output.server.lua_cfg.settings.dwindle_preserve_split,
           output.server.lua_cfg.settings.gap_px);
}

// Diffs the tree against the output's live tiled-view set -- see the
// "Changed" section in the file comment above for why this replaces
// Hyprland's onWindowCreatedTiling/onWindowRemovedTiling hooks.
void sync(Output& output) {
    auto tiled = layout::getTiledViews(output);

    std::vector<DwindleNode*> leaves;
    collectLeaves(output.dwindle_root.get(), leaves);
    for(auto* leaf : leaves) {
        if(std::find(tiled.begin(), tiled.end(), leaf->view) == tiled.end()) {
            removeLeaf(output, leaf->view);
        }
    }

    for(auto* v : tiled) {
        if(!findLeaf(output.dwindle_root.get(), v)) { insertLeaf(output, v); }
    }
}

}  // namespace

void arrange(Output& output) {
    sync(output);
    if(!output.dwindle_root) { return; }
    output.dwindle_root->box = output.usable_box;
    recalc(output.dwindle_root.get(),
           output.server.lua_cfg.settings.dwindle_preserve_split,
           output.server.lua_cfg.settings.gap_px);
}

void toggleSplit(View* view) {
    if(!view || !view->output) { return; }
    DwindleNode* leaf = findLeaf(view->output->dwindle_root.get(), view);
    if(!leaf || !leaf->parent) { return; }
    leaf->parent->split_top = !leaf->parent->split_top;
    arrange(*view->output);
}

void swapSplit(View* view) {
    if(!view || !view->output) { return; }
    DwindleNode* leaf = findLeaf(view->output->dwindle_root.get(), view);
    if(!leaf || !leaf->parent) { return; }
    std::swap(leaf->parent->children[0], leaf->parent->children[1]);
    arrange(*view->output);
}

void rotateSplit(View* view, int angle_deg) {
    if(!view || !view->output) { return; }
    DwindleNode* leaf = findLeaf(view->output->dwindle_root.get(), view);
    if(!leaf || !leaf->parent) { return; }

    DwindleNode* parent      = leaf->parent;
    int          normalized  = ((angle_deg / 90) % 4 + 4) % 4;
    bool         should_swap = false;

    switch(normalized) {
        case 0:
            break;
        case 1:
            if(parent->split_top) { should_swap = true; }
            parent->split_top = !parent->split_top;
            break;
        case 2:
            should_swap = true;
            break;
        case 3:
            if(!parent->split_top) { should_swap = true; }
            parent->split_top = !parent->split_top;
            break;
        default:
            break;
    }

    if(should_swap) { std::swap(parent->children[0], parent->children[1]); }
    arrange(*view->output);
}

void adjustSplitRatio(View* view, float delta) {
    if(!view || !view->output) { return; }
    DwindleNode* leaf = findLeaf(view->output->dwindle_root.get(), view);
    if(!leaf || !leaf->parent) { return; }
    leaf->parent->split_ratio =
        std::clamp(leaf->parent->split_ratio + delta, 0.1f, 1.9f);
    arrange(*view->output);
}

void moveToRoot(View* view, bool stable) {
    if(!view || !view->output) { return; }
    Output&      output = *view->output;
    DwindleNode* x      = findLeaf(output.dwindle_root.get(), view);
    if(!x || !x->parent || !x->parent->parent) {
        return;  // no parent (alone), or already a direct child of root
    }

    DwindleNode* parent = x->parent;
    int          x_idx  = (parent->children[0].get() == x) ? 0 : 1;

    DwindleNode* ancestor = parent;
    DwindleNode* root     = parent->parent;
    while(root->parent) {
        ancestor = root;
        root     = root->parent;
    }
    int swap_idx = (root->children[0].get() == ancestor) ? 1 : 0;

    std::swap(parent->children[x_idx], root->children[swap_idx]);
    parent->children[x_idx]->parent  = parent;
    root->children[swap_idx]->parent = root;

    if(stable) { std::swap(root->children[0], root->children[1]); }

    arrange(output);
}

void destroyTree(Output& output) { output.dwindle_root.reset(); }

}  // namespace dwindle
