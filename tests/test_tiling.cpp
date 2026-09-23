#include "tiling.hpp"

#include <cstdio>
#include <cstdlib>

using tilewm::Box;

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static int area_of(Box b) {
    return b.w * b.h;
}

int main() {
    // Empty / degenerate inputs produce no boxes.
    CHECK(tilewm::master_stack(0, Box{0, 0, 800, 600}).empty());
    CHECK(tilewm::master_stack(3, Box{0, 0, 0, 600}).empty());

    // A single window always takes the whole area.
    {
        auto v = tilewm::master_stack(1, Box{0, 0, 800, 600});
        CHECK(v.size() == 1);
        CHECK(v[0].x == 0 && v[0].y == 0 && v[0].w == 800 && v[0].h == 600);
    }

    // Three windows: 1 master + 2 stack, default mfact 0.55.
    {
        auto v = tilewm::master_stack(3, Box{0, 0, 1000, 800});
        CHECK(v.size() == 3);
        // Master takes the full height on the left.
        CHECK(v[0].x == 0 && v[0].y == 0 && v[0].w == 550 && v[0].h == 800);
        // Stack shares the right column, split vertically with no gaps.
        CHECK(v[1].x == 550 && v[1].y == 0 && v[1].w == 450 && v[1].h == 400);
        CHECK(v[2].x == 550 && v[2].y == 400 && v[2].w == 450 && v[2].h == 400);
        // No pixel loss: columns tile the area exactly.
        CHECK(area_of(v[0]) + area_of(v[1]) + area_of(v[2]) == 1000 * 800);
    }

    // Offset areas are respected.
    {
        auto v = tilewm::master_stack(2, Box{100, 50, 800, 600}, 1, 0.5f);
        CHECK(v.size() == 2);
        CHECK(v[0].x == 100 && v[0].y == 50 && v[0].w == 400 && v[0].h == 600);
        CHECK(v[1].x == 500 && v[1].y == 50 && v[1].w == 400 && v[1].h == 600);
    }

    if (failures == 0) {
        std::puts("test_tiling: all checks passed");
        return 0;
    }
    std::printf("test_tiling: %d check(s) failed\n", failures);
    return 1;
}
