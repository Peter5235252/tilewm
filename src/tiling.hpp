#pragma once

// Pure tiling geometry for tilewm: no wlroots types here on purpose,
// so this header stays unit-testable on any machine.
//
// master-stack layout: the first `nmaster` windows share the left
// "master" column (stacked vertically), the rest share the right
// "stack" column (also stacked vertically). `mfact` controls the
// width fraction of the master column when both columns are occupied.

#include <vector>

namespace tilewm {

struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

inline std::vector<Box> master_stack(int count, Box area, int nmaster = 1, float mfact = 0.55f) {
    std::vector<Box> out;
    if (count <= 0 || area.w <= 0 || area.h <= 0) {
        return out;
    }
    if (nmaster < 1) {
        nmaster = 1;
    }
    if (mfact <= 0.05f) {
        mfact = 0.05f;
    }
    if (mfact >= 0.95f) {
        mfact = 0.95f;
    }

    const int nmaster_clamped = count < nmaster ? count : nmaster;
    const int nstack = count - nmaster_clamped;

    int master_w = area.w;
    int stack_x = area.x;
    if (nstack > 0) {
        master_w = static_cast<int>(area.w * mfact);
        stack_x = area.x + master_w;
    }
    const int stack_w = area.w - master_w;

    out.reserve(static_cast<std::size_t>(count));

    // Master column: split vertically among nmaster_clamped windows,
    // handing remainder pixels to the first windows so nothing is lost.
    for (int i = 0; i < nmaster_clamped; ++i) {
        const int h = area.h / nmaster_clamped;
        const int extra = (i < area.h % nmaster_clamped) ? 1 : 0;
        const int y = area.y + i * h + (i < area.h % nmaster_clamped ? i : area.h % nmaster_clamped);
        out.push_back(Box{area.x, y, master_w, h + extra});
    }

    // Stack column: same vertical split for the remaining windows.
    for (int i = 0; i < nstack; ++i) {
        const int h = area.h / nstack;
        const int y = area.y + i * h + (i < area.h % nstack ? i : area.h % nstack);
        const int extra = (i < area.h % nstack) ? 1 : 0;
        out.push_back(Box{stack_x, y, stack_w, h + extra});
    }

    return out;
}

} // namespace tilewm
