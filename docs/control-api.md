# Control API

Every window, tab and pane a Contour daemon hosts can be listed, driven and watched from the shell, from
scripts and from coding agents, with the same `contour` binary that runs the terminal. The verbs below talk to
the daemon's control API; `contour api list` shows everything the API offers, including what has no verb yet.

## Where you are

A shell hosted by the daemon finds its surroundings in four environment variables:

| Variable | Holds |
| --- | --- |
| `$CONTOUR_PANE` | The shell's own pane, `pane:<uuid>`. The default target of every verb. |
| `$CONTOUR_TAB` | Its tab, `tab:<uuid>`. |
| `$CONTOUR_WINDOW` | Its window, `window:<uuid>`. |
| `$CONTOUR_DAEMON` | The daemon's socket path. |

## Targets

Wherever a verb takes a pane, tab or window (`--pane P`), `P` is resolved in this order, and the first kind of
match wins:

1. a full id (`pane:018f3c4e-...`);
2. a label, compared case-insensitively (see `contour label`);
3. a 1-based position, for tabs and windows;
4. an id suffix of at least four hex digits.

More than one match is an error that lists the candidates, one per line. Without `--pane`, a verb acts on
`$CONTOUR_PANE`; a pane that has since closed is an error (exit status 3), never a silent fallback to another
pane.

## Verbs

| Verb | What it does |
| --- | --- |
| `contour ls [--window W]` | Lists windows, tabs and panes as a tree; ids are shortened to their last 8 hex digits and `*` marks the active ones. |
| `contour new [--window W \| --new-window] [--cwd DIR] [--label L] [--no-focus] [-- CMD...]` | Opens a tab and prints its pane id. |
| `contour split [--pane P] [--right \| --below] [--ratio PCT] [--cwd DIR] [--label L] [--no-focus] [-- CMD...]` | Splits a pane and prints the new pane's id. |
| `contour send [--pane P] [--enter] [--wait] TEXT...\|-` | Types text into a pane. `--enter` appends a carriage return, `-` reads standard input, `--wait` waits for the command the text starts and exits with that command's own status. |
| `contour pane capture [--pane P] [--scrollback N \| --all]` | Prints a pane's text. |
| `contour close [--pane P \| --tab T]` | Closes a pane, or a tab with all its panes. |
| `contour label OBJECT LABEL` | Labels a window, tab or pane (`contour label pane:018f logs`); an empty label clears it. |
| `contour block list [--pane P] [--limit N]` | Lists a pane's recent command blocks, newest first: `id  exit  duration  command`. |
| `contour block read [BLOCK] [--pane P] [--prompt \| --output]` | Prints a block's text; without `BLOCK`, the pane's last finished block. With neither flag, the prompt and then the output. |
| `contour wait [BLOCK] [--pane P] [--timeout SECONDS]` | Waits for a block to finish and exits with the command's own status. |
| `contour events [--follow] [--kind K[,K...]] [--pane P]` | Prints the first matching event and exits, or with `--follow` every event until Ctrl+C (exit status 130). |
| `contour api list [-q]` | Lists the API's methods and events; `-q` prints names only. |
| `contour api describe NAME` | Prints one method's or event's schemas and examples. |
| `contour api call NAME [JSON\|@FILE\|-]` | Calls any method with the given request, sent as is and without context. The output is indented on a terminal and one line otherwise. |
| `contour api wit` | Prints the API as the WIT package `terminal-extensions:control@0.1.0` a WebAssembly extension imports, exactly as `wit/control/world.wit` holds it. Reaches no daemon and takes none of the connection options. |

`contour pane capture` is spelled that way because `contour capture` is the older verb that captures the
screen of the terminal it runs in.

Options that look like words (`--pane`, but also the bare `pane`) can be mistaken for typed text: put `--`
before text that starts with the name of an option, as in `contour send --enter -- wait`.

### Connection options

Every verb takes:

| Option | Meaning |
| --- | --- |
| `--json` | Prints the reply JSON as received, plus a newline, instead of the human form. |
| `--daemon PATH\|HOST:PORT\|NAME\|URL` | The daemon. Defaults to `$CONTOUR_DAEMON`, then the default socket. A verb never starts a daemon. |
| `--token-file FILE` | The token a `HOST:PORT` daemon expects. |
| `--tls-ca FILE` | The CA certificate (PEM) a `HOST:PORT` daemon is verified against. |

A `HOST:PORT` daemon without `--tls-ca` is refused before anything is sent, with a message naming the
option: control requests, which can type into panes, never travel over an unverified connection.

A bare word (`--daemon homepc`) names a saved host of the configuration's `hosts:` section, and an
`ssh://` URL an ad-hoc one, exactly as `contour client --host` reads them (see
[Remote hosts](remote-hosts.md)): the host's own `token_file` and `tls_ca` apply, and `--token-file` and
`--tls-ca` do not. A saved `tls://` host without `tls_ca` is refused like a `HOST:PORT` daemon without
`--tls-ca`. A name no saved host has exits 4 with the names that are saved.

### Windows opened by `contour new --new-window`

A window created by a control connection is followed by the primary viewer, the GUI whose launch took every
window, and so appears in the open GUI. With no GUI attached it appears at the next launch.

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
| 3 | The pane, tab, window or block named does not exist, or the name matches several. |
| 4 | No daemon could be reached. |
| 130 | Interrupted by Ctrl+C. |

`contour send --wait` exits with the command's own status instead (0 to 255). A command with no exit code,
or one that was superseded, exits 1 with a note on standard error.

## Permissions

A daemon's TCP tokens carry grants (`contour daemon --access-file`): `read` for listing, `screen` for
capturing, reading blocks and program status (`pane.programStatusChanged`, and the `programStatus` of
`contour ls --json`), `input` for typing, and `write` for changing the workspace. A call outside the
token's grants is refused as forbidden (exit status 1). The daemon's socket grants everything to its owner.
A connection holds at most 64 subscriptions and 256 pending waits; one more is refused as `limitReached`
(exit status 1). Each verb opens a connection of its own, so only a script driving `contour api call` over one
connection meets it.

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
contour agent setup            # Claude Code and ~/.agents
contour agent setup --agent claude --dry-run
```

| Agent | Skill file |
| --- | --- |
| `claude` (Claude Code) | `~/.claude/skills/contour/SKILL.md`, or beneath `$CLAUDE_CONFIG_DIR` when that is set |
| `agents` (Codex, OpenCode and other agents reading `~/.agents`) | `~/.agents/skills/contour/SKILL.md` |

Rerun it after upgrading Contour: a file it generated is updated, one that is already current is left alone,
and a file it did not generate is kept unless you pass `--force`. `--dry-run` reports what would happen, and
`--json` prints the outcomes as an array.
