# Contour - A modern C++ Terminal Emulator
[![CI Build](https://github.com/contour-terminal/contour/workflows/Build/badge.svg)](https://github.com/contour-terminal/contour/actions?query=workflow%3ABuild)
[![codecov](https://codecov.io/gh/contour-terminal/contour/branch/master/graph/badge.svg)](https://codecov.io/gh/contour-terminal/contour)
[![C++17](https://img.shields.io/badge/standard-C%2B%2B%2017-blue.svg?logo=C%2B%2B)](https://isocpp.org/)
[![Discord](https://img.shields.io/discord/479301317337284608.svg?label=&logo=discord&logoColor=ffffff&color=7389D8&labelColor=6A7EC2)](https://discord.gg/ncv4pG9)
[![Twitch Live Stream](https://img.shields.io/badge/Twitch-Live%20Stream-blue?style=flat-square)](https://twitch.tv/christianparpart)

![alt text](docs/contour-screenshots-0.1.0-pre2.png "Screenshot")

**IMPORTANT: THIS PROJECT IS IN BETA STAGE & ACTIVE DEVELOPMENT**

`contour` is a modern terminal emulator, for everyday use. It is aiming
for power users with a modern feature mindset.

## Features

- ✅ Available on all 3 major platforms, Linux, OS/X, Windows (Windows is alpha stage).
- ✅ GPU-accelerated rendering.
- ✅ Font ligatures support (such as in Fira Code).
- ✅ Unicode: Emoji support (-: 🌈 💝 😛 👪 - including ZWJ, VS15, VS16 emojis :-)
- ✅ Unicode: Grapheme cluster support
- ✅ Bold and italic fonts
- ✅ High-DPI support.
- ✅ Vertical Line Markers (quickly jump to markers in your history!)
- ✅ Blurred behind transparent background when using Windows 10 or KDE window manager on Linux.
- ✅ Runtime configuration reload
- ✅ 256-color and Truecolor support
- ✅ Key binding customization
- ✅ Color Schemes
- ✅ Profiles (grouped customization of: color scheme, login shell, and related behaviours)
- ✅ [Synchronized rendering](https://github.com/contour-terminal/contour/wiki/VTExtensions#synchronized-output) (via `DECSM 2026` / `DECRM 2026`)
- ✅ Text reflow (configurable via `DECSM 2027` / `DECRM 2027`)
- ✅ Clickable hyperlinks via [OSC 8](https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda)
- ✅ Clipboard setting via OSC 52
- ✅ Sixel inline images
- ✅ Terminal page [buffer capture VT extension](https://github.com/contour-terminal/contour/wiki/VTExtensions#buffer-capture) to quickly extract contents.

## CLI - Command Line Interface

```txt
  Usage:

    contour [terminal] [config STRING] [profile STRING] [debug STRING] [live-config]
                       [working-directory STRING] [class STRING] [PROGRAM ARGS...]
    contour help
    contour version
    contour parser-table
    contour list-debug-tags
    contour capture [logical] [timeout FLOAT] [lines INT] output STRING

  Detailed description:

    contour [terminal]
        Spawns a new terminal application.

        Options:

            [config STRING]             Path to configuration file to load at startup.
                                        [default: /home/trapni/.config/contour/contour.yml]
            [profile STRING]            Terminal Profile to load.
            [debug STRING]              Enables debug logging, using a comma seperated list of tags.
            [live-config]               Enables live config reloading. [default: false]
            [working-directory STRING]  Sets initial working directory. [default: .]
            [class STRING]              Sets the WM_CLASS property of the window. [default: contour]
            [PROGRAM ARGS...]           Executes given program instead of the configuration provided one.

```

## Example Configuration File

```yaml
word_delimiters: " /\\()\"'-.,:;<>~!@#$%^&*+=[]{}~?|│"
default_profile: ubuntu_vm
profiles:
    ubuntu_vm:
        shell: "ssh ubuntu-vm"
        terminal_size:
            columns: 130
            lines: 30
        environment:
            TERM: xterm-256color
            COLORTERM: truecolor
        font:
            size: 12
            render_mode: lcd
            regular: "Fira Code"
            bold: "Fira Code:style=bold"
            italic: "Hack:style=italic"
            bold_italic: "Hack:style=bold italic"
            emoji: "emoji"
        tab_width: 8
        history:
            limit: 8000
            scroll_multiplier: 3
            auto_scroll_on_update: true
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
    - { mods: [Control],        key: '0',           action: ResetFontSize }
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
    parse_errors: true
    invalid_output: true
    unsupported_output: true
    raw_input: false
    raw_output: false
    trace_input: false
    trace_output: false

```

## Installing from source

### Prerequisites Linux

This is tested on Ubuntu 19.04, but *any* recent Linux with latest C++17 compiler should do:

```sh
sudo apt install \
    "g++-9" cmake pkg-config make libfreetype6-dev qtbase5-dev libqt5gui5 extra-cmake-modules \
    libfontconfig1-dev libharfbuzz-dev libfontconfig-dev
```

To enable blur-behind feature on transparent background, you'll need the following packages:

```sh
sudo apt install libkf5windowsystem-dev
```

And set pass `-DCONTOUR_BLUR_PLATFORM_KWIN=ON` to cmake when configuring the project.

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
* [ANSI escape code](https://en.wikipedia.org/wiki/ANSI_escape_code) in Wikipedia

### License

```
Contour - A modern C++ Terminal Emulator
-------------------------------------------

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```
