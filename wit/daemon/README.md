# terminal-extensions:daemon@0.1.0

`world.wit` is the normative contract between a terminal's daemon (the *host*) and an extension (the
*guest*) that runs with no window attached: it starts declared processes, keeps a small durable store,
owns panes and serves tabs. This page says what the WIT text cannot: when the host calls what, what it
promises, and how the contract is carried in a binary.

## Lifecycle

1. **load**: called once, first, with the extension's configuration as one JSON document
   (`config-json`) and `host-info`: every root the manifest declares, resolved to absolute, normalised
   directories, the user's home directory, and the local time's offset from UTC at load.
2. **update**: called once per *coalesced* hint. The host keeps one slot per hint (`wake`, `procs`,
   `panes`, `tabs`, `actions`); a hint raised again before the guest is called is delivered once.
   Pending hints are delivered in that fixed order, not in the order they were raised.
   Hints carry no payload: the guest reads the current state through the matching import (`procs`,
   `panes`, `tabs`, `actions-take`), so a hint lost to coalescing loses nothing.

There is no `render`: a daemon extension shows itself through `summary-set` and the tabs it serves.
An extension holds its own state between calls, and what must outlive the instance in its store.

Processes and panes outlive the instance. When the host re-instantiates a guest (after a fault, or a
daemon upgrade), running processes and open panes are kept, and the new instance finds them by the
`tag` it gave them, through `procs` and `panes`, instead of assuming they failed. Until the daemon has
restored its sessions after a start, an upgrade or a recovery, `panes` answers `limit`: a guest concludes
nothing from that answer and lists again later: on the `panes` hint the daemon raises for every extension
whose panes it kept, or at its next wake.

## What a host guarantees

- **Calls are serialized per instance**, never on the daemon's I/O thread. An extension needs no locks.
- **Every import returns at once**, but for the durable `store-set` and `store-remove`. None of the
  others blocks longer than a memory copy: a process is started and later reported through the `procs`
  hint, never waited for.
- **Every call has a deadline.** A call that exceeds it is stopped and counts as a fault, as does a
  trap; repeated faults disable the extension. The durable calls return once the store is synced, and
  the deadline is paused across them; instead, they are bounded by a durable budget per callback: 64
  such calls or 2 s of them, whichever comes first. Past it, the next `store-set` or `store-remove` of
  that callback answers `limit` and writes nothing. That `limit` means "try again in a later callback",
  which starts with a fresh budget, not "the store is full".
- **Time comes from `now`.** The module reads no clock of its own: `monotonic-ms` measures intervals,
  `wall-ms` dates things. `wake` asks for one `wake` hint, replacing an earlier request; the host may
  deliver it later than asked, never earlier.
- **Process results are kept until `proc-release`**, across guest restarts: at most 1 MiB per stream,
  2 MiB per process and 16 MiB in total; past the total, the host releases the oldest result first.
  A stream the host cut short is reported `truncated`. Output is read with `proc-take` in pages of at
  most 64 KiB; `chunk.remaining` says how much of the stream follows.
- **`store-set` is durable when it returns**: the value is written and synced before the reply. Values
  are at most 64 KiB.
- **One call's arguments are at most 65,664 bytes**: a 64 KiB value plus its key of at most 128 bytes;
  a call over it *traps*, a fault that counts toward disabling the extension. It is not answered with
  `limit`.
- **`not-supported-yet` is a capability not yet served, never a fault.** Every import is declared from
  the first version on, so bindings are generated once; an import the host does not serve yet answers
  `not-supported-yet` (`file-error::not-supported-yet` for `file-read`), and the guest shows that it
  lacks the capability.
- **Ownership is enforced.** A guest reaches only its own processes, panes and tabs; another
  extension's answer `not-owned`.
- **No ambient authority.** The module sees no files, no sockets, no environment. A command or pane
  is one its manifest declares, run directly (never through a shell), with arguments checked against
  their declared kinds, in a directory inside a granted root. `file-read` reads regular files inside a
  root declared readable; no `..`, no symlink leaving the root.
- **`file-read` words a missing directory apart from a missing file.** `directory-gone`: a directory on
  the path, the root's own or one below it, does not exist. A missing file in an existing directory is
  `not-regular`, as is a directory, a pipe or a device. A symlink anywhere on the path, a file with a
  second name (a hard link) and a file on another device than the root's are `escapes`; a file over
  64 KiB is `too-large`, by what was read, not by the size the file claimed.

## Versions

The package follows semver. Adding an import is a minor version; changing or removing one is a major
version. The host serves a fixed set of package versions, and a module importing a major version it
does not serve does not load. A module imports only the functions it calls.

## The binary contract

As for the status-line world (`wit/statusline/README.md`): the Canonical ABI of the WebAssembly
Component Model, on a core module built for `wasm32-wasip1`, not componentized.

- **Imports** are named by module `terminal-extensions:daemon/host@0.1.0` and function name. A module
  may import nothing from another world's modules: the host checks each world against its exact list.
- **Exports** are `load` and `update`, plus `cabi_realloc`, which the host calls to place arguments in
  linear memory.
- The module has **no start function**; one that needs initialization exports `_initialize`, called
  once before `load` under `load`'s deadline.
- Values follow the Canonical ABI. A result that does not flatten to one core value is written to
  memory at an address the guest passes as the import's last parameter.
