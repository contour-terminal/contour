# OSC 3008 — Hierarchical Context Signalling

Contour consumes **OSC 3008**, the [UAPI.15 hierarchical context signalling
specification](https://uapi-group.org/specifications/specs/osc_context/).

A terminal connects a user with programs, and control of the program side is passed around while the
user works: a shell invokes a command, `run0` acquires privileges, `systemd-nspawn` enters a
container, `ssh` connects to another machine. OSC 3008 lets each of those components tell the
terminal that a new **context** begins, and carry metadata describing it.

The value is not that this is more metadata. It is that it arrives **without any configuration**:
systemd 258 and later ship a shell snippet that emits these sequences around every interactive
command, so on such a system Contour learns command boundaries, exit statuses and working
directories with no shell integration installed at all.

## Sequences

```
OSC 3008 ; start= CTXID [ ; field=value ]... ST
OSC 3008 ; end=   CTXID [ ; field=value ]... ST
```

`start=` initiates a context, **updates** one that already exists, or **returns to** one further up.
`end=` terminates one. Contexts nest: the newest is active, and everything written while it is
active belongs to it.

Because `;` separates fields, a literal semicolon in a value is written as the four characters
`\x3b`, and a literal backslash as `\x5c`. Values are UTF-8 and are stored as sent.

### Fields on `start=`

| Field | Meaning |
|---|---|
| `type=` | `service`, `session`, `shell`, `command`, `vm`, `container`, `elevate`, `chpriv`, `subcontext`, `remote`, `boot`, `app` |
| `user=`, `hostname=`, `machineid=`, `bootid=`, `pid=`, `pidfdid=`, `comm=` | who and where the announcing process is |
| `cwd=` | the working directory (`shell`, `command`) |
| `cmdline=` | the command line (`command`) |
| `vm=`, `container=` | the VM or container being entered |
| `targetuser=`, `targethost=`, `sessionid=` | what is being connected or elevated to |

### Fields on `end=`

`exit=` (`success`, `failure`, `crash`, `interrupt`), `status=` (numeric), and `signal=` (symbolic,
e.g. `SIGSEGV`). Note that `crash`, `interrupt` and `signal=` express things
[OSC 133](osc-133-shell-integration.md)'s numeric exit code structurally cannot.

## What Contour does with it

- **Tracks the ancestry**, so every line of output is associated with the context that produced it.
  The association survives reflow and scrolling into history.
- **Derives semantic marks** when no shell integration is present, so prompt navigation, output
  folding and "copy last command output" work with no setup. See *Interaction with OSC 133* below.
- **Tints the page background** under a boundary context, if the colour scheme opts in.
- **Shows a breadcrumb** in the indicator status line via the `{Context}` placeholder.
- **Resolves the working directory** for the tab tooltip and "Open Current Folder".
- **Replicates all of it to daemon clients**, so a `contour attach` pane behaves like a local one.

## Interaction with OSC 133

Both protocols describe the same prompt cycle, so exactly one of them owns a session's semantic
marks, and the rule is one-way:

- If **OSC 133 speaks at any point**, it owns the marks for the rest of the session. OSC 3008 then
  contributes metadata only — working directories, identity, and the richer exit information — and
  never a mark.
- If OSC 133 has said nothing, OSC 3008 stands in for it: a `type=shell` update marks the prompt, a
  `type=command` start marks where output begins, and its `end=` marks where output stopped.

OSC 3008 can never take the marks back, which is what stops a session flapping between sources.
There is one thing it cannot supply: OSC 133's `;B`, the border between the prompt and what the user
types. OSC 3008 has no event there, so a session driven by it alone reports the prompt's line span
but not the column input begins at.

## Reset behaviour

**RIS and DECSTR deliberately do not clear the context stack.** The specification makes this a
safety property: a program running down the ancestry must not be able to erase context a program
above it established, and RIS is something any program can send. A shell that announced "you are
inside container X" must still be able to say so after a program inside that container resets the
terminal. The stack is destroyed when the session is.

## Limits

Configurable through the [`osc_context`](../configuration/README.md) section: `max_depth` (default
16) bounds the ancestry, and `max_retained_contexts` (default 256) bounds how much history is kept
for scrolled-back output. Per the specification, hitting the depth limit drops the **newer**
contexts, so a program deep in the ancestry cannot push out the one above it. Field lengths are
protocol constants and are not configurable.

## Security

**Every field is a display hint. Nothing gates behaviour on one.**

Any program that can write to the terminal can emit `type=elevate` or `user=root`, so nothing
Contour draws from these sequences is an assurance. Two consequences worth stating outright:

- **The absence of an elevate context is not a statement that nothing is running as root.** A
  program in a genuinely elevated shell can simply `end=` the context, and no terminal can detect
  that. This is why Contour ships no default tint and never decorates a context with a lock, a
  shield or a warning colour: doing so would teach users to read a spoofable signal as authority.
- **A `cwd=` is not trusted for opening or spawning unless it is provably this machine's.** A
  context carries no host authority, so a container's `/app` would otherwise pass every locality
  test and open the host's directory of the same name. Contour requires a matching `machineid=` (or
  failing that a matching `hostname=`) and refuses when a container, VM or remote boundary is in
  force. A context that claims nothing at all is treated as unknown, not local — which matters
  because nothing emits a `remote` context for `ssh` today, so a remote host's own sequences look
  entirely local.

To opt out of all of it: `osc_context: { enabled: false }`.

## Not implemented

- **Killing a context tree via `pidfdid=`.** That field is a pidfd *inode*, not a handle, so it can
  only *verify* a `pidfd_open(pid)` — and the protocol carries no process-group id and no cgroup
  path, so "kill this context and its children" is not implementable from the fields that exist.
  Inside a container `pid=` names another PID namespace, so opening it locally names an unrelated
  process, and a `run0` context's pid is root-owned, so the case a user most wants would fail with
  `EPERM`.
- **Re-entering a container or remote host to open its working directory.** Executing `nsenter` or
  `ssh` on the basis of a spoofable field is exactly the rule above being broken.
