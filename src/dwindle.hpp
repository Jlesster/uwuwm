#pragma once

extern "C" {
#include <wlr/util/box.h>
}

#include <memory>

struct Output;
struct View;

namespace dwindle {
// One node of the per-output BSP tree. A leaf has view != nullptr and no
// children; an internal ("split") node has view == nullptr and both
// children set. Owned exclusively through Output::dwindle_root -- an
// internal node owns its two children, so destroying the root destroys
// the whole tree. Defined here (not hidden in dwindle.cpp) purely so
// Output.hpp's `std::unique_ptr<dwindle::DwindleNode> dwindle_root`
// member has a reachable complete type wherever Output's destructor is
// actually emitted (output.cpp) -- nothing outside dwindle.cpp is meant
// to touch these fields directly.
struct DwindleNode {
    View*                        view   = nullptr;  // non-null only on leaves
    DwindleNode*                 parent = nullptr;  // non-owning
    std::unique_ptr<DwindleNode> children[2];       // null on leaves

    // true: this node's two children stack top/bottom. false: side by
    // side, left/right. Recomputed from box's aspect ratio on every
    // recalc unless RuntimeConfig::dwindle_preserve_split is set --
    // matches Hyprland's default (dwindle:preserve_split = 0), where
    // togglesplit/rotatesplit only "stick" if you turn that on.
    bool split_top = false;

    // children[0]'s share of this node's box along the split axis, as a
    // multiplier of an even split: 1.0 = 50/50, clamped to [0.1, 1.9]
    // same as Hyprland's dwindle:splitratio range.
    float split_ratio = 1.0f;

    wlr_box box{};
};

// The one entry point layout::arrange() calls when output.layout_mode ==
// LayoutMode::Dwindle. Reconciles the persistent per-output tree against
// the output's *current* tiled-view set (a window may have appeared,
// disappeared, or changed tag visibility since the last call -- uwuwm has
// no separate "window opened/closed" tiling hook the way Hyprland's
// onWindowCreatedTiling/onWindowRemovedTiling do), then recomputes every
// leaf's box and applies it via View::setGeometry.
void arrange(Output& output);

// Flips the orientation (left/right <-> top/bottom) of the split directly
// above `view`. No-op if `view` is untiled, alone on its output, or not
// found in the tree. Mirrors Hyprland's "togglesplit" layoutmsg.
void toggleSplit(View* view);

// Swaps the two children of the split directly above `view` (mirrors
// left<->right or top<->bottom without changing the split orientation).
// Mirrors Hyprland's "swapsplit".
void swapSplit(View* view);

// Rotates the split directly above `view` by `angle_deg`, normalized to a
// multiple of 90 -- 90/270 flip the orientation (swapping children on one
// of the two), 180 just swaps children, 0 is a no-op. Mirrors Hyprland's
// "rotatesplit".
void rotateSplit(View* view, int angle_deg = 90);

// Nudges the split ratio of the split directly above `view` by `delta`
// (clamped to Hyprland's [0.1, 1.9] range, where 1.0 is an even split).
// Mirrors Hyprland's "splitratio [+-]N".
void adjustSplitRatio(View* view, float delta);

// Drops `view`'s whole subtree from wherever it is and re-inserts it as a
// child of the tree's root split, i.e. moves it to occupy half the
// output's tiled area directly, however deep it used to be nested. If
// `stable` (the default), the moved node keeps its screen side instead of
// swapping sides with whatever used to be there. Mirrors Hyprland's
// "movetoroot".
void moveToRoot(View* view, bool stable = true);

// Called from Output's destructor (and anywhere else an output's dwindle
// tree needs to be thrown away wholesale, e.g. before a hot output
// removal) to release the tree without walking it view-by-view -- the
// tree owns nothing but itself, so this is just resetting the root
// unique_ptr, but it needs DwindleNode's complete type, which is why it's
// a function here rather than something Output.hpp can do inline.
void destroyTree(Output& output);

}  // namespace dwindle
