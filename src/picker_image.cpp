#include "picker_image.hpp"

#include <algorithm>
#include <limits>

#include "mousepos.hpp"

namespace keyloom {
namespace {
bool contiguous(uint32_t mask) {
    if (mask == 0) return false;
    while ((mask & 1U) == 0) mask >>= 1;
    return (mask & (mask + 1U)) == 0;
}

uint32_t component(unsigned char value, uint32_t mask) {
    if (mask == 0) return 0;
    int shift = 0;
    while (((mask >> shift) & 1U) == 0) ++shift;
    const uint32_t maximum = mask >> shift;
    return static_cast<uint32_t>(((static_cast<uint64_t>(value) * maximum + 127) / 255) << shift) & mask;
}
}

PickerPixels convert_picker_pixels(const std::vector<unsigned char>& rgba, int source_width, int source_height,
                                   int width, int height, const PickerPixelFormat& format) {
    if (source_width <= 0 || source_height <= 0 || width <= 0 || height <= 0 ||
        (format.bits_per_pixel != 16 && format.bits_per_pixel != 24 && format.bits_per_pixel != 32) ||
        (format.scanline_pad != 8 && format.scanline_pad != 16 && format.scanline_pad != 32) ||
        !contiguous(format.red_mask) || !contiguous(format.green_mask) || !contiguous(format.blue_mask) ||
        (format.red_mask & format.green_mask) || (format.red_mask & format.blue_mask) ||
        (format.green_mask & format.blue_mask) ||
        (format.bits_per_pixel < 32 && ((format.red_mask | format.green_mask | format.blue_mask) >> format.bits_per_pixel) != 0))
        throw PickerError("Unsupported X11 screen format for the mouse position picker.");
    const uint64_t source_pixels = static_cast<uint64_t>(source_width) * static_cast<uint64_t>(source_height);
    const uint64_t stride = ((static_cast<uint64_t>(width) * static_cast<uint64_t>(format.bits_per_pixel) +
                              static_cast<uint64_t>(format.scanline_pad - 1)) /
                             static_cast<uint64_t>(format.scanline_pad)) * static_cast<uint64_t>(format.scanline_pad / 8);
    if (source_pixels > rgba.size() / 4 || stride > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        stride > std::numeric_limits<size_t>::max() / static_cast<size_t>(height))
        throw PickerError("Invalid screenshot dimensions for the mouse position picker.");
    PickerPixels result{static_cast<int>(stride), std::vector<uint8_t>(static_cast<size_t>(stride) * static_cast<size_t>(height), 0)};
    const int bytes_per_pixel = format.bits_per_pixel / 8;
    for (int y = 0; y < height; ++y) {
        const int source_y = static_cast<int>(static_cast<int64_t>(y) * source_height / height);
        for (int x = 0; x < width; ++x) {
            const int source_x = static_cast<int>(static_cast<int64_t>(x) * source_width / width);
            const size_t source = (static_cast<size_t>(source_y) * static_cast<size_t>(source_width) + static_cast<size_t>(source_x)) * 4;
            const uint32_t pixel = component(rgba[source], format.red_mask) | component(rgba[source + 1], format.green_mask) |
                                   component(rgba[source + 2], format.blue_mask);
            const size_t dest = static_cast<size_t>(y) * static_cast<size_t>(result.stride) + static_cast<size_t>(x) * static_cast<size_t>(bytes_per_pixel);
            for (int b = 0; b < bytes_per_pixel; ++b) {
                const int shift = format.least_significant_byte_first ? b * 8 : (bytes_per_pixel - 1 - b) * 8;
                result.bytes[dest + static_cast<size_t>(b)] = static_cast<uint8_t>((pixel >> shift) & 0xff);
            }
        }
    }
    return result;
}
}  // namespace keyloom
