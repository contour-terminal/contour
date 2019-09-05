# glterm - OpenGL terminal emulator

### Purpose

**glterm** is a purely minimal OpenGL based terminal emulator, based on libterminal.
It purely serves as example on how to hack a terminal with libterminal, and thus will be:

* single-windowed
* single-tabbed
* zero-configuration (except command line flags)

One may compare it to Suckless Terminal [ST](https://st.suckless.org/).

### Why OpenGL?

Because it's platform independant, and **glterm** is meant to build and run on Linux, Windows, Mac OS/X with
the least platform dependant code as possible.

### Prerequisites Linux

This is tested on Ubuntu 19.04, but *any* Linux should do:

```!sh
apt install libfreetype6-dev libglew-dev libglfw3-dev libglm-dev libfontconfig1-dev
```

### Prerequisites Windows 10

For Windows, you must have Windows 10, 2018 Fall Creators Update.
It will neither build nor run on any prior Windows OS, due to libterminal making use of [ConPTY API](https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/).

```!psh
vcpkg install freetype fontconfig glew glfw3 glm
```

### Prerequisites Mac OS/X

Here be lions.
