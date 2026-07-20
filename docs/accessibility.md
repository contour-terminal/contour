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

## The window around the terminal

The tab bar is exposed as a tab list whose tabs announce themselves as you switch between them, the
window and tab buttons say what they do rather than naming the glyph they are drawn with, and every
field on the settings page carries its own label.

## Things that are only announced

Some of what a terminal does has no representation in the accessibility tree at all — nothing's state
changed, so nothing is reported unless it is said explicitly. Contour announces:

| Event | Politeness |
|-------|-----------|
| The bell | Polite — it waits for your screen reader to finish its sentence |
| A desktop notification (OSC 99 / OSC 777) | Polite |
| Read-only mode being switched on or off | Assertive — it interrupts, because it changes what typing does |

Set `accessibility_announcements: false` in `contour.yml` to switch these off. They cost nothing while
no assistive client is attached.

## Reading a selection aloud

Select some text and press ++ctrl+shift+s++, or pick **Read Aloud** from the right-click menu, to have
the operating system's speech synthesizer read it. ++ctrl+shift+s++ again reads the new selection.

Text is prepared before it is spoken: the blank padding every terminal line carries out to the right
margin is dropped, runs of empty lines collapse to a single pause, and a very long selection is cut at
a line boundary so that selecting a build log gives a readable excerpt rather than many minutes of
speech.

!!! note "This needs a speech engine, and it is optional"

    Reading aloud needs Qt's TextToSpeech module at build time and a speech engine with an installed
    voice at run time — on Linux that means `speech-dispatcher` or `flite`. Where either is missing the
    feature is simply not offered: the menu row does not appear and the shortcut does nothing, rather
    than appearing and staying silent.

    A build that omitted it says so in its own build log, so a packager can see it without having to
    run the application.

## Platform notes

| Platform | Technology | Notes |
|----------|-----------|-------|
| Linux    | AT-SPI 2 | Works with Orca and with KDE Plasma's zoom — see the note below. |
| Windows  | UI Automation | Works with Magnifier's *follow keyboard focus* / *text cursor* modes. |
| macOS    | NSAccessibility | Qt maps the text interface to `AXTextArea`. Not verified. |

!!! important "KDE Plasma: caret tracking is off by default"

    Plasma's zoom (`Meta`+`+` / `Meta`+`-` / `Meta`+`0`) follows the **mouse pointer** out of the box and
    ignores the text caret until you say otherwise. Turn on **Enable caret tracking** in
    *System Settings → Accessibility → Screen Magnifier*; without it the zoom will not follow the cursor
    in Contour — or in any other application — no matter what the application reports.

    KWin consumes the AT-SPI `object:text-caret-moved` signal for this. If tracking still does not follow
    after enabling the option, `KWIN_WAYLAND_ZOOM_FORCE_LEGACY_TEXT_CARET_TRACKING=1` forces KWin onto
    that AT-SPI path rather than its newer one.

Qt's own environment variables apply. In particular `QT_ACCESSIBILITY=0` disables accessibility for the
process, and on Linux `QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1` forces the AT-SPI bridge on even when no client
has announced itself.

## Verifying it

To inspect what Contour actually exposes:

* **Linux** — [Accerciser](https://gitlab.gnome.org/GNOME/accerciser). Select the Contour window; the
  terminal appears with a text interface, and the caret offset updates as you type. A *shell prompt* child
  appears while you are at a prompt and disappears while a command runs.
* **Windows** — Accessibility Insights, or `Inspect.exe` from the Windows SDK.
