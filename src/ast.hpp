#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "value.hpp"

namespace keyloom {

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

/// An expression tree. `Literal` carries a value; `Variable`, `Unary` and
/// `Binary` carry a name/operator; `Call` carries its function name and args.
struct Expr {
    enum class Kind { Literal, Variable, Unary, Binary, Call };

    Kind kind = Kind::Literal;
    Value literal;
    std::string name;
    std::vector<ExprPtr> args;
    int line = 1;
};

enum class StmtKind {
    Cursor,
    Mouse,
    Key,
    Consume,
    Type,
    Wait,
    Scroll,
    Log,
    Notify,
    ClipWrite,
    ClipRead,
    Launch,
    Set,
    If,
    Repeat,
    While,
    Assert,
    Call,
    Send,
};

const char* stmt_kind_name(StmtKind kind);

/// One statement, or one `when` event block. Keeping the fields flat mirrors the
/// original tuple-based nodes and keeps the checker/runtime dispatch readable.
/// One widget in a persistent `gui` declaration. Labels are structural and
/// have no identifier; inputs and buttons are addressable from callbacks and
/// expression functions such as `gui_value()` and `gui_set_text()`.
struct GuiWidget {
    enum class Kind { Label, Input, Button };

    Kind kind = Kind::Label;
    std::string id;
    std::string text;
    int line = 1;
};

/// A persistent GUI panel. `title` is also used as the panel identifier by
/// expression functions such as `gui_set_visible()`.
struct GuiPanel {
    std::string title;
    std::vector<GuiWidget> widgets;
    int line = 1;
};

/// A top-level `remap` declaration. `source` is one physical key; `target`
/// is a key or `+`-separated chord. The parser validates names; the checker
/// validates chord contents so parse errors stay focused on syntax.
struct Remap {
    std::string source;
    std::string target;
    int line = 1;
};

struct Node {
    StmtKind stmt = StmtKind::Log;
    bool is_event = false;

    // Event fields.
    std::string event_kind;  // "key" | "mouse" | "timer" | "app"
    std::string event_key;   // upper-case key name, or lower-case mouse button
    std::string edge;        // "down" | "up" | "held" | "tick" | "starts"
    double interval = 0.0;

    // Statement fields.
    std::string a;  // cursor action / mouse button / key chord / variable name
    std::string b;  // mouse action / key action / clip action
    std::string c;  // hold token
    std::vector<ExprPtr> exprs;
    bool has_duration = false;
    double duration = 0.0;

    std::vector<Node> body;
    std::vector<Node> otherwise;
    int line = 1;

    std::string kind_name() const { return is_event ? "event" : stmt_kind_name(stmt); }
};

struct Script {
    std::string name;
    std::string path = "<memory>";
    std::vector<std::pair<std::string, Value>> options;
    std::vector<Node> events;
    std::vector<Remap> remaps;
    std::vector<GuiPanel> panels;

    const Value* option(const std::string& key) const {
        for (const auto& entry : options) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
};

}  // namespace keyloom
