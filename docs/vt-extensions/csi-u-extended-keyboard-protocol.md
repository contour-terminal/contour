# CSIu Extended Keyboard Protocol

## TL;DR

The main problem with the classic way of processing keyboard input is, that it is highly ambiuous
when modifiers need to be used.

CSIu aims to solve this by enabling disambiguation of keyboard events that would be ambiguous otherwise.

## New VT Sequences

VT sequence                | Short description
---------------------------|--------------------------
`CSI > {flags} u`          | Enter extended keyboard protocol mode
`CSI = {flags} u`          | Request enhancement to the currently active protocol
`CSI = {flags} ; {mode} u` | Request enhancement to the currently active protocol
`CSI < {count} u`          | Leave the current keyboard protocol mode
`CSI < u`                  | Leave the current keyboard protocol mode

## How entering and leaving works

In order to more conveniently enable the application to configure the keyboard without
breaking the calling application, when the current program is about to exit, the terminal
leverages an internal stack of keyboard protocol flags.

When an application starts, it can push a new desired protocol state onto the keyboard protocol stack,
and thus get keyboard events respectively to the flags being set.

When the application exits, the flags are being previously stored on the top of the stack,
are now simply being popped from the flag stack, thus, effectively making the old state active again.

This stack must store at most 32 flags. The bottom-most stack has no flags set, and thus, will
act like the legacy keyboard protocol. This bottom-most stack item cannot be popped from the stack.

When the terminal hard-resets, the keyboard's flag stack is being set back to 1 element (the legacy flags).

## Operating Mode Flags

This is the number of flags that can be **or**'d together into a flag bitset either when entering
a new keyboard-protocol session, or when enhancing the currently active keyboard protocol session.

Binary      | Decimal | short description
------------|---------|------------
`0b0'0001`  |  ` 1`   | Disambiguation
`0b0'0010`  |  ` 2`   | Report all event types (press, repeat, release)
`0b0'0100`  |  ` 4`   | Report alternate keys
`0b0'1000`  |  ` 8`   | Report all keys as control sequence
`0b1'0000`  |  `16`   | Report associated text

## Entering CSIu mode

Syntax: `CSI > {flags} u`

By default, `flags` is set to `1`, but `flags` ben a a binary-or'd set of flags to enable.

Passing `0` will act like `1`.

### Flag: Disambiguation

Any non-ambiuous key event, such as regular text without any modifiers pressed, is is sent out in UTF-8 text as is.

Non-printable key events (such as `F3` or `ESC`), and printable text key events with modifiers being pressed,
are sent out in form of a control sequence.

Syntax (for some key events): `CSI ? {Code} ; {Modifier} ~`

Syntax (for text and most other key events): `CSI ? {Code} ; {Modifier} u`

`{Code}` is the numerical form of the unicode codepoint for textual key events,
and a special assigned numeric value from the Unicode PUA range otherwise.

Key       | Code | Final Char
----------|-------------------
`<Enter>` | `27` | `u`

### Flag: Report all event types

With this flag enabled, an additional sub parameter to the modifier is passed
to indicate if the key was just pressed, repeated, or released.
A repeat-event is usually triggered by the operating system to simulate key press events
at a fixed rate without needing to press and release that key many times.

Event type  | Code
------------|-----------
Key Press   | `1`
Key Repeat  | `2`
Key Release | `3`

### Report alternate keys

TODO ...

### Report all keys as control sequence

With this flag enabled, not just otherwise ambiguous events are sent as control sequence,
but every key event is sent as control sequence instead.

### Report associated text

TODO ...

## Change currently active protocol

Syntax: `CSI = {flags} ; {mode} u`

The top of the keyboard's flags stack can be changed in one of the three ways:

Mode value | description
-----------|------------------------------------------------
`1`        | set given flags to currently active flags
`2`        | add given flags to currently active flags
`3`        | remove given flags from currently active flags

This will immediately take affect.

## Leaving CSIu mode

Syntax: `CSI < {count} u`

When terminating an application session, an application must pop off what was previously pushed
onto the keyboard's flags stack.

`count` can be greater than `1` in order to pop multiple flags at once.
Passing a bigger number than the number of elements in the flags stack minus the bottom-most legacy entry,
will truncate the number `count` value to number of elements in the flags stack minus `1`.
