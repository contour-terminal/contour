# Contour - A modern C++ Terminal Emulator
[![Ubuntu](https://github.com/christianparpart/contour/workflows/Ubuntu/badge.svg)](https://github.com/christianparpart/contour/actions?query=workflow%3AUbuntu)
[![Windows](https://github.com/christianparpart/contour/workflows/Windows/badge.svg)](https://github.com/christianparpart/contour/actions?query=workflow%3AWindows)
[![OS/X](https://github.com/christianparpart/contour/workflows/MacOS/badge.svg)](https://github.com/christianparpart/contour/actions?query=workflow%3AMacOS)
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

* ✅ Available on all three major platforms (Linux, OS/X, Windows 10)
* ✅ Runtime configuration reload
* ✅ Font Ligatures Support (such as in Fira Code)
* ✅ GPU-accelerated rendering
* ✅ 256-color and Truecolor support
* ✅ Key binding customization
* ✅ Color Schemes
* ✅ Profiles (grouped customization of: color scheme, login shell, and related behaviours)
* ✅ Vertical Line Markers (quickly jump to markers in your history!)
* 🚧 Emoji support (-: 🌈 💝 😛 👪 :-)
* ⏳ Terminal Multiplexer Mode (like TMUX/screen) with a graphical as well as a text based frontend
* ⏳ History Search
* ⏳ Activity/Inactivity/Bell System Notification
* ⏳ Multiple Sessions, Windows, Tabs, Panes (like TMUX/screen)
* ⏳ Shell Integration (inspired by iTerm2)
* ⏳ Inline Images (inspired by iTerm2, conforming to their custom VT sequences for compatibility)

## A word on vertical line markers

Suppose you type a lot in the terminal, and I bet you do. Some commands may have inconveniently long
output and you need a way to conveniently scroll the terminal viewport up to the top of that
command. This is what this feature is there for. You can easily walk up/down your markers
like you'd walk up code folds or markers in VIM or other editors.

Set a mark:

```sh
echo -ne "\033[>M"
```

Example key bindings:

```yaml
input_mapping:
    - { mods: [Alt, Shift], key: 'k', action: ScrollMarkUp }
    - { mods: [Alt, Shift], key: 'j', action: ScrollMarkDown }
```

It is recommended to integrate the marker into your command prompt, such as `$PS1` in bash or sh to
have automatic markers set.

## CLI - Command Line Interface

```txt
Usage: contour [options]
Contour Terminal Emulator

Options:
  -h, --help            Displays this help.
  -v, --version         Displays version information.
  -c, --config <PATH>   Path to configuration file to load at startup
                        [~/.config/contour/conour.yml].
  -p, --profile <NAME>  Terminal Profile to load.
```

## Example Configuration File

```yaml
word_delimiters: " /\\()\"'-.,:;<>~!@#$%^&*+=[]{}~?|│"
default_profile: ubuntu_vm
profiles:
    ubuntu_vm:
        shell: "ssh ubuntu-vm"
        terminalSize:
            columns: 130
            lines: 30
        environment:
            TERM: xterm-256color
            COLORTERM: truecolor
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
        colors: google_dark

color_schemes:
    google_dark:
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
    - { mods: [Alt, Shift],     key: 'k',           action: ScrollMarkUp }
    - { mods: [Alt, Shift],     key: 'j',           action: ScrollMarkDown }

logging:
    file: "/path/to/contour.log"
    parseErrors: true
    invalidOutput: true
    unsupportedOutput: true
    rawInput: false
    rawOutput: false
    traceInput: false
    traceOutput: false

```

## Installing from source

### Prerequisites Linux

This is tested on Ubuntu 19.04, but *any* recent Linux with latest C++17 compiler should do:

```sh
sudo apt install \
    "g++-9" libfreetype6-dev qtbase5-dev libqt5gui5 extra-cmake-modules \
    libfontconfig1-dev libharfbuzz-dev
```

To enable blur-behind feature on transparent background, you'll need the following packages:

```sh
sudo apt install libkf5windowsystem-dev
```

And set pass `-DCONTOUR_BLUR_PLATFORM_KWIN=ON` to cmake when configuring the project.

In case you want to improve performance slightly and run at at least Linux, you can add
`-DLIBTERMINAL_EXECUTION_PAR=ON` to the cmake configuration and make sure to have `libtbb-dev`
installed beforehand.

### Prerequisites Windows 10

For Windows, you must have Windows 10, 2018 Fall Creators Update, and Visual Studio 2019, installed.
It will neither build nor run on any prior Windows OS, due to libterminal making use of [ConPTY API](https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/).

```psh
vcpkg install freetype fontconfig harfbuzz qt5-base
```

### Prerequisites Mac OS/X

```psh
brew install freetype fontconfig harfbuzz boost qt5
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
* [Text Terminal HOWTO (Chapter 7.2, PTY)](http://tldp.org/HOWTO/Text-Terminal-HOWTO-7.html#ss7.2)
