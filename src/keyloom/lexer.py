from __future__ import annotations

from dataclasses import dataclass
import re


@dataclass(frozen=True)
class Token:
    kind: str
    value: object
    line: int
    col: int


class LexError(Exception):
    pass


_TOKEN_RE = re.compile(
    r"(?P<SPACE>[ \t\r]+)"
    r"|(?P<COMMENT>\#[^\n]*)"
    r"|(?P<STRING>\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*')"
    r"|(?P<DURATION>(?:\d+(?:\.\d+)?)(?:ms|s|m|h)\b)"
    r"|(?P<NUMBER>\d+(?:\.\d+)?)"
    r"|(?P<IDENT>[A-Za-z_][A-Za-z0-9_]*)"
    r"|(?P<OP>==|!=|<=|>=|&&|\|\||[{}(),.=+*/%<>!-])"
    r"|(?P<NEWLINE>\n)"
)


def _decode_string(body: str) -> str:
    """Decode the language's small escape set in a single pass.

    Doing chained ``str.replace`` calls is subtly wrong: ``"\\\\n"`` (a literal
    backslash followed by ``n``) would turn into a backslash plus a newline.
    """
    def replace(match: re.Match) -> str:
        char = match.group(1)
        return {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", "'": "'", '"': '"'}.get(char, "\\" + char)
    return re.sub(r"\\(.)", replace, body, flags=re.DOTALL)


def tokenize(source: str) -> list[Token]:
    out: list[Token] = []
    line, col, pos = 1, 1, 0
    while pos < len(source):
        match = _TOKEN_RE.match(source, pos)
        if not match:
            snippet = source[pos:pos + 24].splitlines()[0]
            raise LexError(f"Unexpected character {source[pos]!r} at {line}:{col} near {snippet!r}")
        kind = match.lastgroup or ""
        raw = match.group()
        if kind == "NEWLINE":
            out.append(Token("NEWLINE", "\\n", line, col))
            line += 1
            col = 1
        elif kind == "SPACE" or kind == "COMMENT":
            pass
        elif kind == "STRING":
            body = raw[1:-1]
            out.append(Token("STRING", _decode_string(body), line, col))
        elif kind == "DURATION":
            number = float(re.match(r"\d+(?:\.\d+)?", raw).group())
            # Determine the unit robustly for values such as 1.5s.
            unit = re.sub(r"^\d+(?:\.\d+)?", "", raw)
            factor = {"ms": 0.001, "s": 1.0, "m": 60.0, "h": 3600.0}[unit]
            out.append(Token("DURATION", number * factor, line, col))
        elif kind == "NUMBER":
            number = float(raw) if "." in raw else int(raw)
            out.append(Token("NUMBER", number, line, col))
        elif kind == "IDENT":
            lowered = raw.lower()
            if lowered in ("and", "or", "not"):
                out.append(Token("OP", lowered, line, col))
            else:
                out.append(Token("IDENT", raw, line, col))
        else:
            out.append(Token("OP", raw, line, col))
        consumed = len(raw)
        pos += consumed
        if kind != "NEWLINE":
            col += consumed
    out.append(Token("EOF", None, line, col))
    return out
