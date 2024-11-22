# Dark and Light Mode detection

Most modern operating systems and desktop environments do support Dark and Light themes,
this includes at least MacOS, Windows, KDE Plasma, Gnome, and probably others.

Some even support switching from dark to light and light to dark mode based on sun rise / sun set.

In order to not make the terminal emulator look bad after such switch, we must
enable the applications inside the terminal to detect when the terminal has
updated the color palette. This may happen either due to the operating system having
changed the current theme or simply because the user has explicitly requested to
reconfigure the currently used theme.

Ideally we are getting CLI tools like [delta]() to query the theme mode before sending out RGB values
to the terminal to make the output look more in line with the rest of the desktop.

But also TUIs like vim should be able to reflect dark/light mode changes as soon as the
desktop has charnged from light to dark and vice versa, e.g. to make it more pleasing to the eyes.

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

## Example source code

Please have a look at our example C++ [source code](https://github.com/contour-terminal/contour/blob/master/examples/detect-dark-light-mode.cpp)
in order to see how to implement this in your own application.
