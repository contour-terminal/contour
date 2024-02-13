 Installation

`contour` is packaged and available for installation on multiple distributions.

## Windows 10 or newer

Please download Contour for Windows (the `.msi` file) from our [release page](https://github.com/contour-terminal/contour/releases/latest/), and double click on it to install.

## Mac OS/X

```sh
brew install contour
```

## Fedora Linux

Use the official [package](https://packages.fedoraproject.org/pkgs/contour-terminal/contour-terminal/)

```sh
sudo dnf install contour-terminal
```

## Arch Linux

Please use the AUR, at [https://aur.archlinux.org/packages/contour-git](https://aur.archlinux.org/packages/contour).

## Ubuntu Linux

Please download Contour for Ubuntu Linux (the `.deb` files) from our official [release](https://github.com/contour-terminal/contour/releases) page,
and then use the following command to install:

```sh
sudo dpkg -i ~/Downloads/contour-0.3.12.262-UBUNTU_VERSION-ARCH.deb
```

If you want to provide feedback in case of any crashes, also install the debug symbols (`.ddeb`-file), e.g.:

```sh
sudo dpkg -i ~/Downloads/contour-0.3.12.262-UBUNTU_VERSION-ARCH.ddeb
```

## Flatpak

Contour is available at the Flathub store.

[![Get it on Flathub](https://raw.githubusercontent.com/flatpak-design-team/flathub-mockups/master/assets/download-button/download.svg?sanitize=true)](https://flathub.org/apps/details/org.contourterminal.Contour)


## Installing from source

Contour is best installed from supported package managers, but you can build
from source by following the instruction below. You can Qt 5 or Qt 6,
by default contour will be compiler with Qt 6, to change Qt version use `QTVER=5 ./scripts/install-deps.sh` to fetch dependencies and cmake flag `-D CONTOUR_QT_VERSION=5`.

### UNIX-like systems (Linux, FreeBSD, OS/X)

#### Prerequisites

```sh
./scripts/install-deps.sh
```

This script *might* ask you for the administrator password if a package dependency
can be insalled via the system package manager.

#### Compile

You can use cmake presets to compile contour. The full list of available presets can be seen using `cmake --list-presets`. To compile release build for linux or MacOs use `linux-release` or `macos-release` accordingly. FreeBSD users can use `linux-release` or configure cmake manually.

```sh
cmake --preset linux-release 
cmake --build --preset linux-release

# Optionally, if you want to install from source
cmake --build --preset linux-release --target install
```

Please mind, if you want to install into a system root, e.g. `/usr/local`, you may need to prefix
the install command with `sudo`.

Also, ensure that the terminfo file is correctly resolved, as the terminfo library
is very limited in locating  the correct terminfo files (e.g. it does not search in `/usr/local`),
you can symlink into `~/.terminfo` however.

### Windows 10 or newer

#### Prerequisites

For Windows, you must have Windows 10, 2018 Fall Creators Update, and Visual Studio 2019, installed.
It will neither build nor run on any prior Windows OS, due to libterminal making use of [ConPTY API](https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/).

1. Set up [vcpkg](https://vcpkg.io/en/getting-started.html), preferably somewhere high up in the folder hierarchy, and add the folder to your `PATH`.

```
cd C:\
git clone git clone https://github.com/Microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
```

2. Install Visual Studio Build Tools (make sure to select the CLI tools for
   C++, which you might need to do in the separate components tab).
3. Install Qt6 (i.e. to C:\Qt)
4. Open the _developer_ version of Powershell.
5. In the `contour` source folder execute `.\scripts\install-deps.ps1`. This step may take a _very_ long time.


#### Compile

In the _developer_ version of Powershell:

```psh
# change paths accordingly if you installed QT and vcpkg to somewhere else
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DCMAKE_PREFIX_PATH=C:\Qt\6.5.0\msvc2019_64\lib\cmake
cmake --build build/

# Optionally, if you want to install from source
cmake --build build/ --target install
```

