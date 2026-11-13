# Sanitizers and leak checking

The default Linux preset, `clang-asan`, builds with `CONTOUR_SANITIZE=address`: AddressSanitizer and
UndefinedBehaviorSanitizer, and with them LeakSanitizer, which reports at a process's exit every block
nothing points to any more. `clang-tsan` builds with ThreadSanitizer instead; the two cannot be combined.

## Leaks that are not ours

A Qt application leaves blocks behind that belong to system libraries: the speech-dispatcher client, XKB's
keymaps and compose tables, the D-Bus connection, fontconfig and FreeType, and the GL drivers. Most are
freed, if at all, by the libraries' own teardown, which runs after LeakSanitizer has reported. Without a
filter, closing a debug `contour client` prints a report of some 180 KB that hides any leak of our own.

`cmake/lsan-suppressions.txt` is the one list of what is filtered, each entry with the reason. An entry
names a system library (`leak:libdbus-1.so`): a leak is suppressed when any frame of the stack that
allocated it lies there. Two entries name a Qt function instead, where Qt itself leaks a per-thread object
or where the frames above it belong to a GL driver that was unloaded before the report, and so have no name
to match. No entry names a frame of Contour's own code; that would hide our leaks along with theirs.

The list is applied in two ways, from the same file:

- **Built in.** In an AddressSanitizer build, `cmake/LeakSanitizerSuppressions.cmake` turns the file into
  the string `__lsan_default_suppressions()` returns, and builds that function into every executable of
  the project. The sanitizer runtime asks for it at start-up, so a plain run of `contour` or of a test
  suite applies the list with no environment at all. An edit of the file rebuilds that one C file and
  relinks.
- **At run time.** `LSAN_OPTIONS=suppressions=cmake/lsan-suppressions.txt` applies the same file to a
  binary built without it, or adds to the built-in list.

A run's report ends with "Suppressions used", the entries that matched and how much each hid.

## Where leak checking runs

Every unit suite, the offscreen application runs (`ctest -L e2e`) and the X server workflows
(`ctest -L e2e-display`) run with leak checking. A leak in our code fails the test that leaked, after the
suite itself has passed. `contour_e2e_display_ctrl_d_quits_clean` closes a daemon client the way a user does,
with Ctrl+D in its only shell, and fails on any report the client prints as it exits.

Two routes still turn it off, each saying why where it does:

- The four `contour_gui_test` runs (`src/contour/CMakeLists.txt`). Their pixel checks leave indirect leaks
  inside Qt Quick's grab paths (`QQuickItem::grabToImage()`, the software renderer under
  `QQuickWindow::grabWindow()`), with no direct leak behind them.
- `scripts/run-display-tests.sh`, the real-compositor route, for what leaks under Weston's EGL stack. A
  caller's `LSAN_OPTIONS` wins.

## Adding an entry

When a report names only system libraries:

1. Find the outermost frame in a system library, not Qt's own code where you can avoid it, and add
   `leak:<library file name>` with a comment saying what the library keeps and why it is not freed.
2. Run the report again to see the entry under "Suppressions used".
3. Check that a deliberate leak of our own is still reported: allocate and drop a block in a test case, run
   it, and see the report name the test's frame.

A frame of `<unknown module>` belongs to a library that was unloaded before the report; no entry can name
it. Name the nearest frame that has a name, as the `QRhi::endFrame` entry does, and say so in its comment.
