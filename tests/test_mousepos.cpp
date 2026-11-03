#include "harness.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cli.hpp"
#include "mousepos.hpp"

using namespace keyloom;

TEST_CASE(coordinate_format_matches_loom_syntax) {
    CHECK_EQ(format_coordinates(640, 420), std::string("640, 420"));
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
