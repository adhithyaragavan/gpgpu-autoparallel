"""Phase 2.5 item 4: just enough C structure for the LLM baseline's checker.

Finds statement extents (a `for` and everything its body covers) and the
function each line is in, by tokenizing the source with comments, strings
and preprocessor lines blanked out. This is not a C parser: it relies on
the code being ordinary statement-structured C (PolyBench is), and the
checker only uses it to tie each inserted pragma to the loop after it and
to group pragmas nested in a parallel region. Lines are 1-based.
"""
import re

_TOKEN = re.compile(r"[A-Za-z_]\w*|\d[\w.]*|\S")


def _mask(src):
    """src with comments, string/char literals and preprocessor lines
    replaced by spaces (newlines kept, so offsets and lines still match)."""
    out = list(src)
    i, n = 0, len(src)
    line_start = True
    while i < n:
        c = src[i]
        if line_start and c in " \t":
            i += 1
            continue
        if line_start and c == "#":
            # Preprocessor line, with backslash continuations.
            while i < n and src[i] != "\n":
                if src[i] == "\\" and i + 1 < n and src[i + 1] == "\n":
                    out[i] = " "
                    i += 2
                    continue
                out[i] = " "
                i += 1
            continue
        line_start = False
        if c == "\n":
            line_start = True
            i += 1
        elif src.startswith("//", i):
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            for k in range(i, min(j + 1, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


class Source:
    def __init__(self, text):
        self.text = text
        masked = _mask(text)
        self.toks = [(m.group(), m.start()) for m in _TOKEN.finditer(masked)]
        self._line_of = []
        line = 1
        for ch in text:
            self._line_of.append(line)
            if ch == "\n":
                line += 1
        self._line_of.append(line)
        self._first_tok = {}
        for k, (_, off) in enumerate(self.toks):
            self._first_tok.setdefault(self.line(off), k)
        self.functions = self._find_functions()

    def line(self, offset):
        return self._line_of[offset]

    def _match(self, k, open_, close):
        depth = 0
        for j in range(k, len(self.toks)):
            t = self.toks[j][0]
            if t == open_:
                depth += 1
            elif t == close:
                depth -= 1
                if depth == 0:
                    return j
        raise ValueError(f"unbalanced {open_} at line {self.line(self.toks[k][1])}")

    def _stmt_end(self, k):
        """Index of the last token of the statement starting at token k."""
        t = self.toks[k][0]
        if t == "{":
            return self._match(k, "{", "}")
        if t in ("for", "while", "switch"):
            return self._stmt_end(self._match(k + 1, "(", ")") + 1)
        if t == "if":
            e = self._stmt_end(self._match(k + 1, "(", ")") + 1)
            if e + 1 < len(self.toks) and self.toks[e + 1][0] == "else":
                return self._stmt_end(e + 2)
            return e
        if t == "do":
            e = self._stmt_end(k + 1)
            return self._match(e + 2, "(", ")") + 1  # while ( ... ) ;
        depth = 0
        for j in range(k, len(self.toks)):
            u = self.toks[j][0]
            if u in "({[":
                depth += 1
            elif u in ")}]":
                depth -= 1
            elif u == ";" and depth == 0:
                return j
        raise ValueError(f"no end for statement at line {self.line(self.toks[k][1])}")

    def stmt_at(self, line):
        """(first token, start line, end line) of the statement whose first
        token is the first on `line`, or None."""
        k = self._first_tok.get(line)
        if k is None:
            return None
        e = self._stmt_end(k)
        return self.toks[k][0], line, self.line(self.toks[e][1])

    def _find_functions(self):
        """(name, first line, last line) of each function definition:
        `name ( ... ) {` at brace depth 0."""
        funcs, depth, k = [], 0, 0
        while k < len(self.toks):
            t = self.toks[k][0]
            if t == "{":
                depth += 1
            elif t == "}":
                depth -= 1
            elif depth == 0 and t == "(" and k > 0 and re.match(r"[A-Za-z_]", self.toks[k - 1][0]):
                close = self._match(k, "(", ")")
                if close + 1 < len(self.toks) and self.toks[close + 1][0] == "{":
                    end = self._match(close + 1, "{", "}")
                    funcs.append((self.toks[k - 1][0], self.line(self.toks[k - 1][1]),
                                  self.line(self.toks[end][1])))
                    k = end + 1
                    continue
                k = close
            k += 1
        return funcs

    def function_at(self, line):
        for name, lo, hi in self.functions:
            if lo <= line <= hi:
                return name
        return ""

    def for_lines(self):
        """Line of every `for` keyword that starts a line (PolyBench style)."""
        return sorted(l for l, k in self._first_tok.items() if self.toks[k][0] == "for")
