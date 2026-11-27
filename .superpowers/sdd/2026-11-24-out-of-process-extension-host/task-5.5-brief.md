### Task 5.5: FreeBSD sandbox
**Files:**
- Create: `host/Sandbox_freebsd.cpp`.
- Modify: `host/HostBootstrap_posix.cpp` (FreeBSD caps gain `RLIMIT_AS`; D104), `process/SandboxProbe_test.cpp` (a FreeBSD expectation column), `CMakeLists.txt` (`FreeBSD` appended to `_CONTOUR_SANDBOXED_PLATFORMS`; D110).

**Produces:**
- `DescriptorRole { Control, Lane, Page, Wake, StandardError, EngineDevice }` and its rights table (`cap_rights_t` sets per role, ioctls none, fcntls `F_GETFL`).
- `lockDown()` limits every descriptor in `SandboxDescriptors` (which gains roles) and then calls `cap_enter()`.
- `restrictBeforeThreads()`: `procctl(PROC_NO_NEW_PRIVS_CTL)` where defined, otherwise nothing.

- [ ] **Step 1: Write the tests** (they run on FreeBSD CI only). The probe ops table gains FreeBSD expectations:
  - open → `ECAPMODE`;
  - socket → `ECAPMODE`;
  - `pdfork` → `EAGAIN` (`RLIMIT_NPROC`);
  - `send-descriptor` → `ENOTCAPABLE` or success. Capsicum cannot deny `SCM_RIGHTS` on a right-holding socket; the result is recorded, and D70/R38 cover the parent;
  - an `ioctl` on control → `ENOTCAPABLE`.
  - `"Sandbox.rightsTableCoversEveryRole"` (compiles everywhere, a pure table test).
- [ ] **Step 2: Implement.** Rights per role:
  - control: `READ|WRITE|RECV|SEND|EVENT|SHUTDOWN|FSTAT`;
  - lanes: the same minus `FSTAT`;
  - page: `MMAP_R|FSTAT`;
  - wake: `READ|EVENT`;
  - stderr and the WAMR null device: `WRITE|FSTAT`.
- [ ] **Step 3: Size `RLIMIT_AS`** = `dataBytes` (WAMR Copy reserves nothing beyond it); CI's `procstat -v` measurement goes in the report.
- [ ] **Step 4: Verify by reading** against a checklist:
  - `cap_rights_init` variadic use;
  - `<sys/capsicum.h>` and `libcasper` not needed;
  - `cap_enter` after every thread;
  - WAMR's interpreter needs no new descriptor (the null device was opened before);
  - `kern.arandom` and `getrandom` work in capability mode;
  - `clock_gettime` is allowed;
  - every `#ifdef __FreeBSD__` path compiles in the mind's eye against FreeBSD 14 headers.
- [ ] **Step 5: CI** only if the owner allows a push: the FreeBSD leg runs `vtextension_process_test` and `vtextension_helper_test` (WAMR). Otherwise mark it outstanding.
- [ ] **Step 6: Commit** `feat(vtextension): the extension host enters capability mode on FreeBSD`.


---

## Plan decisions (verbatim)

- D104. FreeBSD:
  - Capsicum rights per descriptor role (a table), no ioctls, fcntl `GETFL` only, `cap_enter` in `lockDown`.
  - `RLIMIT_AS = dataBytes` (closes D38's open FreeBSD cap; WAMR Copy reserves nothing extra).
  - `PROC_NO_NEW_PRIVS_CTL` where defined.
  - Proven by CI only. Capsicum cannot refuse `SCM_RIGHTS` on a socket with `SEND`; D70/R38 cover the parent.
- D110 / R43: a platform joins `_CONTOUR_SANDBOXED_PLATFORMS` (out-of-process by default) only once its CI leg ran green.

## Controller notes (binding)

- **FreeBSD CI never runs on pull requests.**
  - `.github/workflows/build.yml`'s `freebsd` job has `if: github.event_name != 'pull_request'`. It runs only on master pushes, release tags and the merge queue.
  - So this code cannot be proven before merge. Ruling: **FreeBSD does NOT join `_CONTOUR_SANDBOXED_PLATFORMS` in this branch.** The code lands dormant (FreeBSD keeps in-process extensions), and Step 5 is "outstanding".
  - Do not change the workflow's triggers. That is the owner's call, and it goes in the report as a recommendation.
- **The dormant code still compiles on FreeBSD in the merge queue, so a compile error there would block the merge.** Be strict about the read-through checklist in Step 4.
  - Read FreeBSD 14/15 man pages and headers from the web (`cap_rights_limit(2)`, `cap_ioctls_limit(2)`, `cap_fcntls_limit(2)`, `procctl(2)`, `capsicum(4)`) and cite them in the report.
  - Wrap only the parts that cannot compile elsewhere in `#if defined(__FreeBSD__)`.
  - Keep the rights table and `DescriptorRole` platform-neutral, so `Sandbox.rightsTableCoversEveryRole` compiles and runs on Linux.
  - Check the CI traps in `task-ci1-report.md`.
- **`SandboxDescriptors` gains roles.** The Linux sandbox and the probe use `SandboxDescriptors` too, so keep the Linux path's behaviour unchanged: the Linux sandbox tests and the helper suites must still pass.
- **Before reporting, run locally:**
  - `ctest -R 'vtextension|wasm_runtime_containment'` and `ctest -L lint`;
  - CI's clang-tidy check: `CLANG_TIDY=clang-tidy ./scripts/clang-tidy-diff.sh` (see build.yml ~239) over the diff against `origin/master`;
  - `/usr/bin/clang-format --dry-run --Werror`, `typos` and `editorconfig-checker` on the changed files.
- **Rules:**
  - Do not push.
  - One commit (plus a separate refactor commit if `SandboxDescriptors` roles touch Linux code). Each commit builds on its own.
  - `git commit -s`, no trailers. Never mention the plan, task numbers or "phase".
