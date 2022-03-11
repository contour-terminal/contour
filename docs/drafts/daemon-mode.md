# Contour Daemon Mode

THIS IS A DRAFT DOCUMENT

Let's implement a terminal multiplexing server that `Contour` can connect to.

## Requirements

- support for handling multiple sessions
- support concurrent clients to the same sessions
- a session has one or more terminals, with a server managed layout (stacked, tabbed, tiled, ...)
- connected clients to the same terminal can independantly scroll in history.
- communication via `AF_UNIX` and `AF_INET`.
- attached clients to a terminal receive:
  - An initial fullscreen redraw event,
    - and then incremental updates via VT sequences,
  - A fullscreen redraw can be requested at any time.
  - history data can be requested on demand by the client
  - history-clear events can be pushed to all clients by the server
  - any necessary resources (such as image data) will be sent on-demand to the client.

## Server Design Thoughts

- standalone executable only linking to the absolutely necessary libraries (such as libterminal, but not Qt)
- should support binary upgrade
- server protocol must be specced (see below?)

...

## Client Design Thoughts

- have specialized `Terminal` class with a networked `Pty` that
  - sends input (keyboard/mouse) events via network
  - receives screen update events from remote BUT maintains local screen buffer for fast redraws

...

## Terminal Multiplexing Protocol

- either text based for easy adoption or protobuf-based (or alike) for optimal performance (or both?)

TODO

