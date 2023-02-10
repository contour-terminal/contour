# Miscelenous Advanced Configuration Options

## Platform plugin

Overrides the auto-detected platform plugin to be loaded.

Possible (incomplete list of) values are:

- auto        The platform will be auto-detected.
- xcb         Uses XCB plugin (for X11 environment).
- cocoa       Used to be run on Mac OS/X.
- direct2d    Windows platform plugin using Direct2D.
- winrt       Windows platform plugin using WinRT.

Default: auto

    platform_plugin: auto


## Default PTY read buffer size

This is an advance option. Use with care!
Default: 16384

    read_buffer_size: 16384


## New-Terminal spawn behaviour

This flag determines whether to spawn new process or not when creating new terminal

If this option is set to `false`, then simply a new terminal window is being
created rather thena fully creating a new process.

Default: `false`

    spawn_new_process: false

# Text reflow on resize

Whether or not to reflow the lines on terminal resize events.

Default: `true`

    reflow_on_resize: true

# Backspace character

There is little consistency between systems as to what should be sent when the
user presses the backspace key. By default Contour sends ^? but if you prefer
to send ^H (or something else again) then you can add an entry to the
`input_mapping` section of your config file like:

```
- { mods: [], key: BackSpace, action: SendChars, chars: "\x08" }
- { mods: [Control], key: BackSpace, action: SendChars, chars: "\x7F" }
```
