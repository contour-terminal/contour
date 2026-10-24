# terminal-extensions:control@0.1.0

`world.wit` lets a WebAssembly extension drive the terminal that hosts it: list its windows, tabs and
panes, open and split them, type into a pane, read what a pane shows and the command blocks it ran.
The verbs are the terminal's control API, the same one scripts reach over its socket, minus the verbs
that wait or stream (`block.wait`, `events.subscribe`, `events.unsubscribe`): a guest must not block
on a shell.

`world.wit` is generated from the host's method catalog. Do not edit it: regenerate it with the host's
`api wit` verb (`<terminal> api wit > wit/control/world.wit`), or by running the catalog's golden test
with `UPDATE_WIT=1` set. The golden test fails whenever the file and the catalog disagree.

## Importing the interface

An extension imports one interface, `control`. One that draws a status line as well includes its own
world and imports the interface beside it:

```wit
package my:extension;

world extension {
    include terminal-extensions:statusline/statusline@0.1.0;
    import terminal-extensions:control/control@0.1.0;
}
```

In the core module, the imports come from the module `terminal-extensions:control/control@0.1.0`,
one function per verb, named like the WIT func (`pane-split`). Each follows the Canonical ABI: the
request's flattened values, then the address the `result` is written to. A request of more than 16
flat values is passed by address instead. A host refuses to load a module that imports a function it
does not provide.

## Permissions

Nothing is callable until it is declared and granted. The extension's manifest declares what it needs,
one line per permission:

```
control read
control input
```

The user approves the declared permissions together with the extension's commands, and the approval
is bound to the manifest: a manifest that declares a new permission asks again. A verb whose
permission is not declared, or not granted yet, or denied, fails with `forbidden`, and the host does
nothing. A verb that runs a program -- `tab-create` or `pane-split` naming a `command` -- needs `input`
as well, since running a program is typing it: `write` alone may rearrange the workspace, not run
what it likes in it.

The prompt names each permission by what it lets the extension do:

| Permission | The prompt says                                      |
| ---------- | ---------------------------------------------------- |
| `read`     | May see the windows, tabs and panes.                 |
| `screen`   | May read what panes show, and their command history. |
| `input`    | May type into panes.                                 |
| `write`    | May open, split, close and label tabs and panes.     |

Each verb needs one of them, and some requests a second:

| Verb               | Permission | Also needs                       |
| ------------------ | ---------- | -------------------------------- |
| `daemon-info`      | `read`     |                                  |
| `workspace-list`   | `read`     |                                  |
| `tab-create`       | `write`    | `input`, for running a command   |
| `pane-split`       | `write`    | `input`, for running a command   |
| `pane-send`        | `input`    |                                  |
| `pane-capture`     | `screen`   |                                  |
| `pane-close`       | `write`    |                                  |
| `tab-close`        | `write`    |                                  |
| `object-set-label` | `write`    |                                  |
| `block-list`       | `screen`   |                                  |
| `block-read`       | `screen`   |                                  |
| `api-list`         | `read`     |                                  |
| `api-describe`     | `read`     |                                  |

Each func's doc comment in `world.wit` says the same.

## What a host guarantees

- **Errors are values.** A verb that fails returns a `control-error`; it never traps the guest. A
  `request` the guest lowered wrongly (a string outside its memory, invalid UTF-8, an unknown enum
  case) traps, as the Canonical ABI does.
- **The caller is the extension's pane.** A target left empty (`pane: ""`) means the pane the
  extension runs beside, as it means the calling shell's pane over the socket.
- **`pane-send` does not wait.** A `send-request` whose `wait` is not `none` fails with
  `bad-request`.
- **The guest's deadline keeps running.** A verb counts against the deadline of the call it is made
  from; one the host cannot answer in that time fails with `failed` ("timed out").
- **Replies are bounded.** Text a verb returns (`pane-capture`, `block-read`) is cut from its head
  to fit the host's reply cap, and says so in its `truncated` field.
