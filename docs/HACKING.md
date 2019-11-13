# Contour Terminal Emulator - Internals

## Configuration file schema & auto completion

Add the following block to your Visual Studio Code's `settings.json`:

```json
    "yaml.schemas": {
        "docs/contour.schema.json": ["contour.yaml"]
    },
```

## Repository Layout

* **docs/** - Project related documentation
* **cmake/** - CMake helper modules
* **3rdparty/** - Thirdparty dependencies, provided as git submodules
* **src/** - project specific source code
  * **ground/** - fundamentals library
  * **terminal/** - core terminal library
  * **terminal_view/** - library covering the OpenGL view parts
  * **contour/** - terminal emulator application (business logic)

## Process Threading Model

Contour Terminal Emulator is multithreaded.
The main thread is solely for receiving user input events and displaying screen updates plus some necessary administrative tasks.

The slave application will be spawned as a process and a
thread is being associated with it to exclusively read the
applications output, which will be processed inside the same thread.

## How terminal input is being processed

The user can send keyboard and mouse input
which the connected slave (application) will read
from its `stdin`.

1. The native OS will emit an event, which will be intercepted by GLFW3 (the OpenGL Framework)
2. The GLFW input events are being mapped to `terminal` API input events (see `InputGenerator`)
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
