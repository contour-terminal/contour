# Clickable Links

This is also known as the `OSC 8` feature,
as it is implemented by quite some other terminal emulators and client applications already.

## Syntax

`OSC 8 ;; URL ST TEXT OSC 8 ;; ST`

`OSC 8 ; id=ID ; URL ST TEXT OSC 8 ;; ST`

## Client Tooling

- `ls` on Linux supports parameter `--hyperlinks=auto`.
- GCC 10+ supports hyperlinks for diagnostic output, see `-fdiagnostics-hyperlink=auto`

