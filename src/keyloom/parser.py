from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .lexer import Token, tokenize, LexError


class ParseError(Exception):
    def __init__(self, message: str, token: Token | None = None):
        if token:
            message = f"{message} at {token.line}:{token.col}"
        super().__init__(message)
        self.token = token


@dataclass
class Node:
    kind: str
    args: tuple[Any, ...] = ()
    body: list["Node"] = field(default_factory=list)
    otherwise: list["Node"] = field(default_factory=list)
    line: int = 1


@dataclass
class Script:
    name: str
    options: dict[str, Any]
    events: list[Node]
    path: str = "<memory>"


class Parser:
    PRECEDENCE = {
        "or": 1, "||": 1,
        "and": 2, "&&": 2,
        "==": 3, "!=": 3, "<": 3, "<=": 3, ">": 3, ">=": 3,
        "+": 4, "-": 4,
        "*": 5, "/": 5, "%": 5,
    }

    def __init__(self, source: str, path: str = "<memory>"):
        self.source = source
        self.path = path
        try:
            self.tokens = tokenize(source)
        except LexError as exc:
            raise ParseError(str(exc)) from exc
        self.i = 0
        self.options: dict[str, Any] = {}
        self.events: list[Node] = []

    @property
    def t(self) -> Token:
        return self.tokens[self.i]

    def advance(self) -> Token:
        cur = self.t
        if cur.kind != "EOF":
            self.i += 1
        return cur

    def is_value(self, value: object) -> bool:
        return self.t.value == value

    def accept(self, value: object) -> bool:
        if self.is_value(value):
            self.advance()
            return True
        return False

    def expect(self, value: object) -> Token:
        if self.is_value(value):
            return self.advance()
        raise ParseError(f"Expected {value!r}, found {self.t.value!r}", self.t)

    def expect_kind(self, kind: str) -> Token:
        if self.t.kind == kind:
            return self.advance()
        raise ParseError(f"Expected {kind}, found {self.t.kind} {self.t.value!r}", self.t)

    def skip_lines(self) -> None:
        while self.t.kind == "NEWLINE":
            self.advance()

    def parse(self) -> Script:
        self.skip_lines()
        self.expect("loom")
        name = str(self.expect_kind("STRING").value)
        self.expect("{")
        self.skip_lines()
        while not self.accept("}"):
            if self.t.kind == "EOF":
                raise ParseError("Unclosed loom block", self.t)
            if self.accept("option"):
                key = str(self.expect_kind("IDENT").value)
                self.expect("=")
                value_expr = self.parse_expr()
                if not (isinstance(value_expr, tuple) and value_expr[0] == "literal"):
                    raise ParseError("Option values must be literal strings, numbers, or booleans", self.t)
                value = value_expr[1]
                if key in self.options:
                    raise ParseError(f"Option {key!r} is declared more than once", self.t)
                self.options[key] = value
                self.end_statement()
            elif self.is_value("when"):
                self.events.append(self.parse_when())
            else:
                raise ParseError("Only 'option' and 'when' are allowed at the top level", self.t)
            self.skip_lines()
        self.skip_lines()
        if self.t.kind != "EOF":
            raise ParseError("Unexpected content after loom block", self.t)
        if not self.events:
            raise ParseError("A script needs at least one 'when' event block")
        return Script(name, self.options, self.events, self.path)

    def end_statement(self) -> None:
        if self.t.kind == "NEWLINE":
            self.skip_lines()
        elif self.t.value == "}":
            return
        elif self.t.kind != "EOF":
            raise ParseError("Expected the end of the line", self.t)

    def parse_when(self) -> Node:
        start = self.expect("when")
        if self.accept("key"):
            key = self.parse_key_name()
            if self.accept("stays"):
                self.expect("held")
                self.expect("every")
                event = ("key", key, "held", self.parse_duration())
            else:
                self.expect("goes")
                edge = str(self.expect_kind("IDENT").value).lower()
                if edge not in ("down", "up"):
                    raise ParseError("Key event must be 'goes down' or 'goes up'", self.t)
                event = ("key", key, edge, None)
        elif self.accept("timer"):
            self.expect("every")
            interval = self.parse_duration()
            event = ("timer", None, "tick", interval)
        elif self.accept("app"):
            self.expect("starts")
            event = ("app", None, "starts", None)
        else:
            raise ParseError("Expected 'key', 'timer', or 'app' after 'when'", self.t)
        self.expect("{")
        body = self.parse_block()
        return Node("event", event, body, line=start.line)

    def parse_block(self) -> list[Node]:
        body: list[Node] = []
        self.skip_lines()
        while not self.accept("}"):
            if self.t.kind == "EOF":
                raise ParseError("Unclosed block", self.t)
            body.append(self.parse_statement())
            self.skip_lines()
        return body

    def parse_statement(self) -> Node:
        tok = self.t
        if self.accept("cursor"):
            action = str(self.expect_kind("IDENT").value).lower()
            if action in ("warp", "move"):
                x = self.parse_expr()
                self.expect(",")
                y = self.parse_expr()
                self.end_statement()
                return Node("cursor", (action, x, y), line=tok.line)
            if action == "glide":
                x = self.parse_expr(); self.expect(","); y = self.parse_expr()
                self.expect("over"); duration = self.parse_duration(); self.end_statement()
                return Node("cursor", (action, x, y, duration), line=tok.line)
            if action == "center":
                self.end_statement(); return Node("cursor", ("center",), line=tok.line)
            raise ParseError("Cursor action must be warp, move, glide, or center", self.t)
        if self.accept("mouse"):
            button = str(self.expect_kind("IDENT").value).lower()
            action = str(self.expect_kind("IDENT").value).lower()
            token_name = None
            duration = None
            if action in ("hold", "free", "tap"):
                if action == "tap" and self.accept("for"):
                    duration = self.parse_duration()
                if self.accept("as"):
                    token_name = str(self.expect_kind("IDENT").value)
            else:
                raise ParseError("Mouse action must be hold, free, or tap", self.t)
            self.end_statement()
            return Node("mouse", (button, action, token_name, duration), line=tok.line)
        if self.accept("key"):
            key_name = self.parse_key_chord()
            action = str(self.expect_kind("IDENT").value).lower()
            if action not in ("press", "free", "tap"):
                raise ParseError("Key action must be press, free, or tap", self.t)
            duration = None
            if action == "tap" and self.accept("for"):
                duration = self.parse_duration()
            self.end_statement()
            return Node("key", (key_name, action, duration), line=tok.line)
        if self.accept("type"):
            value = self.parse_expr(); self.end_statement()
            return Node("type", (value,), line=tok.line)
        if self.accept("wait"):
            duration = self.parse_duration(); self.end_statement()
            return Node("wait", (duration,), line=tok.line)
        if self.accept("scroll"):
            x = self.parse_expr(); self.expect(","); y = self.parse_expr(); self.end_statement()
            return Node("scroll", (x, y), line=tok.line)
        if self.accept("log") or self.accept("say"):
            value = self.parse_expr(); self.end_statement()
            return Node("log", (value,), line=tok.line)
        if self.accept("notify"):
            value = self.parse_expr(); self.end_statement()
            return Node("notify", (value,), line=tok.line)
        if self.accept("clip"):
            action = str(self.expect_kind("IDENT").value).lower()
            if action == "write":
                value = self.parse_expr(); self.end_statement()
                return Node("clip_write", (value,), line=tok.line)
            if action == "read":
                self.expect("into"); name = str(self.expect_kind("IDENT").value); self.end_statement()
                return Node("clip_read", (name,), line=tok.line)
            raise ParseError("Clipboard action must be write or read", self.t)
        if self.accept("launch"):
            value = self.parse_expr(); self.end_statement()
            return Node("launch", (value,), line=tok.line)
        if self.accept("set"):
            name = str(self.expect_kind("IDENT").value); self.expect("="); expr = self.parse_expr(); self.end_statement()
            return Node("set", (name, expr), line=tok.line)
        if self.accept("if"):
            condition = self.parse_expr(); self.expect("{"); body = self.parse_block()
            otherwise: list[Node] = []
            saved = self.i
            self.skip_lines()
            if self.accept("else"):
                self.expect("{"); otherwise = self.parse_block()
            else:
                self.i = saved
            return Node("if", (condition,), body, otherwise, tok.line)
        if self.accept("repeat"):
            count = self.parse_expr(); self.expect("{"); body = self.parse_block()
            return Node("repeat", (count,), body, line=tok.line)
        if self.accept("while"):
            condition = self.parse_expr(); self.expect("{"); body = self.parse_block()
            return Node("while", (condition,), body, line=tok.line)
        if self.accept("assert"):
            expr = self.parse_expr(); self.end_statement()
            return Node("assert", (expr,), line=tok.line)
        if self.accept("call"):
            expr = self.parse_expr(); self.end_statement()
            return Node("call", (expr,), line=tok.line)
        raise ParseError("Unknown statement", self.t)

    def parse_key_name(self) -> str:
        token = self.t
        if token.kind not in ("IDENT", "NUMBER"):
            raise ParseError("Expected a key name such as A, F8, or 1", token)
        self.advance()
        return str(token.value).upper()

    def parse_key_chord(self) -> str:
        parts = [self.parse_key_name()]
        while self.accept("+"):
            parts.append(self.parse_key_name())
        return "+".join(parts)

    def parse_duration(self) -> float:
        if self.t.kind == "DURATION":
            return float(self.advance().value)
        if self.t.kind == "NUMBER":
            # Bare numeric time values are seconds.
            return float(self.advance().value)
        raise ParseError("Expected a duration such as 250ms, 2s, or 1m", self.t)

    def parse_expr(self, min_prec: int = 0):
        tok = self.advance()
        if tok.kind == "NUMBER" or tok.kind == "DURATION":
            left = ("literal", tok.value)
        elif tok.kind == "STRING":
            left = ("literal", tok.value)
        elif tok.kind == "IDENT":
            ident = str(tok.value)
            low = ident.lower()
            if low in ("true", "false"):
                left = ("literal", low == "true")
            elif self.accept("("):
                args = []
                if not self.accept(")"):
                    while True:
                        args.append(self.parse_expr())
                        if self.accept(")"):
                            break
                        self.expect(",")
                left = ("call", ident, tuple(args))
            else:
                left = ("variable", ident)
        elif tok.value in ("-", "+", "not", "!"):
            left = ("unary", str(tok.value), self.parse_expr(6))
        elif tok.value == "(":
            left = self.parse_expr()
            self.expect(")")
        else:
            raise ParseError("Expected an expression", tok)

        while self.t.kind == "OP" and self.t.value in self.PRECEDENCE:
            op = str(self.t.value)
            prec = self.PRECEDENCE[op]
            if prec < min_prec:
                break
            self.advance()
            right = self.parse_expr(prec + 1)
            left = ("binary", op, left, right)
        return left


def parse(source: str, path: str = "<memory>") -> Script:
    return Parser(source, path).parse()
