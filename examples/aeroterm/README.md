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

```
Aero Terminal Emulator.

Usage:
  aeroterm [OPTIONS ...]

Options:
  -h, --help                  Shows this help and quits.
      --log-parser-error      Enables logging of parser errorrs.
      --log-raw-input         Enables logging of raw input.
      --log-raw-output        Enables logging of raw output.
      --log-invalid-output    Enables logging of invalid output sequences.
      --log-unsupported-output
                              Enables logging of unsupported output sequences.
      --log-trace-output      Enables logging of output trace.
  -S, --font-size=PIXELS      Defines character font-size. [12]
  -C, --columns=COUNT         Defines number of text columns. [130]
  -L, --lines=COUNT           Defines number of text lines. [25]
  -F, --font=PATTERN          Defines font family. [Fira Code, Ubuntu Mono, Consolas, mo
                              nospace]
  -P, --cursor-shape=SHAPE    Defines cursor shape. [block]
  -s, --shell=SHELL           Defines shell to invoke. [/bin/bash]
  -r, --background-red=PCT    Background red color [0.0]
  -g, --background-green=PCT  Background red color [0.0]
  -b, --background-blue=PCT   Background red color [0.0]
  -T, --background-transparency=PCT
                              Defines background transparency. [1.0]
  -A, --background-blur       Enable background blur.
```

## Installing from source

### Prerequisites Linux

This is tested on Ubuntu 19.04, but *any* recent Linux with latest C++17 compiler should do:

```!sh
apt install libfreetype6-dev libglew-dev libglfw3-dev libglm-dev libfontconfig1-dev libharfbuzz-dev
```

### Prerequisites Windows 10

For Windows, you must have Windows 10, 2018 Fall Creators Update, and Visual Studio 2019, installed.
It will neither build nor run on any prior Windows OS, due to libterminal making use of [ConPTY API](https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/).

```!psh
vcpkg install freetype fontconfig glew glfw3 glm harfbuzz
```

### Prerequisites Mac OS/X

Here be lions.
