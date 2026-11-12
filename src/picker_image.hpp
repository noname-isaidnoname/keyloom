#pragma once

#include <cstdint>
#include <vector>

namespace loomwright {

struct PickerPixelFormat {
    int bits_per_pixel;
    int scanline_pad;
    bool least_significant_byte_first;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
};

struct PickerPixels {
    int stride;
    std::vector<uint8_t> bytes;
};

/// Nearest-neighbor conversion of RGBA screenshot data to an X11 ZPixmap.
PickerPixels convert_picker_pixels(const std::vector<unsigned char>& rgba, int source_width, int source_height,
                                   int width, int height, const PickerPixelFormat& format);

}  // namespace loomwright
