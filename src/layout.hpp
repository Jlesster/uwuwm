#pragma once

extern "C" {
#include <wlr/util/box.h>
}

#include <cstdint>
#include <vector>

struct Output;
struct View;

namespace layout {

// Which tiling algorithm arrange() uses for a given output. Per-output
// (not global) so different monitors can run different layouts, same as
// master_factor already is.
enum class LayoutMode { MasterStack, Dwindle };

void arrange(Output& output);
void incMasterFactor(Output& output, double delta);
int  masterCount(Output& output);

// Every mapped, non-minimized, non-floating, non-fullscreen view on
// `output` whose tags overlap its currently-viewed tagset -- i.e.
// exactly the set either tiling algorithm is responsible for placing.
// Shared between arrange() (master-stack) and dwindle.cpp so both
// algorithms agree on what "tiled" means without duplicating the filter.
std::vector<View*> getTiledViews(Output& output);

// Computes the box a not-yet-mapped tiled window would be assigned if it
// were added to `output` right now, without mutating anything. Lets
// XdgToplevel::handleCommit size a new window's *first* configure correctly
// instead of letting the client pick its own default and potentially
// rendering oversized -- and visually covering whatever's already
// tiled -- until it catches up with a corrective post-map resize.
//
// Master-stack only -- dwindle's insertion point depends on which leaf
// the cursor is over and which window is focused, neither of which is
// known ahead of a real insert, so a new dwindle window's first configure
// just uses whatever size the client requests and gets corrected on the
// next arrange() after it maps, same as it would on any tiling WM whose
// layout is genuinely insertion-order/cursor-dependent.
wlr_box previewTileBox(Output& output);

}  // namespace layout
