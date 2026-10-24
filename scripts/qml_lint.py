"""Shared by the QML lint gates (check-qml-colors.py, check-qml-sizes.py): one comment/string scanner."""

from __future__ import annotations


def code_lines(text: str) -> list[str]:
    """Returns text's lines with comments blanked out and string literals kept.

    A scanner rather than a regex because `//` inside a string ("https://...") is not a comment and a
    quote inside a comment does not open a string. Block comments carry across lines.
    """
    result: list[str] = []
    in_block = False
    for line in text.splitlines():
        out: list[str] = []
        quote = ""
        index = 0
        while index < len(line):
            char = line[index]
            pair = line[index : index + 2]
            if in_block:
                if pair == "*/":
                    in_block = False
                    index += 2
                else:
                    index += 1
            elif quote:
                out.append(char)
                if char == "\\" and index + 1 < len(line):
                    out.append(line[index + 1])
                    index += 2
                    continue
                if char == quote:
                    quote = ""
                index += 1
            elif pair == "//":
                break
            elif pair == "/*":
                in_block = True
                index += 2
            else:
                if char in "\"'":
                    quote = char
                out.append(char)
                index += 1
        result.append("".join(out))
    return result
