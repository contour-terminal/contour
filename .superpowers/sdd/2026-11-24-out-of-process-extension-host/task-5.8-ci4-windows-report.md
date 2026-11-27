# 5.8 CI round 4 (Windows): report

**Base:** 42aeb492b. **Head:** 5446a7855. Not pushed.

## Commits

| Commit | What |
|---|---|
| `773748ec9` (was 42aeb492b) | feat(vtextension): an extension host the loader ends is logged with why — **item 3 folded in** |
| `5446a7855` (new) | fix(vtextension): an extension host is started with no console, and the loader probe says what it may load |

The tree after the autosquash is identical to the tree before it.

## What the CI log refutes, and what is left

The facts from run 45790815170:

- The runner's window station is WinSta0, so the service-window-station premise is gone.
- The contained probe dies with 0xC0000142 before main, writing nothing.
- The probe no longer imports ole32 or userenv at all.
- The same probe, with the same environment and job, starts uncontained.

What differs between the two runs is only what the container changes:

- the token;
- the working directory: the executable's own directory instead of the volume root;
- the environment's three folders;
- the launch flags. Until now both runs used `CREATE_NO_WINDOW`, which gives the child **a console of its
  own**. The system serves that console with a console host. The console client, the child, connects to it
  in kernelbase's process initialisation.

**The leading hypothesis, UNVERIFIED.** Since Windows 8, the console host (conhost.exe) is started as a
child of the console client. Only the contained launch sets `PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`
(Process_win32.cpp). So a contained client given a console of its own may fail its console setup at startup,
and only when contained.

Nothing here verifies that, and wine cannot. A green CI run would prove that `DETACHED_PROCESS` works. It
would not prove why `CREATE_NO_WINDOW` failed.

**The remaining candidates.** The brief named them, and the diagnostic below checks each:

- **A library from the build directory the container cannot read.** The launcher already grants ALL
  APPLICATION PACKAGES read and execute on the executable's directory, and that grant reaches the files
  directly in it. An unreadable library would also give 0xC0000135 or 0xC0000022 rather than 0xC0000142.
- **A library whose initialisation fails under the container token.** For example, the C runtime reading the
  profile or registry.

## Item 1: a diagnostic that names the cause (in `5446a7855`)

**`extension_host_loader_probe`** (`process/testing/LoaderProbe.cpp`, test-only, never installed):

- Its C runtime is linked statically (`MSVC_RUNTIME_LIBRARY MultiThreaded[Debug]`), so it imports
  **kernel32 alone**. `llvm-readobj --coff-imports` shows only KERNEL32.dll, and a test asserts it.
- It writes one line each to its stderr, which is the handle the launch gives it:
  - `started`;
  - `console=ok|error:<code>`, from `AllocConsole()`, freed again at once. This says directly whether a
    contained process may get a console;
  - `library=<path> ok|error:<code>` for each argument, loaded with `LoadLibraryExW` and kept loaded;
  - `end`.
- Its entry point is `main()`. Its wide arguments are parsed from `GetCommandLineW()`, because
  `CommandLineToArgvW` lives in shell32 and the probe must not import it. clang-tidy's internal-linkage check
  rejects `wmain`.

**The import reading** moved to `process/testing/PeImports_win32.{hpp,cpp}`:

- `startupImportsOf(file)` is the reader that was in StartupImports_test.
- New: `importClosureOf(executable)` walks the imports depth-first. Each library is looked up beside the
  executable, then in the system directory, as the loader does for a library not already loaded. API sets
  are skipped, because they are not files. Each library is listed after what it imports, which is the
  loader's initialisation order. Imports found nowhere are listed as `unresolved`.
- The `_win32` suffix keeps the Linux clang-tidy plan from failing on a header no Linux translation unit
  reaches.

**The tests** (`process/LoaderProbe_test.cpp`; Windows):

- `LoaderProbe.importsKernel32Only` asserts that the loader probe imports `kernel32.dll` and nothing else.
- `LoaderProbe.containedProcessLoadsWhatItsExecutableImports` runs the data-driven `LoaderRuns` table:

  | Run | Isolation | Loads |
  |---|---|---|
  | uncontained, nothing | none | nothing |
  | uncontained, an executable's libraries | none | the import closure |
  | contained, nothing | AppContainer | nothing |
  | contained, an executable's libraries | AppContainer | the import closure |

  - Every run goes through `makeWin32ProcessLauncher(systemAppContainerApi(), …)` with
    `defaultHelperEnvironment()` and a captured stderr. That is the production launch path, including the
    grant, the job, the environment and the flags.
  - An "an executable's libraries" run covers each entry of `ContainedExecutables`: the probe always, and in
    the helper suites the extension host too (`VTEXTENSION_HELPER_PATH`). `contour-extension-host-wamr` is
    covered through `vtextension_wamr_helper_test`.
  - The test **WARNs the whole report on every run**, pass or fail, so the CI log shows each library's result
    and the console line.
  - It CHECKs a clean exit, `started`, a `console=` line, `library=<path> ok` for each library (a failure
    names the path), `end`, and an empty `unresolved`.
  - Contained runs are skipped under wine (`continue`, not `SKIP`, since the CI verify step treats a reported
    skip as a failure). Wine stubs AppContainers.
- Compiled into `vtextension_process_test` (probe row) and into both helper suites (probe and host rows).
  - All three depend on `extension_host_loader_probe` and get `VTEXTENSION_LOADER_PROBE_PATH`.
  - CI's `[appcontainer]` verify step runs `vtextension_process_test` and `vtextension_wamr_helper_test`, so
    both will print the contained loader report.

**How the next CI run will read:**

- *"contained, nothing" fails to start, or ends 0xC0000142.* The container itself kills a kernel32-only
  process, so no library is at fault. The cause is process-level: console or token.
- *"contained, nothing" passes and says `console=error:…`.* A contained process may not create a console.
  That confirms the console hypothesis, together with the fix below.
- *A `library=… error:1114|5|126` line.* It names the culprit and the kind of failure.

## Item 2 (and the fix applied in the same push): no console for the extension host

`Process_win32.cpp` now starts every child with `DETACHED_PROCESS` instead of `CREATE_NO_WINDOW`. The file
head and `ProcessLauncher.hpp`'s overview say so.

Why this is right independently of CI:

- The extension host reads and writes only the handles it is given. Its stdin and stdout are the null device
  and its stderr is given explicitly (`STARTF_USESTDHANDLES`). It has no use for a console.
- A console of its own costs one console host process per extension host.
- `DETACHED_PROCESS` keeps both properties `CREATE_NO_WINDOW` was chosen for: the child is attached to no
  console of Contour's, and it receives none of its Ctrl+C events.

I applied the fix before CI confirmed it, as a deliberate trade. Each guess costs a CI round, and this push
carries the diagnostic that judges it:

- If CI passes, the contained loader probe's `console=` line says whether a contained process could have had
  a console at all.
- If CI still fails, the loader report names the library.

**The build-directory hypothesis.** CI's vcpkg triplet is `x64-windows` (dynamic), and DLLs are staged into
`bin`: Qt and vcpkg DLLs for `contour_gui_test`, and windeployqt for contour, which may place the VC runtime
there too. The probe and the extension host import only system libraries and the C/C++ runtime. Locally the
debug runtime sits app-local in `bin`, and the loader probe loaded it.

So the only non-system libraries the contained processes can load from `bin` are the runtime DLLs. The
contained launch grants ALL APPLICATION PACKAGES read and execute on that directory, and its files inherit
the grant. If that grant did not reach them, the loader report would show `error:5`.

**Install layouts.** I have not verified them on real Windows:

- Program Files is readable by ALL APPLICATION PACKAGES.
- A portable or zip layout is covered by the same grant at provision time.
- Where this user cannot grant the directory, the launcher already logs that the installer must grant it.

## Item 3: truthful exit texts (in `773748ec9`)

`KnownExitCodes` now states meanings only:

- 0xC0000022: "access was denied (STATUS_ACCESS_DENIED)";
- 0xC0000135: "a library it needs was not found (STATUS_DLL_NOT_FOUND)";
- 0xC0000142: "a library it loads failed to initialise (STATUS_DLL_INIT_FAILED)".

The test row is updated.

The startup-import guard's rationale (StartupImports_test) is reworded too, in `5446a7855` (now
`b8f54327d`), not in `773748ec9`. It no longer blames the CI failure on a window station. It says only what is documented: a service's window station admits no app
container, and the extension host needs none of those libraries.

## Tests

- **Windows cross build** (`out/clangcl-debug`): `vtextension_process_test`, `extension_host_probe`,
  `extension_host_loader_probe`, `vtextension_wamr_helper_test` and `contour-extension-host`. Exit 0, no
  warnings. The loader probe's only import is KERNEL32.dll.
- **Under wine 11** (own prefix):

  | Run | Result |
  |---|---|
  | `vtextension_process_test` | 128 cases, 125 passed, 3 skipped, 1703 assertions |
  | `vtextension_process_test "LoaderProbe.*"` | passed |
  | `vtextension_wamr_helper_test "~[.measure]"` | 52 cases, 51 passed, 1 skipped, 2052 assertions |
  | `vtextension_wamr_helper_test "[appcontainer]"` | 5 cases, 1441 assertions, all passed |

  Under wine, the uncontained loader runs resolved and loaded ntdll, kernelbase, kernel32, ucrtbase, ws2_32,
  msvcrt, sechost, advapi32 and the app-local debug runtime: all `ok`, `console=ok`.
- **Linux ASan:** `ctest -R vtextension` passed 7 of 7, `-L lint` 35 of 35, warning-free.
- **clang-tidy diff:** Windows tree 7 TUs clean, Linux tree 2 TUs clean. Two findings were fixed: `wmain`
  internal linkage, and the Linux plan failing on a header no Linux translation unit reaches.
- **Format:** clang-format 22 and editorconfig-checker are clean.

## Concerns

- **The console hypothesis is unconfirmed.** It rests on Microsoft's documented 0xC0000142 for a failed
  console connection, and on elimination. The diagnostic in this push confirms or refutes it in one CI run,
  and names the library if it is something else.
- **Should CI pass, the diagnostic should stay.** `LoaderProbe.*` costs a few hundred milliseconds and is the
  first thing to look at if a contained start ever fails again.
- **Install layouts** (Program Files, portable) are reasoned about, not run. Only the CI build directory will
  have been exercised.


## Fix round 1 (review of 5446a7855)

All fixes are amended into `5446a7855`, which is now **`b8f54327d`**; its message is reworded to match. It is
still the head commit, so the commits below it are unchanged.

### Important

1. **API sets are kept in the closure** (`PeImports_win32.cpp`). Each `api-ms-win-*`/`ext-ms-win-*` import is
   listed by its bare name, where it is first imported, in dependency order, so before the library importing
   it. The loader probe loads it by name, and `LoadLibraryExW` resolves it through the API set schema.
   - So on real Windows, an init failure in ucrtbase shows on the `api-ms-win-crt-*` line, before
     `vcruntime140.dll`'s.
   - For a library named without a directory, the probe also writes `resolved=<name> <file>`: the file the
     loader mapped it to, from `GetModuleFileNameW`.
   - Under wine the API sets resolve to kernelbase, kernel32 and so on, all `ok`, so the test stays
     meaningful.
2. **The report handle is read once** (`LoaderProbe.cpp`). `GetStdHandle(STD_ERROR_HANDLE)` is read at
   start, before `AllocConsole()` can repoint the standard handles, and every line goes to that handle.
   - The console is freed at once. On a desktop run a console window may flash; the file head says so.
   - `AllocConsoleWithOptions(ALLOC_CONSOLE_MODE_NO_WINDOW)` would avoid that. It is declared only under
     `NTDDI_WIN11_GE` and exists only on recent Windows 11, so I did not use it.

### Minor

3. **The rationale, honestly stated.** This report now gives the UNVERIFIED conhost-as-child versus
   `PROCESS_CREATION_CHILD_PROCESS_RESTRICTED` hypothesis, and that a green run proves only that
   `DETACHED_PROCESS` works. The pseudoconsole-page rationale is gone. Process_win32.cpp carries the same
   rationale as a comment just before the `CreateProcessW` call.
5. **`make_preferred()`.** It is applied to the executable's directory and to the system directory, so paths
   beside the executable use backslashes. Under wine, `…/bin\ucrtbased.dll` no longer mixes separators.
6. **KnownDLLs.** The list is **read from the registry**:
   `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs`, through `RegOpenKeyExW` and
   `RegEnumValueW`, REG_SZ values, lower-cased. A listed name is resolved only from System32; any other is
   looked up beside the executable first, then in System32. Where the key cannot be read, the list is empty.
7. `CHECK(!closure.libraries.empty())` is added.
8. **A readable failure.** The `library=` assertion compares
   `afterPrefix(…).value_or("(no line)") == "ok"`, so a failure prints the actual `error:<code>`, or
   `(no line)`, rather than `{?}`.
9. **Credit.** The StartupImports rationale rewording is credited to `5446a7855`/`b8f54327d` above, not to
   `773748ec9`.

### clang-tidy findings fixed on the way

- `fileOf` moved into the anonymous namespace (misc-use-internal-linkage).
- The KnownDLLs key is now a null-terminated `wchar_t const*` instead of a `wstring_view` passed by `.data()`
  (bugprone-suspicious-stringview-data-usage).

### Tests

- **Windows cross build:** `vtextension_process_test`, `extension_host_probe`, `extension_host_loader_probe`,
  `vtextension_wamr_helper_test` and `contour-extension-host`. Exit 0, no warnings. The loader probe still
  imports KERNEL32.dll alone.
- **Under wine:**

  | Run | Result |
  |---|---|
  | `vtextension_process_test` | 128 cases, 125 passed, 3 skipped, 1733 assertions |
  | `vtextension_process_test "[appcontainer]"` | 6 cases, 5 passed, 1 skipped |
  | `vtextension_wamr_helper_test "[appcontainer]"` | 5 cases, 1502 assertions, all passed |

  The uncontained loader report now lists the API sets with their resolved files.
- **clang-tidy diff:** Windows tree 5 TUs clean. The Linux tree has no changed first-party lines this round.
- **Format:** clang-format 22 and editorconfig-checker are clean.
- **Linux ASan:** not rebuilt. Every file touched this round is Windows-only or Windows-guarded.

### Concerns

- **The cause is still unverified.** The next Windows CI run carries the diagnostic and the
  `DETACHED_PROCESS` fix.
- **API-set lines name the contract, not its host.** When an API set fails to load, its line names the API
  set; the library behind it appears only as `resolved=` on success. On failure, the CI log shows which
  contract failed and that it came before the libraries importing it. The resolved host must then be
  inferred: `api-ms-win-crt-*` maps to ucrtbase.

## Follow-up after the re-review

All three items are amended into the head commit, which is now `ce9062c11` (was `b8f54327d`).

1. **A missing extension API set no longer fails the test.** For an `ext-ms-win-*` name only, error 126
   (ERROR_MOD_NOT_FOUND) is reported as a WARN ("not on this system, which a process starts without").
   Any other error on an extension set still fails, and so does every error on an `api-ms-win-*` set or a
   real DLL. The prefix is the `ExtensionApiSetPrefix` constant.
2. **The KnownDLLs read is visible.** `knownDlls()` is now public in `PeImports_win32.hpp`. A new case,
   `LoaderProbe.knownDllsAreRead`, WARNs how many names it read, and outside wine (`runsOnWine()`) CHECKs that
   the list is not empty. Under wine it read 34.
3. **The registry key is RAII.** The key is held by a `std::unique_ptr<HKEY__, RegistryKeyClose>`; the manual
   `RegCloseKey` is gone.

**Tests:**

- The Windows cross build is clean, with no warnings.
- Under wine:
  - `vtextension_process_test "[appcontainer]"`: 7 cases, 6 passed, 1 skipped.
  - `vtextension_wamr_helper_test "[appcontainer]"`: 6 cases, 1502 assertions, all passed.
- clang-tidy diff (Windows tree, 3 TUs), clang-format 22 and editorconfig-checker are clean.
