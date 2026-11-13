#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "value.hpp"

namespace loomwright {

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

/// The binary operators the language exposes. The parser resolves the
/// spelling once, so the evaluator dispatches on an enum instead of
/// comparing operator strings while a script runs.
enum class BinaryOp {
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    And,
    Or,
};

/// The unary operators the language exposes.
enum class UnaryOp { Negate, Plus, Not };

/// An expression tree. `Literal` carries a value; `Variable`, `Unary` and
/// `Binary` carry a name/operator; `Call` carries its function name and args;
/// `List` carries its element expressions; `Index` carries a base and an index.
struct Expr {
    enum class Kind { Literal, Variable, Unary, Binary, Call, List, Index };

    Kind kind = Kind::Literal;
    Value literal;
    std::string name;
    std::vector<ExprPtr> args;
    int line = 1;
    /// The resolved operator of a `Unary` or `Binary` node.
    BinaryOp binary_op = BinaryOp::Add;
    UnaryOp unary_op = UnaryOp::Negate;
    /// True when the expression tree contains a call that may wait, such as a
    /// call to a user-defined function. Such an expression needs a coroutine
    /// to evaluate; an expression without one is evaluated without
    /// suspending, which keeps the hot path free of coroutine frames.
    bool may_wait = false;
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
    For,
    Break,
    Continue,
    Assert,
    Call,
    Return,
    Send,
};

const char* stmt_kind_name(StmtKind kind);

/// One statement, or one `when` event block. Keeping the fields flat keeps the
/// node tuple-style and the checker/runtime dispatch readable.
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
    std::string event_kind;  // "key" | "mouse" | "timer" | "app" | "gui"
    std::string event_key;   // upper-case key name, or lower-case mouse button
    /// Every key of a chord binding such as `CTRL+ALT+T`, modifiers first and
    /// the primary key last. Empty for a plain single-key binding.
    std::vector<std::string> chord_parts;
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
    /// True when the statement, or anything below it, may wait: a `wait`, a
    /// tap, a glide, or an expression that calls a user function. Such a
    /// statement runs in a coroutine; everything else runs without one.
    bool may_suspend = false;

    std::string kind_name() const { return is_event ? "event" : stmt_kind_name(stmt); }
};

/// A user-defined function. `params` are the parameter names; the body runs
/// in its own scope and returns through a `return` statement.
struct Function {
    std::string name;
    std::vector<std::string> params;
    std::vector<Node> body;
    int line = 1;
};

struct Script {
    std::string name;
    std::string path = "<memory>";
    std::vector<std::pair<std::string, Value>> options;
    std::vector<Node> events;
    std::vector<Remap> remaps;
    std::vector<GuiPanel> panels;
    std::vector<Function> functions;

    const Value* option(const std::string& key) const {
        for (const auto& entry : options) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
};

}  // namespace loomwright
