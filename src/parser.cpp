#include "parser.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lexer.hpp"

namespace keyloom {

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
        case StmtKind::Assert: return "assert";
        case StmtKind::Call: return "call";
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
                        error("Option " + py_repr_string(key) + " is declared more than once");
                    }
                }
                script.options.emplace_back(key, value_expr->literal);
                end_statement();
            } else if (is_value("remap")) {
                script.remaps.push_back(parse_remap());
            } else if (is_value("when")) {
                script.events.push_back(parse_when());
            } else {
                error("Only 'option', 'remap', and 'when' are allowed at the top level");
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
        raise_at("Expected " + py_repr_string(value) + ", found " + current().value.repr(), current());
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
            node.event_key = parse_key_name();
            parse_event_edge(node, "Key");
        } else if (accept("mouse")) {
            node.event_kind = "mouse";
            node.event_key = parse_mouse_button();
            parse_event_edge(node, "Mouse");
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
            error("Expected 'key', 'mouse', 'timer', or 'app' after 'when'");
        }
        expect("{");
        node.body = parse_block();
        return node;
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

    Node parse_statement() {
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
            node.duration = parse_duration();
            node.has_duration = true;
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
                expect("{");
                node.otherwise = parse_block();
            } else {
                index_ = saved;
            }
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
        std::string chord = parse_key_name();
        while (accept("+")) chord += "+" + parse_key_name();
        return chord;
    }

    double parse_duration() {
        if (current().kind == TokenKind::Duration) return advance().value.as_double();
        if (current().kind == TokenKind::Number) return advance().value.as_double();
        error("Expected a duration such as 250ms, 2s, or 1m");
    }

    static int precedence_of(const std::string& op) {
        static const std::unordered_map<std::string, int> table = {
            {"or", 1},  {"||", 1},  {"and", 2}, {"&&", 2},
            {"==", 3},  {"!=", 3},  {"<", 3},   {"<=", 3},
            {">", 3},   {">=", 3},  {"+", 4},   {"-", 4},
            {"*", 5},   {"/", 5},   {"%", 5},
        };
        const auto found = table.find(op);
        return found == table.end() ? -1 : found->second;
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
            left->args.push_back(parse_expr(6));
        } else if (token.value.type == Value::Type::Str && token.value.text == "(") {
            left = parse_expr();
            expect(")");
        } else {
            raise_at("Expected an expression", token);
        }

        while (current().kind == TokenKind::Op && current().value.type == Value::Type::Str) {
            const std::string op = current().value.text;
            const int precedence = precedence_of(op);
            if (precedence < min_precedence || precedence < 0) break;
            advance();
            ExprPtr node = std::make_shared<Expr>();
            node->kind = Expr::Kind::Binary;
            node->name = op;
            node->line = current().line;
            node->args.push_back(left);
            node->args.push_back(parse_expr(precedence + 1));
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

}  // namespace keyloom
