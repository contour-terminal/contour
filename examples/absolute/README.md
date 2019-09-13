# |absolute| Terminal Emulator

### Purpose

`|absolute|` is a purely minimal terminal emulator, based on libterminal.
It purely serves as example on how to hack a terminal with libterminal, and thus will be:

* single-windowed
* single-tabbed

### Features

* Available on all 3 major platforms, Windows, Linux, OS/X.
* Font Ligatures Support (such as in Fira Code).
* GPU-acelerated rendering.

### Prerequisites Linux

This is tested on Ubuntu 19.04, but *any* recent Linux with latest C++17 compiler should do:

```!sh
apt install libfreetype6-dev libglew-dev libglfw3-dev libglm-dev libfontconfig1-dev libharfbuzz-dev
```

### Prerequisites Windows 10

For Windows, you must have Windows 10, 2018 Fall Creators Update, installed.
It will neither build nor run on any prior Windows OS, due to libterminal making use of [ConPTY API](https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/).

```!psh
vcpkg install freetype fontconfig glew glfw3 glm harfbuzz
```

### Prerequisites Mac OS/X

Here be lions.
