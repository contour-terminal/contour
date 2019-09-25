# Contour - A modern C++ Terminal Emulator
[![CircleCI](https://circleci.com/gh/christianparpart/contour.svg?style=svg)](https://circleci.com/gh/christianparpart/contour)
[![codecov](https://codecov.io/gh/christianparpart/contour/branch/master/graph/badge.svg)](https://codecov.io/gh/christianparpart/contour)
[![C++17](https://img.shields.io/badge/standard-C%2B%2B%2017-blue.svg?logo=C%2B%2B)](https://isocpp.org/)

![alt text](docs/contour-win32-acrylic-background.png "Screenshot")

**IMPORANT: THIS PROJECT IS IN ALPHA STAGE & ACTIVE DEVELOPMENT**

## Purpose

`contour` is a terminal emulator, for everyday use.

## Features

* Available on all 3 major platforms, Windows, Linux, OS/X.
* Font Ligatures Support (such as in Fira Code).
* GPU-acelerated rendering.

## CLI - Command Line Interface

```txt
Contour Terminal Emulator.

Usage:
  contour [OPTIONS ...]

Options:
  -h, --help                  Shows this help and quits.
  -c, --config=PATH           Specifies path to config file to load from (and save to). [contour.yml]
```

## Example Configuration File

```yaml
shell: "ssh ubuntu-vm"

terminalSize:
    columns: 130
    lines: 30

fontSize: 12
fontFamily: "Fira Code, Cascadia Code, Ubuntu Mono, Consolas, monospace"
tabWidth: 8

cursor:
    shape: block
    blinking: true

background:
    opacity: 0.9
    blur: false

logging:
    file: "/path/to/contour.log"
    parseErrors: true
    invalidOutput: true
    unsupportedOutput: true
    rawInput: false
    rawOutput: false
    traceInput: false
    traceOutput: false

colors: # Color scheme: Google Dark
    default:
        background: '#1d1f21'
        foreground: '#c5c8c6'
    normal:
        black:   '#1d1f21'
        red:     '#cc342b'
        green:   '#198844'
        yellow:  '#fba922'
        blue:    '#3971ed'
        magenta: '#a36ac7'
        cyan:    '#3971ed'
        white:   '#c5c8c6'
    bright:
        black:   '#969896'
        red:     '#cc342b'
        green:   '#198844'
        yellow:  '#fba922'
        blue:    '#3971ed'
        magenta: '#a36ac7'
        cyan:    '#3971ed'
        white:   '#ffffff'
```

## Keyboard Bindings

| Shortcut                          | Action                 |
|-----------------------------------|------------------------|
| <kbd>Control</kbd>+<kbd>Shift</kbd>+<kbd>=</kbd>      | Increase font size     |
| <kbd>Control</kbd>+<kbd>Shift</kbd>+<kbd>-</kbd>      | Decrease font size     |
| <kbd>Alt</kbd>+<kbd>Enter</kbd>  | Toggle fullscreen mode |
| <kbd>Control</kbd>+<kbd>Mouse Wheel Up</kbd> | Increases font size |
| <kbd>Control</kbd>+<kbd>Mouse Wheel Down</kbd> | Decreases font size |
| <kbd>Alt</kbd>+<kbd>Mouse Wheel Up</kbd> | Increases transparency |
| <kbd>Alt</kbd>+<kbd>Mouse Wheel Down</kbd> | Decreases transparency |

## Installing from source

### Prerequisites Linux

This is tested on Ubuntu 19.04, but *any* recent Linux with latest C++17 compiler should do:

```sh
apt install libfreetype6-dev libglew-dev libglfw3-dev libglm-dev libfontconfig1-dev libharfbuzz-dev
```

### Prerequisites Windows 10

For Windows, you must have Windows 10, 2018 Fall Creators Update, and Visual Studio 2019, installed.
It will neither build nor run on any prior Windows OS, due to libterminal making use of [ConPTY API](https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/).

```psh
vcpkg install freetype fontconfig glew glfw3 glm harfbuzz
```

### Prerequisites Mac OS/X

Here be lions.

# References

- [VT510](https://vt100.net/docs/vt510-rm/): VT510 Manual, see Chapter 5.
- [ECMA-35](http://www.ecma-international.org/publications/standards/Ecma-035.htm):
    Character Code Structure and Extension Techniques
- [ECMA-43](http://www.ecma-international.org/publications/standards/Ecma-043.htm):
    8-bit Coded Character Set Structure and Rules
- [ECMA-48](http://www.ecma-international.org/publications/standards/Ecma-048.htm):
    Control Functions for Coded Character Sets
- [ISO/IEC 8613-6](https://www.iso.org/standard/22943.html):
    Character content architectures
- [xterm](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html): xterm control sequences
- [console\_codes](http://man.he.net/man4/console_codes) Linux console codes
- [Summary of ANSI standards for ASCII terminals](http://www.inwap.com/pdp10/ansicode.txt)
