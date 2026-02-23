# Binary Paste Mode

Applications running inside a terminal currently have no way to receive binary clipboard data
(such as images) from the user's clipboard. The traditional paste mechanism — including
[Bracketed Paste Mode](https://en.wikipedia.org/wiki/Bracketed-paste) (DEC mode 2004) —
only supports plain text.

Binary Paste Mode is a terminal protocol extension that allows the terminal emulator
to deliver binary clipboard data to applications that opt in, complete with MIME type
metadata and base64 encoding. This enables use cases such as pasting images into
terminal-based editors, chat clients, or file managers.

## Feature Detection

An application can detect support by sending a **DECRQM** query for mode 2033:

```
CSI ? 2033 $ p   Query Binary Paste Mode
```

Response: `CSI ? 2033 ; Ps $ y` where `Ps` indicates:

| `Ps` | Meaning                                                          |
|------|------------------------------------------------------------------|
| `1`  | Mode is set (enabled)                                            |
| `2`  | Mode is reset (disabled)                                         |
| `0`  | Mode not recognized (terminal does not support Binary Paste)     |

If the terminal does not recognize mode 2033, it will respond with `Ps = 0`,
allowing the application to distinguish between "supported but disabled" and
"not supported at all".

## Mode Control

Applications opt in via standard DECSET/DECRST sequences using **DEC private mode 2033**:

```
CSI ? 2033 h     Enable Binary Paste Mode
CSI ? 2033 l     Disable Binary Paste Mode (default)
```

Binary Paste Mode is disabled by default and resets to disabled on hard reset (RIS)
and soft reset (DECSTR).

## Sub-Command Architecture

All binary paste operations use a single DCS sequence format with a **sub-command
character** as the first byte of the data string:

```
DCS 2033 [; params] b <sub-cmd><payload> ST
```

Where:

| Component | Description |
|-----------|-------------------------------------------------------------------------|
| `2033`    | First parameter, matching the mode number                               |
| `params`  | Optional additional parameters (e.g., byte count for data delivery)     |
| `b`       | DCS final character (mnemonic: *binary*)                                |
| `sub-cmd` | First byte of data string: sub-command identifier                       |
| `payload` | Sub-command-specific data                                               |
| `ST`      | String Terminator (`ESC \`)                                             |

### Sub-command Summary

| Sub-cmd | Direction      | Purpose                         |
|---------|----------------|---------------------------------|
| `d`     | Terminal → App | Data delivery (paste event)     |
| `c`     | App → Terminal | Configure MIME type preferences |

## Data Delivery (`d`)

When Binary Paste Mode is enabled and the user initiates a paste action while the
system clipboard contains a matching MIME type, the terminal sends the following DCS
sequence to the application via the PTY:

```
DCS 2033 ; Ps b d <mime-type> ; <base64-encoded-data> ST
```

Where:

| Component               | Description                                                             |
|-------------------------|-------------------------------------------------------------------------|
| `Ps`                    | Second parameter: byte count of the original (pre-encoding) binary data |
| `d`                     | Sub-command: data delivery                                              |
| `<mime-type>`           | MIME type of the delivered content (e.g., `image/png`)                  |
| `<base64-encoded-data>` | Base64-encoded binary data                                              |

### Example

A 1234-byte PNG image on the clipboard produces:

```
ESC P 2033 ; 1234 b dimage/png ; iVBORw0KGgoAAAANSUhEUgAA... ESC \
```

The `Ps` (size) parameter allows the application to pre-allocate a buffer before
decoding the base64 payload. If absent or zero, the size is unknown.

### Size Validation

When `Ps` is present and non-zero, the application **must** verify that the decoded
payload size matches the declared `Ps` value. If the sizes do not match, the
application **should** discard the entire paste and treat it as a protocol error.

| Condition            | Action                               |
|----------------------|--------------------------------------|
| `Ps` absent or zero  | Accept (size unknown, no validation) |
| Decoded size == `Ps` | Accept                               |
| Decoded size != `Ps` | Discard and report error             |

## MIME Preference Configuration (`c`)

Applications can configure which MIME types they accept and their priority order
by sending:

```
DCS 2033 b c <mime-list> ST
```

Where `<mime-list>` is a **comma-separated** list of MIME types in priority order
(highest priority first):

```
ESC P 2033 b cimage/png,image/svg+xml,text/html ESC \
```

### Behavior

- **Any MIME type is valid**: not limited to `image/*` types. Applications may
  request `text/html`, `application/json`, `text/plain`, etc.
- **Priority order**: the terminal checks the system clipboard for each listed
  type in order and delivers the first match via the `d` sub-command.
- **Default fallback**: if no preferences are configured, the terminal uses its
  built-in default list (see Terminal Defaults below).
- **Empty payload**: sending `DCS 2033 b c ST` (no MIME types) resets preferences
  to terminal defaults.
- **Replacement**: sending a new configure sequence replaces all previous preferences.
- **Mode gating**: the configure sequence is silently ignored if mode 2033 is not
  enabled.
- **Cleared on reset**: preferences are cleared when mode 2033 is disabled
  (DECRST 2033), on soft reset (DECSTR), and on hard reset (RIS).

### Terminal Defaults

When no application preferences are configured, the terminal uses the following
built-in priority list:

| Priority | MIME Type       |
|----------|-----------------|
|    1     | `image/png`     |
|    2     | `image/jpeg`    |
|    3     | `image/gif`     |
|    4     | `image/bmp`     |
|    5     | `image/svg+xml` |

### Separator Choice

Commas (`,`) are used to separate MIME types instead of semicolons because:
- Semicolons (`;`) are already used in the data delivery sub-command to separate
  the MIME type from the base64 payload.
- Commas align with the HTTP `Accept` header convention for MIME type lists.

## Interaction with Bracketed Paste Mode

Binary Paste Mode is independent of Bracketed Paste Mode (2004). Both can be
active simultaneously. When both are active and the clipboard contains a matching
MIME type, the DCS binary paste sequence takes precedence:

| Mode 2004 | Mode 2033 | Has Match | Behavior                   |
|-----------|-----------|-----------|----------------------------|
|    off    |   off     |   any     | Normal text paste          |
|    on     |   off     |   any     | Bracketed text paste       |
|    off    |   on      |   yes     | DCS binary paste           |
|    on     |   on      |   yes     | DCS binary paste           |
|    any    |   on      |   no      | Fall through to text paste |

Only one representation is sent per paste action — the terminal never sends both
a DCS binary paste and a bracketed text paste for the same clipboard content.

**Note:** if `text/plain` appears in the application's MIME preference list, it
will be delivered via the DCS binary paste format (the application explicitly opted
into receiving it through the binary paste protocol).

## Size Limits

Terminals implementing this extension may enforce size limits on binary paste data:

- **Hard limit**: Payloads exceeding the limit are silently dropped (no DCS sent).
  Recommended: 10 MB pre-encoding.
- **Soft limit**: Payloads between the soft and hard limit may trigger a user
  confirmation prompt. Recommended: 5 MB pre-encoding.

The base64 encoding inflates the wire size by approximately 33%.

## Future Extensions

The following sub-commands are reserved for future use:

| Sub-cmd | Direction      | Purpose                                     |
|---------|----------------|---------------------------------------------|
|  `r`    | App → Terminal | Request clipboard contents (clipboard read) |
|  `?`    | Terminal → App | Report available MIME types                 |

Clipboard read would require a permission model to prevent unauthorized clipboard
access by applications.

Multi-MIME delivery (sending all matching types in a single paste event with an
end-of-batch marker `DCS 2033 ; 0 b d ST`) is also under consideration.

## Adoption State

| Support  | Terminal/Toolkit/App | Notes                              |
|----------|----------------------|------------------------------------|
| ✅       | Contour              | since `0.6.3` (initial prototype)  |
| not yet  | Kitty                |                                    |
| not yet  | WezTerm              |                                    |
| not yet  | Ghostty              |                                    |
| not yet  | foot                 |                                    |
| not yet  | tmux                 |                                    |
| ...      | ...                  |                                    |

If your project adds support for this feature, please
[open an issue](https://github.com/contour-terminal/contour/issues) or submit a PR
so we can update this table.

## Reference

- [Bracketed Paste Mode](https://en.wikipedia.org/wiki/Bracketed-paste) — DEC mode 2004, text-only predecessor
- [OSC 52](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h3-Operating-System-Commands) — clipboard read/write via escape sequences (text/base64)
- [Kitty Clipboard Protocol](https://sw.kovidgoyal.net/kitty/clipboard/) — full bidirectional clipboard protocol (OSC 5522)
- [DECRQM (Request Mode)](https://vt100.net/docs/vt510-rm/DECRQM.html) — mode support query
