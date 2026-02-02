#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Demo script for mathematical bracket and symbol rendering in Contour terminal.

Showcases the pixel-perfect rendering of Unicode mathematical symbols
U+23A7 through U+23B3, which include curly bracket pieces, integral
extensions, horizontal line extensions, bracket sections, and summation
symbols.

Run this inside the Contour terminal with builtin box drawing enabled
to see the rendered glyphs:

    python3 test/demo_math_symbols.py
"""

# Mathematical bracket/symbol codepoints
LEFT_CURLY_UPPER_HOOK = "\u23A7"     # ⎧
LEFT_CURLY_MIDDLE     = "\u23A8"     # ⎨
LEFT_CURLY_LOWER_HOOK = "\u23A9"     # ⎩
CURLY_EXTENSION       = "\u23AA"     # ⎪
RIGHT_CURLY_UPPER_HOOK = "\u23AB"    # ⎫
RIGHT_CURLY_MIDDLE    = "\u23AC"     # ⎬
RIGHT_CURLY_LOWER_HOOK = "\u23AD"    # ⎭
INTEGRAL_EXTENSION    = "\u23AE"     # ⎮
HORIZ_LINE_EXTENSION  = "\u23AF"     # ⎯
UPPER_LEFT_SECTION    = "\u23B0"     # ⎰
UPPER_RIGHT_SECTION   = "\u23B1"     # ⎱
SUMMATION_TOP         = "\u23B2"     # ⎲
SUMMATION_BOTTOM      = "\u23B3"     # ⎳

# Square bracket pieces (already implemented, for comparison)
LEFT_SQ_UPPER  = "\u23A1"           # ⎡
LEFT_SQ_EXT    = "\u23A2"           # ⎢
LEFT_SQ_LOWER  = "\u23A3"           # ⎣
RIGHT_SQ_UPPER = "\u23A4"           # ⎤
RIGHT_SQ_EXT   = "\u23A5"           # ⎥
RIGHT_SQ_LOWER = "\u23A6"           # ⎦

# ANSI color helpers
BOLD = "\033[1m"
DIM  = "\033[2m"
RESET = "\033[0m"
CYAN = "\033[36m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
MAGENTA = "\033[35m"
WHITE = "\033[37m"


def header(title: str) -> None:
    """Print a section header."""
    width = 60
    print()
    print(f"{CYAN}{BOLD}{'=' * width}{RESET}")
    print(f"{CYAN}{BOLD}  {title}{RESET}")
    print(f"{CYAN}{BOLD}{'=' * width}{RESET}")
    print()


def subheader(title: str) -> None:
    """Print a subsection header."""
    print(f"  {GREEN}{BOLD}{title}{RESET}")
    print()


def left_curly(height: int) -> list[str]:
    """Build a left curly bracket of given height (minimum 3)."""
    height = max(3, height)
    middle = height // 2
    lines = []
    for i in range(height):
        if i == 0:
            lines.append(LEFT_CURLY_UPPER_HOOK)
        elif i == middle:
            lines.append(LEFT_CURLY_MIDDLE)
        elif i == height - 1:
            lines.append(LEFT_CURLY_LOWER_HOOK)
        else:
            lines.append(CURLY_EXTENSION)
    return lines


def right_curly(height: int) -> list[str]:
    """Build a right curly bracket of given height (minimum 3)."""
    height = max(3, height)
    middle = height // 2
    lines = []
    for i in range(height):
        if i == 0:
            lines.append(RIGHT_CURLY_UPPER_HOOK)
        elif i == middle:
            lines.append(RIGHT_CURLY_MIDDLE)
        elif i == height - 1:
            lines.append(RIGHT_CURLY_LOWER_HOOK)
        else:
            lines.append(CURLY_EXTENSION)
    return lines


def left_square(height: int) -> list[str]:
    """Build a left square bracket of given height."""
    height = max(2, height)
    lines = []
    for i in range(height):
        if i == 0:
            lines.append(LEFT_SQ_UPPER)
        elif i == height - 1:
            lines.append(LEFT_SQ_LOWER)
        else:
            lines.append(LEFT_SQ_EXT)
    return lines


def right_square(height: int) -> list[str]:
    """Build a right square bracket of given height."""
    height = max(2, height)
    lines = []
    for i in range(height):
        if i == 0:
            lines.append(RIGHT_SQ_UPPER)
        elif i == height - 1:
            lines.append(RIGHT_SQ_LOWER)
        else:
            lines.append(RIGHT_SQ_EXT)
    return lines


def summation(height: int) -> list[str]:
    """Build a summation symbol of given height (minimum 2)."""
    height = max(2, height)
    lines = []
    for i in range(height):
        if i == 0:
            lines.append(SUMMATION_TOP)
        elif i == height - 1:
            lines.append(SUMMATION_BOTTOM)
        else:
            lines.append(" ")
    return lines


def integral(height: int) -> list[str]:
    """Build an integral extension of given height."""
    return [INTEGRAL_EXTENSION for _ in range(height)]


def print_side_by_side(columns: list[tuple[list[str], str]], spacing: int = 3) -> None:
    """Print multiple columns of lines side by side with labels underneath."""
    if not columns:
        return

    max_height = max(len(col[0]) for col in columns)

    # Pad all columns to the same height (centered vertically)
    padded = []
    for lines, label in columns:
        pad_top = (max_height - len(lines)) // 2
        pad_bottom = max_height - len(lines) - pad_top
        padded_lines = ["  "] * pad_top + lines + ["  "] * pad_bottom
        padded.append((padded_lines, label))

    sep = " " * spacing

    for row in range(max_height):
        parts = []
        for lines, _ in padded:
            parts.append(f"    {lines[row]}")
        print(sep.join(parts))

    # Print labels
    labels = []
    for _, label in padded:
        labels.append(f"    {DIM}{label}{RESET}")
    print(sep.join(labels))


def demo_character_table() -> None:
    """Show all 13 characters with their codepoints and names."""
    header("Character Reference Table")

    chars = [
        (0x23A7, LEFT_CURLY_UPPER_HOOK, "LEFT CURLY BRACKET UPPER HOOK"),
        (0x23A8, LEFT_CURLY_MIDDLE, "LEFT CURLY BRACKET MIDDLE PIECE"),
        (0x23A9, LEFT_CURLY_LOWER_HOOK, "LEFT CURLY BRACKET LOWER HOOK"),
        (0x23AA, CURLY_EXTENSION, "CURLY BRACKET EXTENSION"),
        (0x23AB, RIGHT_CURLY_UPPER_HOOK, "RIGHT CURLY BRACKET UPPER HOOK"),
        (0x23AC, RIGHT_CURLY_MIDDLE, "RIGHT CURLY BRACKET MIDDLE PIECE"),
        (0x23AD, RIGHT_CURLY_LOWER_HOOK, "RIGHT CURLY BRACKET LOWER HOOK"),
        (0x23AE, INTEGRAL_EXTENSION, "INTEGRAL EXTENSION"),
        (0x23AF, HORIZ_LINE_EXTENSION, "HORIZONTAL LINE EXTENSION"),
        (0x23B0, UPPER_LEFT_SECTION, "UPPER LEFT OR LOWER RIGHT CURLY BRACKET SECTION"),
        (0x23B1, UPPER_RIGHT_SECTION, "UPPER RIGHT OR LOWER LEFT CURLY BRACKET SECTION"),
        (0x23B2, SUMMATION_TOP, "SUMMATION TOP"),
        (0x23B3, SUMMATION_BOTTOM, "SUMMATION BOTTOM"),
    ]

    print(f"  {BOLD}{'CP':>8}  {'Glyph':^5}  Name{RESET}")
    print(f"  {'─' * 56}")
    for cp, glyph, name in chars:
        print(f"  {YELLOW}U+{cp:04X}{RESET}    {WHITE}{BOLD}{glyph}{RESET}     {name}")
    print()


def demo_curly_brackets() -> None:
    """Show curly brackets of various heights."""
    header("Curly Brackets at Various Heights")

    subheader("Left curly brackets (3, 5, 7, 9 rows)")
    columns = []
    for h in (3, 5, 7, 9):
        columns.append((left_curly(h), f"h={h}"))
    print_side_by_side(columns, spacing=5)
    print()

    subheader("Right curly brackets (3, 5, 7, 9 rows)")
    columns = []
    for h in (3, 5, 7, 9):
        columns.append((right_curly(h), f"h={h}"))
    print_side_by_side(columns, spacing=5)
    print()

    subheader("Matched pairs with content")
    for h in (3, 5, 7):
        lbrace = left_curly(h)
        rbrace = right_curly(h)
        content_lines = []
        for i in range(h):
            if i == h // 2:
                content_lines.append(f" {MAGENTA}expression{RESET} ")
            elif i == h // 2 - 1 and h >= 5:
                content_lines.append(f" {DIM}  term_a  {RESET} ")
            elif i == h // 2 + 1 and h >= 5:
                content_lines.append(f" {DIM}  term_b  {RESET} ")
            else:
                content_lines.append("           ")

        for i in range(h):
            print(f"    {lbrace[i]}{content_lines[i]}{rbrace[i]}")
        print()


def demo_bracket_sections() -> None:
    """Show the compact bracket section characters."""
    header("Bracket Sections (S-curves)")

    print(f"  {UPPER_LEFT_SECTION}  U+23B0  Upper-left / Lower-right curly bracket section")
    print(f"  {UPPER_RIGHT_SECTION}  U+23B1  Upper-right / Lower-left curly bracket section")
    print()

    subheader("Combined with extensions for small brackets")
    # A small left bracket using sections
    print(f"    {UPPER_LEFT_SECTION}")
    print(f"    {UPPER_LEFT_SECTION}")
    print()
    print(f"    {UPPER_RIGHT_SECTION}")
    print(f"    {UPPER_RIGHT_SECTION}")
    print()


def demo_summation() -> None:
    """Show summation symbols."""
    header("Summation Symbols")

    subheader("Individual pieces")
    print(f"    {SUMMATION_TOP}   U+23B2  Summation top")
    print(f"    {SUMMATION_BOTTOM}   U+23B3  Summation bottom")
    print()

    subheader("Assembled summation symbols (2, 3, 4 rows)")
    columns = []
    for h in (2, 3, 4):
        columns.append((summation(h), f"h={h}"))
    print_side_by_side(columns, spacing=5)
    print()

    subheader("Summation in mathematical context")
    # Display a mock summation expression:  sum_{i=0}^{n} x_i
    h = 4
    sigma = summation(h)
    for i in range(h):
        if i == 0:
            superscript = f" {DIM}n{RESET}"
        elif i == h - 1:
            subscript = f" {DIM}i=0{RESET}"
            print(f"      {sigma[i]}{subscript}  {MAGENTA}x{DIM}i{RESET}")
        elif i == 1:
            print(f"      {sigma[i]}{superscript}")
            continue
        else:
            print(f"      {sigma[i]}")
            continue
    print()


def demo_cosine_definition() -> None:
    """Show the Taylor series definition of cos(x) using summation symbols."""
    header("Taylor Series: cos(x)")

    #                  ∞
    #  cos(x)  =      ⎲    (-1)ⁿ · x²ⁿ
    #                  ⎳   ⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯
    #                 n=0      (2n)!

    frac_bar = HORIZ_LINE_EXTENSION * 12

    print(f"                     {DIM}∞{RESET}")
    print(f"    {YELLOW}cos(x){RESET}  =      "
          f"{SUMMATION_TOP}    {MAGENTA}(-1){RESET}{DIM}ⁿ{RESET} {MAGENTA}·{RESET} {MAGENTA}x{RESET}{DIM}²ⁿ{RESET}")
    print(f"                   {SUMMATION_BOTTOM}   {frac_bar}")
    print(f"                  {DIM}n=0{RESET}      {MAGENTA}(2n)!{RESET}")
    print()

    subheader("Expanded terms")
    #               x²     x⁴     x⁶
    # = 1  -  ─────  +  ─────  -  ─────  +  ...
    #              2!      4!      6!

    bar2 = HORIZ_LINE_EXTENSION * 3
    bar3 = HORIZ_LINE_EXTENSION * 3

    print(f"                   {MAGENTA}x²{RESET}      {MAGENTA}x⁴{RESET}      {MAGENTA}x⁶{RESET}")
    print(f"      = {YELLOW}1{RESET}  {DIM}-{RESET}  "
          f"{bar2}  {DIM}+{RESET}  {bar3}  {DIM}-{RESET}  {bar3}  {DIM}+ ...{RESET}")
    print(f"              {MAGENTA}2!{RESET}       {MAGENTA}4!{RESET}       {MAGENTA}6!{RESET}")
    print()


def demo_extensions() -> None:
    """Show extension characters."""
    header("Extension Characters")

    subheader("Curly bracket extension and integral extension")
    print(f"    {CURLY_EXTENSION}  U+23AA  Curly bracket extension")
    print(f"    {INTEGRAL_EXTENSION}  U+23AE  Integral extension")
    print()

    subheader("Horizontal line extension")
    print(f"    {HORIZ_LINE_EXTENSION}{HORIZ_LINE_EXTENSION}{HORIZ_LINE_EXTENSION}{HORIZ_LINE_EXTENSION}"
          f"{HORIZ_LINE_EXTENSION}{HORIZ_LINE_EXTENSION}{HORIZ_LINE_EXTENSION}{HORIZ_LINE_EXTENSION}"
          f"  U+23AF  (8 cells)")
    print()


def demo_mixed_brackets() -> None:
    """Show square and curly brackets side by side."""
    header("Square vs. Curly Bracket Comparison")

    h = 5
    columns = [
        (left_square(h), "square ["),
        (left_curly(h), "curly {"),
        (right_curly(h), "curly }"),
        (right_square(h), "square ]"),
    ]
    print_side_by_side(columns, spacing=5)
    print()

    subheader("Nested brackets")
    lsq = left_square(7)
    lcurly = left_curly(5)
    rcurly = right_curly(5)
    rsq = right_square(7)

    for i in range(7):
        inner_start = 1
        inner_end = 6
        if inner_start <= i < inner_end:
            j = i - inner_start
            print(f"    {lsq[i]} {lcurly[j]}  {DIM}row {j}{RESET}  {rcurly[j]} {rsq[i]}")
        else:
            print(f"    {lsq[i]}             {rsq[i]}")
    print()


def demo_matrix() -> None:
    """Show a mathematical matrix using brackets."""
    header("Matrix Display")

    subheader("3x3 matrix with square brackets")
    rows = [
        " 1  0  0 ",
        " 0  1  0 ",
        " 0  0  1 ",
    ]
    h = len(rows)
    lsq = left_square(h)
    rsq = right_square(h)
    for i in range(h):
        print(f"    {lsq[i]} {YELLOW}{rows[i]}{RESET} {rsq[i]}")
    print()

    subheader("System of equations with curly bracket")
    equations = [
        f" {YELLOW}2x + 3y = 7{RESET}  ",
        f" {YELLOW} x -  y = 1{RESET}  ",
        f" {YELLOW}4x + 2y = 10{RESET} ",
    ]
    h = len(equations)
    lc = left_curly(h)
    for i in range(h):
        print(f"    {lc[i]}{equations[i]}")
    print()


def main() -> None:
    print(f"\n{BOLD}{CYAN}")
    print("  ╔══════════════════════════════════════════════════════╗")
    print("  ║   Mathematical Symbol Rendering Demo                ║")
    print("  ║   Unicode U+23A7 through U+23B3                    ║")
    print("  ║   Contour Terminal Emulator                         ║")
    print("  ╚══════════════════════════════════════════════════════╝")
    print(f"{RESET}")

    demo_character_table()
    demo_curly_brackets()
    demo_bracket_sections()
    demo_summation()
    demo_cosine_definition()
    demo_extensions()
    demo_mixed_brackets()
    demo_matrix()

    print(f"\n  {DIM}End of demo. All 13 mathematical symbols (U+23A7-U+23B3)")
    print(f"  should be rendered with pixel-perfect built-in glyphs.{RESET}\n")


if __name__ == "__main__":
    main()
