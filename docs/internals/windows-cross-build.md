# Building Windows executables on Linux

The `linux-to-windows-clangcl-*` presets build Windows x64 executables on a Linux host and run their tests
under [wine](https://www.winehq.org/). It is a development aid: a Windows-only code path can be compiled,
and a Windows-only failure looked at, without a Windows machine. Releases and the required CI check stay on
real Windows with MSVC (`msvc-ci`, `msvc-release`).

**Scope: everything, the GUI included.** With Qt for Windows available (below), `all` builds: every library,
every test suite, and `contour.exe` with the Qt/QML GUI, deployed beside its Qt runtime by windeployqt. Without
it, configure with `-DCONTOUR_FRONTEND_GUI=OFF` for the Qt-free targets and the headless `contour.exe`.

## Prerequisites

- **LLVM**: `clang-cl`, `lld-link`, `llvm-rc`, `llvm-mt` and `llvm-lib` on `PATH` (Fedora: `clang`,
  `lld`, `llvm`).
- **The MSVC headers and libraries and the Windows SDK**, laid out as
  [msvc-wine](https://github.com/mstorsjo/msvc-wine) installs them:
  ```sh
  git clone https://github.com/mstorsjo/msvc-wine
  cd msvc-wine
  ./vsdownload.py --dest /opt/msvc     # accepts Microsoft's license on the way
  ./install.sh /opt/msvc
  ```
  The toolchain looks for it in `MSVC_WINE_ROOT`, a CMake cache variable or an environment variable of
  that name, and falls back to `/opt/msvc`. It takes the newest MSVC toolset and SDK found there, unless
  `MSVC_WINE_TOOLSET_VERSION` / `MSVC_WINE_SDK_VERSION` name one. Only the headers, libraries and the
  redistributable runtime DLLs are used: the compiler is `clang-cl`, not `cl.exe` under wine.
- **Qt for Windows**, for the GUI: an official Qt MSVC 2022 x64 build, as the Qt online installer lays it out
  on Windows (`<version>/msvc2022_64`). Copy it from a Windows machine, or symlink it from a mounted
  Windows drive (read-only is enough; nothing is written into it):
  ```sh
  sudo mkdir -p /opt/qt-windows
  sudo ln -s /mnt/windows/Qt/6.11.1 /opt/qt-windows/6.11.1   # or: cp -a <Qt>/6.11.1 /opt/qt-windows/
  ```
  The toolchain looks for it in `QT_WINDOWS_ROOT`, a CMake cache variable or an environment variable of that
  name, and falls back to `/opt/qt-windows/6.11.1/msvc2022_64`. Qt's build tools in that tree (moc, rcc,
  qmlcachegen, qmltyperegistrar, qsb, ...) are Windows executables, so Qt runs the host's instead:
  **a Linux Qt of the same minor release** with its tool packages (Fedora: `qt6-qtbase-devel`,
  `qt6-qtdeclarative-devel`, `qt6-qtshadertools-devel`, `qt6-qttools-devel`), found through `QT_HOST_PATH`
  (default `/usr`) and `QT_HOST_PATH_CMAKE_DIR` (default `/usr/lib64/cmake` where that exists, else
  `/usr/lib/cmake`). A newer patch release on the host is accepted as it is: Qt's version check asks for at
  least the target's version, so a 6.11.2 host serves a 6.11.1 target with no override. A host Qt older than
  the target is refused (`-DQT_NO_PACKAGE_VERSION_CHECK=ON` would force it); a newer minor release passes the
  check but generates code for headers it was not made for. Keep the host at the target's minor release.
- **wine** (64-bit) to run what was built, and for the GUI to build at all: deploying Qt runs the Qt tree's
  `windeployqt.exe` under it. Without wine only `-DCONTOUR_FRONTEND_GUI=OFF` builds.
- **The C library dependencies**: `scripts/cross-windows-deps.sh` downloads Fedora's prebuilt `mingw64-*`
  packages of OpenSSL, libssh2, zlib, FreeType, HarfBuzz and cairo (and what they depend on) and lays them
  out in `out/windows-cross-deps`, where the presets look:
  ```sh
  scripts/cross-windows-deps.sh
  ```
  It needs `dnf`, `rpm2cpio` and `cpio`, and no root: the packages are only downloaded. These are C
  libraries, so MinGW-built DLLs serve MSVC-ABI code; `lld-link` reads their GNU import libraries as they
  are. The C++ dependencies (Catch2, yaml-cpp, GSL, glaze, libunicode, boxed-cpp, ...) are not taken from
  there: CPM fetches and builds them with `clang-cl`, as it does on any machine without them installed.

  The GUI needs no other library from there: of what CI's Windows job takes from vcpkg, cairo's fontconfig
  feature comes with the `mingw64-cairo` package's dependencies, and Qt is the official MSVC build, as
  CI's is (which pins 6.10 for now: aqtinstall cannot yet fetch 6.11 for Windows).

  This is the one place the cross build differs from CI's Windows job, which builds every dependency with
  vcpkg. vcpkg cannot cross-build OpenSSL from Linux (its Windows port drives `nmake` and `cmd.exe`), which
  is why prebuilt DLLs are used for the C libraries.

## Building and testing

```sh
scripts/cross-windows-deps.sh                                   # once
export WINEPREFIX=/tmp/my-wine-prefix WINEDEBUG=-all            # the build runs windeployqt.exe under wine
cmake --preset linux-to-windows-clangcl-release
cmake --build --preset linux-to-windows-clangcl-release
ctest --preset linux-to-windows-clangcl-release -R vthost_test
```

Building `contour.exe` runs windeployqt as Windows CI does, but as the Qt tree's own `windeployqt.exe` under
wine: it stages the Qt DLLs, plugins and QML modules beside `contour.exe` in the build tree's `bin/`. So the
build, too, needs wine and a wine prefix (the variables above), and it should not have `DISPLAY` pointing at
a desktop you care about.

`linux-to-windows-clangcl-debug` is the same in a Debug configuration (the debug C++ runtime,
`msvcp140d.dll` and `ucrtbased.dll`, comes from the msvc-wine install). The presets exist only on a Linux
host.

`ctest` runs each test binary through `CMAKE_CROSSCOMPILING_EMULATOR`: `<build>/wine-run.sh`, which the
toolchain writes. It runs wine with `WINEPATH` naming Microsoft's C++ runtime DLLs, the dependencies' `bin/`
and Qt's, and `WINEDLLOVERRIDES` preferring Microsoft's `msvcp140`/`vcruntime140` over wine's
reimplementations. It is a script rather than an `env WINEPATH=... wine` command list because `WINEPATH` is
`;`-separated, and a `;` does not survive a CMake list being expanded into a custom command. This applies to every test
registered as `add_test(NAME x COMMAND <target>)`: CMake applies the emulator only when the command names a
target, never to a `$<TARGET_FILE:...>` path.

To run a test binary by hand, run it through the same script, e.g.
`out/linux-to-windows-clangcl-release/wine-run.sh out/linux-to-windows-clangcl-release/bin/vthost_test.exe`.
Use a wine prefix of your own, so that nothing you do touches another one:

```sh
export WINEPREFIX=/tmp/my-wine-prefix WINEDEBUG=-all
```

Keeping a `wineserver -p` running in that prefix makes each start take milliseconds instead of seconds.

### Debugging under wine

- `WINEDEBUG=+winsock` (or `+thread`, `+file`, ...) traces what wine does underneath a call; `-all`
  silences it.
- `winedbg vthost_test.exe "<case>"` runs under wine's debugger; `winedbg --gdb` serves gdb instead.
  A hung test is attached to with `winedbg <wine pid>` (`info process` lists them), then `bt all`.
- The test binaries' own `CONTOUR_TEST_TRACE=1` prints each case and section as it starts, which locates a
  hang without a debugger.

## Running contour.exe under wine

The build tree's `bin/` already holds what windeployqt staged, and `wine-run.sh` puts the dependencies' DLLs
and the C++ runtime on `WINEPATH`. For a directory that carries everything itself, as an installed Contour
does, install it and add those two:

```sh
build=out/linux-to-windows-clangcl-release
cmake --install "$build" --prefix out/contour-windows
cp out/windows-cross-deps/bin/*.dll out/contour-windows/bin/                            # MinGW C libraries
cp /opt/msvc/vc/Redist/MSVC/*/x64/Microsoft.VC*.CRT/*.dll out/contour-windows/bin/      # C++ runtime
mv out/contour-windows/bin/conpty.dll out/contour-windows/bin/conpty.dll.off            # see below
```

Then, in a wine prefix and an X server of your own (Xvfb here; never a desktop you are using):

```sh
export WINEPREFIX=/tmp/my-wine-prefix WINEDEBUG=-all
Xvfb :73 -screen 0 1280x1024x24 -nolisten tcp &
export DISPLAY=:73
mkdir -p "$WINEPREFIX" && wineboot -i
export WINEDLLOVERRIDES="msvcp140,msvcp140_1,msvcp140_2,vcruntime140,vcruntime140_1=n,b"
run() { QSG_RHI_BACKEND=opengl wine out/contour-windows/bin/contour.exe terminal daemon-mode in_process cmd.exe; }

# A monospace font: the prefix has neither Cascadia Mono, the default, nor Consolas, the fallback. The first
# run writes the default configuration and exits for want of a font; name the one copied in, and run again.
cp /usr/share/fonts/liberation-mono-fonts/*.ttf "$WINEPREFIX/drive_c/windows/Fonts/"
run
sed -i 's/family: Cascadia Mono/family: Liberation Mono/' \
    "$WINEPREFIX/drive_c/users/$USER/AppData/Local/contour/contour.yml"
run
```

`xdotool type`/`key` drive it and `import -window root shot.png` captures it, as the e2e harness does.

What happens, measured with wine 11.20 (Staging, Fedora) on Xvfb, Qt 6.11.1:

| | Under wine |
|---|---|
| Starts | Yes. `contour.exe help`/`version` work, and the GUI loads its QML in ~170 ms. |
| Window | Yes, with Contour's own title bar, tab strip and status line. |
| Renders, OpenGL (`QSG_RHI_BACKEND=opengl`) | Yes: wine's `opengl32` on the host's Mesa (llvmpipe on Xvfb). |
| Renders, Direct3D 11 (`auto`, the default) | No: Fedora's wine takes `dxgi`/`d3d11` from DXVK, which has no `CreateSwapChainForComposition`; Qt retries, and DXVK then divides by zero inside `dxgi.dll` under Qt's swap-chain setup (a guess: Xvfb reports no refresh rate). wined3d, wine's own Direct3D, could not be swapped in beside the executable and is not measured. |
| Runs a shell | Yes, `cmd.exe`, through wine's own `CreatePseudoConsole`: typed input reaches it and its output renders. Only with the bundled `conpty.dll` moved away: the bundled ConPTY (OpenConsole) fails to start with HRESULT 0xD000000D (STATUS_INVALID_PARAMETER), and the session ends. The default profile's `powershell.exe` is not a working PowerShell in wine, so name the shell (`cmd.exe`). |
| Fonts | A fresh prefix has neither Cascadia Mono nor Consolas, and finding no font at all ends the process with an uncaught exception (exit code 3); copy a monospace font into the prefix. |

Test suites with the GUI build: `vtbackend_test` (3509 cases), `vtcontrol_test` (59) and `vtrasterizer_test`
(200) pass whole. `text_shaper_test` fails the 13 cases that ask DirectWrite for Windows' own fonts.
`vtpty_test` fails 31 of 169, the ConPTY cases among them. `contour_test` fails 8 of 1227, the PowerShell
integration cases, there being no `pwsh`. `contour_gui_test` aborts on
`Require(_display == &display)` in `TerminalSession::detachDisplay()` in the first case whose harness
detaches a display (some `[display]` cases and the WindowSwitcher's). Real Windows CI passes these, so read
it as a lead, not a finding, until it shows on Windows.

## Known differences from Windows

wine is not Windows, and a failure here is only a lead until it is reproduced on Windows. Known gaps:

- **AF_UNIX `AcceptEx` fails** (wine 11: the completion reports `WSAEINVAL`). core-cpp's IOCP listener
  accepts AF_UNIX connections with `AcceptEx`, so every case that serves a daemon over AF_UNIX
  (`[afunix]`) fails or takes a different path under wine. A plain `accept()` works.
- **Console and process semantics** differ: the ConPTY, console-mode and some `StdioChild` cases fail.
- **NTFS ACLs and `ssh.exe`** are not Windows': the file-privacy and real-ssh cases fail.
- Timing is not Windows' timing: a race that needs a Windows scheduler's pause may never show here.

Measured with wine 11.20: of `vthost_test`'s 1667 cases, 1652 pass and 12 fail, every failure one of the
gaps above (AF_UNIX accepts, the live-upgrade successor processes, `ssh.exe`, NTFS ACLs, the stdio bridge);
`crispy_test`, `vtparser_test`, `vtworkspace_test`, `vtconformance_test` and `testing_test` pass whole.

What does work: compiling and linking every target, the GUI included, with the Windows headers and `-Werror`
(which finds warnings MSVC does not report), running the platform-neutral cases of the suites, and running
the GUI with OpenGL (above).
