#!/usr/bin/env python3
"""Lossless-to-browser inline JavaScript whitespace/comment reduction.

The checked-in HTML stays readable and unchanged. Only the gzip-compressed
firmware response is optimized. No property/key renaming or AST transforms.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))
from rjsmin import jsmin  # Apache-2.0, vendored in scripts/vendor/rjsmin.py

SCRIPT_RE = re.compile(rb"(<script\b[^>]*>)([\s\S]*?)(</script\s*>)", re.IGNORECASE)
SOURCE_RE = re.compile(rb"\bsrc\s*=", re.IGNORECASE)


def minify_html_js(content: bytes) -> bytes:
    """Only remove syntactically inessential JS spaces/comments inside script tags.

    Other HTML, script tags, CSS, quoted strings, template literals, URL text,
    identifiers and all JSON/ECU schema fields remain source-identical.
    """
    def replace(match):
        opening, original_js, closing = match.groups()
        if SOURCE_RE.search(opening):
            return match.group(0)
        optimized_js = jsmin(original_js.decode("utf-8")).encode("utf-8")
        return opening + optimized_js + closing
    return SCRIPT_RE.sub(replace, content)


if __name__ == "__main__":
    path = Path(sys.argv[1])
    result = minify_html_js(path.read_bytes())
    sys.stdout.buffer.write(result)
