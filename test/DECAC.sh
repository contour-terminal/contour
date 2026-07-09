#! /bin/bash
#
# DECAC - Assign Color (VT525)
#
#   CSI Ps1 ; Ps2 ; Ps3 , |
#
#     Ps1 = item     1 = normal text, 2 = window frame (GUI tab color)
#     Ps2 = foreground palette index (0..255)
#     Ps3 = background palette index (0..255)
#
#   Omitting the colors (CSI Ps1 , |) resets the item to its host default.
#   A hard reset (RIS, ESC c) also clears an application-assigned tab color.

clear

echo "DECAC demo - watch the tab color and the default text colors change."
echo

# --- Item 2: window frame -> GUI tab background color -------------------------

echo "Coloring this tab RED (palette index 1)..."
echo -ne "\033[2;15;1,|"     # item 2, fg=15 (white), bg=1 (red) -> red tab
sleep 2

echo "Now GREEN (palette index 2)..."
echo -ne "\033[2;0;2,|"      # item 2, bg=2 (green) -> green tab
sleep 2

echo "Now BLUE (palette index 4)..."
echo -ne "\033[2;15;4,|"     # item 2, bg=4 (blue) -> blue tab
sleep 2

echo "Resetting the tab color to the default..."
echo -ne "\033[2,|"          # item 2, no colors -> reset tab color
sleep 2

# --- Item 1: normal text -> default foreground / background -------------------

echo
echo "Setting the default text colors (fg=3 yellow, bg=4 blue)..."
echo -ne "\033[1;3;4,|"       # item 1, fg=3 (yellow), bg=4 (blue)
sleep 2
echo "This line is drawn with the new default colors."
sleep 2

echo -ne "\033[1,|"          # item 1, no colors -> reset default fg/bg
echo "Default text colors reset."
sleep 1

echo
echo "Press <Return> to finish (this also issues a hard reset)."
read

echo -ne "\033c"             # RIS - hard reset: clears any assigned tab color too
