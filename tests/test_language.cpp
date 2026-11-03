#include "harness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "checker.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "value.hpp"

using namespace keyloom;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool has_error(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.level == "error") return true;
    }
    return false;
}

bool has_message_containing(const std::vector<Diagnostic>& diagnostics, const std::string& needle) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE(parse_two_spots_example) {
    const std::string source = R"LOOM(
loom "two-spots" {
    option input = "evdev"
    option output = "auto"
    option panic = "PAUSE"
    when key A goes down {
        cursor warp 640, 420
        mouse left hold as spot_a
    }
    when key A goes up {
        mouse left free as spot_a
    }
    when key D goes down {
        cursor warp 1080, 420
        mouse left hold as spot_d
    }
    when key D goes up {
        mouse left free as spot_d
    }
}
)LOOM";
    const Script script = parse(source);
    CHECK_EQ(script.name, std::string("two-spots"));
    CHECK_EQ(script.events.size(), static_cast<size_t>(4));
    CHECK_EQ(script.events[0].event_kind, std::string("key"));
    CHECK_EQ(script.events[0].event_key, std::string("A"));
    CHECK_EQ(script.events[0].edge, std::string("down"));
    CHECK_EQ(script.events[0].body.size(), static_cast<size_t>(2));
    CHECK_EQ(script.events[0].body[0].a, std::string("warp"));
    CHECK_EQ(script.events[0].body[1].c, std::string("spot_a"));
    CHECK_EQ(script.events[0].line, 6);
}

TEST_CASE(parse_nested_flow_and_expressions) {
    const Script script = parse(R"LOOM(
loom "flow" {
    when app starts {
        set count = 3
        if count > 1 {
            repeat count {
                log "loop"
            }
        } else {
            log "single"
        }
    }
}
)LOOM");
    CHECK_EQ(script.events.at(0).body.at(0).kind_name(), std::string("set"));
    CHECK_EQ(script.events.at(0).body.at(1).kind_name(), std::string("if"));
    CHECK_EQ(script.events.at(0).body.at(1).body.at(0).kind_name(), std::string("repeat"));
    CHECK_EQ(script.events.at(0).body.at(1).otherwise.size(), static_cast<size_t>(1));
}

TEST_CASE(parse_held_timer_event) {
    const Script script = parse(R"LOOM(
loom "held" {
    when key F8 stays held every 50ms {
        log "held"
    }
}
)LOOM");
    const Node& event = script.events.at(0);
    CHECK_EQ(event.event_kind, std::string("key"));
    CHECK_EQ(event.event_key, std::string("F8"));
    CHECK_EQ(event.edge, std::string("held"));
    CHECK_EQ(event.interval, 0.05);
}

TEST_CASE(parse_timer_and_app_events) {
    const Script script = parse(R"LOOM(
loom "timers" {
    when timer every 1.5s { log "tick" }
    when app starts { log "start" }
}
)LOOM");
    CHECK_EQ(script.events.at(0).interval, 1.5);
    CHECK_EQ(script.events.at(0).edge, std::string("tick"));
    CHECK_EQ(script.events.at(1).edge, std::string("starts"));
}

TEST_CASE(rejects_unknown_top_level_statement) {
    CHECK_THROWS(parse("loom \"bad\" { cursor warp 1, 2 }"), ParseError);
}

TEST_CASE(rejects_a_script_without_events) {
    CHECK_THROWS(parse("loom \"empty\" {}"), ParseError);
}

TEST_CASE(rejects_duplicate_options) {
    CHECK_THROWS(parse("loom \"dup\" { option panic = \"PAUSE\"\n option panic = \"END\"\n when app starts { log 1 } }"),
                 ParseError);
}

TEST_CASE(rejects_unclosed_blocks) {
    CHECK_THROWS(parse("loom \"open\" {\n when app starts {\n log 1\n"), ParseError);
}

TEST_CASE(static_check_accepts_the_spot_example) {
    const std::string path = std::string(KEYLOOM_SOURCE_DIR) + "/examples/two_spots.loom";
    const Script script = parse(read_file(path), path);
    const auto diagnostics = check_script(script);
    CHECK_TRUE(!has_error(diagnostics));
    CHECK_TRUE(has_message_containing(diagnostics, "Script syntax and static checks passed"));
}

TEST_CASE(static_check_rejects_unknown_options_and_values) {
    const Script script = parse(R"LOOM(
loom "bad-options" {
    option output = "tape"
    option nonsense = 1
    when app starts { log "x" }
}
)LOOM");
    const auto diagnostics = check_script(script);
    CHECK_TRUE(has_error(diagnostics));
    CHECK_TRUE(has_message_containing(diagnostics, "Unknown option 'nonsense'"));
    CHECK_TRUE(has_message_containing(diagnostics, "Invalid value 'tape' for option 'output'"));
}

TEST_CASE(static_check_rejects_unknown_panic_key) {
    const Script script = parse(R"LOOM(
loom "bad-panic" {
    option panic = "NOT_A_KEY_XYZ"
    when app starts { log "x" }
}
)LOOM");
    const auto diagnostics = check_script(script);
    CHECK_TRUE(has_error(diagnostics));
    CHECK_TRUE(has_message_containing(diagnostics, "Unknown key 'NOT_A_KEY_XYZ'"));
}

TEST_CASE(static_check_rejects_unknown_keys_and_buttons) {
    const Script script = parse(R"LOOM(
loom "bad-keys" {
    when key NOTAKEY goes down {
        mouse pinky hold as token
    }
}
)LOOM");
    const auto diagnostics = check_script(script);
    CHECK_TRUE(has_error(diagnostics));
    CHECK_TRUE(has_message_containing(diagnostics, "Unknown key 'NOTAKEY'"));
    CHECK_TRUE(has_message_containing(diagnostics, "Unknown mouse button 'pinky'"));
}

TEST_CASE(sees_else_on_the_next_line) {
    const Script script = parse(R"LOOM(
loom "else-line" {
    when app starts {
        if true {
            log "yes"
        }
        else {
            log "no"
        }
    }
}
)LOOM");
    CHECK_EQ(script.events.at(0).body.at(0).otherwise.size(), static_cast<size_t>(1));
}

TEST_CASE(parses_arithmetic_without_spaces) {
    const Script script = parse(R"LOOM(
loom "math" {
    when app starts {
        set count = 5-2
        assert count == 3
    }
}
)LOOM");
    const auto& expression = script.events.at(0).body.at(0).exprs.at(0);
    CHECK_TRUE(expression->kind == Expr::Kind::Binary);
    CHECK_EQ(expression->name, std::string("-"));
}

TEST_CASE(decodes_string_escapes_once) {
    const auto tokens = tokenize("loom \"x\" { when app starts { log \"a\\nb\" log \"c\\\\nd\" } }");
    std::vector<std::string> strings;
    for (const auto& token : tokens) {
        if (token.kind == TokenKind::String) strings.push_back(token.value.text);
    }
    CHECK_EQ(strings, std::vector<std::string>({"x", "a\nb", "c\\nd"}));
}

TEST_CASE(parses_numeric_keys_and_chords) {
    const Script script = parse(R"LOOM(
loom "numeric-keys" {
    when key 1 goes down {
        key CTRL+1 tap for 50ms
    }
}
)LOOM");
    CHECK_EQ(script.events.at(0).event_key, std::string("1"));
    CHECK_EQ(script.events.at(0).body.at(0).a, std::string("CTRL+1"));
    CHECK_EQ(script.events.at(0).body.at(0).duration, 0.05);
}

TEST_CASE(parses_consume_statements) {
    const Script script = parse(R"LOOM(
loom "consume" {
    when key A goes down {
        consume key A
    }
}
)LOOM");
    const Node& node = script.events.at(0).body.at(0);
    CHECK_EQ(node.kind_name(), std::string("consume"));
    CHECK_EQ(node.a, std::string("A"));
}

TEST_CASE(checks_whole_script_consumption_and_panic_passthrough) {
    const Script script = parse(R"LOOM(
loom "consume" {
    option panic = "A"
    when key A goes down { consume key A }
    when key D goes down { consume key D }
}
)LOOM");
    const auto diagnostics = check_script(script);
    CHECK_TRUE(!has_error(diagnostics));
    CHECK_TRUE(has_message_containing(diagnostics, "never consumed"));
}

TEST_CASE(reports_unbalanced_hold_tokens) {
    const Script script = parse(R"LOOM(
loom "holds" {
    when key A goes down {
        mouse left hold as stuck
        mouse right free as missing
    }
}
)LOOM");
    const auto diagnostics = check_script(script);
    CHECK_TRUE(has_message_containing(diagnostics, "Mouse hold token 'stuck' is never freed"));
    CHECK_TRUE(has_message_containing(diagnostics, "Mouse hold token 'missing' is freed but never held"));
}

TEST_CASE(parses_durations_and_bare_numbers) {
    const Script script = parse(R"LOOM(
loom "durations" {
    when app starts {
        wait 250ms
        wait 2
    }
}
)LOOM");
    CHECK_EQ(script.events.at(0).body.at(0).duration, 0.25);
    CHECK_EQ(script.events.at(0).body.at(1).duration, 2.0);
}

TEST_CASE(lexer_reports_unexpected_characters_with_a_position) {
    try {
        tokenize("loom \"x\" {\n  cursor warp 1, @\n}");
        CHECK_TRUE(false);
    } catch (const ParseError&) {
        CHECK_TRUE(false);
    } catch (const LexError& error) {
        const std::string message = error.what();
        CHECK_TRUE(message.find("Unexpected character '@'") != std::string::npos);
        CHECK_TRUE(message.find("at 2:18") != std::string::npos);
    }
}

TEST_CASE(parses_mouse_button_events) {
    const Script script = parse(R"LOOM(
loom "mouse" {
    when mouse left goes down {
        log "left down"
    }
    when mouse right goes up {
        log "right up"
    }
    when mouse middle stays held every 80ms {
        log "middle held"
    }
}
)LOOM");
    CHECK_EQ(script.events.size(), static_cast<size_t>(3));
    CHECK_EQ(script.events.at(0).event_kind, std::string("mouse"));
    CHECK_EQ(script.events.at(0).event_key, std::string("left"));
    CHECK_EQ(script.events.at(0).edge, std::string("down"));
    CHECK_EQ(script.events.at(1).event_key, std::string("right"));
    CHECK_EQ(script.events.at(1).edge, std::string("up"));
    CHECK_EQ(script.events.at(2).event_key, std::string("middle"));
    CHECK_EQ(script.events.at(2).edge, std::string("held"));
    CHECK_TRUE(script.events.at(2).interval > 0.0);
    CHECK_TRUE(!has_error(check_script(script)));
}

TEST_CASE(checker_rejects_unknown_mouse_buttons) {
    const Script script = parse(R"LOOM(
loom "bad-mouse" {
    when mouse backwards goes down {
        log "never"
    }
}
)LOOM");
    const auto diagnostics = check_script(script);
    CHECK_TRUE(has_error(diagnostics));
    CHECK_TRUE(has_message_containing(diagnostics, "Unknown mouse button"));
}

TEST_CASE(formats_python_style_values) {
    CHECK_EQ(py_float_str(1.5), std::string("1.5"));
    CHECK_EQ(py_float_str(1.0), std::string("1.0"));
    CHECK_EQ(py_float_str(0.1), std::string("0.1"));
    CHECK_EQ(py_float_str(0.0001), std::string("0.0001"));
    CHECK_EQ(py_float_str(0.00001), std::string("1e-05"));
    CHECK_EQ(Value::of_float(1e16).to_string(), std::string("1e+16"));
    CHECK_EQ(Value::of_bool(true).to_string(), std::string("True"));
    CHECK_EQ(py_repr_string("a'b"), std::string("\"a'b\""));
    CHECK_EQ(py_repr_string("a\nb"), std::string("'a\\nb'"));
}
