# 5.8 CI round 2: review round 1 report

The base was b5732de14. The new head is **785859fbf**. Nothing is pushed: the branch was rebased (see the
history section below), so it will need a force-push.

## History

The last four commits, oldest first:

- `32946a759 test(vtextension): one wine check for every test binary` (new commit: minor 1)
- `df749af5a fix(vtextension): a contained extension host's environment names its container's own folders`
  (was 7842a084d; item 1 is folded in and the message is rewritten)
- `785859fbf fix(vtextension): an instance refused for its answer's size is gone before the refusal is sent`
  (unchanged)

The wine migration is no longer in `bcb5c5142` (was 77695a0c7). I restored that commit's nine test files
from the original 598181d55 and dropped Wine.hpp from it. Its other content is unchanged:

- the ApplyPatchOnceTest fix;
- the original 598181d55 test changes, which add WasmtimeEngine_test's own wine copy.

The migration is now its own `test(vtextension)` commit, placed right after f987e9194. That is where the
migration's patch was cut and where all five copies exist, and it comes before anything else uses the
header.

The tree before the round-1 work was byte-identical to b5732de14 after this rebase. The round-1 fixup was
then folded into df749af5a, and the result is byte-identical to the fixup head.

## 1. Contained environment: the container's own folders (Important)

### Docs

- **GetAppContainerFolderPath** (userenv.h) returns "the path of the local app data folder for the
  specified app container". The SID is passed as a string, so the code spells it with
  ConvertSidToStringSidW. The caller frees the result with CoTaskMemFree.
- **"Launch an AppContainer"** (learn.microsoft.com/windows/win32/secauthz/implementing-an-appcontainer)
  gives Windows' own convention, which I adopted:
  - `LOCALAPPDATA=…\Packages\<name>\AC`
  - `TEMP=…\AC\Temp`
  - `TMP=…\AC\Temp`

So LOCALAPPDATA is the folder itself, and TEMP and TMP are its `Temp` subfolder.

### Design

- **`LaunchSpelling_win32.hpp`.** `ContainerVariables` is now a table of
  `{name, subfolder}` rows: `{LOCALAPPDATA, ""}`, `{TEMP, "Temp"}`, `{TMP, "Temp"}`.
  `containedEnvironment(environment, containerFolder)`:
  - drops every entry named like a row, ignoring case;
  - appends each row, naming its folder beneath `containerFolder`, in UTF-8.

  The parent's environment is no longer consulted, and the lookup parameter is gone.
- **`AppContainerApi`** gains a `folderPath` seam: `expected<path, HRESULT>(PSID)`. The system version does
  ConvertSidToStringSidW and GetAppContainerFolderPath, then CoTaskMemFree. `ole32` is now linked into
  `vtextension_remote_process` for CoTaskMemFree.
- **`AppContainerProvision::prepare`** returns `PreparedContainer {sid, folder}`. It asks for the folder
  once per provision, together with the SID. If the system refuses, the result is the refusal
  `{Io, LaunchStep::ContainerFolder, HRESULT}`.
  - The new step has a row in the refusal table: "to name its app container's folder", with an HResult
    code.
  - `Io` is in `DetailedLaunchErrors`, so the launcher's existing refusal logging says the step and code
    once.
- **`Process_win32.cpp`.** `ContainedAttributes` carries the folder. A contained launch encodes
  `containedEnvironment(spec.environment, contained->folder)`. Contained and uncontained launches still go
  through the single `environmentBlock` call.

### Deviation

The brief asks for "the Win32 code". The step carries an HRESULT instead, because GetAppContainerFolderPath
returns one natively. A ConvertSidToStringSidW failure is carried as `HRESULT_FROM_WIN32(GetLastError())`,
so one code kind covers both calls. The log line prints it in hex, for example `0x80070005`.

### Probe (`testing/LaunchProbe.cpp`, `--contained`)

For each row of `ContainerVariables`, the probe reports two lines:

- `folder-NAME=`, one of:
  - `inside`: the value lies under the folder the system names for the probe's **own token's** container SID
    (TokenAppContainerSid, then the same `folderPath`), compared case-insensitively on a path-component
    boundary;
  - `outside`;
  - `absent`;
  - `unknown`: a value is set, but the probe has no container folder.
- `value-NAME=`, the value itself.

The SID comes from the child's token rather than the parent's profile, so the check is independent of the
launcher's own path.

### Tests

- **`AppContainer_test`, `Sandbox.windowsHelperRunsContained`:**
  - Uncontained: every variable is `absent`. The probe gets SystemRoot only.
  - Contained: every variable is `inside`.
  - Both runs: no value equals, case-insensitively, the parent's own LOCALAPPDATA, TEMP, TMP or
    USERPROFILE.
  - "Names the parent's profile directory" means *equals* it. A prefix test cannot work, because the
    container's folder lies under the user's profile by design (`…\AppData\Local\Packages`).
- **`AppContainer.profileIsEnsuredOnce`:**
  - The folder is asked once across two prepares and comes back in `PreparedContainer`.
  - A new section, "its folder refused", checks the refusal `{Io, ContainerFolder, E_ACCESSDENIED}` and its
    log line (`app container's folder (error 0x80070005`).
- **`LaunchSpelling_test`:**
  - The folders are named beneath the given folder.
  - A `temp` or `LocalAppData` the caller asked for is replaced.
  - An empty request still gets all three variables.
  - The full contained block is sorted.

### Under wine

These run under wine:

- the uncontained probe half, including the new `absent` checks;
- the `profileIsEnsuredOnce` sections, which use fakes;
- the LaunchSpelling and LaunchRefusal cases.

The contained half SKIPs as before. Wine stubs AppContainers, and its contained launch fails ("the system
lacks what it takes"), so the `inside` checks run only on real Windows.

## 2. Minor 1

Done; see the history section above.

## 3. Minor 3

The `Process_win32.cpp` file-head comment no longer says "the environment it is given and nothing else" for
the contained case. It now says the contained case also gets LOCALAPPDATA, TEMP and TMP set to the
container's own folders, whatever it was given or the parent has, and that its working directory is the
executable's own. The paragraph above it no longer claims the system points these variables itself.

## 4. Minor 4

`processVariable` is kept: `defaultHelperEnvironment()` still uses it for SystemRoot. Its doc now says that
an empty value counts as absent, because GetEnvironmentVariableW reads 0 for both.

## Verification

- **Windows cross build** (`out/clangcl-debug`): built process_test, the probe, wasmtime_test,
  remote_wamr_test, wamr_helper_test and contour-extension-host. Exit 0, no warnings.
- **Under wine 11** (own prefix, `WINEDEBUG=-all`):

  | Run | Result |
  |---|---|
  | `vtextension_process_test "[appcontainer]"` | 4 cases, 3 passed, 1 skipped (contained, wine), 86 assertions |
  | `vtextension_process_test "LaunchSpelling.*,LaunchRefusal.*,AppContainer.*"` | 12 cases, 107 assertions, all passed |
  | `vtextension_process_test` | 124 cases, 121 passed, 3 skipped |
  | `vtextension_wamr_helper_test "~[.measure]"` | 49 cases, 48 passed, 1 skipped |
  | `vtextension_remote_wamr_test` | 53 cases, all passed |
  | `vtextension_wasmtime_test` | 65 cases, 64 passed, 1 skipped |

- **Linux ASan:** the build is warning-free. `ctest -R "vtextension|apply_patch_once"` passes 8 of 8, and
  `ctest -L lint` passes 35 of 35.
- **clang-tidy diff:**
  - Linux tree: 3 TUs clean.
  - Windows tree: 8 TUs clean, with `readability-duplicate-include` off. That check is a false positive of
    the cross setup, as reported last round.
  - One finding was fixed: modernize-raw-string-literal in LaunchSpelling_test.
- **Format:** clang-format 22 (`/usr/bin/clang-format`) `--dry-run --Werror` and editorconfig-checker are
  clean on every changed file.

## What only real Windows can prove

- That the launch now starts contained, and that the probe reads `inside` for all three variables. A wrong
  folder fails the CHECK and names the value.
- That GetAppContainerFolderPath works for our profile. The provision asks the parent's view, the probe its
  own token's.

If CreateProcessW still wants something more, the refusal line says so.

## Concerns

- Setting LOCALAPPDATA to the container folder rests on the reviewer's ruling and Microsoft's documented
  convention. It is unmeasured on a real kernel. If Windows' own reroute were *derived from* LOCALAPPDATA in
  the block, it could produce a doubled path. The probe's `inside` and `value-` lines would show that on the
  next Windows run.
- The branch is rebased past the pushed b5732de14, so it needs a force-push.
