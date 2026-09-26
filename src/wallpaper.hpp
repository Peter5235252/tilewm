#pragma once

// Image decoding for aquawm wallpapers: PNG/JPEG (sniffed by magic bytes)
// decoded to 8-bit RGBA. Pure CPU code, no wlroots types, unit-testable.

#include <cstdint>
#include <string>
#include <vector>

namespace aquawm {

bool decode_image(const char *path, std::vector<uint8_t> &rgba, int &width,
    int &height, std::string &error);

} // namespace aquawm
