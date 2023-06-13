# Input Modes

Just like with the power of Vi-like modes in some editors, Contour Terminal comes
with so called vi-like modes to empower the advanced user with very fast access
to the screen and its history.

## Select, Yank, Paste

<video width="100%" controls>
  <source src="/videos/contour-normal-mode-select-and-yank.webm" type="video/webm">
  Your browser does not support the video tag.
</video>

This little videos shows how to get into normal mode (Ctrl+Shift+Space) and move to the
text that is to be yanked (`y`) into the clipboard.
Mind, the clipboard is being pasted with newlines being stripped off (`<Shift-p>`).

Note how the statusline at the bottom is reflecting the current input mode.

For more information on what motions Contour supports, please refer to its documentation [here](../input-modes.md).
