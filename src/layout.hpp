#pragma once

extern "C" {
#include <wlr/util/box.h>
}

#include <cstdint>

struct Output;

namespace layout {

void arrange(Output& output);
void incMasterFactor(Output& output, double delta);
int  masterCount(Output& output);

// Computes the box a not-yet-mapped tiled window would be assigned if it
// were added to `output` right now, without mutating anything. Lets
// XdgToplevel::handleCommit size a new window's *first* configure correctly
// instead of letting the client pick its own default and potentially
// rendering oversized -- and visually covering whatever's already
// tiled -- until it catches up with a corrective post-map resize.
wlr_box previewTileBox(Output& output);

}  // namespace layout
