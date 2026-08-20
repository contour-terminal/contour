#! /bin/sh
# VT-stream workload for the contour_e2e_vt_stream test: pushes a representative slice of the VT
# feature surface through the REAL pipeline (vtparser -> vtbackend -> vtrasterizer -> RhiRenderer)
# in one short offscreen run. The brief sleeps let the render loop pick frames up so the
# rasterizer/renderer actually draw what the backend staged.

printf '\033]0;coverage e2e title\007'                 # OSC 0: window title
printf '\033[1mBold\033[0m \033[3mItalic\033[0m \033[4mUnderline\033[0m \033[7mInverse\033[0m\n'
printf '\033[31mred \033[42mon-green\033[0m \033[38;5;208m256color\033[0m '
printf '\033[38;2;100;150;200mRGBcolor\033[0m\n'

# Box drawing + block elements (builtin glyph rasterization).
printf '┌──┐ █▓▒░\n└──┘\n'

# Wide (CJK) + emoji glyphs (double-width handling, font fallback).
printf 'wide: 你好 emoji: \U0001f600\n'

# Hyperlink (OSC 8).
printf '\033]8;;https://contour-terminal.org\033\\hyperlink\033]8;;\033\\ \n'

sleep 0.3

# Alternate screen round-trip + clear.
printf '\033[?1049h'
printf 'alt screen content\n'
sleep 0.2
printf '\033[?1049l'

# Scroll region + cursor movement + scrollback churn.
printf '\033[2;10r\033[2;1H'
i=0
while [ "$i" -lt 40 ]; do
    printf 'scroll line %d\n' "$i"
    i=$((i + 1))
done
printf '\033[r'

sleep 0.3
# OSC 3008 hierarchical context signalling: a nested ancestry, per-line association, the tint path and
# the {Context} breadcrumb, all through the real parser and the real renderer.
printf '\033]3008;start=e2e-session;type=session;user=%s;hostname=%s\033\\' "$(id -un)" "$(uname -n)"
printf '\033]3008;start=e2e-box;type=container;container=coverage-e2e;cwd=/app\033\\'
printf 'inside a container\n'
printf '\033]3008;start=e2e-root;type=elevate;targetuser=root\033\\'
printf 'elevated line\n'
sleep 0.3
printf '\033]3008;end=e2e-root;exit=failure;status=139;signal=SIGSEGV\033\\'
printf '\033]3008;end=e2e-box;exit=success\033\\'
printf '\033]3008;end=e2e-session\033\\'

exit 0
