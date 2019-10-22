# Contour - A modern C++ Terminal Emulator
![Build](https://github.com/christianparpart/contour/workflows/Ubuntu/badge.svg)
![Build](https://github.com/christianparpart/contour/workflows/Windows/badge.svg)
[![codecov](https://codecov.io/gh/christianparpart/contour/branch/master/graph/badge.svg)](https://codecov.io/gh/christianparpart/contour)
[![C++17](https://img.shields.io/badge/standard-C%2B%2B%2017-blue.svg?logo=C%2B%2B)](https://isocpp.org/)

![alt text](docs/contour-win32-acrylic-background.png "Screenshot")

**IMPORANT: THIS PROJECT IS IN ALPHA STAGE & ACTIVE DEVELOPMENT**

## Purpose

`contour` is a terminal emulator, for everyday use.

## Features

* Available on all 3 major platforms, Windows, Linux, OS/X.
* Font Ligatures Support (such as in Fira Code).
* GPU-accelerated rendering.
* Blurred behind transparent background when using Windows 10 or KDE window manager on Linux.

## Mission Statement

The following is an incomplete list of features that contour does or will support.

* [x] Available on all three major platforms (Linux, OS/X, Windows 10)
* [x] Runtime configuration reload
* [x] Font Ligatures Support (such as in Fira Code)
* [x] GPU-accelerated rendering
* [x] 256-color and Truecolor support
* [x] Key binding customization
* [x] Color Schemes
* [ ] Profiles (grouped customization of: color scheme, login shell, and related behaviours)
* [ ] Terminal Multiplexer Mode (like TMUX/screen) with a graphical as well as a text based frontend
* [ ] History Search
* [ ] Activity/Inactivity/Bell System Notification
* [ ] Multiple Sessions, Windows, Tabs, Panes (like TMUX/screen)
* [ ] Shell Integration (inspired by iTerm2)
* [ ] Inline Images (inspired by iTerm2, conforming to their custom VT sequences for compatibility)

## CLI - Command Line Interface

```txt
Contour Terminal Emulator.

Usage:
  contour [OPTIONS ...]

Options:
  -h, --help                  Shows this help and quits.
  -c, --config=PATH           Specifies path to config file to load from (and save to).
                              [~/.config/contour/contour.yml]
```

## Example Configuration File

```yaml
shell: "ssh ubuntu-vm"

terminalSize:
    columns: 130
    lines: 30

fontSize: 12
fontFamily: "Fira Code, Hack, Cascadia Code, Ubuntu Mono, Consolas, monospace"
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
    cursor: '#b0b030'
    selection: '#30c0c0'
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
    - { mods: [Control, Alt],   key: S,             action: ScreenshotVT }
    - { mods: [Control, Shift], key: Equal,         action: IncreaseFontSize }
    - { mods: [Control, Shift], key: Minus,         action: DecreaseFontSize }
    - { mods: [Control, Shift], key: N,             action: NewTerminal }
    - { mods: [Control],        mouse: WheelUp,     action: IncreaseFontSize }
    - { mods: [Control],        mouse: WheelDown,   action: DecreaseFontSize }
    - { mods: [Alt],            mouse: WheelUp,     action: IncreaseOpacity }
    - { mods: [Alt],            mouse: WheelDown,   action: DecreaseOpacity }
    - { mods: [Shift],          mouse: WheelUp,     action: ScrollPageUp }
    - { mods: [Shift],          mouse: WheelDown,   action: ScrollPageDown }
    - { mods: [],               mouse: WheelUp,     action: ScrollUp }
    - { mods: [],               mouse: WheelDown,   action: ScrollDown }
    - { mods: [Shift],          key: UpArrow,       action: ScrollOneUp }
    - { mods: [Shift],          key: DownArrow,     action: ScrollOneDown }
    - { mods: [Shift],          key: PageUp,        action: ScrollPageUp }
    - { mods: [Shift],          key: PageDown,      action: ScrollPageDown }
    - { mods: [Shift],          key: Home,          action: ScrollToTop }
    - { mods: [Shift],          key: End,           action: ScrollToBottom }
```

## Installing from source

### Prerequisites Linux

This is tested on Ubuntu 19.04, but *any* recent Linux with latest C++17 compiler should do:

```sh
apt install libfreetype6-dev libglew-dev libglfw3-dev libglm-dev libfontconfig1-dev libharfbuzz-dev
```

To enable blur-behind feature on transparent background, you'll need the following packages:

```sh
apt install libx11-dev
```

And set pass `-DCONTOUR_BLUR_PLATFORM_KWIN_X11=ON` to cmake when configuring the project.

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
