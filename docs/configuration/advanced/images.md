# Advanced Image configuration

### Sixel scrolling mode

Enable or disable sixel scrolling (SM/RM ?80 default)

    sixel_scrolling: true

### Sixel register capacity

Configures the maximum number of color registers available
for rendering Sixel graphics.

    sixel_register_count: 4096

### Maximum image size

The maximum width and height in pixels of an image to be accepted is derived from the size of the
screen the window is on, and updates when the window moves to another screen.

Applications can query it, and lower it for themselves, via `XTSMGRAPHICS`
(`CSI ? 2 ; 4 S` reads the maximum, `CSI ? 2 ; 3 ; <width> ; <height> S` sets it).

!!! note

    `max_width` and `max_height` are **deprecated and ignored**. They are still accepted, so existing
    configurations keep loading, but no longer have any effect. Their default of `0` already meant
    "use the screen size", so only configurations that set an explicit cap change behaviour.
