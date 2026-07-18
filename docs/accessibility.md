# Accessibility

Contour reports where the terminal caret is to the operating system, so assistive technology can follow
it. This is what makes a screen magnifier keep the cursor in view while you type, and what lets a screen
reader know where it should be reading.

There is nothing to switch on. Reporting is always available and costs effectively nothing while no
assistive client is attached; the platform activates it when one appears.

## What is reported

| Reported | Meaning |
|----------|---------|
| The caret's position and screen rectangle | Where the cursor is, updated whenever it moves. |
| The visible screen's text | The grid contents, addressed by offset, rows separated by newlines. |
| The shell prompt region | Where the current prompt is — see below. |

The caret is reported whenever it **moves and is visible**, and also whenever it **becomes visible after
having been hidden** — so an application that hides the cursor while it redraws and then shows it again
does not leave a client pointing at a stale position.

A blinking cursor is *not* reported on every blink. Only the focused pane reports, so a split window does
not make a magnifier jump between panes.

## The prompt region

When your shell has [OSC 133 shell integration](vt-extensions/osc-133-shell-integration.md) enabled,
Contour additionally exposes the *live shell prompt* — the one you are typing at — as its own focusable
region. Assistive technology that follows focus (rather than the text caret) uses this to keep the prompt
itself in view, which is usually what you want while typing a command.

While a command is running there is no prompt to report, and Contour says so rather than pointing at a
stale one. Without shell integration, or with a shell that marks only prompt starts, the caret is still
reported as usual — only the prompt region is unavailable.

This needs no opt-in beyond the shell integration itself: it is driven by the OSC 133 marks directly and
does **not** require [DEC mode 2034](vt-extensions/semantic-block-query.md).

## Platform notes

| Platform | Technology | Notes |
|----------|-----------|-------|
| Linux    | AT-SPI 2 | Works with Orca and with KDE Plasma's zoom (enable *follow focus* or *follow text caret*). |
| Windows  | UI Automation | Works with Magnifier's *follow keyboard focus* / *text cursor* modes. |
| macOS    | NSAccessibility | Qt maps the text interface to `AXTextArea`. Not verified. |

Qt's own environment variables apply. In particular `QT_ACCESSIBILITY=0` disables accessibility for the
process, and on Linux `QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1` forces the AT-SPI bridge on even when no client
has announced itself.

## Verifying it

To inspect what Contour actually exposes:

* **Linux** — [Accerciser](https://gitlab.gnome.org/GNOME/accerciser). Select the Contour window; the
  terminal appears with a text interface, and the caret offset updates as you type. A *shell prompt* child
  appears while you are at a prompt and disappears while a command runs.
* **Windows** — Accessibility Insights, or `Inspect.exe` from the Windows SDK.
