#include "parser.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "builtins.hpp"
#include "lexer.hpp"

namespace loomwright {

const char* stmt_kind_name(StmtKind kind) {
    switch (kind) {
        case StmtKind::Cursor: return "cursor";
        case StmtKind::Mouse: return "mouse";
        case StmtKind::Key: return "key";
        case StmtKind::Consume: return "consume";
        case StmtKind::Type: return "type";
        case StmtKind::Wait: return "wait";
        case StmtKind::Scroll: return "scroll";
        case StmtKind::Log: return "log";
        case StmtKind::Notify: return "notify";
        case StmtKind::ClipWrite: return "clip_write";
        case StmtKind::ClipRead: return "clip_read";
        case StmtKind::Launch: return "launch";
        case StmtKind::Set: return "set";
        case StmtKind::If: return "if";
        case StmtKind::Repeat: return "repeat";
        case StmtKind::While: return "while";
        case StmtKind::For: return "for";
        case StmtKind::Break: return "break";
        case StmtKind::Continue: return "continue";
        case StmtKind::Assert: return "assert";
        case StmtKind::Call: return "call";
        case StmtKind::Return: return "return";
        case StmtKind::Send: return "send";
    }
    return "unknown";
}

namespace {

class Parser {
public:
    Parser(const std::string& source, std::string path) : path_(std::move(path)) {
        try {
            tokens_ = tokenize(source);
        } catch (const LexError& error) {
            throw ParseError(error.what());
        }
    }

    Script parse_script() {
        Script script;
        script.path = path_;
        skip_lines();
        expect("loom");
        script.name = expect_kind(TokenKind::String).value.to_string();
        expect("{");
        skip_lines();
        while (!accept("}")) {
            if (current().kind == TokenKind::Eof) error("Unclosed loom block");
            if (accept("option")) {
                const std::string key = expect_kind(TokenKind::Ident).value.to_string();
                expect("=");
                ExprPtr value_expr = parse_expr();
                if (value_expr->kind != Expr::Kind::Literal) {
                    error("Option values must be literal strings, numbers, or booleans");
                }
                for (const auto& entry : script.options) {
                    if (entry.first == key) {
                        error("Option " + quote_string(key) + " is declared more than once");
                    }
                }
                script.options.emplace_back(key, value_expr->literal);
                end_statement();
            } else if (is_value("remap")) {
                script.remaps.push_back(parse_remap());
            } else if (is_value("gui")) {
                script.panels.push_back(parse_gui_panel());
            } else if (is_value("func")) {
                script.functions.push_back(parse_function());
            } else if (is_value("when")) {
                script.events.push_back(parse_when());
            } else {
                error("Only 'option', 'remap', 'gui', 'func', and 'when' are allowed at the top level");
            }
            skip_lines();
        }
        skip_lines();
        if (current().kind != TokenKind::Eof) error("Unexpected content after loom block");
        if (script.events.empty()) throw ParseError("A script needs at least one 'when' event block");
        return script;
    }

private:
    const Token& current() const { return tokens_[index_]; }

    const Token& advance() {
        const Token& token = current();
        if (token.kind != TokenKind::Eof) ++index_;
        return token;
    }

    bool is_value(const std::string& value) const {
        return current().value.type == Value::Type::Str && current().value.text == value;
    }

    bool accept(const std::string& value) {
        if (!is_value(value)) return false;
        advance();
        return true;
    }

    [[noreturn]] void raise_at(const std::string& message, const Token& token) {
        throw ParseError(message + " at " + std::to_string(token.line) + ":" + std::to_string(token.col),
                         token.line, token.col);
    }

    [[noreturn]] void error(const std::string& message) { raise_at(message, current()); }

    Token expect(const std::string& value) {
        if (is_value(value)) return advance();
        raise_at("Expected " + quote_string(value) + ", found " + current().value.repr(), current());
    }

    Token expect_kind(TokenKind kind) {
        if (current().kind == kind) return advance();
        raise_at(std::string("Expected ") + token_kind_name(kind) + ", found " +
                     token_kind_name(current().kind) + " " + current().value.repr(),
                 current());
    }

    void skip_lines() {
        while (current().kind == TokenKind::Newline) advance();
    }

    void end_statement() {
        if (current().kind == TokenKind::Newline) {
            skip_lines();
        } else if (is_value("}")) {
            return;
        } else if (current().kind != TokenKind::Eof) {
            error("Expected the end of the line");
        }
    }

    Node parse_when() {
        const Token start = expect("when");
        Node node;
        node.is_event = true;
        node.line = start.line;
        if (accept("key")) {
            node.event_kind = "key";
            const std::vector<std::string> parts = parse_key_chord_parts();
            node.event_key = parts.back();
            if (parts.size() > 1) node.chord_parts = parts;
            parse_event_edge(node, "Key");
        } else if (accept("mouse")) {
            node.event_kind = "mouse";
            node.event_key = parse_mouse_button();
            parse_event_edge(node, "Mouse");
        } else if (accept("gui")) {
            node.event_kind = "gui";
            if (accept("window")) {
                expect("closes");
                node.event_key = "window";
                node.edge = "close";
            } else {
                node.event_key = parse_gui_id();
                expect("clicked");
                node.edge = "clicked";
            }
        } else if (accept("timer")) {
            expect("every");
            node.event_kind = "timer";
            node.edge = "tick";
            node.interval = parse_duration();
        } else if (accept("app")) {
            expect("starts");
            node.event_kind = "app";
            node.edge = "starts";
        } else {
            error("Expected 'key', 'mouse', 'timer', 'gui', or 'app' after 'when'");
        }
        expect("{");
        node.body = parse_block();
        return node;
    }

    GuiPanel parse_gui_panel() {
        const Token start = expect("gui");
        GuiPanel panel;
        panel.line = start.line;
        panel.title = expect_kind(TokenKind::String).value.to_string();
        if (panel.title.empty()) error("GUI panel title cannot be empty");
        expect("{");
        skip_lines();
        while (!accept("}")) {
            if (current().kind == TokenKind::Eof) error("Unclosed gui block");
            const Token widget_start = current();
            GuiWidget widget;
            widget.line = widget_start.line;
            if (accept("label")) {
                widget.kind = GuiWidget::Kind::Label;
                widget.text = expect_kind(TokenKind::String).value.to_string();
            } else if (accept("input")) {
                widget.kind = GuiWidget::Kind::Input;
                widget.id = parse_gui_id();
                expect("=");
                widget.text = expect_kind(TokenKind::String).value.to_string();
            } else if (accept("button")) {
                widget.kind = GuiWidget::Kind::Button;
                widget.id = parse_gui_id();
                expect("=");
                widget.text = expect_kind(TokenKind::String).value.to_string();
            } else {
                error("Expected 'label', 'input', or 'button' in gui block");
            }
            panel.widgets.push_back(std::move(widget));
            end_statement();
            skip_lines();
        }
        return panel;
    }

    std::string parse_gui_id() {
        const Token token = expect_kind(TokenKind::Ident);
        std::string id = token.value.to_string();
        for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return id;
    }

    Remap parse_remap() {
        const Token start = expect("remap");
        Remap remap;
        remap.line = start.line;
        expect("key");
        remap.source = parse_key_name();
        expect("to");
        remap.target = parse_key_chord();
        end_statement();
        return remap;
    }

    Function parse_function() {
        const Token start = expect("func");
        Function function;
        function.line = start.line;
        function.name = expect_kind(TokenKind::Ident).value.to_string();
        if (accept("(")) {
            if (!accept(")")) {
                while (true) {
                    function.params.push_back(expect_kind(TokenKind::Ident).value.to_string());
                    if (accept(")")) break;
                    expect(",");
                }
            }
        }
        expect("{");
        function.body = parse_block();
        return function;
    }

    void parse_event_edge(Node& node, const char* noun) {
        if (accept("stays")) {
            expect("held");
            expect("every");
            node.edge = "held";
            node.interval = parse_duration();
            return;
        }
        expect("goes");
        std::string edge = expect_kind(TokenKind::Ident).value.to_string();
        for (char& c : edge) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (edge != "down" && edge != "up") {
            error(std::string(noun) + " event must be 'goes down' or 'goes up'");
        }
        node.edge = edge;
    }

    std::vector<Node> parse_block() {
        std::vector<Node> body;
        skip_lines();
        while (!accept("}")) {
            if (current().kind == TokenKind::Eof) error("Unclosed block");
            body.push_back(parse_statement());
            skip_lines();
        }
        return body;
    }

    /// True when the statement, or anything below it, may wait: a `wait`, a
    /// tap, a glide, or an expression that calls a user function. The runtime
    /// uses this to run the common statements without a coroutine frame.
    static bool may_suspend(const Node& node) {
        switch (node.stmt) {
            case StmtKind::Wait: return true;
            case StmtKind::Mouse: return node.b == "tap";
            case StmtKind::Key: return node.b == "tap";
            case StmtKind::Cursor: return node.a == "glide";
            default: break;
        }
        for (const auto& expr : node.exprs) {
            if (expr->may_wait) return true;
        }
        for (const auto& child : node.body) {
            if (child.may_suspend) return true;
        }
        for (const auto& child : node.otherwise) {
            if (child.may_suspend) return true;
        }
        return false;
    }

    Node parse_statement() {
        Node node = parse_statement_body();
        node.may_suspend = may_suspend(node);
        return node;
    }

    Node parse_statement_body() {
        const Token start = current();
        Node node;
        node.line = start.line;

        if (accept("cursor")) {
            node.stmt = StmtKind::Cursor;
            std::string action = expect_kind(TokenKind::Ident).value.to_string();
            for (char& c : action) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            node.a = action;
            if (action == "warp" || action == "move") {
                node.exprs.push_back(parse_expr());
                expect(",");
                node.exprs.push_back(parse_expr());
                end_statement();
                return node;
            }
            if (action == "glide") {
                node.exprs.push_back(parse_expr());
                expect(",");
                node.exprs.push_back(parse_expr());
                expect("over");
                node.duration = parse_duration();
                node.has_duration = true;
                end_statement();
                return node;
            }
            if (action == "center") {
                end_statement();
                return node;
            }
            error("Cursor action must be warp, move, glide, or center");
        }
        if (accept("mouse")) {
            node.stmt = StmtKind::Mouse;
            node.a = expect_kind(TokenKind::Ident).value.to_string();
            for (char& c : node.a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string action = expect_kind(TokenKind::Ident).value.to_string();
            for (char& c : action) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            node.b = action;
            if (action == "hold" || action == "free" || action == "tap") {
                if (action == "tap" && accept("for")) {
                    node.duration = parse_duration();
                    node.has_duration = true;
                }
                if (accept("as")) node.c = expect_kind(TokenKind::Ident).value.to_string();
            } else {
                error("Mouse action must be hold, free, or tap");
            }
            end_statement();
            return node;
        }
        if (accept("key")) {
            node.stmt = StmtKind::Key;
            node.a = parse_key_chord();
            std::string action = expect_kind(TokenKind::Ident).value.to_string();
            for (char& c : action) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            node.b = action;
            if (action != "press" && action != "free" && action != "tap") {
                error("Key action must be press, free, or tap");
            }
            if (action == "tap" && accept("for")) {
                node.duration = parse_duration();
                node.has_duration = true;
            }
            end_statement();
            return node;
        }
        if (accept("consume")) {
            node.stmt = StmtKind::Consume;
            expect("key");
            node.a = parse_key_name();
            end_statement();
            return node;
        }
        if (accept("send")) {
            node.stmt = StmtKind::Send;
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        if (accept("type")) {
            node.stmt = StmtKind::Type;
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        if (accept("wait")) {
            node.stmt = StmtKind::Wait;
            node.exprs.push_back(parse_expr());
            if (node.exprs.back()->kind == Expr::Kind::Literal) {
                node.duration = node.exprs.back()->literal.as_double();
                node.has_duration = true;
            }
            end_statement();
            return node;
        }
        if (accept("scroll")) {
            node.stmt = StmtKind::Scroll;
            node.exprs.push_back(parse_expr());
            expect(",");
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        if (accept("log") || accept("say")) {
            node.stmt = StmtKind::Log;
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        if (accept("notify")) {
            node.stmt = StmtKind::Notify;
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        if (accept("clip")) {
            std::string action = expect_kind(TokenKind::Ident).value.to_string();
            for (char& c : action) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            node.b = action;
            if (action == "write") {
                node.stmt = StmtKind::ClipWrite;
                node.exprs.push_back(parse_expr());
                end_statement();
                return node;
            }
            if (action == "read") {
                node.stmt = StmtKind::ClipRead;
                expect("into");
                node.a = expect_kind(TokenKind::Ident).value.to_string();
                end_statement();
                return node;
            }
            error("Clipboard action must be write or read");
        }
        if (accept("launch")) {
            node.stmt = StmtKind::Launch;
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        if (accept("set")) {
            node.stmt = StmtKind::Set;
            node.a = expect_kind(TokenKind::Ident).value.to_string();
            expect("=");
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        if (accept("if")) {
            node.stmt = StmtKind::If;
            node.exprs.push_back(parse_expr());
            expect("{");
            node.body = parse_block();
            const size_t saved = index_;
            skip_lines();
            if (accept("else")) {
                if (is_value("if")) {
                    // `else if` chains: the nested block is stored as the only
                    // statement of the else branch.
                    node.otherwise.push_back(parse_statement());
                } else {
                    expect("{");
                    node.otherwise = parse_block();
                }
            } else {
                index_ = saved;
            }
            return node;
        }
        if (accept("for")) {
            node.stmt = StmtKind::For;
            node.a = expect_kind(TokenKind::Ident).value.to_string();
            if (accept("=")) {
                node.exprs.push_back(parse_expr());
                std::string direction = expect_kind(TokenKind::Ident).value.to_string();
                for (char& c : direction) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (direction != "to" && direction != "downto") {
                    error("For loop range must use 'to' or 'downto'");
                }
                node.b = direction;
                node.exprs.push_back(parse_expr());
                if (accept("step")) {
                    node.exprs.push_back(parse_expr());
                }
            } else {
                expect("in");
                node.b = "in";
                node.exprs.push_back(parse_expr());
            }
            expect("{");
            node.body = parse_block();
            return node;
        }
        if (accept("break")) {
            node.stmt = StmtKind::Break;
            end_statement();
            return node;
        }
        if (accept("continue")) {
            node.stmt = StmtKind::Continue;
            end_statement();
            return node;
        }
        if (accept("return")) {
            node.stmt = StmtKind::Return;
            if (current().kind != TokenKind::Newline && !is_value("}")) {
                node.exprs.push_back(parse_expr());
            }
            end_statement();
            return node;
        }
        if (accept("repeat")) {
            node.stmt = StmtKind::Repeat;
            node.exprs.push_back(parse_expr());
            expect("{");
            node.body = parse_block();
            return node;
        }
        if (accept("while")) {
            node.stmt = StmtKind::While;
            node.exprs.push_back(parse_expr());
            expect("{");
            node.body = parse_block();
            return node;
        }
        if (accept("assert")) {
            node.stmt = StmtKind::Assert;
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        if (accept("call")) {
            node.stmt = StmtKind::Call;
            node.exprs.push_back(parse_expr());
            end_statement();
            return node;
        }
        error("Unknown statement");
    }

    std::string parse_key_name() {
        const Token& token = current();
        if (token.kind != TokenKind::Ident && token.kind != TokenKind::Number) {
            raise_at("Expected a key name such as A, F8, or 1", token);
        }
        advance();
        std::string name = token.value.to_string();
        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return name;
    }

    std::string parse_mouse_button() {
        const Token& token = current();
        if (token.kind != TokenKind::Ident) {
            raise_at("Expected a mouse button such as left, right, or middle", token);
        }
        advance();
        std::string name = token.value.to_string();
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return name;
    }

    std::string parse_key_chord() {
        const std::vector<std::string> parts = parse_key_chord_parts();
        std::string chord = parts.front();
        for (size_t index = 1; index < parts.size(); ++index) chord += "+" + parts[index];
        return chord;
    }

    std::vector<std::string> parse_key_chord_parts() {
        std::vector<std::string> parts;
        parts.push_back(parse_key_name());
        while (accept("+")) parts.push_back(parse_key_name());
        return parts;
    }

    double parse_duration() {
        if (current().kind == TokenKind::Duration) return advance().value.as_double();
        if (current().kind == TokenKind::Number) return advance().value.as_double();
        error("Expected a duration such as 250ms, 2s, or 1m");
    }

    /// The operators the expression parser accepts, with their precedence.
    struct OperatorInfo {
        int precedence;
        BinaryOp op;
    };

    static bool operator_info(const std::string& op, OperatorInfo& out) {
        if (op == "or" || op == "||") { out = {1, BinaryOp::Or}; return true; }
        if (op == "and" || op == "&&") { out = {2, BinaryOp::And}; return true; }
        if (op == "==") { out = {3, BinaryOp::Equal}; return true; }
        if (op == "!=") { out = {3, BinaryOp::NotEqual}; return true; }
        if (op == "<") { out = {3, BinaryOp::Less}; return true; }
        if (op == "<=") { out = {3, BinaryOp::LessEqual}; return true; }
        if (op == ">") { out = {3, BinaryOp::Greater}; return true; }
        if (op == ">=") { out = {3, BinaryOp::GreaterEqual}; return true; }
        if (op == "+") { out = {4, BinaryOp::Add}; return true; }
        if (op == "-") { out = {4, BinaryOp::Subtract}; return true; }
        if (op == "*") { out = {5, BinaryOp::Multiply}; return true; }
        if (op == "/") { out = {5, BinaryOp::Divide}; return true; }
        if (op == "%") { out = {5, BinaryOp::Modulo}; return true; }
        return false;
    }

    /// True when one of the arguments may wait. A call to a user-defined
    /// function may wait, so the evaluator needs a coroutine for it.
    static bool args_may_wait(const std::vector<ExprPtr>& args) {
        for (const auto& argument : args) {
            if (argument->may_wait) return true;
        }
        return false;
    }

    ExprPtr parse_expr(int min_precedence = 0) {
        const Token token = advance();
        ExprPtr left;
        if (token.kind == TokenKind::Number || token.kind == TokenKind::Duration ||
            token.kind == TokenKind::String) {
            left = std::make_shared<Expr>();
            left->kind = Expr::Kind::Literal;
            left->literal = token.value;
            left->line = token.line;
        } else if (token.kind == TokenKind::Ident) {
            const std::string ident = token.value.to_string();
            std::string lowered;
            for (char c : ident) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (lowered == "true" || lowered == "false") {
                left = std::make_shared<Expr>();
                left->kind = Expr::Kind::Literal;
                left->literal = Value::of_bool(lowered == "true");
                left->line = token.line;
            } else if (accept("(")) {
                left = std::make_shared<Expr>();
                left->kind = Expr::Kind::Call;
                left->name = ident;
                left->line = token.line;
                // A built-in function never waits; a user-defined function may.
                // The callee is resolved once here so evaluation dispatches on
                // the enum instead of comparing spellings.
                if (const BuiltinInfo* info = find_builtin_function(ident)) {
                    left->builtin = info->id;
                }
                left->may_wait = left->builtin == Builtin::None;
                if (!accept(")")) {
                    while (true) {
                        left->args.push_back(parse_expr());
                        if (accept(")")) break;
                        expect(",");
                    }
                }
            } else {
                left = std::make_shared<Expr>();
                left->kind = Expr::Kind::Variable;
                left->name = ident;
                left->line = token.line;
            }
        } else if (token.value.type == Value::Type::Str &&
                   (token.value.text == "-" || token.value.text == "+" || token.value.text == "not" ||
                    token.value.text == "!")) {
            left = std::make_shared<Expr>();
            left->kind = Expr::Kind::Unary;
            left->name = token.value.text;
            left->line = token.line;
            left->unary_op = token.value.text == "-"   ? UnaryOp::Negate
                             : token.value.text == "+" ? UnaryOp::Plus
                                                      : UnaryOp::Not;
            left->args.push_back(parse_expr(6));
            left->may_wait = args_may_wait(left->args);
        } else if (token.value.type == Value::Type::Str && token.value.text == "(") {
            left = parse_expr();
            expect(")");
        } else if (token.value.type == Value::Type::Str && token.value.text == "[") {
            left = std::make_shared<Expr>();
            left->kind = Expr::Kind::List;
            left->line = token.line;
            if (!accept("]")) {
                while (true) {
                    left->args.push_back(parse_expr());
                    if (accept("]")) break;
                    expect(",");
                }
            }
            left->may_wait = args_may_wait(left->args);
        } else {
            raise_at("Expected an expression", token);
        }

        // Subscripts bind tighter than any operator: `items[0] + 1`.
        while (is_value("[")) {
            advance();
            ExprPtr index = parse_expr();
            expect("]");
            ExprPtr node = std::make_shared<Expr>();
            node->kind = Expr::Kind::Index;
            node->line = index->line;
            node->args.push_back(left);
            node->args.push_back(index);
            node->may_wait = args_may_wait(node->args);
            left = node;
        }

        while (current().kind == TokenKind::Op && current().value.type == Value::Type::Str) {
            const std::string op = current().value.text;
            OperatorInfo info{};
            if (!operator_info(op, info)) break;
            if (info.precedence < min_precedence) break;
            advance();
            ExprPtr node = std::make_shared<Expr>();
            node->kind = Expr::Kind::Binary;
            node->name = op;
            node->binary_op = info.op;
            node->line = current().line;
            node->args.push_back(left);
            node->args.push_back(parse_expr(info.precedence + 1));
            node->may_wait = args_may_wait(node->args);
            left = node;
        }
        return left;
    }

    std::string path_;
    std::vector<Token> tokens_;
    size_t index_ = 0;
};

}  // namespace

Script parse(const std::string& source, const std::string& path) {
    Parser parser(source, path);
    return parser.parse_script();
}

}  // namespace loomwright
