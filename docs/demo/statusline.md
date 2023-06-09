# Statusline

The Statusline in Contour is an implementation of the
[DEC VT320 statusline feature](https://www.vt100.net/docs/vt320-uu/appendixe.html).
We aim to revive this feature as we see great use to it.

## Indicator Statusline

This is the most obvious and probably most used one. It's the statusline as
you may be used to it.

Contour shows relevant contextual information on it, such as:

- the clock,
- current VT emulation mode,
- input mode

and much more, depending on the context.

This line can be actively toggled by the user via configuration, e.g. via:

```yaml
input_mapping:
    - { mods: [Control, Alt],   key: '.',           action: ToggleStatusLine }
```

## Host Programmable Statusline

This statusline can be alternatively displayed and can be
written to and fully controlled like the main display.

There are not many applications yet doing so, but there would be a great use to
such a feature by any application that has fast output but a rarely updating
status line, maybe even tmux or screen could make use of it to trivialize
the common use-case.

If you consider playing around with it, have a look at the VT sequences

- `DECSASD` - Select Active Status Display
- `DECSSDT` - Select Status Display (Line) Type
