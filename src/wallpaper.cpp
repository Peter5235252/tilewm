// PNG/JPEG decoding via libpng's simplified API and libjpeg-turbo.

#include "wallpaper.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include <jpeglib.h>
#include <png.h>
}

namespace tilewm {
namespace {

bool read_file(const char *path, std::vector<uint8_t> &data, std::string &error) {
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        error = std::string("cannot open ") + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        std::fclose(f);
        error = std::string("suspicious size for ") + path;
        return false;
    }
    data.resize(static_cast<std::size_t>(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        error = std::string("cannot read ") + path;
        return false;
    }
    std::fclose(f);
    return true;
}

bool is_png(const std::vector<uint8_t> &data) {
    static const uint8_t magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return data.size() > 8 && std::memcmp(data.data(), magic, 8) == 0;
}

bool is_jpeg(const std::vector<uint8_t> &data) {
    return data.size() > 2 && data[0] == 0xFF && data[1] == 0xD8;
}

bool decode_png_file(const char *path, std::vector<uint8_t> &rgba, int &width,
    int &height, std::string &error) {
    png_image img;
    std::memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&img, path) == 0) {
        error = std::string("png: ") + img.message;
        return false;
    }
    img.format = PNG_FORMAT_RGBA;
    rgba.resize(PNG_IMAGE_SIZE(img));
    if (png_image_finish_read(&img, nullptr, rgba.data(), 0, nullptr) == 0) {
        error = std::string("png: ") + img.message;
        png_image_free(&img);
        return false;
    }
    width = static_cast<int>(img.width);
    height = static_cast<int>(img.height);
    return true;
}

bool decode_jpeg_mem(const std::vector<uint8_t> &data, std::vector<uint8_t> &rgba,
    int &width, int &height, std::string &error) {
    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data.data(), static_cast<unsigned long>(data.size()));
    if (jpeg_read_header(&cinfo, TRUE) != 1) {
        jpeg_destroy_decompress(&cinfo);
        error = "jpeg: bad header";
        return false;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    width = static_cast<int>(cinfo.output_width);
    height = static_cast<int>(cinfo.output_height);
    rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3;
    std::vector<uint8_t> row(row_stride);
    JSAMPROW rows[1] = {row.data()};
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, rows, 1);
        uint8_t *dst =
            rgba.data() + (static_cast<std::size_t>(cinfo.output_scanline) - 1) *
                              static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            dst[4 * x + 0] = row[3 * x + 0];
            dst[4 * x + 1] = row[3 * x + 1];
            dst[4 * x + 2] = row[3 * x + 2];
            dst[4 * x + 3] = 0xFF;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

} // namespace

bool decode_image(const char *path, std::vector<uint8_t> &rgba, int &width,
    int &height, std::string &error) {
    if (path == nullptr || path[0] == '\0') {
        error = "empty image path";
        return false;
    }
    // PNG goes straight from the file (libpng handles I/O); anything else
    // is sniffed for JPEG after reading it into memory.
    {
        std::vector<uint8_t> probe;
        if (!read_file(path, probe, error)) {
            return false;
        }
        if (is_png(probe)) {
            return decode_png_file(path, rgba, width, height, error);
        }
        if (is_jpeg(probe)) {
            return decode_jpeg_mem(probe, rgba, width, height, error);
        }
    }
    error = std::string("unsupported image format: ") + path;
    return false;
}

} // namespace tilewm
