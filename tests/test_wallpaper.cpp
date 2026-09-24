#include "wallpaper.hpp"

#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

int main() {
    // Decodes the real shipped asset (tests run with CWD=build/).
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        std::string err;
        CHECK(tilewm::decode_image("../assets/wallpaper.jpg", rgba, w, h, err));
        CHECK(w > 1000 && h > 500);
        CHECK(rgba.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        // Opaque JPEG: every alpha byte must be 0xFF.
        bool opaque = true;
        for (std::size_t i = 3; i < rgba.size(); i += 4) {
            if (rgba[i] != 0xFF) {
                opaque = false;
                break;
            }
        }
        CHECK(opaque);
    }

    // Missing and garbage inputs fail cleanly.
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        std::string err;
        CHECK(!tilewm::decode_image("/nonexistent.jpg", rgba, w, h, err));
        CHECK(!err.empty());
        CHECK(!tilewm::decode_image("", rgba, w, h, err));
    }

    if (failures == 0) {
        std::puts("test_wallpaper: all checks passed");
        return 0;
    }
    std::printf("test_wallpaper: %d check(s) failed\n", failures);
    return 1;
}
