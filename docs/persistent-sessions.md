# Persistent sessions (daemon mode)

!!! warning "Experimental"
    Daemon mode is new in 0.7.0 and still settling. It is built into every Contour and covered by
    the test suite, but its command-line flags and its wire protocol may change between releases.

Contour already gives you tabs and split panes inside one window. Daemon mode is different: it
moves your sessions into a **background process**, so they survive the window that shows them.
Close the window, reattach later, or attach from another machine — the shells keep running
either way.

That buys you three things:

- A long build, a database migration or a training run keeps going after you close the window —
  deliberately or by accident.
- Work done over SSH survives the link dropping. Reconnect, reattach, carry on.
- The same sessions are reachable from a second machine, over an encrypted connection.

If you have used `tmux` or `screen`, this is the same idea, with two differences: the sessions are
rendered by a real Contour window (images, hyperlinks, sized text and all), and Contour can speak
tmux's own protocol in both directions — so it interoperates with tmux rather than replacing it.

## Quick start

```sh
contour client
```

That is the whole thing. `contour client` looks for a daemon, **starts one if none is running**,
and opens a window showing its sessions. Close the window and the shells stay alive; run
`contour client` again to pick them back up — with their scrollback, so you can scroll back through
what the session did while nothing was watching.

To manage the daemon yourself, start it first:

```sh
contour daemon &      # runs in the FOREGROUND; background it or use a service manager
contour client        # attaches to the daemon you started
```

The distinction matters, because the two daemons have different lifetimes:

| How it started | When it exits |
| --- | --- |
| Auto-started by `contour client` | Once its **last session** is gone — it belongs to the client that needed it, so nothing is left behind |
| Started by you (`contour daemon`) | Never on its own. It keeps serving with zero sessions, ready for the next client |

`contour daemon --exit-with-last-session` opts a hand-started daemon into the first behaviour
(tmux spells this `exit-empty`). The policy rides on the *daemon*, so a second client attaching
inherits it, and the daemon ends only when every session across every client is gone.

!!! note "`contour daemon` does not detach by itself"
    It runs in the foreground and logs to standard error, which is what you want under a service
    manager or in a container. Background it with `&`, or let `contour client` start it for you.

## Detaching versus ending a session

Because sessions outlive the window, "close" needs two answers — and Contour gives you both.
Which one you get depends on *what* you close:

| What you close | What happens to the shell |
| --- | --- |
| The window (its close button, or quitting Contour) | **Detach.** It keeps running; the next `contour client` shows it again |
| A **pane** — *Close Pane* in the right-click menu, or the `ClosePane` action | **Ends.** That session is closed on the daemon and does not come back |
| A **tab** — its ✕, or the `CloseTab` action | **Ends**, together with every pane in it |
| Typing `exit` in the shell | **Ends**, as always — the shell decided |

This is the same split tmux draws between detaching a client and `kill-pane`, and it works the same
for a local session as for one hosted by a daemon: a pane you end is ended everywhere, including for
any other client attached to that daemon.

Neither `ClosePane` nor `CloseTab` ships with a default key binding — bind them yourself if you want
one:

```yaml
input_mapping:
    - { mods: [Ctrl, Shift], key: 'W', action: ClosePane }
```

!!! tip "Ending a whole daemon's worth of sessions"
    Closing panes one at a time is the fine-grained way. To end everything at once, stop the daemon
    — see [Stopping a daemon](#stopping-a-daemon).

## Running several daemons side by side

Daemons are told apart by a **label**, exactly as tmux's `-L` does:

```sh
contour daemon --label work
contour client --label work
```

The label names a socket under your per-user runtime directory. Resolution order:

1. `--socket PATH` — used verbatim.
2. `$CONTOUR_MUX` — used verbatim (analogous to tmux's `$TMUX`).
3. `$XDG_RUNTIME_DIR/contour/<label>`.
4. `<temp>/contour-<uid>/<label>` when there is no runtime directory (tmux's
   `/tmp/tmux-<uid>/<label>` shape).

The daemon binds three endpoints, all derived from that one path: the control socket itself,
`<socket>-native` for Contour's own clients, and `<socket>-tmux` for the tmux binary.

The socket directory is created `0700` and checked on every bind: it must be a real directory,
owned by you, and must not be world-accessible. Group access is permitted, matching tmux.

!!! warning "Keep socket paths short"
    A Unix socket path is limited to roughly 100 bytes by the operating system, not by Contour. A
    deeply nested `--socket` path will fail to bind.

## `contour daemon` options

| Option | What it does |
| --- | --- |
| `--socket PATH` | Path of the control socket file. Defaults to `$XDG_RUNTIME_DIR/contour/LABEL`, respecting `$CONTOUR_MUX`. |
| `--label NAME` | Socket label distinguishing daemon instances. Default `default`. |
| `--config FILE` | Configuration file whose profile supplies the hosted sessions' terminal settings — including `history.limit`, which is how much scrollback a session keeps for clients that attach later. |
| `--profile NAME` | Which profile of that configuration to take them from. Defaults to the configuration's default profile. |
| `--exit-with-last-session` | Terminates the daemon once its last hosted session is gone (tmux's `exit-empty`). Off by default. |
| `--tmux-compat-socket LABEL` | Additionally binds tmux's own discovery path `/tmp/tmux-<uid>/<LABEL>`, so a plain `tmux -L LABEL -C attach-session` finds this daemon. |
| `--listen-tcp HOST:PORT` | Also serve the native protocol over TCP. Opt-in; always TLS-encrypted and token-authenticated. See [Remote access](#remote-access-over-tcp). |
| `--token TOKEN` | Preshared token every TCP client must present. Required with `--listen-tcp`. Visible to other local users through the process list — prefer `--token-file`. |
| `--token-file FILE` | Reads the token from `FILE` instead, so the secret is protected by that file's permissions. Trailing newlines are ignored. |
| `--tls-cert FILE`, `--tls-key FILE` | PEM certificate and matching private key for the TCP listener. Omit both to have the daemon generate an ephemeral self-signed certificate. |
| `--log TAGS` | Enable logging for a comma-separated list of tags, or `all`. See [Diagnostics](#diagnostics). |
| `--log-file FILE` | Append log output to `FILE` instead of standard error; `-` means standard error explicitly. |

## `contour client` options

| Option | What it does |
| --- | --- |
| `--socket PATH`, `--label NAME` | Which daemon to attach to; same resolution as above. |
| `--tmux` | Attach to a **real tmux server** instead of a Contour daemon. See [tmux interoperability](#tmux-interoperability). |
| `--tmux-socket PATH` | The tmux server socket (tmux's `-S`) for `--tmux`. |
| `--profile NAME` | Config profile the window renders remote sessions with. |
| `--config FILE` | Configuration file to load. |
| `--connect-tcp HOST:PORT` | Connect to a remote daemon over TLS instead of a local socket. |
| `--token TOKEN`, `--token-file FILE` | The preshared token to present, inline or read from a file. |
| `--tls-ca FILE` | PEM trust anchor pinning the daemon's certificate. Omitted means encrypt-but-do-not-verify. |
| `--log TAGS`, `--log-file FILE` | As above — and **passed on to a daemon this client auto-starts**, which is otherwise the one instance nobody can configure. |

## tmux interoperability

Contour speaks tmux's control-mode protocol in both directions, pinned to tmux 3.7b semantics.

### Attach tmux tooling to a Contour daemon

`<socket>-tmux` speaks the binary protocol the stock tmux client uses, so the real tmux binary can
attach to a Contour daemon:

```sh
tmux -S "$XDG_RUNTIME_DIR/contour/default-tmux" -C attach-session
```

To let tmux find the daemon by label alone, have the daemon bind tmux's own discovery path too:

```sh
contour daemon --tmux-compat-socket mine
tmux -L mine -C attach-session
```

!!! warning "`--tmux-compat-socket` is opt-in for a reason"
    That path is where tmux looks for *its* server. With the Contour daemon down, a `new-session`
    against it would silently start a real tmux server there instead.

Control mode (`-C`) is supported; `-CC` is not — Contour never renders a full terminal client for
tmux. Arbitrary startup commands are refused; the endpoint accepts attach- and new-session-shaped
startup only.

### Show a real tmux server in a Contour window

The other direction: point Contour at a tmux server and mirror it.

```sh
contour client --tmux                       # the default tmux server
contour client --tmux --tmux-socket /tmp/x  # a specific one
```

tmux windows become Contour tabs, tmux panes become splits, and input travels back as
`send-keys`. Content mirrors live.

!!! note "tmux replay carries text, not images"
    Contour replays each pane's full history through `capture-pane`, so scrollback survives a
    reattach on this path too. Images do not, and that is a tmux limitation rather than Contour's:
    `capture-pane` serialises text and colour, never image data. Images drawn *while you are
    attached* display normally; images drawn *before* you attached cannot be recovered. Contour's
    own native protocol has no such gap — it transfers images by id.

## Remote access over TCP

By default a daemon is reachable only through its Unix socket, where the filesystem permissions
are the access control. `--listen-tcp` additionally exposes the native protocol over the network:

```sh
# On the host running the sessions:
umask 077 && openssl rand -base64 32 > ~/.contour-token
contour daemon --listen-tcp 127.0.0.1:9090 --token-file ~/.contour-token

# On the machine you are sitting at:
contour client --connect-tcp server.example.com:9090 --token-file ~/.contour-token
```

Properties worth knowing before you open a port:

- **TLS is not optional.** The TCP endpoint is always encrypted; there is no plaintext mode.
- **A token is required.** TCP has no filesystem gate to fall back on, so `--listen-tcp` without
  `--token`/`--token-file` refuses to start rather than serving anonymously.
- **Prefer `--token-file`.** A token passed as `--token` is readable by every other user on the
  machine through the process list; a file carries permissions.
- **Without `--tls-cert`/`--tls-key` the daemon generates an ephemeral self-signed certificate**,
  and without `--tls-ca` the client encrypts but does **not** verify who it is talking to. That is
  a trust-on-first-use posture: it stops passive eavesdropping, not an active
  machine-in-the-middle. For anything beyond a trusted network, pass a real certificate on the
  daemon and pin it with `--tls-ca` on the client — the certificate must then also be **issued for
  the host you connect to**, not merely signed by that CA.
- **Bind deliberately.** `127.0.0.1` reaches only the local machine; `0.0.0.0` reaches everyone who
  can route to you. Prefer forwarding the loopback port over SSH to exposing it directly.

## Stopping a daemon

Send it `SIGINT` or `SIGTERM` — <kbd>Ctrl</kbd>+<kbd>C</kbd> if it is in the foreground, or
`kill <pid>`. On Windows, a console-control event does the same.

There is deliberately **no** `contour daemon --stop` verb, no pid file, and no bundled systemd
unit; use your service manager if you want supervision.

!!! warning "Sessions do not survive the daemon"
    Persistence is against the *client*, not against the daemon process. Stopping the daemon ends
    every shell it hosts. Shutdown is abrupt by design — attached clients see the connection close
    with no goodbye, exactly as a real `tmux kill-server` looks to them.

## Diagnostics

The daemon logs through Contour's category system; `contour list-debug-tags` prints every tag.
The `vthost.*` categories are the daemon's:

| Category | Default | What it records |
| --- | --- | --- |
| `vthost.daemon` | **on** | Endpoints bound, listeners started, signals, shutdown |
| `vthost.conn` | off | Per-connection accept, handshake, disconnect |
| `vthost.session` | off | Session spawn, resize, exit |
| `vthost.tmux` | off | Control-mode and tmux-binary attach or rejection |
| `vthost.client` | off | The client side of the native protocol |
| `vthost.trace.proto` | off | One line per protocol message, both directions |
| `vthost.trace.tmux` | off | Every control-mode line, both directions |

```sh
contour daemon --log 'vthost.conn,vthost.session'
contour daemon --log 'vthost.*' --log-file /var/log/contour-daemon.log
```

Three behaviours to know:

- `*` is a plain prefix match, so `vthost.*` sweeps in the verbose `vthost.trace.*` tier as well.
  Name `vthost.trace.*` to select the trace tier alone.
- A filter is a *selection*: it disables the categories it does not name. Real failures are exempt
  — asking for extra detail never takes error reporting away.
- `--log-file` **appends** and never writes colour escapes, so restarting a daemon against the
  same file does not erase why the previous run died.

Traces report sizes, never payloads: a keystroke message shows a byte count, a screen update a
line count, and a token only as present or absent. A trace of a busy session must not become a
transcript of what you typed.

## Platform notes

Daemon mode works on Linux, macOS, BSD and Windows 10 1803 or newer, over Unix domain sockets
everywhere. On Windows, sessions are backed by ConPTY, the socket directory is governed by NTFS
ACLs rather than POSIX permission bits, and the tmux-binary (`<socket>-tmux`) endpoint is absent —
it needs file-descriptor passing, which Windows sockets do not have.

## After upgrading Contour, restart the daemon

The client and the daemon must be the **same build**. The protocol handshake requires an exact
version match, so a daemon left running from before an upgrade will refuse the new client — and it
refuses in a way that is deliberately indistinguishable from a wrong token, so that a
network attacker learns nothing from probing. The cost of that choice is that the error cannot tell
you which of the two it was.

So if a client will not attach right after an upgrade, stop the daemon and let it start again:

```sh
kill "$(pgrep -f 'contour daemon')"   # or Ctrl+C in its terminal
contour client                        # starts a matching daemon
```

Remember that this ends the sessions it was hosting. An auto-started daemon avoids the problem
entirely, since it exits with its last session.

## Current limitations

- **`contour client` opens a window.** There is no terminal-only client at present; if you need
  one, attach with `tmux -C` through the compatibility endpoint described above.
- **You get the scrollback the daemon still holds, and no more.** Attaching replays a session's
  history, but the daemon keeps only `history.limit` lines of it per session — 1000 unless the
  profile it was started with says otherwise. Lines that scrolled past that are gone, and there is
  no way to ask for them back.
- **A session sitting on the alternate screen has no scrollback to show.** Attach while `vim`,
  `less` or `htop` is running and you get that program's screen, which never has history of its
  own; the shell's scrollback appears when the program exits.
- Sessions end with the daemon (see [Stopping a daemon](#stopping-a-daemon)).
- Images are not recoverable through tmux-mode replay (see the note under
  [tmux interoperability](#tmux-interoperability)).

For the architecture behind all of this — the two protocols, the delta transport, the stable row
identity in the grid — see [the internals page](internals/vthost.md).
