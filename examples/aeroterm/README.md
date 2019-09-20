# Aero Terminal Emulator

![alt text](AeroTerm-Acrylic-Background.png "Aero Terminal with Acrylic Background")

## Purpose

`aeroterm` is a purely minimal terminal emulator, based on
[libterminal](https://github.com/christianparpart/libterminal/).
It purely serves as example on how to hack a terminal with libterminal, and thus will be:

* single-windowed
* single-tabbed

## Features

* Available on all 3 major platforms, Windows, Linux, OS/X.
* Font Ligatures Support (such as in Fira Code).
* GPU-acelerated rendering.

## CLI - Command Line Interface

```txt
Aero Terminal Emulator.

Usage:
  aeroterm [OPTIONS ...]

Options:
  -h, --help                  Shows this help and quits.
  -c, --config=PATH           Specifies path to config file to load from (and save to). [aeroterm.yml]
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
