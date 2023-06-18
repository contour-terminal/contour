# Contour Terminal Emulator - Internals

## Repository Layout

```
docs/             - Contour documentation and website
cmake/            - CMake helper modules
scripts/          - general development assisting and CI helper scripts
src/              - Contour source code
  contour/        - GUI terminal emulator application (business logic)
  crispy/         - fundamentals library
  text_shaper/    - font location and text shaping & rasterization library
  vtbackend/      - core terminal library
  vtparser/       - VT parser library
  vtpty/          - PTY library
  vtrasterizer/   - frontend independant Terminal rendering library
test/             - Contains a set of test scripts.
```

### `crispy` library

This library contains super generic helper functions that
are too small to be their own library and ease general development.

### `text_shaper` library

This library abstracts font location service, text shaping, as well as
glyph rasterization into a platform independant API.

### `vtpty` library

`vtpty` provides an abstract API to construct a PTY handle
to communicate between a terminal emulator (master) and a connected application (slave).

### `vtparser` library

`vtparser` library provides an API to parse VT sequences. This library
can be used on the master side (terminal emulator)
as well as slave side (terminal application, e.g. the shell).

### `vtbackend` library

Here we implement the actual terminal emulation. This is a headless library
and cannot just be used for a GUI terminal emulator software.
In order to render the screen state of a terminal to
pixels, the `vtrasterizer` library can be used.

### `vtrasterizer` library

`vtrasterizer` implements rasterizing the screen state into a bitmap.
This is done target independent, which means, that the caller must provide
the respective implementation to decide how the pixels are being stored (or rendered).

## Process Threading Model

Contour Terminal Emulator is multithreaded.
The main thread is solely for receiving user input events and
displaying screen updates plus some necessary administrative tasks.

The slave application will be spawned as a process and a
thread is being associated with it to exclusively read the
application's output, which will be processed inside the same thread.
That thread is responsible for updating the internal terminal state.

## How terminal input is being processed

The user can send keyboard and mouse input
which the connected slave (application) will read
from its `stdin`.

1. The native OS will emit an event, which will be intercepted by GLFW3 (the OpenGL Framework)
2. The target GUI input events are being mapped to `terminal` API input events (see `InputGenerator`)
3. The `terminal::InputEvent` objects are send to the respective `terminal_view::TerminalView`, which will apply key binds, if applicable, or pass the events to `terminal::Terminal`.
4. The `terminal::Terminal` instance will use `terminal::InputGenerator` to generate VT sequences depending on current mode flags.
5. The generated VT sequences are being transmitted to the slave application.

## How terminal output is being processed

The connected slave (application) may write a stream of characters to `stdout` or `stderr`,
which will be read by the terminal.

1. The VT stream is being parsed by a VT `Parser` that emits raw events
2. These events are taken by the `OutputHandler`, and translated to `Command` variant types - in case of a VT function (such as ESC, CSI, OSC) a unique ID is being constructed. This unique ID is then mapped to a `FunctionDef` with a `FunctionHandler` whereas the latter will perform semantic analysis in order to emit the higher level `Command` variant types.
3. The `Command` variant types are then processed in order by the `Screen` instance, that ultimatively interprets them.
4. A callback hooks is being invoked to notify about screen updates (useful for displaying updated screen contents).
