#include "harness.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cli.hpp"
#include "mousepos.hpp"
#include "picker_image.hpp"

using namespace keyloom;

TEST_CASE(coordinate_format_matches_loom_syntax) {
    CHECK_EQ(format_coordinates(640, 420), std::string("640, 420"));
}

TEST_CASE(picker_pixels_convert_rgb565_in_both_byte_orders) {
    const std::vector<unsigned char> rgba{255, 0, 0, 255, 0, 0, 255, 255};
    PickerPixelFormat format{16, 32, true, 0xf800, 0x07e0, 0x001f};
    auto result = convert_picker_pixels(rgba, 2, 1, 2, 1, format);
    CHECK_EQ(result.stride, 4);
    CHECK_TRUE(result.bytes == std::vector<uint8_t>({0x00, 0xf8, 0x1f, 0x00}));
    format.least_significant_byte_first = false;
    result = convert_picker_pixels(rgba, 2, 1, 2, 1, format);
    CHECK_TRUE(result.bytes == std::vector<uint8_t>({0xf8, 0x00, 0x00, 0x1f}));
}

TEST_CASE(picker_pixels_respect_padding_and_nearest_neighbor_scaling) {
    const std::vector<unsigned char> rgba{255, 0, 0, 255, 0, 0, 255, 255};
    PickerPixelFormat format{24, 32, false, 0xff0000, 0x00ff00, 0x0000ff};
    const auto result = convert_picker_pixels(rgba, 2, 1, 3, 1, format);
    CHECK_EQ(result.stride, 12);
    CHECK_TRUE(result.bytes == std::vector<uint8_t>({255, 0, 0, 255, 0, 0, 0, 0, 255, 0, 0, 0}));
    format.bits_per_pixel = 32;
    format.least_significant_byte_first = true;
    const auto little = convert_picker_pixels(rgba, 2, 1, 2, 1, format);
    CHECK_TRUE(little.bytes == std::vector<uint8_t>({0, 0, 255, 0, 255, 0, 0, 0}));
}

TEST_CASE(picker_pixels_reject_invalid_sizes_and_formats) {
    const std::vector<unsigned char> rgba{255, 0, 0, 255};
    PickerPixelFormat format{16, 32, true, 0xf800, 0x07e0, 0x001f};
    CHECK_THROWS(convert_picker_pixels(rgba, 2, 1, 2, 1, format), PickerError);
    CHECK_THROWS(convert_picker_pixels(rgba, 1, 1, 0, 1, format), PickerError);
    format.green_mask = format.red_mask;
    CHECK_THROWS(convert_picker_pixels(rgba, 1, 1, 1, 1, format), PickerError);
    format.bits_per_pixel = 8;
    CHECK_THROWS(convert_picker_pixels(rgba, 1, 1, 1, 1, format), PickerError);
}

TEST_CASE(tooltip_prefers_below_and_right) {
    const auto position = tooltip_position(100, 100, 1368, 768, 80, 30);
    CHECK_EQ(position.first, 118);
    CHECK_EQ(position.second, 118);
}

TEST_CASE(tooltip_flips_when_it_would_leave_the_screen) {
    // Near the right edge: move the tooltip to the left of the cursor.
    const auto right = tooltip_position(1360, 100, 1368, 768, 80, 30);
    CHECK_EQ(right.first, 1262);
    CHECK_EQ(right.second, 118);
    // Near the bottom edge: move the tooltip above the cursor.
    const auto bottom = tooltip_position(100, 760, 1368, 768, 80, 30);
    CHECK_EQ(bottom.first, 118);
    CHECK_EQ(bottom.second, 712);
}

TEST_CASE(tooltip_and_pointer_clamp_to_the_screen) {
    const auto clamped = clamp_to_screen(-5, 9999, 1368, 768);
    CHECK_EQ(clamped.first, 0);
    CHECK_EQ(clamped.second, 767);
    const auto position = tooltip_position(0, 0, 60, 40, 80, 30);
    CHECK_EQ(position.first, 0);
    CHECK_EQ(position.second, 0);
}

TEST_CASE(screenshot_commands_prefer_spectacle) {
    const ExecutableFinder finder = [](const std::string& name) { return "/usr/bin/" + name; };
    const auto commands = screenshot_commands("/tmp/shot.png", finder);
    CHECK_EQ(commands.size(), static_cast<size_t>(5));
    CHECK_EQ(commands.at(0), std::vector<std::string>({"spectacle", "-b", "-n", "-f", "--scaled", "-o",
                                                       "/tmp/shot.png"}));
    CHECK_EQ(commands.at(1), std::vector<std::string>({"grim", "/tmp/shot.png"}));
    CHECK_EQ(commands.at(4), std::vector<std::string>({"import", "-window", "root", "/tmp/shot.png"}));
}

TEST_CASE(screenshot_commands_are_empty_without_tools) {
    const ExecutableFinder finder = [](const std::string&) { return std::string(); };
    CHECK_TRUE(screenshot_commands("/tmp/shot.png", finder).empty());
    CHECK_TRUE(available_screenshot_tools(finder).empty());
}

TEST_CASE(cli_parses_the_picker_flag) {
    const CliArgs args = parse_cli({"--get-mouse-pos"});
    CHECK_TRUE(args.get_mouse_pos);
    CHECK_TRUE(!args.run.has_value());
}

TEST_CASE(cli_rejects_conflicting_modes_and_bad_backends) {
    CHECK_THROWS(parse_cli({"--check", "a.loom", "--run", "b.loom"}), CliUsageError);
    CHECK_THROWS(parse_cli({"--output-backend", "tape"}), CliUsageError);
    CHECK_THROWS(parse_cli({"--run"}), CliUsageError);
}

TEST_CASE(cli_accepts_docs_topics_and_script_shorthand) {
    const CliArgs docs = parse_cli({"--docs", "events"});
    CHECK_EQ(*docs.docs, std::string("events"));
    const CliArgs bare = parse_cli({"--docs"});
    CHECK_EQ(*bare.docs, std::string("all"));
    const CliArgs script = parse_cli({"demo"});
    CHECK_EQ(*script.script, std::string("demo"));
}

TEST_CASE(capture_desktop_reports_missing_tools) {
    const ExecutableFinder finder = [](const std::string&) { return std::string(); };
    try {
        (void)capture_desktop(finder);
        CHECK_TRUE(false);  // unreachable: capture_desktop must throw
    } catch (const PickerError& error) {
        CHECK_TRUE(std::string(error.what()).find("screenshot tool") != std::string::npos);
    }
}

TEST_CASE(cli_prints_picked_position) {
    const auto captured = keyloom_test::capture_output([] {
        return run_cli({"--get-mouse-pos"}, [](bool) -> std::optional<std::pair<int, int>> {
            return std::pair<int, int>{12, 34};
        });
    });
    CHECK_EQ(captured.code, 0);
    CHECK_EQ(keyloom_test::strip_trailing_newlines(captured.out), std::string("12, 34"));
}

TEST_CASE(cli_cancel_returns_130) {
    const auto captured = keyloom_test::capture_output([] {
        return run_cli({"--get-mouse-pos"}, [](bool) -> std::optional<std::pair<int, int>> {
            return std::nullopt;
        });
    });
    CHECK_EQ(captured.code, 130);
    CHECK_TRUE(captured.err.find("cancelled") != std::string::npos);
}

TEST_CASE(cli_reports_picker_startup_errors) {
    const auto captured = keyloom_test::capture_output([] {
        return run_cli({"--get-mouse-pos"}, [](bool) -> std::optional<std::pair<int, int>> {
            throw PickerError("no display");
        });
    });
    CHECK_EQ(captured.code, 2);
    CHECK_TRUE(captured.err.find("no display") != std::string::npos);
}
