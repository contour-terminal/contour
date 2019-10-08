# Contour - A modern C++ Terminal Emulator
[![CircleCI](https://circleci.com/gh/christianparpart/contour.svg?style=svg)](https://circleci.com/gh/christianparpart/contour)
[![codecov](https://codecov.io/gh/christianparpart/contour/branch/master/graph/badge.svg)](https://codecov.io/gh/christianparpart/contour)
[![C++17](https://img.shields.io/badge/standard-C%2B%2B%2017-blue.svg?logo=C%2B%2B)](https://isocpp.org/)
[Build](https://github.com/christianparpart/contour/workflows/.github/workflows/linux.yml/badge.svg)

![alt text](docs/contour-win32-acrylic-background.png "Screenshot")

**IMPORANT: THIS PROJECT IS IN ALPHA STAGE & ACTIVE DEVELOPMENT**

## Purpose

`contour` is a terminal emulator, for everyday use.

## Features

* Available on all 3 major platforms, Windows, Linux, OS/X.
* Font Ligatures Support (such as in Fira Code).
* GPU-acelerated rendering.

## Mission Statement

The following is an incomplete list of features that contour does or will support.

* [x] Available on all three major platforms (Linux, OS/X, Windows 10)
* [x] Runtime configuration reload
* [x] Font Ligatures Support (such as in Fira Code)
* [x] GPU-accelerated rendering
* [x] 256-color and Truecolor support
* [x] Key binding customization
* [ ] Terminal Multiplexer Mode (like TMUX/screen) with a graphical as well as a text based frontend
* [ ] Multiple Sessions, Windows, Tabs, Panes (like TMUX/screen)
* [ ] History Search
* [ ] Activity/Inactivity/Bell System Notification
* [ ] Color Schemes
* [ ] Profiles (grouped customization of: color scheme, login shell, and related behaviours)
* [ ] Shell Integration (inspired by iTerm2)
* [ ] Inline Images (inspired by iTerm2, conforming to their custom VT sequences for compatibility)

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

history:
    limit: 8000
    scrollMultiplier: 3
    autoScrollOnUpdate: true

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

input_mapping:
    - { mods: [Alt],            key: Enter,         action: ToggleFullscreen }
    - { mods: [Control, Alt],   key: "S",           action: ScreenshotVT }
    - { mods: [Control, Shift], key: "+",           action: IncreaseFontSize }
    - { mods: [Control, Shift], key: "-",           action: DecreaseFontSize }
    - { mods: [Control],        mouse: WheelUp,     action: IncreaseFontSize }
    - { mods: [Control],        mouse: WheelDown,   action: DecreaseFontSize }
    - { mods: [Alt],            mouse: WheelUp,     action: IncreaseOpacity }
    - { mods: [Alt],            mouse: WheelDown,   action: DecreaseOpacity }
    - { mods: [],               mouse: WheelUp,     action: ScrollUp }
    - { mods: [],               mouse: WheelDown,   action: ScrollDown }
    - { mods: [Shift],          mouse: WheelUp,     action: ScrollPageUp }
    - { mods: [Shift],          mouse: WheelDown,   action: ScrollPageDown }
    - { mods: [Shift],          key: PageUp,        action: ScrollPageUp }
    - { mods: [Shift],          key: PageDown,      action: ScrollPageDown }
    - { mods: [Control],        key: Home,          action: ScrollToTop }
    - { mods: [Control],        key: End,           action: ScrollToBottom }
```

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

```psh
brew install freetype fontconfig glew glfw3 glm harfbuzz boost
```


# References

* [VT510](https://vt100.net/docs/vt510-rm/): VT510 Manual, see Chapter 5.
* [ECMA-35](http://www.ecma-international.org/publications/standards/Ecma-035.htm):
    Character Code Structure and Extension Techniques
* [ECMA-43](http://www.ecma-international.org/publications/standards/Ecma-043.htm):
    8-bit Coded Character Set Structure and Rules
* [ECMA-48](http://www.ecma-international.org/publications/standards/Ecma-048.htm):
    Control Functions for Coded Character Sets
* [ISO/IEC 8613-6](https://www.iso.org/standard/22943.html):
    Character content architectures
* [xterm](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html): xterm control sequences
* [console\_codes](http://man.he.net/man4/console_codes) Linux console codes
* [Summary of ANSI standards for ASCII terminals](http://www.inwap.com/pdp10/ansicode.txt)
