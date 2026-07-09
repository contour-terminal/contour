#! /bin/bash
#
# DECATC - Alternate Text Color (VT525)
#
#   CSI Ps1 ; Ps2 ; Ps3 , }
#
#     Ps1 = attribute combination (enumerated per VT525 manual, NOT a bitmask):
#           0 Normal, 1 Bold, 2 Reverse, 3 Underline, 4 Blink, 5 Bold reverse,
#           6 Bold underline, 7 Bold blink, 8 Reverse underline, ... 15 all four.
#     Ps2 = foreground palette index (0..255)
#     Ps3 = background palette index (0..255)
#
#   DECATC only takes effect in DECSTGLT "Alternate Color" mode (CSI 1 ) {); it is the
#   backwards-compatibility model for pre-ANSI-color terminals, where text color comes
#   from the attributes rather than SGR color sequences. In that mode SGR colors are
#   ignored entirely: a combination with no assignment renders in the default text colors.
#   Omitting the colors (CSI Ps1 , }) resets that entry -- including CSI 0 , } for normal
#   text; RIS clears all assignments and returns to ANSI SGR color mode.

clear

echo "DECATC demo - color chosen by text attribute (VT240-style Alternate Color mode)."
echo

# Assign vivid colors to a few attribute combinations (before entering alternate mode).
echo -ne "\033[1;15;1,}" # 1 = bold           -> white on red
echo -ne "\033[3;0;2,}"  # 3 = underline      -> black on green
echo -ne "\033[4;15;4,}" # 4 = blink          -> white on blue
echo -ne "\033[6;15;5,}" # 6 = bold underline -> white on magenta

# Enter Alternate Color mode so the assignments above drive the on-screen colors.
echo -ne "\033[1){"
sleep 1

echo "Bold text uses the DECATC bold colors:"
echo -ne "  \033[1mThis is bold\033[0m\n"
sleep 1

echo "Underlined text uses the DECATC underline colors:"
echo -ne "  \033[4mThis is underlined\033[0m\n"
sleep 1

echo "Blinking text uses the DECATC blink colors:"
echo -ne "  \033[5mThis is blinking\033[0m\n"
sleep 1

echo "Bold + underline uses its own combination colors:"
echo -ne "  \033[1;4mBold and underlined\033[0m\n"
sleep 1

echo
echo "Back in ANSI SGR color mode, DECATC has no effect and normal colors return:"
echo -ne "\033[3){" # DECSTGLT 3 = ANSI SGR color (the default)
echo -ne "  \033[1mBold is back to normal SGR colors\033[0m\n"
sleep 1

echo
echo "Press <Return> to finish (this also issues a hard reset)."
read

echo -ne "\033c" # RIS - hard reset: clears all DECATC assignments and mode
