# Dark and Light Mode detection

Most modern operating systems and desktop environments do support Dark and Light themes,
this includes at least MacOS, Windows, KDE Plasma, Gnome, and probably others.

Some even support switching from dark to light and light to dark mode based on sun rise / sun set.

In order to not make the terminal emulator look bad after such switch, we must
enable the applications **inside** the terminal to detect when the terminal has
updated the color palette. This may happen either due to the operating system having
changed the current theme or simply because the user has explicitly requested to
reconfigure the currently used theme (e.g. because the user requested to change the terminal profile,
also containing a different color scheme).

Ideally we are getting CLI tools like [delta]() to query the theme mode before sending out RGB values
to the terminal to make the output look more in line with the rest of the desktop.

But also TUIs like vim should be able to reflect dark/light mode changes as soon as the
desktop has changed to light/dark mode or the user has changed the terminal profile.

## Query the current theme mode?

Send `CSI ? 996 n` to the terminal to explicitly request the current
color preference (dark mode or light mode) by the operating system.

The terminal will reply back in either of the two ways:

VT sequence       | description
------------------|---------------------------------
`CSI ? 997 ; 1 n` | DSR reply to indicate dark mode
`CSI ? 997 ; 2 n` | DSR reply to indicate light mode

## Request unsolicited DSR on color palette updates

Send `CSI ? 2031 h` to the terminal to enable unsolicited DSR (device status report) messages
for color palette updates and `CSI ? 2031 l` respectively to disable it again.

The sent out DSR looks equivalent to the already above mentioned.
This notification is not just sent when dark/light mode has been changed
by the operating system / desktop, but also if the user explicitly changed color scheme,
e.g. by configuration.

### When to send out the DSR?

A terminal emulator should only send out the DSR when the palette has been updated due to a change in the
terminal emulator's color palette, e.g. because the user has changed the terminal profile directly
or indirectly by changing the operating system theme.

## Example source code

Please have a look at our example C++ [source code](https://github.com/contour-terminal/contour/blob/master/examples/detect-dark-light-mode.cpp)
in order to see how to implement this in your own application.

## Adoption State

| Support  | Terminal/Toolkit/App | Notes                                                                                               |
|----------|----------------------|-----------------------------------------------------------------------------------------------------|
| ✅       | Contour              | since `0.4.0`                                                                                       |
| ✅       | Ghostty              | since `1.0.0`                                                                                       |
| ✅       | Kitty                | since [`0.38.1`](https://sw.kovidgoyal.net/kitty/changelog/#detailed-list-of-changes)               |
| ✅       | Neovim               | since [`d460928`](https://github.com/neovim/neovim/commit/d460928263d0ff53283f301dfcb85f5b6e17d2ac) |
| ✅       | tmux                 | see tracker: [tmux#4269](https://github.com/tmux/tmux/issues/4269)                                  |
| not yet  | LazyGit              | see tracker: [lazygit#4366](https://github.com/jesseduffield/lazygit/issues/4366)                   |
| ✅       | Helix                | since [`23a647a`](https://github.com/helix-editor/helix/commit/23a647aee81a69f356b018bdaa49b634b9499cec)|
| not yet  | WezTerm              | see tracker: [wezterm#6454](https://github.com/wez/wezterm/issues/6454)                             |
| not yet  | Zellij               | see tracker: [zellij#3831](https://github.com/zellij-org/zellij/issues/3831)                        |
| not yet  | VTE                  | see tracker: [vte#2740](https://gitlab.gnome.org/GNOME/vte/-/issues/2740)                           |
| not yet  | Vim                  | see tracker: [vim#17251](https://github.com/vim/vim/issues/17251)                                   |
| not yet  | Nushell              | see tracker: [nushell#15227](https://github.com/nushell/nushell/issues/15227)                       |

## Tools

* [rod](https://github.com/leiserfg/rod): Terminal Dark/Light Mode Detection Tool
