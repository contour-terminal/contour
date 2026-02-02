#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Demo script for parenthesis piece rendering in Contour terminal.

Showcases the pixel-perfect rendering of Unicode parenthesis pieces
U+239B through U+23A0, which assemble into tall parentheses for
mathematical expressions.

Run this inside the Contour terminal with builtin box drawing enabled:

    python3 test/demo_parenthesis_pieces.py
"""

# Parenthesis piece codepoints
LEFT_PAREN_UPPER_HOOK = "\u239B"  # ⎛
LEFT_PAREN_EXTENSION  = "\u239C"  # ⎜
LEFT_PAREN_LOWER_HOOK = "\u239D"  # ⎝
RIGHT_PAREN_UPPER_HOOK = "\u239E" # ⎞
RIGHT_PAREN_EXTENSION  = "\u239F" # ⎟
RIGHT_PAREN_LOWER_HOOK = "\u23A0" # ⎠

# Other bracket pieces for comparison
LEFT_CURLY_UPPER_HOOK  = "\u23A7" # ⎧
LEFT_CURLY_MIDDLE      = "\u23A8" # ⎨
LEFT_CURLY_LOWER_HOOK  = "\u23A9" # ⎩
CURLY_EXTENSION        = "\u23AA" # ⎪
RIGHT_CURLY_UPPER_HOOK = "\u23AB" # ⎫
RIGHT_CURLY_MIDDLE     = "\u23AC" # ⎬
RIGHT_CURLY_LOWER_HOOK = "\u23AD" # ⎭

LEFT_SQ_UPPER  = "\u23A1" # ⎡
LEFT_SQ_EXT    = "\u23A2" # ⎢
LEFT_SQ_LOWER  = "\u23A3" # ⎣
RIGHT_SQ_UPPER = "\u23A4" # ⎤
RIGHT_SQ_EXT   = "\u23A5" # ⎥
RIGHT_SQ_LOWER = "\u23A6" # ⎦

SUMMATION_TOP    = "\u23B2" # ⎲
SUMMATION_BOTTOM = "\u23B3" # ⎳
HORIZ_LINE_EXT   = "\u23AF" # ⎯

# ANSI color helpers
BOLD    = "\033[1m"
DIM     = "\033[2m"
RESET   = "\033[0m"
CYAN    = "\033[36m"
GREEN   = "\033[32m"
YELLOW  = "\033[33m"
MAGENTA = "\033[35m"
RED     = "\033[31m"
WHITE   = "\033[37m"


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


def left_paren(height: int) -> list[str]:
    """Build a left parenthesis of given height (minimum 2)."""
    height = max(2, height)
    lines = []
    for i in range(height):
        if i == 0:
            lines.append(LEFT_PAREN_UPPER_HOOK)
        elif i == height - 1:
            lines.append(LEFT_PAREN_LOWER_HOOK)
        else:
            lines.append(LEFT_PAREN_EXTENSION)
    return lines


def right_paren(height: int) -> list[str]:
    """Build a right parenthesis of given height (minimum 2)."""
    height = max(2, height)
    lines = []
    for i in range(height):
        if i == 0:
            lines.append(RIGHT_PAREN_UPPER_HOOK)
        elif i == height - 1:
            lines.append(RIGHT_PAREN_LOWER_HOOK)
        else:
            lines.append(RIGHT_PAREN_EXTENSION)
    return lines


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
    return ([LEFT_SQ_UPPER]
            + [LEFT_SQ_EXT] * (height - 2)
            + [LEFT_SQ_LOWER])


def right_square(height: int) -> list[str]:
    """Build a right square bracket of given height."""
    height = max(2, height)
    return ([RIGHT_SQ_UPPER]
            + [RIGHT_SQ_EXT] * (height - 2)
            + [RIGHT_SQ_LOWER])


def print_side_by_side(columns: list[tuple[list[str], str]], spacing: int = 3) -> None:
    """Print multiple columns of lines side by side with labels underneath."""
    if not columns:
        return
    max_height = max(len(col[0]) for col in columns)
    padded = []
    for lines, label in columns:
        pad_top = (max_height - len(lines)) // 2
        pad_bottom = max_height - len(lines) - pad_top
        padded_lines = [" "] * pad_top + lines + [" "] * pad_bottom
        padded.append((padded_lines, label))
    sep = " " * spacing
    for row in range(max_height):
        parts = [f"    {lines[row]}" for lines, _ in padded]
        print(sep.join(parts))
    labels = [f"    {DIM}{label}{RESET}" for _, label in padded]
    print(sep.join(labels))


def demo_character_table() -> None:
    """Show all 6 parenthesis piece characters with codepoints and names."""
    header("Parenthesis Pieces \u2014 Character Reference (U+239B\u2013U+23A0)")

    chars = [
        (0x239B, LEFT_PAREN_UPPER_HOOK,  "LEFT PARENTHESIS UPPER HOOK"),
        (0x239C, LEFT_PAREN_EXTENSION,   "LEFT PARENTHESIS EXTENSION"),
        (0x239D, LEFT_PAREN_LOWER_HOOK,  "LEFT PARENTHESIS LOWER HOOK"),
        (0x239E, RIGHT_PAREN_UPPER_HOOK, "RIGHT PARENTHESIS UPPER HOOK"),
        (0x239F, RIGHT_PAREN_EXTENSION,  "RIGHT PARENTHESIS EXTENSION"),
        (0x23A0, RIGHT_PAREN_LOWER_HOOK, "RIGHT PARENTHESIS LOWER HOOK"),
    ]

    print(f"  {BOLD}{'CP':>8}  {'Glyph':^5}  Name{RESET}")
    print(f"  {'\u2500' * 52}")
    for cp, glyph, name in chars:
        print(f"  {YELLOW}U+{cp:04X}{RESET}    {WHITE}{BOLD}{glyph}{RESET}     {name}")
    print()


def demo_parentheses_heights() -> None:
    """Show parentheses at various heights to verify stacking."""
    header("Parentheses at Various Heights")

    subheader("Left parentheses (2, 3, 5, 7, 9 rows)")
    columns = [(left_paren(h), f"h={h}") for h in (2, 3, 5, 7, 9)]
    print_side_by_side(columns, spacing=5)
    print()

    subheader("Right parentheses (2, 3, 5, 7, 9 rows)")
    columns = [(right_paren(h), f"h={h}") for h in (2, 3, 5, 7, 9)]
    print_side_by_side(columns, spacing=5)
    print()

    subheader("Matched pairs with content")
    for h in (2, 4, 6):
        lp = left_paren(h)
        rp = right_paren(h)
        for i in range(h):
            if i == h // 2:
                content = f" {MAGENTA}a + b{RESET} "
            elif i == h // 2 - 1 and h >= 4:
                content = f" {DIM} x\u00b2  {RESET} "
            else:
                content = "       "
            print(f"    {lp[i]}{content}{rp[i]}")
        print()


def demo_junction_check() -> None:
    """Verify seamless junction between hooks and extensions."""
    header("Junction Continuity Check")

    subheader("Hook \u2192 Extension \u2192 Hook (should be seamless)")
    print(f"  {DIM}Left:{RESET}                 {DIM}Right:{RESET}")
    h = 8
    lp = left_paren(h)
    rp = right_paren(h)
    for i in range(h):
        piece_l = "upper hook" if i == 0 else "lower hook" if i == h - 1 else "extension "
        piece_r = "upper hook" if i == 0 else "lower hook" if i == h - 1 else "extension "
        print(f"    {lp[i]}  {DIM}{piece_l}{RESET}        {rp[i]}  {DIM}{piece_r}{RESET}")
    print()


def demo_bracket_comparison() -> None:
    """Side-by-side comparison of all three bracket types."""
    header("Bracket Type Comparison: () vs {} vs []")

    h = 7
    columns = [
        (left_paren(h),   "( paren"),
        (left_curly(h),   "{ curly"),
        (left_square(h),  "[ square"),
        (right_square(h), "square ]"),
        (right_curly(h),  "curly }"),
        (right_paren(h),  "paren )"),
    ]
    print_side_by_side(columns, spacing=4)
    print()

    subheader("Nested: parentheses inside curly braces inside square brackets")
    lsq = left_square(9)
    lcurly = left_curly(7)
    lp = left_paren(5)
    rp = right_paren(5)
    rcurly = right_curly(7)
    rsq = right_square(9)

    for i in range(9):
        # Square bracket layer
        sq_l = lsq[i]
        sq_r = rsq[i]

        # Curly bracket layer (offset by 1)
        j_curly = i - 1
        if 0 <= j_curly < 7:
            cu_l = lcurly[j_curly]
            cu_r = rcurly[j_curly]
        else:
            cu_l = cu_r = " "

        # Parenthesis layer (offset by 2)
        j_paren = i - 2
        if 0 <= j_paren < 5:
            pa_l = lp[j_paren]
            pa_r = rp[j_paren]
            if j_paren == 2:
                content = f" {MAGENTA}x\u00b2+1{RESET}  "
            else:
                content = "       "
        else:
            pa_l = pa_r = " "
            content = "       "

        print(f"    {sq_l} {cu_l} {pa_l}{content}{pa_r} {cu_r} {sq_r}")
    print()


def demo_math_expressions() -> None:
    """Show parentheses used in realistic mathematical expressions."""
    header("Mathematical Expressions")

    # Binomial coefficient: (n choose k) = n! / (k!(n-k)!)
    subheader("Binomial coefficient")
    lp = left_paren(3)
    rp = right_paren(3)
    frac_bar = HORIZ_LINE_EXT * 11
    print(f"    {lp[0]} {YELLOW}n{RESET} {rp[0]}        {YELLOW}n!{RESET}")
    print(f"    {lp[1]}   {rp[1]}  =  {frac_bar}")
    print(f"    {lp[2]} {YELLOW}k{RESET} {rp[2]}     {YELLOW}k!{RESET}{DIM}({RESET}{YELLOW}n-k{RESET}{DIM})!{RESET}")
    print()

    # Function definition with cases (piecewise)
    subheader("Piecewise function")
    lp3 = left_paren(3)
    rp3 = right_paren(3)
    lcurly3 = left_curly(3)
    #             ⎛   ⎞         ⎧   1    if x > 0
    #     sgn     ⎜ x ⎟    =   ⎨   0    if x = 0
    #             ⎝   ⎠         ⎩  -1    if x < 0
    print(f"            {lp3[0]}   {rp3[0]}        {lcurly3[0]}  {YELLOW} 1{RESET}    {DIM}if x > 0{RESET}")
    print(f"    {MAGENTA}sgn{RESET}     {lp3[1]} {YELLOW}x{RESET} {rp3[1]}   =    {lcurly3[1]}  {YELLOW} 0{RESET}    {DIM}if x = 0{RESET}")
    print(f"            {lp3[2]}   {rp3[2]}        {lcurly3[2]}  {YELLOW}-1{RESET}    {DIM}if x < 0{RESET}")
    print()

    # Tall fraction with parentheses
    subheader("Quadratic formula")
    lp3 = left_paren(3)
    rp3 = right_paren(3)
    frac = HORIZ_LINE_EXT * 24
    #                   ⎛            ⎞
    #      -b  ±  √    ⎜  b² - 4ac  ⎟
    #                   ⎝            ⎠
    # x  =  ⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯⎯
    #                   2a
    print(f"                  {lp3[0]}            {rp3[0]}")
    print(f"       {YELLOW}-b{RESET}  \u00b1  \u221a   {lp3[1]}  {YELLOW}b\u00b2 - 4ac{RESET}  {rp3[1]}")
    print(f"                  {lp3[2]}            {rp3[2]}")
    print(f"    {MAGENTA}x{RESET}  =  {frac}")
    print(f"                  {YELLOW}2a{RESET}")
    print()

    # Product with tall parentheses
    subheader("Euler's product formula for sin(x)")
    lp4 = left_paren(4)
    rp4 = right_paren(4)
    frac2 = HORIZ_LINE_EXT * 6
    #                     ∞
    #                     ∏     ⎛       x²      ⎞
    #     sin(x) = x  ·        ⎜  1 - ──────   ⎟
    #                    n=1    ⎜      n²π²     ⎟
    #                           ⎝               ⎠
    # Left paren at col 26 for all rows; inner width = 15; right paren at col 42
    print(f"                    {DIM}\u221e{RESET}")
    print(f"                    \u220f     {lp4[0]}       {YELLOW}x\u00b2{RESET}      {rp4[0]}")
    print(f"    {MAGENTA}sin(x){RESET} = {YELLOW}x{RESET} \u00b7          {lp4[1]}  1 - {frac2}   {rp4[1]}")
    print(f"                   {DIM}n=1{RESET}    {lp4[2]}      {YELLOW}n\u00b2\u03c0\u00b2{RESET}     {rp4[2]}")
    print(f"                          {lp4[3]}               {rp4[3]}")
    print()


def demo_matrix_with_parens() -> None:
    """Show a matrix using parentheses (common in linear algebra)."""
    header("Matrix with Parentheses")

    subheader("3\u00d73 identity matrix")
    rows = [
        " 1  0  0 ",
        " 0  1  0 ",
        " 0  0  1 ",
    ]
    h = len(rows)
    lp = left_paren(h)
    rp = right_paren(h)
    for i in range(h):
        print(f"    {lp[i]} {YELLOW}{rows[i]}{RESET} {rp[i]}")
    print()

    subheader("4\u00d74 rotation matrix")
    rows = [
        " cos\u03b8  -sin\u03b8   0   0 ",
        " sin\u03b8   cos\u03b8   0   0 ",
        "   0      0    1   0 ",
        "   0      0    0   1 ",
    ]
    h = len(rows)
    lp = left_paren(h)
    rp = right_paren(h)
    for i in range(h):
        print(f"    {lp[i]} {YELLOW}{rows[i]}{RESET} {rp[i]}")
    print()

    subheader("Matrix multiplication")
    a_rows = [" a  b ", " c  d "]
    b_rows = [" e  f ", " g  h "]
    r_rows = [" ae+bg  af+bh ", " ce+dg  cf+dh "]

    h = 2
    lp = left_paren(h)
    rp = right_paren(h)
    for i in range(h):
        eq = " =  " if i == 0 else "    "
        dot = f" {DIM}\u00b7{RESET}  " if i == 0 else "    "
        print(f"    {lp[i]} {YELLOW}{a_rows[i]}{RESET} {rp[i]}"
              f"{dot}{lp[i]} {YELLOW}{b_rows[i]}{RESET} {rp[i]}"
              f"{eq}{lp[i]} {MAGENTA}{r_rows[i]}{RESET} {rp[i]}")
    print()


def main() -> None:
    print(f"\n{BOLD}{CYAN}")
    print("  \u2554\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
          "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
          "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
          "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2557")
    print("  \u2551   Parenthesis Pieces Rendering Demo                 \u2551")
    print("  \u2551   Unicode U+239B through U+23A0                     \u2551")
    print("  \u2551   Contour Terminal Emulator                         \u2551")
    print("  \u255a\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
          "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
          "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
          "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u255d")
    print(f"{RESET}")

    demo_character_table()
    demo_parentheses_heights()
    demo_junction_check()
    demo_bracket_comparison()
    demo_math_expressions()
    demo_matrix_with_parens()

    print(f"\n  {DIM}End of demo. All 6 parenthesis pieces (U+239B\u2013U+23A0)")
    print(f"  should be rendered with pixel-perfect built-in glyphs.")
    print(f"  Hooks should have smooth arcs; extensions should be")
    print(f"  edge-aligned vertical lines connecting seamlessly.{RESET}\n")


if __name__ == "__main__":
    main()
