#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prints the code point ranges a TrueType font covers, as the C++ table in
src/text_shaper/NerdFontSymbolRanges.hpp, so that bundling a newer Nerd Fonts release is a matter of
pasting this script's output over that table.

Usage: generate-nerd-font-ranges.py SymbolsNerdFontMono-Regular.ttf

Reads the font's 'cmap' (formats 4 and 12) and its 'post' table directly: no dependencies.
"""
import struct
import sys

# The ranges below U+2000 hold ordinary symbols (arrows, box drawing, ...) the terminal already draws
# or the system's fonts cover; the bundled font is only offered for what is private to Nerd Fonts.
# Anything else it happens to carry is deliberately left to the platform's own fallback.
PRIVATE_USE_AREAS = ((0xE000, 0xF8FF), (0xF0000, 0xFFFFD))
# Nerd Fonts' few non-private-use additions: IEC power symbols, "heavy angle brackets" and one more.
EXTRA_SYMBOLS = ((0x23FB, 0x23FE), (0x2665, 0x2665), (0x26A1, 0x26A1), (0x276C, 0x2771), (0x2B58, 0x2B58))


def table(data: bytes, tag: bytes) -> bytes:
    count = struct.unpack(">H", data[4:6])[0]
    for i in range(count):
        t, _checksum, offset, length = struct.unpack(">4sIII", data[12 + 16 * i : 28 + 16 * i])
        if t == tag:
            return data[offset : offset + length]
    raise SystemExit(f"font has no '{tag.decode()}' table")


def covered(cmap: bytes) -> set:
    """The code points the font gives a real glyph: one other than glyph 0, '.notdef', which a segment
    of a format 4 subtable may well map a gap in its range to."""
    result = set()
    n = struct.unpack(">H", cmap[2:4])[0]
    for i in range(n):
        _platform, _encoding, offset = struct.unpack(">HHI", cmap[4 + 8 * i : 12 + 8 * i])
        fmt = struct.unpack(">H", cmap[offset : offset + 2])[0]
        if fmt == 12:
            groups = struct.unpack(">I", cmap[offset + 12 : offset + 16])[0]
            for g in range(groups):
                lo, hi, first_glyph = struct.unpack(">III", cmap[offset + 16 + 12 * g : offset + 28 + 12 * g])
                result.update(c for c in range(lo, hi + 1) if first_glyph + (c - lo) != 0)
        elif fmt == 4:
            segx2 = struct.unpack(">H", cmap[offset + 6 : offset + 8])[0]
            seg = segx2 // 2
            ends = struct.unpack(f">{seg}H", cmap[offset + 14 : offset + 14 + segx2])
            starts = struct.unpack(f">{seg}H", cmap[offset + 16 + segx2 : offset + 16 + 2 * segx2])
            deltas = struct.unpack(f">{seg}H", cmap[offset + 16 + 2 * segx2 : offset + 16 + 3 * segx2])
            range_offsets_at = offset + 16 + 3 * segx2
            range_offsets = struct.unpack(f">{seg}H", cmap[range_offsets_at : range_offsets_at + segx2])
            for s in range(seg):
                if starts[s] == 0xFFFF:
                    continue
                for c in range(starts[s], ends[s] + 1):
                    if range_offsets[s] == 0:
                        glyph = (c + deltas[s]) & 0xFFFF
                    else:
                        at = range_offsets_at + 2 * s + range_offsets[s] + 2 * (c - starts[s])
                        glyph = struct.unpack(">H", cmap[at : at + 2])[0]
                        if glyph != 0:
                            glyph = (glyph + deltas[s]) & 0xFFFF
                    if glyph != 0:
                        result.add(c)
    return result


def ranges(points: list) -> list:
    out = []
    for p in sorted(points):
        if out and p == out[-1][1] + 1:
            out[-1][1] = p
        else:
            out.append([p, p])
    return out


def main() -> None:
    data = open(sys.argv[1], "rb").read()
    is_fixed_pitch = struct.unpack(">I", table(data, b"post")[12:16])[0]
    points = covered(table(data, b"cmap"))
    keep = [p for p in points
            if any(lo <= p <= hi for lo, hi in PRIVATE_USE_AREAS + EXTRA_SYMBOLS)]
    print(f"// isFixedPitch={is_fixed_pitch}; {len(points)} code points in the font, {len(keep)} offered", file=sys.stderr)
    for lo, hi in ranges(keep):
        print(f"    CodepointRange {{ 0x{lo:04X}, 0x{hi:04X} }},")


main()
