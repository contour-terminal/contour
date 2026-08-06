# Progress indicator (OSC 9;4)

A long-running command — a package install, a build, a large download — knows how far along it is,
but nothing outside it does. This extension lets an application tell the terminal, which then shows
it where the user is already looking: the tab strip, the status line, and the OS taskbar.

The sequence originates with [ConEmu](https://conemu.github.io/) and is implemented by Windows
Terminal, iTerm2 and [Ghostty](https://ghostty.org/docs/vt/osc/conemu). Contour follows the same
grammar, so an application already emitting it needs no changes.

## Syntax

```
OSC 9 ; 4 ; <state> [ ; <progress> ] ST
```

`<state>` selects what the indicator says; `<progress>` is an integer percentage from 0 to 100.

| `<state>` | Meaning | Effect on the percentage |
| --------- | ------- | ------------------------ |
| `0` | Withdraw the indicator | Reset to 0 |
| `1` | Running normally | Set to `<progress>`, or 0 if omitted |
| `2` | Failed | Set to `<progress>` if given, else unchanged |
| `3` | Busy, with no known completion | Unchanged |
| `4` | Paused, or warning | Set to `<progress>` if given, else unchanged |

The asymmetry is deliberate and matches the other implementations. State `1` is *about* the number,
so omitting it means "at the start". States `2` and `4` are about the condition, so an application
that only wants to mark a failure need not restate a percentage it already sent. State `3` has no
meaningful position at all, and carrying the previous one across means an application can go busy
and come back without losing the bar it had drawn.

## Examples

```sh
printf '\033]9;4;1;40\033\\'   # running, 40% complete
printf '\033]9;4;3\033\\'      # busy, no percentage
printf '\033]9;4;4;40\033\\'   # paused at 40%
printf '\033]9;4;2;80\033\\'   # failed at 80%
printf '\033]9;4;0\033\\'      # withdraw the indicator
```

A shell loop reporting real progress:

```sh
for i in $(seq 0 10 100); do
    printf '\033]9;4;1;%d\033\\' "$i"
    sleep 0.2
done
printf '\033]9;4;0\033\\'
```

## Where it shows

- **The tab strip** — a thin bar along the bottom edge of the tab whose active pane reported it.
  Green while running, amber when paused, red on failure; the busy state pulses. It is drawn under
  the tab label rather than beside it, so it costs the title no width and tabs do not reflow as
  applications start and finish.
- **The status line** — via the `{Progress}` placeholder, which renders as `45%`, `BUSY`,
  `PAUSED 45%` or `ERROR 45%`, and collapses to nothing while no operation is in flight. Add it to
  a status line like any other placeholder:

  ```yaml
  status_line:
      indicator:
          right: "{Progress:Bold} | {Clock}"
  ```

- **The OS taskbar**, where the platform provides one. A window has a single taskbar button but may
  host many sessions, so they are reduced to one answer: a failure outranks a pause, which outranks
  a busy session, which outranks a running one — and among equals the *least* complete percentage
  wins, so the indicator reflects the work still outstanding.

## Withdrawing a stale indicator

The terminal cannot tell a genuinely long operation from an application that died holding one, so by
default the indicator persists until the application clears it with state `0`. That is what the
sequence specifies, and what ConEmu and Windows Terminal do. A hard reset (`RIS`) also withdraws it,
which covers the common case of a shell reset after a crash.

If you would rather have the terminal give up on a silent application, set
[`progress_timeout`](../configuration/advanced/misc.md) to a non-zero number of milliseconds.
Applications using the protocol are expected to refresh it roughly once a second, so a value well
above that — 15000 or so, which is what Ghostty hardcodes — is the useful range.

```yaml
# 0 (the default) disables expiry entirely.
progress_timeout: 15000
```

## Notes

- A malformed sequence — an unknown state, a non-numeric percentage — is ignored, leaving whatever
  indicator was already showing. Withdrawing one the application still wants shown would be worse
  than dropping a sequence the terminal cannot read.
- A percentage above 100 is clamped rather than rejected: an application overshooting still means
  "finished".
- `OSC 9` *without* a leading `4;` is ConEmu's simple desktop notification, and still works:
  `printf '\033]9;Build finished\033\\'`. Only a first parameter of exactly `4` selects the progress
  indicator, so a notification whose text merely begins with a `4` is still a notification.
- Progress is per-session state, so it survives detaching and re-attaching in
  [daemon mode](../persistent-sessions.md): a client that attaches mid-operation adopts the bar
  already in flight.
