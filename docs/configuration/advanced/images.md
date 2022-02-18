# Advanced Image configuration

### Sixel scrolling mode

Enable or disable sixel scrolling (SM/RM ?80 default)

    sixel_scrolling: true

### Sixel register capacity

Configures the maximum number of color registers available
for rendering Sixel graphics.

    sixel_register_count: 4096

### Sixel cursor placement conformance

If enabled, the ANSI text cursor is placed at the position
of the sixel graphics cursor after image rendering,
otherwise (if disabled) the cursor is placed underneath the image.

    sixel_cursor_conformance: true

### Maximum image size

Sets the maximum width and height in pixels of an image to be accepted.

A value of 0 defaults to system screen pixel width/height.

Default: `0` (that is: current screen size).

    max_width: 0
    max_height: 0
