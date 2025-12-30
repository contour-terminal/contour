# OSC 133 - Shell Integration

This documentation describes the OSC 133 sequence used for shell integration, inspired by FinalTerm.

## Sequence Specification

**Format:** `OSC 133 ; <Command> [; <Parameters>...] ST`

Where:

* `OSC` is `\033]`.
* `ST` (String Terminator) is `\033\` (or BEL `\007`).
* `<Command>` is a single character identifier (A, B, C, D).

### Commands

#### A - Prompt Start

Sent before the shell prompt starts printing.

**Format:** `OSC 133 ; A [; <Key>=<Value>...] ST`

**Parameters:**

* `click_events=1`: Optional. If present (e.g., `OSC 133;A;click_events=1;ST`), indicates that the terminal should enable mouse click reporting for the prompt area.

**Behavior:**

* Marks the current line as a prompt line (conceptually similar to setting a "mark").
* Notifies the terminal that the prompt is beginning.

#### B - Prompt End

Sent after the shell prompt has finished printing and before user input begins.

**Format:** `OSC 133 ; B ST`

**Behavior:**

* Notifies the terminal that the prompt has ended.

#### C - Command Output Start

Sent after the user has finished typing the command (usually on Enter press) and before the command output begins.

**Format:** `OSC 133 ; C [; <Key>=<Value>...] ST`

**Parameters:**

* `cmdline_url=<EncodedURL>`: Optional. Defines the command line being executed. The URL is percent-encoded.

**Behavior:**

* Notifies the terminal that command execution is starting and output will follow.
* The `cmdline_url` parameter allows the terminal to know exactly what command is being run (useful for features like "Run Recent Command").

#### D - Command Finished

Sent after the command has finished executing and before the next prompt starts.

**Format:** `OSC 133 ; D [; <ExitCode>] ST`

**Parameters:**

* `<ExitCode>`: The integer exit code of the command (e.g., `0` for success).

**Behavior:**

* Notifies the terminal that the command has finished.
* Reports the exit code of the command.

---

## Example Flow

```bash
# Prompt Start
printf "\033]133;A\033\\"

# ... print prompt ...
printf "user@host:~$ "

# Prompt End
printf "\033]133;B\033\\"

# ... user types command (e.g., 'ls') ...

# Command Output Start
printf "\033]133;C\033\\"

# ... command output ...
ls

# Command Finished (exit code 0)
printf "\033]133;D;0\033\\"
```

## Related Extensions

* **SETMARK (CSI > M)**: This extension is deprecated in favor of OSC 133.
It is however similar to `OSC 133 ; A` by triggering `promptStart` and marking the line.
