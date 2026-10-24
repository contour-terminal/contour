# Program status (OSC 7501)

A program can tell the terminal what it is doing: whether it is working, finished, failed, or
waiting for the user to approve something or answer a question. The terminal keeps the latest
report of every program running in it, and shows it where the user will see it even when the
tab is in the background.

Contour implements the [program-status protocol](https://superlogical.com/rex/docs/build/program-status),
revision 0.4.

## Syntax

```
OSC 7501 ; key=value [ : key=value ... ] ST
OSC 7501 ; ? ST
```

Pairs are separated by `:`. A value uses letters, digits and `_ . , + / = -`; a pair holding
anything else, a pair without `=`, and unknown keys are skipped, and a repeated key keeps its last
value.

| Key | Value | Meaning |
| --- | ----- | ------- |
| `state` | `idle`, `working`, `done`, `blocked`, `error`, `clear` | Required. `clear` removes records. |
| `id` | `segment(/segment)*` | Which record; absent for the root. `build/test` lies beneath `build`. |
| `kind` | `permission`, `question`, `auth` | Why it is blocked; only with `blocked`. |
| `progress` | `0`..`100` | Percent done; only with `working` or `blocked`. |
| `app` | `[A-Za-z0-9_.+-]{1,32}` | The program; inherited from the nearest ancestor when absent. |
| `title` | base64 UTF-8 | A short label. |
| `msg` | base64 UTF-8 | One line describing the status. |

A report replaces its record whole: a key it leaves out is cleared.

## Limits

A report that breaks one of these is discarded whole, and the records stay as they were:

- the whole sequence is at most 4096 bytes, a key at most 16;
- `id` is at most 128 bytes, 32 per segment and 8 segments deep;
- `msg` is at most 2732 bytes encoded and 2048 decoded, `title` 256 and 192;
- base64 must be valid (padding is optional);
- decoded text must be UTF-8 without control characters (C0, DEL or C1).

Contour keeps at most **64 records per terminal**, the minimum the protocol requires. A 65th
evicts the record that was updated least recently, so a loop reporting fresh ids cannot grow the
terminal.

## Lifetime

Records belong to the terminal, not to a screen: switching to the alternate screen and back keeps
them, and so does a soft reset (DECSTR). A full reset (RIS) removes all of them.

| State | Ends when |
| ----- | --------- |
| `working`, `blocked` | Replaced, cleared, the process exits, or a new shell prompt starts (OSC 133 `A`) |
| `idle` | Replaced or cleared; Contour keeps it across a prompt |
| `done`, `error` | Replaced or cleared |

A close the user or a script asks for ends the terminal, and its records with it.

## Feature detection

```sh
printf '\033]7501;?\033\\\033[c'
```

Contour answers

```
OSC 7501 ; ? : states=idle,working,done,blocked,error : kinds=permission,question,auth ST
```

before the primary device attributes reply. A terminal that does not support the protocol answers
only the device attributes.

## Where it shows

- **The tab strip.** A tab shows the most urgent record of all its panes: blocked, then error,
  then working, then done. Working (with its progress, when given) and blocked show always; done
  and error only while the tab is in the background. The badge's tooltip is the record's title,
  else its message, else its app, with invisible formatting characters (bidi overrides,
  zero-width characters) removed.
- **The control API.** `contour ls --json` lists each pane's records, and
  `contour events --kind pane.programStatusChanged` follows their changes.
- **Daemon mode.** The daemon keeps the records and mirrors them to every attached client.

Contour does not derive program status from the progress indicator (OSC 9;4), nor the other way
round: they are separate sequences with separate meanings.

## Examples

```sh
# A build: working at 40%, then finished.
printf '\033]7501;state=working:app=cargo:progress=40\033\\'
printf '\033]7501;state=done:app=cargo\033\\'

# Waiting for an approval, with a message.
printf '\033]7501;state=blocked:kind=permission:app=terraform:msg=%s\033\\' \
    "$(printf 'Apply 3 changes?' | base64 -w0)"

# Two regions of a deployment, then clearing both.
printf '\033]7501;state=working:id=deploy/eu:app=deploy\033\\'
printf '\033]7501;state=error:id=deploy/us\033\\'
printf '\033]7501;state=clear:id=deploy\033\\'
```
