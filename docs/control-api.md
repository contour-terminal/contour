# Control API

Every workspace, tab and pane a Contour daemon hosts can be listed, driven and watched from the shell, from
scripts and from coding agents, with the same `contour` binary that runs the terminal. The verbs below talk to
the daemon's control API; `contour api list` shows everything the API offers, including what has no verb yet.

## Where you are

A shell hosted by the daemon finds its surroundings in four environment variables:

| Variable | Holds |
| --- | --- |
| `$CONTOUR_PANE` | The shell's own pane, `pane:<uuid>`. The default target of every verb. |
| `$CONTOUR_TAB` | Its tab, `tab:<uuid>`. |
| `$CONTOUR_WORKSPACE` | Its workspace, `workspace:<uuid>`. |
| `$CONTOUR_DAEMON` | The daemon's socket path. |

## Targets

Wherever a verb takes a pane, tab or workspace (`--pane P`), `P` is resolved in this order, and the first kind of
match wins:

1. a full id (`pane:018f3c4e-...`);
2. a label, compared case-insensitively (see `contour label`);
3. a 1-based position, for tabs and workspaces;
4. an id suffix of at least four hex digits.

More than one match is an error that lists the candidates, one per line. Without `--pane`, a verb acts on
`$CONTOUR_PANE`; a pane that has since closed is an error (exit status 3), never a silent fallback to another
pane.

## Verbs

| Verb | What it does |
| --- | --- |
| `contour ls [--workspace W]` | Lists workspaces, tabs and panes as a tree; ids are shortened to their last 8 hex digits and `*` marks the active ones. |
| `contour new [--workspace W \| --new-workspace] [--cwd DIR] [--label L] [--no-focus] [-- CMD...]` | Opens a tab and prints its pane id. Without `--cwd` the shell starts in the calling pane's directory, when run from one. |
| `contour split [--pane P] [--right \| --below] [--ratio PCT] [--cwd DIR] [--label L] [--no-focus] [-- CMD...]` | Splits a pane and prints the new pane's id. Without `--cwd` the shell starts in the split pane's directory. |
| `contour send [--pane P] [--enter] [--wait] TEXT...\|-` | Types text into a pane. `--enter` appends a carriage return, `-` reads standard input, `--wait` waits for the command the text starts and exits with that command's own status. |
| `contour pane capture [--pane P] [--scrollback N \| --all]` | Prints a pane's text. |
| `contour close [--pane P \| --tab T]` | Closes a pane, or a tab with all its panes. |
| `contour label OBJECT LABEL` | Labels a workspace, tab or pane (`contour label pane:018f logs`); an empty label clears it. |
| `contour block list [--pane P] [--limit N]` | Lists a pane's recent command blocks, newest first: `id  exit  duration  command`. |
| `contour block read [BLOCK] [--pane P] [--prompt \| --output]` | Prints a block's text; without `BLOCK`, the pane's last finished block. With neither flag, the prompt and then the output. |
| `contour switch [--workspace W] (--to-label L \| --to-host H \| --to-workspace W2) [--to-workspace W2]` | Switches the window showing a workspace to another workspace or daemon, in place. See [Switching a window](#switching-a-window). |
| `contour wait [BLOCK] [--pane P] [--timeout SECONDS]` | Waits for a block to finish and exits with the command's own status. |
| `contour events [--follow] [--kind K[,K...]] [--pane P]` | Prints the first matching event and exits, or with `--follow` every event until Ctrl+C (exit status 130). |
| `contour api list [-q]` | Lists the API's methods and events; `-q` prints names only. |
| `contour api describe NAME` | Prints one method's or event's schemas and examples. |
| `contour api call NAME [JSON\|@FILE\|-]` | Calls any method with the given request, sent as is and without context. The output is indented on a terminal and one line otherwise. |
| `contour api wit` | Prints the API as the WIT package `terminal-extensions:control@0.1.0` a WebAssembly extension imports, exactly as `wit/control/world.wit` holds it. Reaches no daemon and takes none of the connection options. |

`contour pane capture` is spelled that way because `contour capture` is the older verb that captures the
screen of the terminal it runs in.

Options that look like words (`--pane`, but also the bare `pane`) can be mistaken for typed text, and a word
that starts with a dash but names no option is refused rather than typed: put `--` before text that starts
with the name of an option or with a dash, as in `contour send --enter -- wait` or
`contour send -- -x`.

### Connection options

Every verb takes:

| Option | Meaning |
| --- | --- |
| `--json` | Prints the reply JSON as received, plus a newline, instead of the human form. |
| `--daemon PATH\|HOST:PORT\|NAME\|URL` | The daemon. Defaults to `$CONTOUR_DAEMON`, then the default socket. A verb never starts a daemon. |
| `--label L` | A local daemon, by the label it serves (`contour daemon --label L`), resolved as `contour client --label` does. Instead of `--daemon`: giving both exits 2. |
| `--token-file FILE` | The token a `HOST:PORT` daemon expects. |
| `--tls-ca FILE` | The CA certificate (PEM) a `HOST:PORT` daemon is verified against. |

A `HOST:PORT` daemon without `--tls-ca` is refused before anything is sent, with a message naming the
option: control requests, which can type into panes, never travel over an unverified connection.

A bare word (`--daemon homepc`) names a saved host of the configuration's `hosts:` section, and an
`ssh://` URL an ad-hoc one, exactly as `contour client --host` reads them (see
[Remote hosts](remote-hosts.md)): the host's own `token_file` and `tls_ca` apply, and `--token-file` and
`--tls-ca` do not. A saved `tls://` host without `tls_ca` is refused like a `HOST:PORT` daemon without
`--tls-ca`. A name no saved host has exits 4 with the names that are saved.

### Workspaces opened by `contour new --new-workspace`

A workspace created by a control connection is followed by the primary viewer, the GUI whose launch took every
workspace, and so appears in the open GUI. With no GUI attached it appears at the next launch.

## Switching a window

`workspace.switch` asks the window that shows a workspace to switch to another workspace, or to another
daemon, in place. It is a socket method (an extension cannot call it) and needs `write`; it is not an MCP tool,
because an agent in a pane must not re-point the window it runs in.

```json
{"workspace":"","toLabel":"admin","toHost":"","toWorkspace":"build"}
```

| Field | Meaning |
| --- | --- |
| `workspace` | The workspace whose window switches (id, label, position or suffix). Empty: the caller's pane's workspace. |
| `toLabel` | A local daemon, by label. |
| `toHost` | A saved host, or an `ssh://` URL. Not together with `toLabel`; with neither, the window stays on this daemon. |
| `toWorkspace` | The destination workspace, as a spec the window resolves against the destination daemon (id, label, position or suffix). Empty lets the window decide. |

From the command line:

```sh
contour switch --to-label admin                       # this pane's window goes to the daemon labelled admin
contour switch --to-host homepc --to-workspace build  # a saved host, and its workspace build
contour switch --to-workspace 2                       # another workspace of the same daemon
contour switch --label work --workspace w1 --to-label admin   # from outside any pane
```

`--daemon`/`--label` choose the daemon the window is on now, and `--workspace` which of its workspaces
(default: the workspace of `$CONTOUR_PANE`, so inside a pane no flags are needed). `--to-host` takes a saved
host's name or an `ssh://` URL; any other URL is refused with exit status 2, and must be saved in the
configuration's `hosts:` section first. Without `--to-workspace`, a destination with exactly one workspace (or none, which
creates one) is used; with several, the window asks which in its switcher popup (outcome `picker`). `contour switch` waits for the window's answer and prints the outcome; `switched`, `focused` and `picker`
exit 0, `refused`
exits 5, `failed` exits 1, and a workspace shown by no window (or by several, none known to have typed last)
exits 3 with the reason. To open a window on another daemon instead of re-pointing one, use
`contour client --label L --workspace W` or `contour client --host H --workspace W` (see
[Remote hosts](remote-hosts.md)).

The window asks its user before it switches: a prompt names the destination in full and offers Deny or
Switch, with **Don't ask again**, which keeps the answer for switches between the same two daemons until
Contour exits; a remembered Deny is answered `refused` at once. The daemon tells the window how long it
waits for the answer (30 seconds), and the prompt stays up only while an allowed switch could still land
before then: the wait less the 10 seconds a switch may take to show its destination, less 2 seconds for the
answer to travel back -- 18 seconds. A prompt left unanswered that long is taken down and answered `refused`
("the user did not answer in time"). An allowed switch must then land within the wait less those 2 seconds,
counted from the request's arrival, whatever it waits for first (the destination daemon's workspaces are
listed before anything moves): one that could no longer start in time fails, and one still landing then gives
up, so a window never switches after `contour switch` gave up. A daemon on another host may state a longer
wait; the window takes at most 5 minutes of it. A destination given as an `ssh://` URL is asked every time (see [Persistent sessions](persistent-sessions.md#switching-a-window)).

The reply is `{"outcome":"switched","detail":""}`. `outcome` is `switched`, `focused` (another window already
shows the destination, and was focused), `picker` (the window asked its user which workspace), `refused` (the
user declined: "the user declined the switch") or `failed`; `detail` says why in a sentence. A window that
does not show the workspace (any more) answers `failed` ("no window here shows workspace N"). A window that does not answer within 30 seconds
is reported as `failed` ("no answer from the window"), and one whose client predates switching as `failed`
("the window cannot switch (client too old)").

Which window switches is decided by the daemon: the client that typed last into the workspace's panes. With
no input recorded, a workspace shown by one window switches that one; by several, the call fails as
`ambiguous` and lists them (`native#3`); by none, as `notFound` ("no client shows workspace ...").

`workspace.list` tells what a switch can act on. Each workspace carries `visibility` (`shown`: a window shows
it; `left`: none does, and a launch brings it back -- the one left last, when several are; `hidden`: a window switched away from it, and a launch
leaves it alone), `clients` (how many windows show it) and `native` (the daemon's own id for it, in decimal,
which a window follows it by).

## Waiting

`contour wait` and `contour send --wait` exit with the finished command's own status, clamped to 0..255. A
block without an exit code, with a negative one, or that was superseded by a newer prompt exits 1, with a
note on standard error. `contour wait` with no `BLOCK` waits for the next block of the pane to finish; one
that already finished is read with `contour block read`. `--timeout SECONDS` is kept by the client: when it
runs out, `contour wait` prints `Error: timed out` and exits 1, and the command in the pane carries on.

```sh
contour send --enter 'make' && contour wait --timeout 600 || echo "make failed or took too long"
```

## Events

`contour events` subscribes to the daemon's events. A line reads `HH:MM:SS type pane details` (times are UTC;
the pane is the last 8 hex digits of its id behind an ellipsis). With `--json` each event is printed as
received, one per line, which is what scripts read:

```sh
contour events --follow --json --kind block.finished | jq -r '.payload.block | "\(.exitCode) \(.command)"'
```

An event of a type this release does not know, from a newer daemon, is printed raw. `--kind` takes a comma
separated list of:

| Event | Fires when |
| --- | --- |
| `block.finished` | A command block finished. |
| `pane.bell` | A pane rang the bell. |
| `pane.titleChanged` | A pane's title changed. |
| `pane.cwdChanged` | A pane's working directory changed. |
| `pane.programStatusChanged` | A pane's program status changed. |
| `pane.created`, `pane.closed` | A pane was created or closed. |
| `tab.created`, `tab.closed` | A tab was created or closed. |
| `client.attached`, `client.detached` | A GUI client attached or detached. |
| `events.dropped` | The subscriber was too slow and events were dropped. |

## Exit statuses

| Status | Meaning |
| --- | --- |
| 0 | Done. |
| 1 | The call failed; the reason is on standard error as `Error: <message>`. |
| 2 | The command line was malformed, or a setting it names cannot be used (a `--tls-ca` that does not parse). |
| 3 | The pane, tab, workspace or block named does not exist, or the name matches several. |
| 4 | No daemon could be reached. |
| 5 | The daemon declined; the reason is on standard error. |
| 130 | Interrupted by Ctrl+C. |

`contour send --wait` exits with the command's own status instead (0 to 255). A command with no exit code,
or one that was superseded, exits 1 with a note on standard error.

## Permissions

A daemon's TCP tokens carry grants (`contour daemon --access-file`): `read` for listing, `screen` for
capturing, reading blocks and program status (`pane.programStatusChanged`, and the `programStatus` of
`contour ls --json`), `input` for typing, and `write` for changing the workspace. A call outside the
token's grants is refused as forbidden (exit status 1). The daemon's socket grants everything to its owner.
`daemon.info` reports the connection's own grants in its `grants` array (by name, in the order above), so a
client can tell what it may call before it tries.
A connection holds at most 64 subscriptions and 256 pending waits; one more is refused as `limitReached`
(exit status 1). Each verb opens a connection of its own, so only a script driving `contour api call` over one
connection meets it.

## Reporting a pane's status

An integration, such as a coding agent's hook, tells the daemon what a pane's program is doing with two
methods. Both need the `input` grant, and both are reachable with `contour api call`:

| Method | What it does |
| --- | --- |
| `pane.reportStatus` | Sets the integration's own record of a pane: `{"pane": "...", "integration": "claude-code", "state": "blocked", "kind": "permission", "title": "...", "message": "...", "progress": 40}`. `integration` is `[a-z0-9-]{1,32}` and becomes the record's id; `state` is `idle`, `working`, `done`, `blocked` or `error`; `kind` (`none`, `permission`, `question`, `auth`) is for `blocked` only. An empty `pane` is the caller's own. |
| `pane.clearStatus` | Removes the record that integration set: `{"pane": "...", "integration": "claude-code"}`. Records a program sent by OSC 7501 are not touched. |

The title and message are untrusted text: control and invisible formatting characters are removed, and each is cut
to 200 bytes. Records the integration sets list as `origin: "integration"`, apart from a program's
`origin: "program"`, with `since` (when the state or kind last changed) and a `generation` that rises whenever the state or kind
changes (not when only the title, message or progress does).
When someone types or pastes into the pane in a Contour window, the integration's `blocked` records become
`working`: the person has answered. Input sent with `pane.send` does not do that, nor do the focus and mouse reports
a window sends by itself, nor does input from a client too old to say which of its input was typed.

These methods need a daemon. In-process mode (no `contour daemon`) has no control socket, so a hook finds none
and does nothing.

## Upgrading the daemon

`daemon.upgrade` replaces the running daemon with a newer release, keeping every program running
(see [Live upgrade](persistent-sessions.md#live-upgrade)); `contour daemon upgrade` is its verb.

```sh
contour api call daemon.upgrade '{"image":""}'
```

`image` names a staged image of the daemon's store; empty, the daemon upgrades to its own installed
executable, the file a package upgrade replaced. The call needs `write` and a connection on the daemon's own
machine, through the daemon's Unix socket: over a TCP token it is refused. It is not offered to
extensions or to the agent's tools.

The reply, `{"started":true,"toRelease":"0.9.1"}`, is sent before the daemon tells its clients it is
upgrading and closes every connection, this one included; the clients reconnect to the same daemon. It is
provisional: the daemon may still decline after it, while it waits for its clients and sessions to settle,
and the switch to the new binary may fail. Either way the daemon then resumes and serves on as before, and
the connection that asked is gone by then. `contour daemon status` tells how it ended: its `upgrades`
count is one higher once the new release serves, and a `last upgrade` line names the failure otherwise.
`contour daemon upgrade` reads it there for you.

A refusal before the reply is an error with code `failed` whose message leads with a stable name, then
says why: `sandboxed: the daemon runs in a sandbox that cannot reach the new release`. The names are
`inProgress`, `notStaged`, `sandboxed`, `formatUnreadable`, `inFlightTailTooLarge`, `targetUnresponsive`,
`notSupported`, `unsupportedSession`, `sessionStarting`, `quiesceTimedOut`, `imageTooLarge`,
`configUnreadable` and `localOnly`. `execFailed`, an `exec` that failed after the daemon stopped its
sessions' pumps and resumed them, always comes after the reply, so this call never returns it; it is the
name `contour daemon status` reports such a failure under. A connection that breaks before the reply
leaves the outcome unknown: ask `contour daemon status` again.

## A workflow: run the tests beside you

```sh
pane=$(contour split --below --no-focus --json | jq -r .pane)
contour send --pane "$pane" --enter --wait -- 'make test'   # exits with make's status
contour block read --pane "$pane" --output                   # what it printed
```

## Agents

`contour agent setup` writes a skill that teaches coding agents to operate Contour panes: where they are,
how targets resolve, every verb with its usage, the exit statuses, the run-the-tests workflow, and the whole
API. The skill is generated from the verb table and the API catalog, so it always matches the binary.

```sh
contour agent setup            # every agent found, and ~/.agents
contour agent setup --agent claude-code --dry-run
```

| Agent | Skill file |
| --- | --- |
| `claude-code` (Claude Code) | `~/.claude/skills/contour/SKILL.md`, or beneath `$CLAUDE_CONFIG_DIR` when that is set |
| `agents` (Codex, OpenCode and other agents reading `~/.agents`) | `~/.agents/skills/contour/SKILL.md` |

Without `--agent`, an agent is set up only when its configuration directory exists (`~/.claude` or
`$CLAUDE_CONFIG_DIR`; `~/.codex` or `$CODEX_HOME`), so nothing is created for an agent you do not use; the
report says which were skipped. Naming an agent with `--agent` sets it up whether or not it is found. The names
are the ones `contour agent report --agent` takes.

Rerun it after upgrading Contour: a file it generated is updated, one that is already current is left alone,
and a file it did not generate is kept unless you pass `--force`. `--dry-run` reports what would happen, and
`--json` prints the outcomes as an array.

### Status hooks

`contour agent setup` also installs the hooks that let a coding agent tell Contour what it is doing, so that
the pane shows a working, blocked (waiting for you) or done status. The hooks run
`contour agent report --agent <claude-code|codex>`, which turns the agent's hook payload into
`pane.reportStatus` or `pane.clearStatus` on the daemon hosting the pane. It prints nothing, always exits 0,
gives up after two seconds (reading the payload included), ignores a payload larger than 1 MiB, and does nothing
outside a pane of a Contour daemon (no `$CONTOUR_DAEMON` or `$CONTOUR_PANE`; a window running its sessions in
process does not pass those on to its own shells). The `contour` binary is written into the hooks by its absolute path (under an AppImage, the
AppImage's), so rerun `contour agent setup` after moving it.

| Agent | Hook file | What is installed |
| --- | --- | --- |
| `claude-code` (Claude Code) | `~/.claude/settings.json`, or beneath `$CLAUDE_CONFIG_DIR` | one hook group for each of `UserPromptSubmit`, `PreToolUse`, `PermissionRequest`, `Notification`, `Elicitation`, `PostToolUse`, `PostToolUseFailure`, `Stop`, `StopFailure` and `SessionEnd` |
| `codex` (Codex) | `~/.codex/hooks.json`, or beneath `$CODEX_HOME` (which must already exist) | one hook group for each of `UserPromptSubmit`, `PreToolUse`, `PermissionRequest`, `PostToolUse`, `Stop`, `Interrupt` and `SessionEnd` |

Your own hooks and settings are kept, and a file is replaced in one step (a symlinked one through its link, which
stays). A hook of ours is one whose command is exactly the binary followed by
`agent report --agent <claude-code|codex>`; a command of yours that merely mentions it is left alone. A hook file that is not valid JSON is left alone and reported, and so is a `$CODEX_HOME` that
names a directory that does not exist (it is not created).

Codex runs only hooks you have reviewed: after `contour agent setup`, open Codex once and trust the new hooks
(its hooks review), or they never run and the pane shows no status.

`contour agent setup --uninstall` removes exactly these hooks (and leaves the skill file as it is), and the groups
and event arrays that held only them; `--dry-run` reports what would change.

Limits:

- Both agents show working (prompt, tool use), needs-you (a permission request; for Claude also a question)
  and done. Claude also shows an error (`StopFailure`); Codex shows idle after an interrupt. Claude's
  "idle, waiting for your input" notification is ignored.
- The event names and payload fields were checked against the agents' hook documentation on 2026-11-11; a
  renamed event shows up as a pane that never changes status.
- The hooks need the daemon. In in-process mode there is no control socket, and the hooks do nothing.

## MCP

`contour mcp` serves the control API to coding agents that speak the Model Context Protocol (MCP): it reads
JSON-RPC requests, one per line, on standard input and writes the replies, one per line, on standard output.
Nothing else is ever written to standard output; diagnostics go to standard error. Register it once:

```sh
claude mcp add contour -- contour mcp
```

An agent started in a Contour pane inherits `$CONTOUR_PANE`, which is its own pane. The server offers these
tools (an agent sees only those the daemon's grants permit, see `daemon.info`):

| Tool | Does | Needs |
| --- | --- | --- |
| `workspace_list` | Lists workspaces, tabs and panes, marking your own | `read` |
| `daemon_info` | Reports the daemon and the connection's grants | `read` |
| `pane_capture` | Reads a pane's screen | `screen` |
| `block_list`, `block_read` | Lists and reads command blocks | `screen` |
| `pane_run` | Runs a command in a pane and waits for its result | `input`, `screen` |
| `pane_type` | Types into a pane | `input` |
| `pane_split`, `tab_create` | Opens a split or a tab | `write` |
| `pane_close`, `tab_close`, `object_setLabel` | Closes or labels | `write` |

`pane_run` needs OSC 133 shell integration in the pane. It types one command line (join several with `&&`
or `;`), waits, and returns the exit code and the last 64 KiB of output (`maxOutputBytes` changes that). A
result of `timedOut` means the command is still running; fetch its output later with `block_read`. It
refuses the agent's own pane, as does `pane_type`: use `pane_split` to get a shell pane.

By default the agent acts only on the panes of its own tab: an action aimed at another tab is refused, and a
label or id suffix is matched within the own tab only. `workspace_list` still lists every tab, so the agent
can tell you what exists; scoping limits what it may do, not what it can see. Without `$CONTOUR_PANE` the
server refuses to start (exit status 2) unless you pass `--allow-all-tabs`, which lets the agent act on every
tab and enables `tab_create` and `tab_close`. `--read-only` leaves out the tools that type or change the
workspace. `--daemon`, `--token-file` and `--tls-ca` work as
for every verb.

The scoping is a guardrail against an agent wandering into the wrong tab, not a sandbox: an agent that can
run shell commands can run `contour` itself, with the same connection. The real boundary is what the
daemon's token grants (`contour daemon --access-file`); give an agent a token that can do no more than you
want it to.
