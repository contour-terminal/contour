 Installation

`contour` is packaged and available for installation on multiple distributions.

## Windows 10 or newer

Please download Contour for Windows from our [release page](https://github.com/contour-terminal/contour/releases/latest/), and double click the installer (an `.msi` file) to install. There are two installers, and you need one of them:

| Installer | Installs for | Default folder | Needs |
| --- | --- | --- | --- |
| `contour-<version>-win64.msi` | everyone on the computer | `C:\Program Files\Contour\` | an administrator's approval |
| `contour-<version>-win64-user.msi` | you only | `%LOCALAPPDATA%\Programs\Contour\` | nothing |

Pick the per-user installer when you cannot or do not want to elevate. Both add Contour to the Start menu
and, unless you deselect their features in the installer, to your `PATH` and your desktop. There is also a `contour-<version>-win64.zip`
that needs no installation at all: unpack it anywhere and run `bin\contour.exe`.

- **The two installers are separate products.** Both can be installed on one computer, and neither ever
  upgrades or removes the other. When a command name is found in both, the per-machine `PATH` comes first.
- **An installer upgrades in place.** Run any later build over an earlier one, or over a release, without
  uninstalling first; an older build of the same release also installs over a newer one. An upgrade keeps the
  folder Contour is in, even a folder you chose; the one exception is an old release's folder with its version
  in the name (`Contour Terminal Emulator 0.8`), which moves to a folder named `Contour` once. Installations
  of Contour 0.6.1 and earlier have to be uninstalled first.
- **A running Contour is closed and opened again.** Windows Installer closes the Contour window it finds
  running and starts it again, with the arguments it had, once the files are replaced. A window whose
  sessions run in its own process (`daemon.mode: in_process`, or the fallback when no daemon could be
  started) loses them. Sessions a daemon hosts keep running: the installer moves a daemon started by hand out
  of the way first, and does not end one. What it leaves is described in
  [Installing a new Contour on Windows](persistent-sessions.md#installing-a-new-contour-on-windows).

### Unattended installation

Run an installer without a dialog from a command prompt or a deployment tool:

```sh
msiexec /i contour-<version>-win64.msi /qn /norestart /l*v contour-install.log
```

Three properties change what it installs. Give them on the command line after the file name
(`INSTALL_ROOT` is older than the other two):

| Property | Default | Effect |
| --- | --- | --- |
| `INSTALL_ROOT=<folder>` | the installer's default folder (table above); on an upgrade, the folder Contour is in | where Contour goes. A folder given here wins over the installed one, which moves the installation. |
| `ADD_TO_PATH=0` | `1` | does not add Contour's `bin` folder to `PATH` |
| `DESKTOP_SHORTCUT=0` | `1` | does not put a shortcut on the desktop |

On an upgrade, `ADD_TO_PATH=0` and `DESKTOP_SHORTCUT=0` remove the entry or the shortcut the earlier
installation made, and a later install with the default does not bring it back: name the feature again with
`ADDLOCAL=CM_C_contour_path` (the `PATH` entry) or `ADDLOCAL=ContourDesktopShortcutFeature` (the shortcut).

`msiexec` reports through its exit code: `0` success; `3010` success, but a restart is needed; `1602` the
installation was cancelled; `1603` a fatal error (also what an installer says when a later version of
Contour is already installed: "A later version of Contour is already installed"); `1618` another installation
is already running; `1638` another build of the same product is already installed with the same product code: this
happens only between builds made before Contour 0.9, which all builds of one release shared; uninstall it
first. Builds of 0.9 and later each have a product code of their own, and upgrade each other. The log named by `/l*v` has the detail.

With the Windows Package Manager:

```sh
winget install --id contour-terminal.Contour --silent --scope machine
winget install --id contour-terminal.Contour --silent --scope user
```

`--scope machine` installs the per-machine installer, `--scope user` the per-user one.

!!! note "Installing as SYSTEM"
    A deployment tool that installs as the SYSTEM account cannot reach your daemon. Contour is still
    installed, but a daemon that you started by hand keeps running from the old folder until it next
    restarts. See [Installing a new Contour on Windows](persistent-sessions.md#installing-a-new-contour-on-windows).

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

## AppImage

Every release also carries `contour-VERSION-x86_64.AppImage`, a single file that bundles Qt 6.11 (which
Debian and Ubuntu do not ship) and everything else Contour needs but the C library. Make it executable
and run it:

```sh
chmod +x contour-VERSION-x86_64.AppImage
./contour-VERSION-x86_64.AppImage
```

It runs on distributions with a glibc at least as new as Ubuntu 24.04's. Without FUSE, run it with
`--appimage-extract-and-run`.

To build it yourself, on the oldest distribution you want it to run on, install the build dependencies
(`scripts/install-deps.sh`), Qt 6.11 with the `qtmultimedia`, `qt5compat`, `qtshadertools` and `qtspeech`
modules (for instance through [aqtinstall](https://github.com/miurahr/aqtinstall)), a C++23 compiler and
`scripts/install-rust-wasm.sh`, then:

```sh
QT_ROOT_DIR=/path/to/Qt/6.11.1/gcc_64 CXX=g++-15 VERSION_STRING=dev ./scripts/build-appimage.sh
```

The script builds Contour, installs it into an `AppDir` and packs it with
[linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) and its Qt plugin, which scans the QML files
for the modules to bundle. The result is `contour-dev-x86_64.AppImage` in the current directory.

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

