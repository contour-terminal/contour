# Semantic Block Query (DEC Mode 2034)

This extension provides a discoverable, machine-readable query mechanism that returns structured
JSON blocks of semantic command data from the terminal. It is designed for programmatic consumers
such as AI agents, accessibility tools, and automation scripts.

!!! note

    Contour's own [accessibility support](../accessibility.md) — reporting the caret and the live prompt
    region to the operating system — does **not** go through this protocol and does not require mode 2034.
    It reads the [OSC 133](osc-133-shell-integration.md) line marks directly, because those are terminal
    memory and are recorded for every shell that emits them. This extension is for *external* consumers
    that want the command history as structured data.

## Prerequisites

This feature requires [OSC 133 shell integration](osc-133-shell-integration.md) to be active
in the running shell. The shell must emit OSC 133 sequences (A, B, C, D) to mark prompt boundaries,
command output regions, and command completion.

## Feature Discovery

### DEC Private Mode 2034

```
Enable:  CSI ? 2034 h       (DECSM)
Disable: CSI ? 2034 l       (DECRM)
Query:   CSI ? 2034 $ p     (DECRQM)  ->  CSI ? 2034 ; Ps $ y
```

Where `Ps` in the DECRQM response is:
- `0` = not recognized (terminal does not support this mode)
- `1` = set (enabled)
- `2` = reset (disabled)

When enabled:
- The terminal reports the command blocks whose command has not started yet: the prompt on screen at this
  point (a shell may enable the mode from its prompt hook), and every block after it. Contour records every
  shell's command blocks whether or not this mode is on (its gutter, sticky header and notifications read
  them), but a reader never sees a command that started before it enabled the mode, so enabling it is never a
  way to read back commands that ran earlier.
- The terminal generates a session token and replies with a DCS containing the token (see Authentication below).
- The query sequence `CSI > Ps ; Pn ; T1 ; T2 ; T3 ; T4 b` becomes available.

When disabled:
- Nothing is reported any more; commands started so far stay hidden even if the mode is enabled again.
- The session token is invalidated.
- The query sequence returns an error response.

## Authentication

When mode 2034 is enabled via DECSM, the terminal generates a 64-bit session token and replies with:

```
DCS > 2034 ; 1 b T1 ; T2 ; T3 ; T4 ST
```

That is: `ESC P > 2034 ; 1 b T1 ; T2 ; T3 ; T4 ESC \`

`T1`..`T4` are the session token: four decimal integers from 0 to 65534, its four 16-bit parts. Every
SBQUERY carries them back as its last four parameters, exactly as received (see Query Syntax below).
There is nothing to convert.

A part is never 65535: a terminal reads every larger number in a CSI parameter as 65535, so that value
stands for all of them. An SBQUERY whose token carries a part of 65535 or more, or leaves a part out, fails
authentication (status 3).

### Token Lifecycle

1. **On DECSET 2034**: Terminal generates a fresh token and replies with the DCS token response.
2. **On SBQUERY**: The token must be included as parameters T1..T4. Missing or invalid tokens produce error responses.
3. **On DECRST 2034**: The token is invalidated, and the commands started so far are hidden from every later session.
4. **On RIS**: As on DECRST 2034. A hard reset returns every mode to its power-up state, this one included.
5. **On re-enable**: A new token is generated; previous tokens are no longer valid.

### Security Note

DECSET normally does not produce a reply. Some terminal multiplexers (e.g. tmux, screen)
may not forward the DCS token response to the application. In such environments, token-based
authentication may not be usable. This is a known limitation.

## Query Syntax — SBQUERY

```
CSI > Ps ; Pn ; T1 ; T2 ; T3 ; T4 b
```

**Mnemonic:** SBQUERY — **S**emantic **B**lock **QUERY**

CSI with `>` leader, final character `b` (for "blocks").

| Parameter | Meaning                                              |
|-----------|------------------------------------------------------|
| Ps        | Query type (see table below)                         |
| Pn        | Count parameter (default 1)                          |
| T1..T4    | The session token, as the DECSET reply carried it    |

| Ps | Meaning                         | Pn                  |
|----|-------------------------------- |---------------------|
| 1  | Last completed command block    | ignored             |
| 2  | Last N completed command blocks | count (default 1)   |
| 3  | Current in-progress command     | ignored             |

A `Ps` of 0, like an omitted one, means 1. Any other `Ps` is answered `DCS > 0 b ST`, as a query with no
data: a reader asking a question the terminal does not know never receives the answer to another.

A *completed* block is one the shell finished with OSC 133;D (or OSC 3008 `end=`). A block that a newer
prompt superseded without one -- the prompt of a nested shell, such as inside `ssh` -- is not reported.

## Response Syntax

The response is a DCS (Device Control String) with `>` leader and final character `b`.

**Success:**

```
DCS > 1 b {JSON} ST
```

That is: `ESC P > 1 b {JSON} ESC \`

**Error — no data or mode disabled:**

```
DCS > 0 b ST
```

**Error — authentication required (missing token):**

```
DCS > 2 b ST
```

**Error — authentication failed (wrong token):**

```
DCS > 3 b ST
```

### Status Codes

| Status | Meaning                          |
|--------|----------------------------------|
| 0      | Mode disabled, no data, or an unknown query type |
| 1      | Success with JSON payload        |
| 2      | Authentication required (no token provided) |
| 3      | Authentication failed (invalid token)       |

## JSON Response Schema

The JSON of step 4 of the [example session](#example-session) below, laid out with whitespace the terminal
does not send:

```json
{
  "version": 1,
  "blocks": [
    {
      "command": "echo hello",
      "prompt": "user@host:~$ echo hello",
      "output": "hello",
      "exitCode": 0,
      "finished": true,
      "outputLineCount": 1
    }
  ]
}
```

### Fields

| Field             | Type              | Description                                                       |
|-------------------|-------------------|-------------------------------------------------------------------|
| `version`         | integer           | Schema version, currently `1`.                                    |
| `blocks`          | array             | Array of command block objects, ordered chronologically.           |
| `command`         | string or null    | The command line from OSC 133;C `cmdline_url` or `cmdline`, or -- when the shell sent none but marked its prompt end with OSC 133;B -- the text typed after the prompt. `null` when neither is known. Longer than 4096 bytes it is cut at a character boundary and ends in `…`. |
| `prompt`          | string            | The prompt the command was typed at, the typed command included: the block's lines from the one OSC 133;A marked down to the one its output starts on. |
| `output`          | string            | What the command printed: the block's lines from the one OSC 133;C marked to where OSC 133;D ended it. For a command still running, what it printed so far, down to the line the cursor is on. |
| `exitCode`        | integer           | From OSC 133;D (or OSC 3008 `end=`). `-1` if unknown.            |
| `finished`        | boolean           | `false` for in-progress commands (Ps=3 query), `true` otherwise. |
| `outputLineCount` | integer           | Number of lines in `output`.                                      |

`prompt` and `output` are read from the block's own lines, wherever they are in the scrollback, so
neither ever carries another command's text. Lines that have scrolled out of the history are missing
from them; once all of a block's lines have, both are empty.

Text fields use JSON's native encoding for control characters (e.g. `\u001b` for ESC),
which ensures the payload never contains a raw ST (ESC \) sequence.

## Example Session

At a `user@host:~$ ` prompt, in a shell whose OSC 133 integration reports each command line
(`cmdline_url`). Every `printf` is a command of its own, and so a command block of its own.

```sh
# 1. Enable the semantic block protocol and capture the token
printf '\033[?2034h'
# Response: DCS > 2034 ; 1 b 41394 ; 50132 ; 58870 ; 1816 ST
# Token values: T1=41394 T2=50132 T3=58870 T4=1816

# 2. Verify it's enabled via DECRQM
printf '\033[?2034$p'
# Response: CSI ? 2034 ; 1 $ y

# 3. Run some commands
ls
echo hello

# 4. Query the last completed command (with token)
printf '\033[>1;1;41394;50132;58870;1816b'
# Response: DCS > 1 b {"version":1,"blocks":[{"command":"echo hello","prompt":"user@host:~$ echo hello","output":"hello","exitCode":0,"finished":true,"outputLineCount":1}]} ST

# 5. Query the last 3 completed commands (with token)
printf '\033[>2;3;41394;50132;58870;1816b'
# Response: DCS > 1 b {JSON} ST, listing `ls`, `echo hello` and the printf of step 4, oldest first

# 6. Query any in-progress command (with token)
printf '\033[>3;1;41394;50132;58870;1816b'
# Response: DCS > 1 b {JSON} ST, listing this printf itself, with "finished":false and "exitCode":-1

# 7. Disable when done (invalidates the token)
printf '\033[?2034l'
```

## Implementation Notes

- **Token storage**: Terminals implementing this specification should generate the session token
  using a cryptographically secure random number generator and store it for the duration of the
  mode being enabled. The token must be regenerated on each enable. Contour draws it from the
  platform's random device.
- **Terminal multiplexers**: DECSET does not normally produce a reply. Multiplexers and terminal
  proxies that intercept DEC mode sequences should be aware that mode 2034 produces a DCS reply
  and must forward it to the requesting application.
- **CLI/tool integration**: Tools should capture the DCS token reply immediately after sending
  DECSET 2034, and read `T1`..`T4` from it (see [Authentication](#authentication)) before issuing
  any SBQUERY.
- **Error handling**: Consumers should check the status byte in every DCS response and handle
  status 2 (missing token) and 3 (invalid token) gracefully, e.g. by re-enabling the mode to
  obtain a fresh token.
- **Capacity limits**: The number of retained completed command blocks is implementation-defined.
  Consumers should not assume an unbounded history.
- **Output encoding**: JSON text fields use standard JSON escaping for control characters
  (e.g. `\u001b` for ESC), ensuring the DCS payload never contains a raw ST sequence.

## See Also

- [OSC 133 - Shell Integration](osc-133-shell-integration.md)
