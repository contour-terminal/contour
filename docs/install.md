 Installation

`contour` is packaged and available for installation on multiple distributions.

## Windows 10 or newer

Please download Contour for Windows (the `.msi` file) from our [release page](https://github.com/contour-terminal/contour/releases/latest/), and double click on it to install.

## macOS

```sh
brew install contour
```

## Fedora Linux

Use the official [package](https://packages.fedoraproject.org/pkgs/contour-terminal/contour-terminal/)

```sh
sudo dnf install contour-terminal
```

## Arch Linux

Use the official [package](https://archlinux.org/packages/extra/x86_64/contour/)

```sh
sudo pacman -S contour
```

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


## Contour daemon on a server

`contour-daemon` is the Qt-free package for machines that only host [persistent sessions](persistent-sessions.md)
and have no display, such as a server you reach over SSH. It ships the `contour` command-line tool
without the GUI stack (no Qt, no QML; the text shaper's Cairo dependency still pulls in the X11 client libraries). Packages are attached to each
[release](https://github.com/contour-terminal/contour/releases) (they are not served from a package repository):

- `.deb` for Ubuntu 24.04 and 26.04 (amd64), named `contour-daemon-<version>-ubuntu<release>-amd64.deb`
- `.rpm` for Fedora 44 (x86_64), named `contour-daemon-<version>-fedora44-x86_64.rpm`

```sh
sudo apt install ./contour-daemon-[0-9]*-ubuntu24.04-amd64.deb   # Ubuntu 24.04 (use ubuntu26.04 on 26.04)
sudo dnf install ./contour-daemon-[0-9]*-fedora44-x86_64.rpm     # Fedora 44
```

CentOS and RHEL (EL9, EL10) packages are not provided yet.
The release also carries `-debuginfo`/`-debugsource` RPMs and a `-dbgsym` `.ddeb`, which the patterns above skip.
The RPM packages come without the bundled status-line extensions; the `.deb` ships them.

`contour-daemon` and the GUI package `contour` conflict, and `contour` already includes the daemon, so a desktop
installation needs nothing more. On Debian and Ubuntu, `apt` replaces one with the other; on Fedora, `dnf install`
refuses, so swap them with `dnf swap contour contour-daemon` (or `dnf install --allowerasing`).

The package installs a systemd user unit but does not enable it. Each user registers and enables their own copy (it then starts with their next login; `daemon-service start` starts it now):

```sh
contour daemon-service install
```

See [Starting the daemon at login](persistent-sessions.md#starting-the-daemon-at-login). The packaged service opens no
listening port (TCP is opt-in, see [Remote access over TCP](persistent-sessions.md#remote-access-over-tcp)); reach it over SSH, see [Remote hosts](remote-hosts.md).

## Installing from source

Contour is best installed from supported package managers, but you can build
from source by following the instruction below.

### UNIX-like systems (Linux, FreeBSD, macOS)

#### Prerequisites

```sh
./scripts/install-deps.sh
```

This script *might* ask you for the administrator password if a package dependency
can be installed via the system package manager.

#### Compile

You can use cmake presets to compile contour. The full list of available presets can be seen using `cmake --list-presets`. To compile release build for linux `clang-release` or `gcc-release`, or `appleclang-release` on macOS accordingly.

```sh
cmake --preset clang-release
cmake --build --preset clang-release

# Optionally, if you want to install from source
cmake --build --preset clang-release --target install
```

Please note, if you want to install into a system root, e.g. `/usr/local`, you may need to prefix
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

