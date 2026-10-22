# terminal-extensions:statusline@0.1.0

`world.wit` is the normative contract between a terminal (the *host*) and an extension (the *guest*)
that draws a status line. This page says what the WIT text cannot: when the host calls what, what it
promises, and how the contract is carried in a binary.

## Lifecycle

1. **load**: called once, first, with the extension's configuration as name/value pairs. It returns
   the `event-kinds` it wants delivered. `resized`, `timer` and `reply` are always delivered.
   The host calls `render` once right after `load` succeeds.
2. **update**: called once per delivered event, in order. It returns a `render-request`:
   `none` if nothing the extension shows has changed, `render` if it has.
3. **render**: called with the size of the surface, in cells, and only after an `update` that
   returned `render`, or once after `load`. A host never calls `render` on a timer of its own;
   an extension that animates asks for a `timer` event with `request-frame` and answers it with
   `render`.

An extension holds its own state between calls. It reads the terminal's current values by calling
the `state` import whenever it wants, including inside `load`.

## What a host guarantees

- **Calls are serialized per instance.** The calls of one instance never overlap, and an instance
  is never entered from two threads at once. An extension needs no locks.
- **Never on the host's I/O or UI threads.** Calls run on a worker the host owns, so a slow
  extension delays its own status line and nothing else.
- **Every call has a deadline.** A call that exceeds it is stopped and counts as a fault, as does
  a trap. Time spent waiting for a `run-command` does not count against it: the host bounds the
  command itself. A host may restart a faulted instance, back off, and eventually stop calling it. The
  output of the last successful `render` stays on display meanwhile.
- **Memory is capped.** Growing past the cap traps.
- **No ambient authority.** The instance sees no files, no sockets, no environment. Time comes from
  `state`, and commands from `run-command`. The time zone does too: `state`'s `utc-offset-minutes` is
  the host's local offset at `unix-time-ms`, so an extension that shows a time of day shows the user's,
  daylight saving included, and reads it again rather than keeping it.
- **`command-output` may be partial.** A host that cannot tell a command's exit code or its standard
  error reports an exit code of 0 and an empty `stderr`; an extension must not rely on either.
- **`run-command` is limited to what the extension declares.** A command that is not declared, or
  whose user approval is still pending, fails the call at once with `command-error`; it never
  blocks.
- **Events are best effort under load.** State events may be coalesced, so an extension sees the
  latest value rather than every intermediate one. When the host's queue is full, events are
  dropped, never delivered late in bulk.
- **`request-frame` is a request.** The host may deliver `timer` later than asked, never earlier,
  and may limit how often it delivers one.

## VT expectations

`render` returns bytes for a terminal of the given `cols` by `rows` cells, as if they were written to
that surface: printable text, SGR attributes, cursor movement. The surface starts empty and is
reset by the host before each `render`; output should not depend on what an earlier `render` drew
or set. The reset aborts any sequence left unterminated, selects the VT500 conformance level with
7-bit replies, soft-resets the terminal (DECSTR: modes, margins, rendition), turns autowrap off (text
past the last column is clipped, never wrapped), erases the surface and homes the cursor.

Queries the extension writes in its output (device attributes, cursor position report, and so on)
are answered like any terminal answers them, but the answer reaches the extension only as a `reply`
event carrying the answer's bytes. It never reaches the program running in the pane. A reply
therefore arrives after the `render` that asked, and an extension that cares answers it with
another `render`.

Output beyond a host-defined size limit is discarded and counts as a fault. Graphics protocols are
for the host to allow or refuse; an extension should not assume them.

Where a host allows Sixel, the surface is in Sixel display mode (DECSDM set), which keeps an image
from scrolling the surface: a Sixel image anchors at the surface's top-left whatever the cursor
position, and is cut off at its bottom edge. That suits a background. An image placed at the cursor
should use the kitty graphics protocol instead.

The surface's cells are the size of the terminal's: `state`'s `font` gives that size in device px, as
`CSI 16 t` reports it, and the font's point size. A host that knows them tells every extension the
surface's new cell size whenever the font is zoomed, reloaded or moved to a screen of another density,
and an extension
that follows `font` also gets a `font-changed` event, the cue to draw an image again at the new size.
A host that draws in no font of its own, such as one serving a session to remote clients, reports
zeros in `state` and sends no `font-changed`: 0 means unknown, never a size.

## The binary contract

The contract is carried by the **Canonical ABI of the WebAssembly Component Model, on a core
module**. This is what `wit-bindgen` emits before `wasm-tools component new` wraps it into a
component; here the module is used as it is and is not componentized. Any language with a
`wit-bindgen` generator produces a conforming extension:

- The target is `wasm32-wasip1`. The host provides WASI preview 1 with no preopened directories and
  no sockets, so a language's standard library works but has no reach beyond the module. Only the
  WASI functions that return at once and reach nothing outside the module may be imported: the
  arguments and environment (both empty), clocks (`clock_time_get`, `clock_res_get`), writing to
  and describing the standard streams (`fd_write`, `fd_close`, `fd_fdstat_get`, `fd_seek`,
  `fd_prestat_get`, `fd_prestat_dir_name`), `random_get`, `proc_exit` and `sched_yield`. A module
  that imports any other, such as `poll_oneoff` (which a sleep compiles to), a `sock_*` or a
  `path_*` function, does not load. An extension never sleeps: it asks for a `timer` event with
  `request-frame`.
- **Imports** are named by module `terminal-extensions:statusline/host@0.1.0` and function name:
  `request-frame`, `log`, `state`, `run-command`. A module imports only what it calls.
- **Exports** are named by function name only: `load`, `update`, `render`. The module also
  exports `cabi_realloc`, which the host calls to place arguments in the module's linear memory,
  and `cabi_post_<name>` for each export whose result owns memory (`cabi_post_render`), which the
  host calls once it has copied the result out.
- The module has **no start function**: a host instantiates it before any deadline can guard
  it. A module that needs initialization is a WASI reactor and exports `_initialize`, which the
  host calls once, before `load`, under the same deadline as `load`.
- Values follow the Canonical ABI: strings and lists are a (pointer, length) pair into linear
  memory, records and tuples are laid out with natural alignment, variants are a discriminant plus
  the widest payload, enums and flags are integers. A result that does not fit in the flat core
  values is written to memory, and the export returns a pointer to it.
- A `bool` is one byte, 0 or 1. The three in the world (`tab.active`, the `focus` event and
  `state-snapshot.focused`) are the wire representation of a yes/no; a host may convert them to
  its own types at the boundary.

A host that implements the Component Model may componentize the same module and run it unchanged;
that takes the WASI preview 1 adapter, because the module imports `wasi_snapshot_preview1`.

## The manifest

An extension declares what it is, and the commands it may run, in a custom section of its module
named `terminal-extension-manifest`. The host reads it by scanning the module's sections, before it
runs any of the module's code. A module with a section that does not parse does not load. A module
without the section declares no commands: it loads, and every `run-command` it makes fails with
`not-declared`. In Rust, the `manifest` crate under `extensions/manifest` emits the section:
`manifest::declare_manifest!("name = git\nversion = 1\n...")`.

The section is UTF-8 text, one declaration per line. Blank lines and lines starting with `#` are
ignored, and blanks around and between words are not significant. The text must be valid UTF-8 and
may hold no control character (C0, DEL or C1) other than line feed, tab, and a carriage return
immediately before a line feed. A module with two manifest sections is refused: linkers concatenate
same-named sections, so only the final module may declare one (a dependency crate must not call
`declare_manifest!`).

```
name = git
version = 1
command status = git -C {cwd} status --porcelain=v2 --branch
```

- `name = <text>`: the extension's name, for display. Required, once. It is chosen by the guest, so
  grants never key on it.
- `version = <integer>`: the manifest format revision the extension was written against. Required,
  once. The only revision is `1`; any other value makes the manifest invalid.
- `command <name> = <argv...>`: a command `run-command` may name. The words after `=` are the
  command's argv. No shell is involved: nothing is split, expanded or quoted beyond that.
  The names are unique, and the argv is not empty. The first element, the program, is literal: a
  placeholder there makes the manifest invalid.

`run-command`'s `args` are name and value pairs.

An argv element of the form `{placeholder}` (a name of ASCII letters, digits, `_` and `-`) is filled
in, whole, from the `run-command` argument of that name; the value becomes exactly one argv element,
whatever it contains. A brace anywhere else,
such as `--dir={cwd}`, is an error: there is no partial-element substitution. A call that does not
supply an argument for every placeholder of the command it names does not match the declaration and
fails as if the command were not declared. Arguments that no placeholder names are ignored.

Any other line, a repeated `name`, `version` or command, a missing `name` or `version`, or a NUL in the
text makes the manifest invalid.

### Argument values

A value is refused, and the call fails as a command not declared with these arguments (the host
reports it as `ArgumentRejected`), when

- it contains a NUL, or
- it starts with `-` and no literal `--` element precedes its placeholder in the command's template:
  the program could take it for an option (`git log {ref}` with `--output=/home/u/.bashrc`).

After a literal `--` a leading `-` is data. A command that takes a free-form value should therefore
declare it after `--`, as in `git log -- {path}`.

### Permission

The host runs a declared command only when the user's configuration allows it, in this order:

1. configured `deny` refuses every command;
2. a command the manifest does not declare fails;
3. the host looks up the user's answer for the **host id** of the installation and the **digest** of
   the manifest. The host id is assigned by the host, such as its configuration key plus the
   canonical path of the module; it is never the manifest's own `name`, which the guest chooses, so
   a copy of an approved manifest does not inherit the approval. The digest is the lower-case hex
   SHA-256 of the section's bytes. A refusal fails the call, an approval lets it through, and no
   answer yet lets it through only when the configuration pre-approves (`allow`); otherwise it fails
   the call as pending while the user is asked elsewhere.

An approval belongs to one host id and one digest. An extension whose manifest changes in any byte
has a new digest and is asked about again.
