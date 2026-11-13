# Building Windows executables on Linux

The `linux-to-windows-clangcl-*` presets build Windows x64 executables on a Linux host and run their tests
under [wine](https://www.winehq.org/). It is a development aid: a Windows-only code path can be compiled,
and a Windows-only failure looked at, without a Windows machine. Releases and the required CI check stay on
real Windows with MSVC (`msvc-ci`, `msvc-release`).

**Scope: the Qt-free targets.** The presets set `CONTOUR_FRONTEND_GUI=OFF`; the GUI needs Qt for
Windows/MSVC, which is not provided this way, and is left for later. Of the Qt-free targets, these build and
are tested today: `vthost_test`, `crispy_test`, `vtparser_test`, `vtworkspace_test`, `vtconformance_test`
and `testing_test`. The rest stop on `clang-cl -Werror` findings in Windows-only code that MSVC does not
report: `text_shaper` (DirectWrite's COM overrides: `-Wmicrosoft-exception-spec`, unused parameters, and so
`vtrasterizer` and the headless `contour` too), `vtcontrol` (an `std::expected` comparison MSVC's library
does not instantiate for clang), `vtbackend_test` (a constant only POSIX uses) and `vtpty`'s
`conpty_handover_probe` (designated initializers that leave fields out). So build named targets rather than
`all`.

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
- **wine** (64-bit) to run what was built. Without it the build still works; the tests just cannot run.
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

  This is the one place the cross build differs from CI's Windows job, which builds every dependency with
  vcpkg. vcpkg cannot cross-build OpenSSL from Linux (its Windows port drives `nmake` and `cmd.exe`), which
  is why prebuilt DLLs are used for the C libraries.

## Building and testing

```sh
scripts/cross-windows-deps.sh                                   # once
cmake --preset linux-to-windows-clangcl-release
cmake --build --preset linux-to-windows-clangcl-release --target vthost_test
ctest --preset linux-to-windows-clangcl-release -R vthost_test
```

`linux-to-windows-clangcl-debug` is the same in a Debug configuration (the debug C++ runtime,
`msvcp140d.dll` and `ucrtbased.dll`, comes from the msvc-wine install). The presets exist only on a Linux
host.

`ctest` runs each test binary through `CMAKE_CROSSCOMPILING_EMULATOR`, which the toolchain sets to wine with
`WINEPATH` naming Microsoft's C++ runtime DLLs and the dependencies' `bin/`, and `WINEDLLOVERRIDES`
preferring Microsoft's `msvcp140`/`vcruntime140` over wine's reimplementations. This applies to every test
registered as `add_test(NAME x COMMAND <target>)`: CMake applies the emulator only when the command names a
target, never to a `$<TARGET_FILE:...>` path.

To run a test binary by hand, give it the same environment; the command `ctest` runs is in
`ctest --preset ... -R vthost_test -V`. Use a wine prefix of your own, so that nothing you do touches
another one:

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

What does work: compiling and linking every Qt-free target with the Windows headers and `-Werror` (which
finds warnings MSVC does not report), and running the platform-neutral cases of the suites.
