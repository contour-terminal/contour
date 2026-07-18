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
- The terminal tracks semantic zones from OSC 133 sequences using line flags and internal metadata.
- The terminal generates a session token and replies with a DCS containing the token (see Authentication below).
- The query sequence `CSI > Ps ; Pn ; T1 ; T2 ; T3 ; T4 b` becomes available.

When disabled:
- All tracked semantic block data is discarded.
- The session token is invalidated.
- The query sequence returns an error response.

## Authentication

When mode 2034 is enabled via DECSM, the terminal generates a 64-bit session token and replies with:

```
DCS > 2034 ; 1 b T1 ; T2 ; T3 ; T4 ST
```

That is: `ESC P > 2034 ; 1 b T1 ; T2 ; T3 ; T4 ESC \`

The 4 integer values (`T1`..`T4`) are the session token. These same values must be included
in every SBQUERY request as additional CSI parameters (see Query Syntax below). No conversion
is needed — the values from the DCS reply are used directly.

### Token Lifecycle

1. **On DECSET 2034**: Terminal generates a fresh token and replies with the DCS token response.
2. **On SBQUERY**: The token must be included as parameters T1..T4. Missing or invalid tokens produce error responses.
3. **On DECRST 2034**: The token is invalidated along with all tracked data.
4. **On re-enable**: A new token is generated; previous tokens are no longer valid.

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
| T1..T4    | Session token as 4 × uint16 values (from DECSET reply) |

| Ps | Meaning                         | Pn                  |
|----|-------------------------------- |---------------------|
| 1  | Last completed command block    | ignored             |
| 2  | Last N completed command blocks | count (default 1)   |
| 3  | Current in-progress command     | ignored             |

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
| 0      | Mode disabled or no data         |
| 1      | Success with JSON payload        |
| 2      | Authentication required (no token provided) |
| 3      | Authentication failed (invalid token)       |

## JSON Response Schema

```json
{
  "version": 1,
  "blocks": [
    {
      "command": "ls -la",
      "prompt": "user@host:~$ ",
      "output": "total 42\ndrwxr-xr-x ...",
      "exitCode": 0,
      "finished": true,
      "outputLineCount": 10
    }
  ]
}
```

### Fields

| Field             | Type              | Description                                                       |
|-------------------|-------------------|-------------------------------------------------------------------|
| `version`         | integer           | Schema version, currently `1`.                                    |
| `blocks`          | array             | Array of command block objects, ordered chronologically.           |
| `command`         | string or null    | The command line from OSC 133;C `cmdline_url` parameter.          |
| `prompt`          | string            | Text content of the prompt region (from Marked line to OutputStart). |
| `output`          | string            | Text content of the command output region.                        |
| `exitCode`        | integer           | From OSC 133;D parameter. `-1` if unknown.                       |
| `finished`        | boolean           | `false` for in-progress commands (Ps=3 query), `true` otherwise. |
| `outputLineCount` | integer           | Number of output lines in the block.                              |

Text fields use JSON's native encoding for control characters (e.g. `\u001b` for ESC),
which ensures the payload never contains a raw ST (ESC \) sequence.

## Example Session

```sh
# 1. Enable the semantic block protocol and capture the token
printf '\033[?2034h'
# Response: DCS > 2034 ; 1 b 41394 ; 50132 ; 58870 ; 1816 ST
# Token values: T1=41394 T2=50132 T3=58870 T4=1816

# 2. Verify it's enabled via DECRQM
printf '\033[?2034$p'
# Response: CSI ? 2034 ; 1 $ y

# 3. Run some commands (with OSC 133 shell integration active)
ls -la
echo hello

# 4. Query the last completed command (with token)
printf '\033[>1;1;41394;50132;58870;1816b'
# Response: DCS > 1 b {"version":1,"blocks":[{"command":"echo hello",...}]} ST

# 5. Query the last 3 completed commands (with token)
printf '\033[>2;3;41394;50132;58870;1816b'

# 6. Query any in-progress command (with token)
printf '\033[>3;1;41394;50132;58870;1816b'

# 7. Disable when done (invalidates the token)
printf '\033[?2034l'
```

## Implementation Notes

- **Token storage**: Terminals implementing this specification should generate the session token
  using a cryptographically secure random number generator and store it for the duration of the
  mode being enabled. The token must be regenerated on each enable.
- **Terminal multiplexers**: DECSET does not normally produce a reply. Multiplexers and terminal
  proxies that intercept DEC mode sequences should be aware that mode 2034 produces a DCS reply
  and must forward it to the requesting application.
- **CLI/tool integration**: Tools should capture the DCS token reply immediately after sending
  DECSET 2034 and parse the 16-character hex token before issuing any SBQUERY. Libraries should
  provide a helper to convert the hex token into the 4 integer parameters.
- **Error handling**: Consumers should check the status byte in every DCS response and handle
  status 2 (missing token) and 3 (invalid token) gracefully, e.g. by re-enabling the mode to
  obtain a fresh token.
- **Capacity limits**: The number of retained completed command blocks is implementation-defined.
  Consumers should not assume an unbounded history.
- **Output encoding**: JSON text fields use standard JSON escaping for control characters
  (e.g. `\u001b` for ESC), ensuring the DCS payload never contains a raw ST sequence.

## See Also

- [OSC 133 - Shell Integration](osc-133-shell-integration.md)
