# Changelog

All notable changes to core-cpp are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). While the major version is 0, a minor
release may break the API; every break is listed under **Breaking** with a migration note. A
release tag `vX.Y.Z` equals the version in `project(core-cpp VERSION X.Y.Z)`, and the release
workflow refuses one without a section here.

## [0.5.0] - 2026-09-26

### Breaking

- **The WFMO backend is removed: the completion port is Windows' only backend** (core-cpp#6).
  `BackendKind::Wfmo`, `WfmoBackend` and the readiness socket transport it drove
  (`WindowsSocket`, `WindowsListener`) are gone, after the release in which IOCP was the default
  and WFMO a fallback. `makeDefaultBackend()` on Windows no longer falls back: a completion port
  the kernel refuses to create is handle exhaustion, and it propagates. `listen`, `listenUnix`,
  `adoptListener`, `adoptSocket` and the dials refuse a loop whose backend lends no completion port
  with `NetErrorCode::Unsupported`, where they used to hand out a readiness socket; a dial refuses
  before it creates a socket, so nothing reaches the peer. Two defects of
  the removed transport go with it: a closed socket's parked read now answers `Cancelled` on every
  platform, where `WindowsSocket` answered `BadHandle` (core-cpp#46), and the WFMO-only
  destruction gap of core-cpp#50 has nothing left to apply to. `connectUnix`'s socket on Windows is
  now made uninheritable, as every other one is (core-cpp#28).
  - *Migration*: no consumer names any of these; a program that did replaces
    `makeBackend(BackendKind::Wfmo)` with `makeDefaultBackend()`. `BackendKind`'s enumerators after
    `Iocp` shift down by one, so a value stored or sent as an integer is re-read by name
    (`toString`). A Windows test double that stood in for the loop's backend and expected a
    socket from the factories needs a completion port, or drives an `ISocket` of its own.
- **`core::platform::EnvironmentProvider` is `ProcessEnvironment`, a `core::Environment`; the
  working directory has its own seam; Windows reads and writes the environment in UTF-8**
  (core-cpp#7). Two seams answered "read `HOME`", with two doubles a mixed test had to keep
  agreeing. `core::Environment` is now the one read seam, and `ProcessEnvironment` -- what a shell
  writes -- derives from it, so code that only reads is handed the same object and
  `testing::TestProcessEnvironment` is the double for both. `set`, `unset`, `exportVariable` and
  `setAndExport` return `std::expected<void, PlatformError>`, where they returned `void` and dropped
  the error; a name that is empty or holds `=` or NUL, or a value that holds NUL, is
  `PlatformError::InvalidArgument` (a new enumerator, last, so no other value moves).
  `changeDirectory` and `currentDirectory` move to `core::platform::WorkingDirectory`
  (`nativeWorkingDirectory()`, `testing::TestWorkingDirectory`), and `homeDirectory`, `userName`
  and `configHome` are the free functions of `<core/platform/UserPaths.hpp>` over a
  `core::Environment const&` (`userName` is new there). On Windows, `core::LiveEnvironment`,
  `core::setProcessEnvironmentVariable` and `unsetProcessEnvironmentVariable` go through
  `GetEnvironmentVariableW`/`SetEnvironmentVariableW`, converting to and from UTF-8, where the
  code-page API mangled a value such as a user profile path outside the ANSI code page; a name or
  value that is not UTF-8 is refused (`std::errc::invalid_argument`), and `ProcessEnvironment::keys()`
  converts UTF-16 names rather than narrowing them a code unit at a time. The read seam's
  interface is unchanged.
  - *Migration*: `<core/platform/EnvironmentProvider.hpp>` is `<core/platform/ProcessEnvironment.hpp>`;
    `EnvironmentProvider` is `ProcessEnvironment`, `nativeEnvironmentProvider()` is
    `nativeProcessEnvironment()`, and `testing::TestEnvironmentProvider` is
    `testing::TestProcessEnvironment` (`<core/platform/testing/TestProcessEnvironment.hpp>`), which no
    longer takes an initial directory. Handle or discard the `std::expected` from every `set`,
    `unset`, `exportVariable` and `setAndExport` (`std::ignore = env.set(...)` where a failure is
    acceptable). Replace `env.changeDirectory(p)` and `env.currentDirectory()` with a
    `WorkingDirectory&` the object is given -- `nativeWorkingDirectory()` in a composition root,
    `testing::TestWorkingDirectory(initial)` with `addValidPath` in a test. Its
    `currentDirectory()` returns a `std::filesystem::path`, where the old member returned a UTF-8
    `std::string`, so the answer round-trips through `changeDirectory()` on Windows whatever it
    spells: a caller that wants the text takes `core::platform::normalizePath(cwd.currentDirectory())`,
    and one that compared with a string literal compares its `generic_string()`. Replace
    `env.homeDirectory()`, `env.userName()` and `env.configHome()` with
    `core::platform::homeDirectory(env)`, `userName(env)` and `configHome(env)`. A function that
    only reads can take a `core::Environment const&` and be handed either double. contour, which
    uses only `core::Environment`, changes nothing.
- **`core::cli::parse()` returns `std::expected<FlagStore, ParseError>` and throws nothing**
  (core-cpp#13). It returned `std::optional<FlagStore>` -- `std::nullopt` for tokens left over --
  and threw `core::cli::ParserError` for a value of the wrong type, a missing value or an explicit
  empty one, and `std::invalid_argument` for a missing required option. Every one of those is now a
  `ParseError`: a `ParseErrorKind` (`NotEnoughArguments`, `InvalidValue`, `EmptyValue`,
  `UnexpectedToken`, `MissingRequiredOption`), the index of the token at fault and a message.
  `ParserError` is removed. Numbers are read whole, with `std::from_chars` (floating point with
  `std::strtod`): `12abc` is refused rather than read as 12, `-1` is no longer accepted -- and
  wrapped -- as an unsigned, a leading `+` or leading whitespace (`+5`, ` 5`), which `std::stoi`
  and `std::stoul` accepted, is refused, and a value out of the type's range is refused rather
  than truncated. `parse()` is `[[nodiscard]]`: a call that discards the result no longer compiles
  under `-Werror`. `App::run()` and `App::reparseParameters()` print the error's message; their signatures are
  unchanged.
  - *Migration*: a call site that tested `has_value()` or used `*parsed` and `parsed->` compiles
    as it is when its variable is `auto`; one that names the type spells
    `std::expected<core::cli::FlagStore, core::cli::ParseError>`, or `auto`. Replace a `try`/`catch`
    around `parse()` with a test of the result, and report `parsed.error().message`. tuidu's
    `parseCommandLine()` (`src/tuidu/Cli.cpp`) is the one consumer call site: it declares
    `std::optional<core::cli::FlagStore> parsed` and catches `std::exception` around the call.
    contour and endo use `core::cli::App` only, and need no change.

### Added

- **`core::async::TaskKind` and `RunTask::kind()`: an around-task hook can tell a coroutine
  resumption from a posted callable** (core-cpp#53). `TaskKind::Callable` is what `post` and
  `tryPost` were given; `TaskKind::Resumption` is a coroutine arriving through `submit` or
  `trySubmit`, as a handle or as `ParkedWork`, a `ResumeOn` hop, or a `KeyedStrands` reroute
  through a retired key strand. A hook -- `StrandOptions::aroundTask` or a `KeyedAroundTask` --
  can now scope per-resumption context to coroutine resumptions only, rather than installing it
  around every callable posted to the same strand or key too. Read from what the task already
  holds, so it adds nothing to a task and costs one load where a hook asks. Additive; no signature
  changes.
- **`core::net::closeLingering` and `LingerBounds`** (`<core/net/LingeringClose.hpp>`, from
  fastcached at `0708dd54`; core-cpp#35): half-close, discard what the peer is still sending until
  it closes or a bound runs out -- the whole drain's time, the bytes discarded, the reads made --
  then close, so a reply written over a request left unread is followed by a FIN rather than
  destroyed by the reset a bare close sends. `HttpLimits::linger` bounds it for `serve`, by
  default 250 ms, 64 KiB and four reads -- smaller than fastcached's two seconds, because `serve`
  handles one connection at a time and a refused request holds its accept loop for up to that
  long. Additive.
- **`core-cpp.open-work`: every `## Open work` entry leads with a core-cpp issue, and that issue is
  open** (core-cpp#12). `scripts/check-open-work.py` reads every such section under `.agent/` and
  `docs/` and the top-level documents, and refuses an entry that does not lead with a core-cpp issue
  link, a link whose text and URL disagree, and a heading with no entries. The ctest runs that
  offline; CI's `style` job also runs it `--online`, which refuses an entry whose issue has closed,
  and one whose issue does not exist (404 or 410); it exits 77 (skipped, a warning in CI) rather
  than failing when it could not ask GitHub. It has a self-test.
- **A nightly job answers whether a provenance row's upstream has moved** (core-cpp#33).
  `core-cpp.upstream-drift` needs the upstream checkouts beside core-cpp, so it skips on every CI
  runner and its coverage there was zero. The `upstream-drift` job of `downstream.yml` checks out
  contour, endo and fastcached with full history as siblings and runs the checker: drift is listed
  in the job summary, and a row the checkouts prove malformed, or an upstream the checker could not
  read, fails the job.
- **`core-cpp.iterator-debug-canary` proves the MSVC Debug runtime's iterator checks are live**
  (core-cpp#11). In every MSVC-driver Debug build (`cl-debug`, `clangcl-debug`) it indexes a
  `std::vector` out of range and passes only on the runtime's own `vector subscript out of range`;
  a build where `_ITERATOR_DEBUG_LEVEL` fell below 2 reads the element instead, and fails.
- **A `windows (clang-tidy)` CI job analyses the Windows sources** (core-cpp#38, core-cpp#44). The
  `clang-tidy` job's preset is Unix-only, so every `windows/` source and every `_WIN32` arm of a
  shared header went unanalysed while it was green. The new job runs the pinned clang-tidy over the
  clang-cl tree's compile database through `scripts/tidy-database.py`, which refuses a result that
  analysed fewer `windows/` sources than git tracks, an analyser other than the pin, and a canary it
  did not report. (`CXX_CLANG_TIDY` is not used there: over clang-cl it hands clang-tidy a command it
  reads with exceptions disabled.) It is one of `ci-ok`'s needs.

### Changed

- **`core::net` and `core::tui` ask a promise for its stop token through
  `core::async::HasStopToken`** (core-cpp#29), and `ctest -L hygiene` refuses the hand-spelled
  `requires { awaiting.promise().stopToken(); }` anywhere in the tree, so the concept is the one
  place that states the contract. No behaviour changes: every site assigned the token to a
  `StopToken`, which is what the concept requires it to convert to.
- **The hygiene scan's `namespace-directory` rule skips a leading forward-declaration block**
  (core-cpp#23). A header under `src/core/<dir>/` that opened with
  `namespace core::platform { class Wakeup; }` was refused, because that was its first named
  namespace, so it had to include the other module's header instead. A block whose body is only
  `class`/`struct`/`union`/`enum` declarations ending in `;` defines nothing and is now skipped,
  and the rule applies to the first namespace after it. `core/tui/TerminalInput.hpp` forward-declares
  `core::platform::Wakeup` accordingly and no longer includes `core/platform/Wakeup.hpp`; a file
  that used `Wakeup` through that include includes it itself (none of the consumers does).

### Fixed

- **`serve`'s refusal of a request it did not read to its end reaches the client** (core-cpp#35).
  A 413 or 400 was written over request bytes still unread, and the connection's bare close then
  sent a reset rather than a FIN: the client read the refusal followed by a connection reset, and
  on Windows could lose the refusal itself. The refusal now closes through `closeLingering`. A
  request read in full and answered closes as before.
- **`NativeFileSystem` reports a failure on a path the ANSI code page cannot spell** (core-cpp#26).
  Its error messages spelled the path with `path::string()`, which on Windows narrows through the
  code page: such a name was mangled, and MSVC's conversion throws there, so the error path itself
  threw out of `readFile`, `rename`, `listDirectory` and the rest. The messages now spell the path
  in UTF-8, as the paths core-cpp hands back already are. POSIX was unaffected.
- **`testing::InMemoryFileSystem` answers as the native backend does in two more places**
  (core-cpp#27). `isExecutableFile`, `permissions` and `setPermissions` follow a symlink to its
  target, so a dangling link is not executable and has no permissions to set, where the fake used
  to judge the link by bits recorded on the link itself. `createDirectory` refuses a path that is
  already there, directory or file, with "File exists", where it used to succeed. What the fake
  still does not model is tabled in `docs/modules/platform.md`.
- **A Windows `SystemPipe`'s sockets are no longer inherited by child processes** (core-cpp#28).
  `::socket()` and `::accept()` hand back inheritable handles there, so a consumer that spawned a
  process -- contour's and endo's shells -- handed the child the loop's wakeup channel, and a child
  that kept it open could hold it alive after the parent closed its end. The sockets are made with
  `WSA_FLAG_NO_HANDLE_INHERIT` and the accepted one has its inheritance cleared, as POSIX already
  sets `FD_CLOEXEC`.
- **Alt+Backspace reaches its key binding** (core-cpp#21). `VtParser` read `ESC DEL` -- how xterm,
  VTE, iTerm and Alacritty send Alt+Backspace -- and `ESC BS` (Alt+Ctrl+H) as a bare Escape followed
  by a plain Backspace, so a modal took the Escape as cancel and `DeleteBigWordBackward` could fire
  only under the Kitty keyboard protocol. Both now decode to one `KeyCode::Backspace` with
  `Modifier::Alt`. No signature changes.
- **A character outside the BMP typed or pasted into a Windows console arrives as valid UTF-8**
  (core-cpp#20). The console delivers U+1F600 as two key events, one per surrogate, and each was
  encoded on its own (CESU-8), which `VtParser` then decoded to two lone surrogates. The Windows
  input now pairs surrogates across reads (`detail::Utf16ToUtf8`, which replaces the private
  `windows/Win32Utf.hpp`), and an unpaired one becomes U+FFFD. In Win32 input mode, where the
  console reports each surrogate as its own `CSI ... _` key, `VtParser` pairs the two keys the same
  way, across reads, and drops a half it cannot pair. `VtParser` drops what no UTF-8
  decoder may produce, on every platform: an encoded surrogate, an overlong encoding and a value
  above U+10FFFF. No public signature changes.
- **The vendoring tool refuses a `DEST` that is a symbolic link** (core-cpp#25). CMake's `EXISTS`
  and `IS_DIRECTORY` resolve through a link, so a link to an empty directory passed every guard, and
  the replacement then renamed the link aside and put a real directory in its place: the consumer's
  link was gone, its target untouched, and the sync reported success. `MODE=sync` now refuses such a
  `DEST` by name, before anything is read or written, and says which directory to name instead.
- **A `clang-tidy` build re-analyses what a changed `.clang-tidy` or a replaced analyser governs**
  (core-cpp#36). Neither was an input of any compile, so editing a rule, or installing another
  clang-tidy at the same path, re-analysed nothing whose object was current: `no work to do` meant
  "clean under the rules in force when each object was built". With `CORE_CPP_CLANG_TIDY` on, every
  analysed compile now depends on the analyser binary and on every `.clang-tidy` from its source's
  directory up to the tree's root, and `core-cpp.tidy-inputs` (registered only in a clang-tidy Ninja
  build, and run by CI's `clang-tidy` job) refuses a build whose statements do not.

## [0.4.3] - 2026-09-26

### Changed

- **A socket operation that has to wait no longer files and takes a park in the loop's id map**
  (core-cpp#52). A frameless one-direction park on a handle's kept registration -- what
  `PosixSocket` files for every read or write that waits -- is now filed in storage the handle's
  watch keeps for that direction, and gets an id made of that storage's slot and a generation that
  moves on per operation. The operation no longer costs an id-map insert and erase or a park made
  and recycled. The `[park]` bench in `core-cpp-net-test` (gcc-release, epoll, median of 5 runs,
  three interleaved rounds) goes from 33.1-33.8 to 17.8-18.6 ns per park filed and taken with 256
  parks live, and from 21.3-22.4 to 17.6-19.2 ns with one. The socket ping-pong bench's user time
  is within its own noise either way. The timer path's drain-step completion bench is about 1 ns
  (1-2%) slower at the median, for the park lookup's two extra branches. No signature changes:
  ids are still never handed out twice, so a late cancel or a ready entry for a retired operation
  still finds nothing, and an idle socket's park is still uncounted, silent and narrowed as before.
  A closed socket's slot hands its park back to the loop's spare list (capped at 64), so a burst
  of connections leaves no burst's worth of parks behind. Measured in fastcached (memcached and
  redis at 1, 16 and 64 connections, quiet host): the park path falls from about 1.5% to 0.7-0.9%
  of samples, roughly 0.2 µs per request, with syscalls and allocations unchanged.

## [0.4.2] - 2026-09-26

### Added

- **`core::log::ScopedCapture::snapshot()`**: a copy of what the capture holds, taken under its
  lock, for reading while other threads still log into it. `text()` is unchanged -- a reference to
  the buffer itself -- and is valid only while no thread logs into the capture, once the writers
  are joined.

### Fixed

- **`core::log::ScopedCapture` takes lines from several threads at once.** Its sink appended to a
  `std::string` without a lock, so a test whose code logged from two threads corrupted the heap
  (contour's Windows test crash). Every append now takes a mutex, and so do `contains()`,
  `count()`, `lines()` and the new `snapshot()`. No signature changes.
- **`DetachedTask` no longer reads a freed frame under clang-cl at `-O0`** (core-cpp#51). A
  trivial empty return object is returned in a register, and clang-cl without optimisation kept the
  starting call's copy of it in the coroutine frame and reloaded it from there after the first
  suspension. When that suspension handed the coroutine to something that ran it to its end first
  -- a pool thread, an event loop on another thread, an `await_suspend` that resumes inline -- the
  frame was already freed. fastcached's shutdown flows, which hop onto a loop from another thread
  and finish there, have this shape. `DetachedTask`'s destructor is now defaulted out of line,
  which makes it user-provided, so every ABI returns it through a pointer the caller owns. The type
  is no longer trivially destructible, trivially copyable or an aggregate; `DetachedTask{}` still
  works through its default constructor, and its members and behaviour are unchanged.

## [0.4.1] - 2026-09-25

### Added

- **`seal()` on `Strand` and `KeyedStrands`: close the offer door, keep running what is queued
  and what comes back.** `close()` drops the queue, so a teardown of "stop the handlers, drain,
  close" dropped whatever arrived between the drain and the close -- a task's finish, a
  coroutine's resumption -- and leaked its frame and whatever it was to settle (found by morph's
  switch review). After `seal()`, `tryPost` and `trySubmit` return false and leave the work with
  the caller, for `KeyedStrands` for every key, a key with no strand included. `post` and `submit`
  are still admitted until `close()`: `submit` is how a coroutine the strand already admitted
  comes back -- `ResumeOn`, `resumeOn(key)`, an `AsyncQueue` push, close or stop -- and dropping it
  would free a detached chain without its finish. Queued work runs as usual, and no kept
  `KeyedStrands` strand is handed out or kept once sealed.
  - **What a drain can see:** `idle()` and `waitIdle()` then mean nothing queued and nothing
    running. A coroutine suspended off the strand -- on a socket, a timer, an `AsyncQueue` -- is
    invisible to both and may still come back, so a consumer counts its own in-flight work. The teardown that loses nothing is `seal()`, then that
    count and `waitIdle()` both done (on the single-threaded WebAssembly build, the base run until
    they are), then `close()`. `post` is new work too: a producer that posts must stop, or offer
    through `tryPost`, before `waitIdle()`.
  - Idempotent; `close()` is unchanged. A patch-compatible addition.

### Known issues

- **MSVC cl 19.51 `/O2` leaks a coroutine's by-value parameters** when its frame is destroyed at
  a suspension point with no statement after it
  ([#54](https://github.com/contour-terminal/core-cpp/issues/54)). Keep a statement after the
  last `co_await` in a coroutine that may be destroyed while suspended.

## [0.4.0] - 2026-09-25

### Breaking

- **A consumer parked on `AsyncQueue::pop` resumes on the executor it was running on when it
  parked**, not on the executor the queue was constructed over -- on a push, a `close()` and a stop
  request alike (see *Added*, the current-executor context). The queue's executor is now the
  fallback, used only for a consumer that parked while no executor was current. The old behaviour
  sent a coroutine that parked on a `Strand` back off it, onto the queue's executor, where it raced
  the state the strand serialises (found by morph, PR #806).
  - *Migration*: a consumer that runs on the queue's own executor -- the usual shape -- sees no
    change. One that parks while running on a different executor now comes back there; if it
    relied on arriving on the queue's executor, hop explicitly after the pop with
    `co_await ResumeOn { queueExecutor }`. An `IExecutor` of your own that resumes coroutines
    should state itself with `core::async::ExecutorScope` around the resumption (as
    `testing::ManualExecutor` does), or its coroutines keep the old behaviour.
  - *Consumers*: fastcached uses `AsyncQueue` in five files (`RaftPeerTransport.hpp` and `.cpp`,
    and fastcache-cli's `LiveEventSource.cpp`, `LiveSourceRig.hpp` and `ScriptedStopSignal.hpp`)
    and is unaffected: every queue there is built over the reactor, and every `pop()` parks either
    on that reactor or outside any executor, so the executor it comes back to is the one it came back
    to before. morph's handlers, which parked on a strand, come back to it now, which is the fix.
- **`core::net::resumeSoonOn` is `core::net::detail::resumeSoonOn`**, and takes the chain's
  unowned root instead of a work-item factory. It is `ResultAwaitable`'s out-of-line completion
  hook and has no other caller.
  - *Migration*: none expected -- fastcached, endo, contour, tuidu, `dbtool` and morph call it
    nowhere. A transport completes an operation through `ResultAwaitable::complete`.

### Added

- **`core::async::Strand`** (`<core/async/Strand.hpp>`): an `IExecutor` over any `IExecutor` that
  runs what it is given one task at a time, FIFO -- a task being one resumption, from the submit to
  the next suspension. `co_await ResumeOn { strand }` hops onto it; `runningHere()` asks whether
  the calling thread is inside one of its tasks. One coroutine pump per strand, queued on the base
  once however many tasks arrive while it is busy, runs at most `StrandOptions::batch` (32) tasks
  per turn before it hands the base back. A task that throws out of `resume()` propagates to
  whoever resumed the pump and does not wedge the strand or lose the tasks behind it -- **except
  under MSVC's `cl`, where it ends the process with a message**: an exception crossing the strand's
  coroutine frames was measured corrupting the thread's executor scopes on `cl-release`
  (`core-cpp.strand-throw-canary`). An allocation that fails leaves the strand as it was: `submit`
  throws with nothing queued, and a key's strand whose replacement pump cannot be made is retired
  if nothing is queued on it. **A refused hand-off abandons the strand's queued work**: a base
  whose `submit` throws when the strand hands it its pump makes that `submit` throw, and the rest
  of the queue -- work other threads queued meanwhile included -- is dropped as destruction drops
  it; a `KeyedStrands` key's strand is retired with it. A key whose first submit cannot allocate
  no longer leaves its new strand registered with nothing to retire it. What a frame freed by that drop submits to
  the same strand (or `KeyedStrands`) from its destructor is dropped as well, not handed to the
  refusing base. `~Strand` waits for a hand-off still inside the base's `submit`. Destroying a strand drops what is
  queued -- a chain rooted in a `DetachedTask` is freed, a `Task`-owned coroutine is left to its
  owner -- and waits for a task running on another thread, but not for the task it is called
  from: a task may release the last reference to the strand's owner. The strand's state outlives
  it, so a coroutine that parked on it and is handed back later (an `AsyncQueue` push after the
  strand died) is dropped the same way rather than reaching freed storage; inside a task,
  `currentExecutor()` is that state, not the `Strand`'s address -- ask `runningHere()`. The base
  must outlive the strand and run what it queued; an `EventLoop` destroyed with the strand's pump
  still in its inbound queue drops it, leaking the strand's state. Written after morph's
  `StrandExecutor`, which consumers should replace with it.
- **`core::async::KeyedStrands<Key, Hash, KeyEqual>`** (`<core/async/KeyedStrands.hpp>`): one
  strand per key over a shared base, made when a key gets work and reclaimed when it runs out.
  `submit(key, ...)`, `co_await strands.resumeOn(key)`, `runningHere(key)`, `runningAnyHere()`,
  `size()`, and `waitIdle()`, which blocks until no key has work, asserts when called from one of
  its own tasks, and is declared only where threads exist. Destroying it from one of its own tasks
  does not wait for that task. A coroutine that parked while its key's
  strand was reclaimed comes back to the key, never to a second strand beside it.
- **Callables, a closed strand's answer, an around-task hook and `idle()` on both strands** -- what
  morph's switch from its own `StrandExecutor` found missing:
  - `post(fn)` / `post(key, fn)` run a callable as one task, held by value in one allocation (the
    census in `StrandAllocation_test.cpp`: one per post and none per submit, to a busy key and to
    an idle one alike, in the steady state -- `KeyedStrands` keeps up to 32 retired strands, with
    their pump frames, queue room and map nodes, and gives them to the next key that needs one;
    without that a post to an idle key cost five). What it throws takes
    a task's way out, and ends the process under MSVC's `cl` as a task's throw does.
  - `tryPost(fn)`, `trySubmit(work)` and their keyed forms return false once the strand is closed
    and leave the work with the caller, which can then run it itself. `close()` is public on both,
    idempotent, and what the destructors call.
  - `StrandOptions::aroundTask` (an `AroundTask`) and `KeyedStrands`' `KeyedAroundTask<Key>`, which
    is given the key: a hook called around every task, a coroutine that came back through the
    strand from another executor included, to install per-task ambient context such as a
    session. A reference set at construction; unset, it costs one branch per task. `Task` carries
    no context of its own, which would cost every `co_await` for every consumer.
  - `idle()`: nothing queued or running. On the single-threaded WebAssembly build, where
    `waitIdle()` does not exist, a host pumps its base until `idle()`; destroying or closing a
    strand there drops what is queued without waiting, since nothing else can be running.
- **The current-executor context** (`<core/async/ExecutorContext.hpp>`): `ExecutorScope` marks the
  calling thread as running a task of an executor, nests, and restores on every exit;
  `currentExecutor()` answers the innermost; `ResumeTarget` is what an awaitable holds across a
  suspension to resume there, with `ResumeTarget::currentOr(fallback)`. `core::net::EventLoop`
  states itself once per turn (and around its teardown drains), `ThreadPoolExecutor` once per
  worker thread, a strand once per batch. The scope is two thread-local stores and no allocation;
  on the loop it is paid per turn, not per resumption, because G2 puts every resumption inside one
  turn's step 2. Measured on the drain path against the same tree without it (gcc-release, one
  pinned core, 4M resumptions, median of five interleaved runs): 35.9 to 36.3 ns per resumption
  with one resumption per turn -- the two stores, paid once per turn -- and 19.6 to 19.7 with a
  turn of 64. The program is `tests/bench/ExecutorContextBench.cpp` (`core-cpp-bench-executor-context`).
  - `core::net`'s socket operations and timers do not read it and keep resuming on their
    `EventLoop` (G2); a strand-bound coroutine hops back with `co_await ResumeOn { strand }`.
- **`core::async::testing::ManualExecutor`** (`<core/async/testing/ManualExecutor.hpp>`): an
  executor a test drains by hand (`runOne`, `drain`, `pending`) that states itself as the current
  executor while it does. `drain(bound)` stops after `DrainBound` (2^20) resumptions by default and
  throws `std::length_error` if work is still queued, so work that requeues itself for ever fails
  a case instead of hanging it. The first public test double of `core::async`.
- A design note, *Strands and the resume context* (`docs/design/strands.md`).

### Changed

- **A socket completion costs less on the loop's thread.** `ResultAwaitable::complete` hands its
  waiter to the drain step (G2) through `EventLoop`'s ready queue, and for a chain rooted in a
  `DetachedTask` -- every connection a server spawns -- the queue entry was a refcounted work item
  whose claim cost five atomic operations per completion; it is now a `detail::CountedClaim`
  costing two, with the same count, arm and teardown behaviour. The waiter a readiness callback
  queues reaches the callback position by a swap of two vectors instead of a range insert and an
  erase, and the drain resumes an entry in place rather than moving it out first. Measured with the
  new `[bench]` cases (`core-cpp-net_backend-test "[bench]"`, gcc-release, median of five on an
  idle host): a completion through a drain-step callback went from 120.4 to 83.4 ns for a
  detached chain, and from 122.6 to 79.3 ns for a chain a caller owns. The ordering (a waiter
  resumes in its callback's position), teardown (a detached chain is freed, a borrowed one resumed)
  and take-back paths are the same, and each is a case in `CompletionClaim_test.cpp`. Two things a
  caller can observe did change:
  - a frameless park holding both of its handle's watch slots gets one callback per report that
    flags both, not two;
  - at teardown, a readiness callback still queued is dropped with its park still marked queued,
    so a requeue for the same reason during teardown is suppressed rather than queued again.
  - A readiness callback that completes one waiter, the common case, has that waiter held in a
    slot and resumed by the drain straight after it returns, with no queue entry made for it; a
    readiness report reaches its park through the handle's watch instead of probing the park
    table; and `EventLoop`'s queue entry is back to 56 bytes from 64.
- **A loop in its steady state allocates nothing per completion or per timer firing.** Measured by
  fastcached as allocations per request, and now asserted over 256 turns in
  `CallbackAllocation_test.cpp`. Four allocations are gone: the ready queue is a ring that keeps its
  capacity (`detail::RingQueue`) rather than a `std::deque`, which allocated a node every few
  entries of a FIFO that never grows; a parked operation no longer spells its never-completed
  answer into a heap string when it is armed, only when that answer is read; a fired or cancelled
  timer's park is recycled like every other; and a turn collects its due deadlines into a vector
  it keeps.
- **Against fastcached's own reactor**, measured by fastcached on this release's `core::net`
  (release/next-031 at b0c82a4) at 64 connections: fewer syscalls per request than the reactor,
  6.0 allocations per request against its 12.6, and total CPU per request at parity within the
  noise. User CPU is not yet at parity: its medians are still about 1 µs per request higher, with
  the runs' ranges overlapping, and a profile puts core-cpp at 7.6% of samples where the reactor's
  own code was 3.6%. What remains is tracked as core-cpp#52 (reusing a socket's park across its
  operations).

### Fixed

- **A loop with more ready connections than `dispatchBatch` spent about 1,460 dispatches per round
  trip; it spends four.** A frameless readiness park -- every parked socket operation -- stays filed
  across its wakes, and a level-triggered backend reports its handle on every wait until it is read,
  which it was not while the owner's callback waited behind `EventLoopOptions::dispatchBatch`. Each
  report queued the callback again, and the copies took the bound's slots doing nothing: 64 socket
  pairs ping-ponging on one loop at the default bound of 64 made 47,424 round trips in 10 s, at
  about 1,460 dispatches each, where 32 pairs made 64,000 in 0.28 s. A park is now queued once per
  reason (a cancel or an abandonment behind a queued readiness still queues), and 64 pairs make
  128,000 round trips in 0.65 s.

### Known issues

- **`DetachedTask` under clang-cl at `-O0`** can read its return object back from a frame that
  has already been freed, when its first suspension hands it to something that runs it to its end
  before `await_suspend` returns -- a pool thread, or an inline executor (core-cpp#51). Debug
  builds with clang-cl only: `cl`, clang and GCC elsewhere, and clang-cl with optimisation, are
  unaffected.

## [0.3.0] - 2026-09-24

### Breaking

Each of these is a defect fixed or a contract made explicit, and each changes what a caller can
observe; the migration is under each.

- **A second read or write armed over a parked one ends the process in every build** (see
  *Fixed*). A Release process that used to hang there now aborts at the violation.
  - *Migration*: audit every path that re-arms a read or write on a socket without the previous
    operation having resolved -- in particular a retry or timeout path that starts a new read
    without `cancelRead()` or awaiting the old one. Install `core::setFailHandler` to route the
    message ("Socket contract violated: ...", naming the direction and the handle) to your logger,
    and to take a stack trace there before the abort.
- **A waiter completed by a drain-step callback resumes in the callback's position**, not at the
  back of the ready queue as in 0.2.1 (see *Fixed*); 0.2.0's order, without resuming inline.
  - *Migration*: code written against 0.2.1 that relied on a flow queued ahead of a readiness
    callback running again -- after a yield -- BEFORE that callback's waiter now sees the waiter
    run first. Code written against 0.2.0 needs nothing.
- **`IHostScheduler::callAfter` must deliver every request it accepts exactly once**, because
  `HostDrivenBackend` now hands it a ticket only the callback frees.
  - *Migration*: a host that dropped pending callbacks at shutdown leaks one small ticket per
    request dropped; deliver them (a late pump finds its backend gone and runs nothing) or accept
    the leak. A host that delivered one twice must stop.
- **`testing::ManualHostScheduler` is neither copyable nor movable, and `clear()` is no longer
  `noexcept`.** Its destructor delivers what is still pending, cleared requests included.
  - *Migration*: hold one per test by value or by reference, and destroy it after the backends it
    serves -- declare it first.

### Added

- **`core::net::contract::SlotDirection`, `contract::secondOperationArmed()` and
  `contract::describeHandle()`** in `<core/net/SocketContract.hpp>`, and an optional handle argument
  (plus a defaulted `std::source_location`) on `contract::claimReadSlot` and
  `contract::claimWriteSlot`, which name it when they end the process. A transport outside
  core-cpp passes its own handle to get it in the message.
- **`EventLoop::inboundFinishedRootCount()`**: how many spawned flows ended off the loop's thread
  and wait for the next turn to release them.

- **core-cpp installs as the CMake package `core-cpp`** (core-cpp#5): `find_package(core-cpp 0.3
  CONFIG REQUIRED)` and `target_link_libraries(app PRIVATE core::net)`, the same names as a source
  build's aliases. Every module target is installed with its `HEADERS` file set (the generated
  `core/Config.hpp` included) in the install component `core-cpp`, with a
  `core-cppConfigVersion.cmake` that is `SameMinorVersion` while core-cpp is 0.x. The package
  config calls `find_dependency()` for exactly the dependency-table rows its installed targets
  link. A target that links a dependency the build fetched rather than found (libunicode, Catch2 or
  Tracy through CPM) cannot be re-found by an installed package and is left out, with a status line
  saying so. See docs/getting-started/install.md.
  - *`CORE_CPP_INSTALL`*, default `PROJECT_IS_TOP_LEVEL`: a vendoring or CPM consumer installs
    nothing of core-cpp's unless it asks. A parent that exports a target of its own linking
    core-cpp's turns it on, or CMake refuses the export as "not in any export set" (found by morph).
  - `core::net`'s `detail/ReadyBatch.hpp` and `detail/ScopeGuard.hpp` joined its `HEADERS` file
    set: `EventLoop.hpp` and `testing/ScriptedBackend.hpp` include them, so the installed headers
    could not compile without them. `core::tui` links stb_image as `$<BUILD_INTERFACE:...>`, and
    `core::testing_main` names its dialog object through the installed `core::testing_dialogs`.
  - `core-cpp.install` installs the build under test into an empty prefix, builds and runs a
    consumer of the package (`tests/consumer-install`), checks that every core-cpp header an
    installed header includes was installed, and configures a parent exporting a target that links
    core-cpp's (`tests/consumer-install-nested`) with `CORE_CPP_INSTALL` on and off.

### Fixed

- **`core::testing::suppressWindowsDialogs()` keeps abort()'s message and turns off Windows Error
  Reporting's UI.** It cleared `_WRITE_ABORT_MSG` with `_CALL_REPORTFAULT`, so an aborting test
  printed nothing; only the fault report is off now, and the message goes to stderr, not a dialog.
  It also asks Windows Error Reporting for no UI, `WerSetFlags(WER_FAULT_REPORTING_NO_UI)` (in
  kernel32; `WerGetFlags` confirms the flag is set), for an unhandled structured exception in a
  process whose error mode was reset (found by morph). `windows-dialog-canary.abort` now requires the message in a Debug build;
  a Release UCRT writes none.

- **A waiter completed by a readiness callback resumes in the callback's position again.** 0.2.1
  made a completion from a callback (`ResultAwaitable::complete()`, and anything a drain-step
  callback hands to `EventLoop::resumeSoon`) join the BACK of the ready queue, where 0.2.0 had
  resumed it inline. A flow queued ahead of the callback that yields once to let already-reported
  readiness run -- fastcached's `AbandonIfPeerGone` -- then resumed before the waiter and read stale
  state. The drain now puts what a callback queued at the front once the callback returns: the
  waiter resumes before anything queued after the callback, still in the drain step and never
  inside the callback (G2). `resumeSoon` from outside a drain-step callback stays FIFO. The
  callback's queue is a member reused across callbacks, so a readiness completion allocates
  nothing for it, and `cancelPending` finds a waiter a callback has queued.

- **A spawned flow that completes inside a sub-task is released, instead of leaking until the
  loop is destroyed.** `EventLoop::spawn` unlinked a finished flow by the frame its ready entry
  named, and a flow parked inside a sub-task (`co_await leaf()`, with `leaf` on a socket read or a
  delay) is resumed through the sub-task's frame: it ran to its end inside that resume, by
  symmetric transfer, and stayed in the loop -- and in `spawnedCount()` -- until `~EventLoop`, on
  normal completion and on `requestStop` alike. A long-lived loop spawning one flow per connection
  grew without bound (found by the contour migration, measured on 0.2.1). `spawn` now runs the
  flow inside a root coroutine owned by the loop -- one more frame allocation per spawn -- whose
  final suspension files it for release; the drain destroys it after the resume that finished it
  returns, in O(1) and on the loop's thread, whichever frame the resume named. A flow that ends on
  another thread (after `co_await ResumeOn { pool }`) hands itself over through the inbound queue
  and is released in the next turn's first step. A flow's exception ends it without being
  rethrown into the root.

- **A second operation armed over a parked one ends the process in every build, instead of hanging
  in Release.** One read and one write operation per socket is the contract, and
  `contract::claimReadSlot`, `contract::claimWriteSlot` and the watch slots of
  `EventLoop::registerPark` enforced it with `assert` alone: under `NDEBUG` the second operation
  displaced the parked one, which was then never resumed -- a silent hang, and the leading suspect
  in a Release-only fastcached stall on 0.2.1. All three now terminate through `core::detail::fail`
  (so a program's `core::setFailHandler` logs it first), naming the direction and the socket's
  native handle, in Debug and Release alike. `claimReadSlot` and `claimWriteSlot` take the handle
  as an optional second argument. The `socket-contract-canary` slot modes, and two new ones for the
  loop's own slots (`watch-read-slot`, `watch-write-slot`), now run on the Release legs as well.
  - *Migration*: a caller that armed a second read or write over a parked one was already broken; in
    a Release build it now fails loudly at the violation instead of hanging later. `IocpSocket`'s
    Release-only handling of an orphaned write, and the two tests that drove displacement under
    `NDEBUG`, are gone with the displacement.

- **A host-driven loop may be destroyed while a pump is out with the host.** `HostDrivenBackend`
  handed `IHostScheduler::callAfter` its own address, and `emscripten_async_call` cannot be
  retracted: a `PlatformLoop` destroyed under Emscripten with a timer armed, or after any off-turn
  `addTimer`, `post` or wake, freed the backend it owns, and the browser's timer then wrote into
  it -- a heap-use-after-free (found by morph's timeout scheduler). Each pump now carries a small
  ticket holding a weak reference to the backend; a late pump finds it expired, runs nothing and
  frees the ticket. Coalescing is unchanged.
  - *`IHostScheduler` states its contract*: every request accepted is delivered exactly once, since
    its state may own storage only the callback frees. A host that drops a request leaks a ticket.
  - *`testing::ManualHostScheduler`* delivers whatever is still pending when it is destroyed,
    including what `clear()` took out, which is no longer `noexcept`; it is no longer copyable or
    movable, since a copy would deliver a ticket twice.

## [0.2.1] - 2026-09-24

Every behaviour change in this section is a defect fixed, and each entry says which guarantee it
restores and what a caller that depended on the defect changes. None of them is a break of a
documented promise, which is why they are under *Fixed* in a patch release.

### Added

- **A public SGR reset in `core::tui_output`**: `buildSgrReset()` beside `buildSgrSequence()`,
  `TerminalOutput::resetStyle()`, and `protocols::SgrReset`. They are the bytes `writeText` already
  ended styled text with, which were private, so Lightweight's dbtool spelled `"\033[0m"` itself.
- **`core::platform::SignalHandler::nativeHandle()`**: the signal fd as the `NativeHandle`
  `TuiRuntimeOptions::signalFd` takes -- the signalfd on Linux while initialized, `InvalidHandle`
  everywhere else -- so a caller no longer converts `initialize()`'s `int`, which on Windows is the
  wrong type for a handle. `initialize()` keeps its `int` for compatibility (found by the endo
  migration).
- **The end of a terminal's input is reported.** `TerminalInput::inputClosed()`, the virtual
  `runtime::InputSource::inputClosed()` (false by default, so an existing source still compiles),
  `TerminalInputSource`'s forward of it, `TuiRuntime::inputClosed()`, and
  `runtime::testing::ScriptedInputSource::closeInput()` to script it. See *Fixed*.
- **`CORE_CPP_WITH_TUI_OUTPUT`**, an option of `core::tui_output`'s own. With `CORE_CPP_WITH_TUI`
  off and this on, core-cpp builds the styled-output leaf by itself and neither finds nor fetches
  libunicode. It defaults to `CORE_CPP_WITH_TUI` on a first configure, is forced on by it
  (`core::tui` links the leaf), and is forced off under Emscripten. A module's own row switching
  off no longer takes a target with a row of its own down with it: `core_cpp_add_modules()` enters
  the directory for that target alone, as the module table always said a row of its own would.
  `tests/consumer-tui-output` and a `consumer-smoke (tui-output)` CI leg assert the configuration.

### Fixed

- **A parked flow is never resumed inside the call that settled it** -- restores guarantee G2,
  every resumption happens in the loop's drain step (`.agent/rules/async-and-net.md`, "a resource
  never resumes its consumer inline"). `ResultAwaitable::complete()` resumed the waiter on the
  spot, so `PosixSocket::close()` -- and `cancelRead()`, `IocpSocket`'s,
  `WindowsSocket::cancelRead()`, and `CompletionWait::close()` under an IOCP listener -- ran the
  closed read's flow before returning. That flow could run to its end and destroy the object still executing `close()`'s
  caller: contour crashed on it deterministically, in `NativeClient::detach` (`_writer.close();
  _connection->close();`, where the first close resumed `runClient`, which destroyed the client).
  Each now settles the operation at once, with the same value (`Cancelled`, or the data that won),
  and hands the waiter to `EventLoop::resumeSoon`; an awaiter whose frame is destroyed while its
  waiter is queued takes it back with `cancelPending`. The listeners already resumed a closed
  accept through the loop's closed-park list, and TLS's `SerialGate` its waiters since Task B11.
  `CloseResumesThroughLoop_test.cpp` holds it over `BackendMatrix`, contour's crash included.
  - *Migration*: a caller that asserted a parked flow's outcome right after `close()` or
    `cancelRead()` runs one loop turn first (`runOnce`, `runUntilIdle`, a `blockOn`). Two
    `cancelRead()` calls in a row no longer retire the read the first victim arms when it runs: the
    second finds the slot empty (fastcached#1233's shape). `testing::InMemorySocket` and
    `testing::ParkingReadableSocket`, which have no loop, still resume inline.
  - *The socket may be gone when the flow runs, whatever the result*: the waiter resumes later
    in the drain, so an owner that destroys the socket first -- `conn->close();
    connections.erase(id);` -- has destroyed it before the flow sees its result, a `Cancelled`
    one or bytes a read already took. A flow must not touch a socket it does not own after its
    operation resumes (`ISocket::close` says so). The transports touch nothing of it: the
    frame-free ones settled a value that does not refer to the socket, and the coroutine-shaped
    ones ask a lifetime token on every way back and unwind with `OperationCancelled` where they
    used to write into freed storage -- WFMO's `WindowsSocket` (its `_readWaiter`), and the TLS
    layer, whose `feedIn` wrote the ciphertext of a read that settled with data into the freed
    session's BIO, on every backend. Both were a heap-use-after-free under AddressSanitizer.
  - *A listener closed and destroyed in one turn* -- `listener->close(); listener.reset();` --
    woke its parked accept through the loop, and the accept then read the freed listener's
    closed flag and descriptor: `PosixListener`, `UnixListener` and WFMO's `WindowsListener`, a
    defect older than this release. The accept now asks the listener's lifetime token first and
    answers `Cancelled` ("the listener was destroyed"). `IocpListener` keeps what an accept reads
    in state it shares, and was not affected.
  - *Teardown*: `~EventLoop` drains what destroying the spawned roots queued -- a borrowed flow
    whose socket or listener a root owned -- so no flow is left suspended with an operation naming
    a destroyed loop; and a chain nobody owns (a `DetachedTask`) that a socket queued is freed at
    teardown as the loop's own, where it used to be resumed and run on.
  - *For contour, missing from 0.1.0's per-consumer summary*: from 0.1.0 until this release,
    core-cpp's sockets resumed a parked read INSIDE `close()`, so code that closes two sockets in a
    row through an object the read flow owns -- contour's `NativeClient::detach` -- was exposed to
    it. 0.1.0 is released, so the note is recorded here rather than there.
- **On Windows, `listenUnix` and `connectUnix` belong to the loop's transport** (found through
  contour). `listenUnix` built the WFMO `WindowsListener` whatever the loop was, and `connectUnix` a
  `WindowsSocket`, while `listen` and `adoptListener` branch to IOCP -- so an IOCP loop, the Windows
  default, served AF_UNIX through readiness and handed out readiness sockets. It now gets the new
  `IocpListener::bindUnix`, whose `AcceptEx` accepts AF_UNIX connections (measured on Windows 11,
  asserted in CI) and hands out `IocpSocket`, and `connectUnix` adopts its socket onto the loop's
  transport. A WFMO loop is unchanged. The socket-path claim both listeners make moved into a shared
  `windows/UnixSocketPath.cpp`.
- **A hung-up terminal no longer spins the TUI at 100% CPU**
  ([core-cpp#49](https://github.com/contour-terminal/core-cpp/issues/49), found by the tuidu
  migration) -- restores the input wait's promise that it waits: with `SIGHUP` ignored, a terminal
  that hangs up leaves its input readable for ever, each read answering EIO or an end of file; the
  runtime's input flow read nothing, re-parked, and was resumed at once, every turn, and the
  process never exited. `TerminalInput::readReadyInput()` now tells the end from "nothing yet" -- a
  read error other than `EAGAIN`, an end of file on a pipe or a file, an end of file on a terminal
  that `poll(2)` reports hung up, a Windows console input handle that can no longer be read -- and
  the runtime then stops watching the handle, delivers what was already read, and ends its input:
  `nextEvent()`, `nextEventFor()` and `nextActivity()` throw `core::async::OperationCancelled`
  without parking once `TuiRuntime::inputClosed()` is true. The same applies when the loop refuses
  the input handle (`FdRegistrationFailed`), where the input flow used to return and leave a
  `nextEvent()` waiting for ever. tuidu fixed the POSIX half in its own copy (tuidu `c20bcac`) and
  it never reached endo, so core-cpp did not have it.
  - *Migration*: **a consumer that treats a cancelled or empty read as "try again" must treat the
    end of input as final**, or the spin moves from the runtime into its own loop: it asks
    `inputClosed()` and exits. endo's `Prompt::read` is the example -- it catches
    `OperationCancelled` and returns an empty line, and its REPL reads again for as long as the
    prompt is ready. tuidu's `runModal` (`tui/runtime/Modal.hpp`) is the other: it returns
    `std::nullopt` on the cancellation, so a caller that shows the modal again on `nullopt` spins
    the same way.
  - `TerminalInput::poll()` records the end in `inputClosed()` too (a hangup with nothing to read on
    POSIX, a failed wait on Windows), but a loop driven by `poll()` itself must ask it; nothing ends
    that loop for it.
- **Piped output no longer carries synchronized-output sequences** -- restores `SyncGuard`'s
  purpose, bracketing a frame for the TERMINAL that renders it. `TerminalOutput::syncGuard()`, and
  a `SyncGuard` constructed directly, wrote `CSI ? 2026 h` / `l` whatever the destination was, so
  every caller had to test `isTerminal()` and choose between a guard and none, and one that did not
  wrote escape sequences into a pipe or a file. The guard now asks the output's `isTerminal()` once
  and writes the sequences only when it answers true; it still flushes at both ends.
  - *Migration*: a capture that wants the sequences answers `isTerminal()` true from its subclass,
    as a terminal-emulating capture already should.
- **`tools/migrate/rewrite.py` rewrites the code after a character literal holding a `"`** (found by
  the contour migration). Its scanner read `'"'` as opening a string, so in
  `os << '"' << crispy::escape(s) << '"'` the symbol was masked as data and left unrewritten. A
  character literal is now one character or one escape sequence, which also keeps a digit
  separator's quotes (`1'000'000`) from being read as one. `renames.json` gains the
  `crispy::Overloaded` -> `core::Overloaded` row the migration guide already listed (contour).
- **A vendored copy configures with `CORE_CPP_TESTING` on** (found by the contour migration). The
  top-level `CMakeLists.txt` added `tests/` unconditionally, and the vendoring file set leaves
  `tests/` out, so the module suites the copy carries (`src/core/**/*_test.cpp`) could never be
  switched on. `tests/` is now added when it exists; a copy without it registers the module suites
  and says so. `docs/vendoring.md` says the same, and the `consumer-smoke (vendored)` CI leg builds
  and runs the exported copy's module suites.
- **The migration table sends the completion types to `core::tui::completer::`**
  ([core-cpp#48](https://github.com/contour-terminal/core-cpp/issues/48), found by the endo
  migration). endo and tuidu declare `CompletionItem`, `CompletionProvider`, `Completer`,
  `CompletionConfig`, `FuzzyMatch`, `FuzzyMatchResult`, `FuzzyConfig`, `SmartCaseMatch` and
  `SmartCaseConfig` in `namespace tui`; `tools/migrate/renames.json` had no row for them, so the
  `tui` namespace row rewrote them to `core::tui::`, which does not compile. Each has a symbol row
  now, and `check_renames_test.py` fails without them. The provenance record of
  `src/core/testing/SuppressWindowsDialogsAtStartup.cpp` no longer calls it a verbatim copy of
  endo's: endo's product variant, which suppressed only under ctest, was dropped.
- **A consumer of `core::tui_output` alone no longer fetches libunicode** (found by Lightweight's
  `dbtool`). The leaf was gated on `CORE_CPP_WITH_TUI`, the same option as the libunicode row, so
  the only configuration that built it also fetched libunicode and, through libunicode's own
  configure, `UCD.zip` from `www.unicode.org` -- for a target that links `core::base` alone.
  `CORE_CPP_WITH_TUI=OFF` with `CORE_CPP_WITH_TUI_OUTPUT=ON` is the configuration `dbtool` wants.

## [0.2.0] - 2026-09-24

### Breaking

- **The `await_ready` of seven public awaiters answers a `constexpr` constant `false`**:
  `core::net::DelayAwaiter`, the awaiter `whenAll` and `whenAny` return, `AsyncQueue<T>::PopAwaiter`,
  and the four `TuiRuntime` awaiters (which also became `noexcept`). It is the fix for
  fastcached#1546 under *Fixed*: the decision it made is `await_suspend`'s now. (`Task`'s two
  awaiters answer as before -- true for a task owning no frame or already finished -- but from a
  member their constructor sets, and are not on this list.) A `co_await` behaves as before; code that called `await_ready()`
  directly to learn whether an await would park gets `false` where it got `true` -- for instance
  `sleepUntil(nullptr, t).await_ready()` -- and core-cpp's own `SleepUntil_test.cpp` and
  `AsyncQueue_test.cpp` asserted exactly that. It stays a `const` member rather than a `static`
  one, which clang-tidy's `readability-static-accessed-through-instance` would report at every
  `co_await` in a caller's code. `ResultAwaitable::await_ready` still answers whether the operation
  settled inline.

  Migration: `co_await` the awaiter, and never call `await_ready()` on it directly; it is the
  compiler's half of the protocol, not a question a caller can ask. A test that asserted an await
  resolves without parking asserts it through the flow instead: that it has finished after one
  turn of the loop, or that its continuation ran exactly once. fastcached's `SleepUntil_test.cpp`
  asserts `true` on its own copy, and changes with the migration.

### Fixed

- **`EventLoop::runUntilIdle` and `testing::TestLoop::drain` no longer return while the waiters of
  a closed handle are still queued.** `notifyHandleClosing` records the parks on a closing handle
  and the next turn queues their waiters after its wait; that turn still reported itself idle,
  because none of its counters counted them, so a drain returned with a closed listener's pending
  `accept` -- or any flow parked through `waitReadable`/`waitWritable` -- woken and not yet run. A
  caller that tore its objects down next left `~EventLoop` to resume or free those frames after
  their owners were gone: fastcached's server teardown reported it as a heap-use-after-free under
  ASan and TSan. A turn is now idle only if it also leaves the ready queue empty, which covers
  every step that queues work rather than the four that had a counter. `ClosedParkIdle_test.cpp`
  closes a listener under a parked accept and asks one `runUntilIdle` to finish it.

- **An `OperationCancelled` thrown out of a `co_await` on `delay` or `sleepUntil`, and on twelve
  other awaiters, is caught again under MSVC 19.44 on ARM64**
  ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)). That compiler's
  ARM64 code generator drops the enclosing `try` of a `co_await` on a temporary awaiter whose
  `await_ready` makes a call, so the exception passed a typed `catch` and `catch (...)` alike and
  reached whatever awaited the flow. The new `windows (cl-release-arm64)` leg then found the same
  loss with no call at all: an `await_suspend` that returned the awaiting coroutine's own handle,
  followed by an `await_resume` that threw -- `Task`'s awaiter over a task owning no frame, whose
  `std::logic_error` passed the awaiting coroutine's `catch`. So `Task`'s two awaiters decide in
  their constructor, where the awaiter is made by the `co_await`, and `await_ready` reads the
  answer. `ResultAwaitable`, every socket operation's awaitable, transfers back into an
  `OperationCancelled` for a flow already stopped too; on the same leg it keeps its handler, which
  `CancelRead_test.cpp` now holds. `core::net::DelayAwaiter::await_ready` read the clock through
  the virtual `IClock::now()`. The 0.1.0 notes say this awaiter carried fastcached's fix when
  `TuiRuntime` moved onto it; it did not, and every `delay` and `sleepUntil` had the shape. Every
  `await_ready` in `src/core` now reads a member or answers a constant, and the decision it made is
  the constructor's or `await_suspend`'s, asked before the flow's stop token is read, so what a `co_await` observes is
  unchanged (a direct call of `await_ready()` is not; see *Breaking*): an elapsed deadline, a null loop, a finished task, an empty `whenAll`, a queued item,
  a free TLS gate, a finished lookup and a buffered input event or agent message all resume without
  parking, and a flow that is already stopped still resumes normally on each of them. The awaiters:
  `DelayAwaiter`, `interruptibleSleepUntil`'s, `Task<T>::Awaiter` and `Task<void>::Awaiter`,
  `whenAll`'s and `whenAny`'s, `AsyncQueue::PopAwaiter`, `ResultAwaitable`, the threaded resolver's
  and the TLS serial gate's, and `TuiRuntime`'s `NextInputEventAwaiter`, `NextEventForAwaiter`,
  `NextActivityAwaiter` and `NextAgentReadyAwaiter`.
- **A socket a listener accepts carries `TCP_NODELAY`, as a dialled one always did.** Only the
  dial paths set it (`posix/DialPrimitives.cpp`, `windows/DialPrimitives.cpp`, and through them the
  completion-port dial); `accept4`/`accept` on POSIX, the WFMO listener's `accept` and the IOCP
  listener's `AcceptEx` handed out sockets with close-on-exec and nothing else, so a server's small
  replies waited on Nagle for the client's delayed ACK. Every dial and every accept on every
  platform now goes through one helper, `detail::applyStreamSocketOptions`, which sets close-on-exec
  (a non-inheritable handle on Windows), `TCP_NODELAY`, and keepalive when a dial asks for it; the
  buffer sizes below are `detail::applySocketBufferSizes`'s, before the connection exists. `WindowsSocket::native()` joins `PosixSocket::native()` and
  `IocpSocket::native()`, for diagnostics and tests.
- **The WFMO backend's TCP listener claims its port exclusively.** It bound with `SO_REUSEADDR`,
  which on Windows lets a second socket bind a port a live listener serves and take its
  connections, where the IOCP listener has always used `SO_EXCLUSIVEADDRUSE`. It uses that too
  now, and fails the bind if the option is refused. `BackendKind::Wfmo` is not the Windows
  default, so only a caller that asked for it by name was exposed.
- **`testing::TestLoop::pendingSubmissions()` and `pendingTimers()` count what was handed over
  between turns.** A `submit` or `schedule` from a thread that is not the loop's worker -- the
  case's own thread outside a turn included -- goes to the inbound queue until turn step 1, and the
  two counters read only the ready queue and the park table, so a case that submitted and then
  counted read 0 (at least 12 of fastcached's cases, on migration). They now add the inbound
  submissions and scheduled deadlines, read under the inbound lock through two new
  `EventLoop` accessors, `inboundSubmissionCount()` and `inboundScheduledCount()` --
  `cancelPending` already searched that queue, so the answer no longer depends on which thread
  submitted. Both counters lost `noexcept`: taking the lock can throw.

### Changed

- **A `PosixSocket` keeps one backend registration for its life, not one per parked operation.**
  Every read or write that parked attached a registration, armed it and detached it again -- on
  epoll an `EPOLL_CTL_ADD` and an `EPOLL_CTL_DEL` per request -- and fastcached's GET benchmark ran
  6.8% slower on `EventLoop` than on its own epoll reactor (geomean of 24 scenarios, -22% at the
  worst). The socket's parks now ask for `RegistrationLifetime::UntilClosed`: the loop registers the
  descriptor the first time it is parked on, a park takes a slot on that registration and changes
  what it is armed for only when it must, and `close()` ends it by announcing the close, as it
  already did. Readability stays armed after a read completes, so the steady state of a
  request/response connection costs no `epoll_ctl` at all; writability is dropped as soon as the
  write is taken, and a readiness report nobody is parked to take narrows the registration after
  that wait. On a loopback echo over epoll (WSL2, clang-22 Release, median of 5) the server's CPU
  per request fell from 9.3-10.5 us to 5.1 us, and its throughput rose from 96-106k to 193-195k
  requests a second at 16, 64 and 256 connections; a raw epoll echo, the floor, is 4.2 us. The
  turn is unchanged. `SocketRegistration_test.cpp` counts the backend calls, and fails with the
  per-park registration.
- **An operation parked on a socket allocates nothing in `EventLoop`, and a turn nobody handed
  work to takes no lock.** Each park was a fresh allocation, filed in a `std::unordered_map` and,
  whatever its registration, in a second map by handle: three allocations and three frees per
  parked operation. Parks are now recycled and kept in an open-addressing table, and a park on a
  socket's lifetime registration is found through that registration rather than the handle map. A
  turn skips the inbound mutex when nothing was posted and the timer heap when no deadline is armed,
  `stop()` sets an atomic, `ResultAwaitable` registers no stop callback on a token that can never
  be stopped, a queued entry no longer moves an empty work item through a temporary, and
  `EpollBackend` no longer zeroes a 768-byte event array on every wait. On fastcached's GET at 16
  connections (WSL2, clang-22 Release, median of 5, the daemon's CPU per request), memcached text
  went from 24.1 to 23.4 us and RESP from 26.9 to 25.0 us, against 23.7 and 25.7 us on fastcached's
  own epoll reactor, and allocations per request from 10.1 to 7.1 and from 26.1 to 23.1.

### Added

- **`CORE_CPP_MSVC_STATIC_RUNTIME_VARIANTS`** (OFF): with an MSVC-ABI compiler, every compiled module
  but `core::testing_main` (whose Catch2 and dialog suppression are built `/MD`, and whose target
  is edited after it is declared) also gets a static-CRT twin, `core::<name>_mt` (target `core-cpp-<name>-mt`), built `/MT` or
  `/MTd` and linking the twins of the modules it links (header-only modules are shared as they
  are). The MSVC linker refuses to mix C runtimes (`LNK2038 ... 'RuntimeLibrary'`, or lld-link's
  `/failifmismatch`), so one build of core-cpp could not serve both fastcached's `/MD` daemon and
  its `/MT` launcher, fastcache-cc; with the option on it can. The twins are declared by
  `core_cpp_add_module` from the module table, so they follow it; they join `CORE_CPP_TARGETS`,
  and are `EXCLUDE_FROM_ALL`, so a parent builds only those it links. Other compilers ignore the
  option with one status line. Two tests pin it on Windows: a `/MT` program linking `core::net`
  and `core::log` fails to link, naming `RuntimeLibrary`, and the same program linking
  `core::net_mt` and `core::log_mt` links and runs a loopback echo.
- **`RegistrationLifetime`** (`<core/net/detail/ParkTable.hpp>`, through `<core/net/EventLoop.hpp>`),
  and a `lifetime` field of it on `ParkEntry`, which `ParkEntry::onReadyCallback` takes as a new
  trailing parameter with a default. `PerPark`, the default, is the registration every park had.
  `UntilClosed` shares one registration per handle among every park on it that asks, kept until
  `EventLoop::notifyHandleClosing` names the handle -- so a caller that asks for it promises to
  announce every close, which is what `PosixSocket` does and what it now asks for.
- **`testing::ScriptedBackend::pushReadiness(HandlerId, Readiness)`**: a scripted wait that reports
  several conditions at once, as one kernel answer does (`EPOLLIN|EPOLLOUT`), so a case can reach
  the choice a backend makes between two watched directions reported in the same wait.
- **`adoptSocket(EventLoop&, platform::NativeHandle, std::string peerAddress)`**, beside
  `adoptFd` and `adoptListener` in `<core/net/Sockets.hpp>`: a connected socket accepted or
  dialled outside core-cpp, driven by the loop the caller chooses. It is what a Windows server
  needs to spread connections over several loops -- one thread accepts and deals each handle out,
  since Windows has no `SO_REUSEPORT` and a completion-port association is one socket to one port
  -- and `adoptFd` answers `Unsupported` there. It builds the socket the loop's backend drives
  (`IocpSocket` where the loop lends a completion port, `WindowsSocket` under WFMO, `PosixSocket`
  on POSIX), takes ownership of the handle on every path and closes it when adoption fails --
  allocating the wrapper throwing included (the opposite of `adoptListener`, which leaves a refused
  handle with its caller), changes no socket
  option beyond what the transport needs to run (non-blocking mode on POSIX), and asserts it is
  called on the loop's thread. `adoptFd` is unchanged. Under WFMO, a readiness event that cannot
  be created or associated is an error value, through the new `WindowsSocket::adopt`; the
  `WindowsSocket` constructor, which `connect` and the WFMO listener still use, can only carry on
  with a socket that never becomes ready.
- **`SocketBufferSizes`** (`<core/net/SocketBuffers.hpp>`), and a `buffers` field of it on both
  `ListenOptions` and `DialOptions`: the kernel send and receive buffers (`SO_SNDBUF`,
  `SO_RCVBUF`) of every socket a listener accepts, and of one dialled socket. Each size is a
  `std::optional<std::size_t>`, and an unset one leaves the kernel's value untouched, which is the
  default. fastcached sizes both to 1 MiB so that a large reply leaves in one `sendmsg`. The sizes
  are asked for before the connection exists -- of the listening socket before `listen`, which its
  accepted sockets inherit (an `AcceptEx` socket included), and of a dialled socket before
  `connect` -- because the TCP window scale is announced in the handshake and tcp(7) asks for them
  first. A size is a request: Linux reports twice what was set and caps an
  unprivileged request at `net.core.wmem_max`/`rmem_max`. `PosixListener::bind` takes two new
  trailing parameters with defaults, `PortSharing sharing` (below) and then the sizes;
  `WindowsListener::bind` and `IocpListener::bind` take the sizes; and `PosixListener`,
  `WindowsListener` and `IocpListener` gain `native()`, as the sockets have. The internal
  `detail::DialStep`, `detail::dialReadiness` and `detail::dialCompletion` take a
  `detail::StreamSocketOptions` where they took a `KeepAlive`.
- **`ListenOptions::sharing`**, a `PortSharing` defaulting to `PortSharing::Exclusive`. With
  `PortSharing::Shared`, several listeners may bind one port; fastcached's daemon binds one per loop
  by default, and without it the second loop's bind failed with `AddressInUse`. Whether the
  connections are spread across them is the platform's: Linux spreads them (`SO_REUSEPORT`), and
  FreeBSD does with `SO_REUSEPORT_LB`, which `listen()` uses wherever the constant is defined. On
  macOS and the other BSDs the binds coexist and the newest listener gets every connection
  (`SO_REUSEPORT`), so one listener per loop leaves all but one loop idle there; accepting on one
  and handing sockets out with `adoptSocket` is what spreads them. On Windows a shared listener is refused with `NetErrorCode::Unsupported` rather
  than mapped to `SO_REUSEADDR`, which there lets a later socket take a held port over. It is the
  UDP sockets' existing enum, so `<core/net/Sockets.hpp>` now includes `<core/net/UdpSocket.hpp>`.
  `tools/migrate/renames.json`'s `core::net::ReusePort` row points at it.
- **`core-cpp.await-ready`**, a `tree-level` check with a self-test (`scripts/check-await-ready.py`,
  run by the `style` job), refusing an `await_ready` body under `src/` or `tests/` that calls a
  function or constructs an object, and an `await_suspend` returning a coroutine handle that
  returns the handle it was given, unless its row names the test that runs it on the ARM64 leg. It cannot see an overloaded operator; the fixed public awaiters
  also carry a `static_assert(core::async::awaitReadyIsConstantFalse<A>())`, new in
  `<core/async/Awaitable.hpp>`, which asks the question at compile time without constructing an
  awaiter (P2280), so a call there fails to compile on GCC 14, Clang 20 and MSVC 19.51 or newer;
  on older compilers it asserts nothing.
- **The `cl-release-arm64` preset and the `windows (cl-release-arm64)` CI leg**, on
  `windows-11-arm`: MSVC's ARM64 code generator is the only one that miscompiles the shape above,
  and no other leg can observe it. The preset expects an arm64 developer shell.

## [0.1.0] - 2026-09-23

The first release: the shared C++23 foundation of the Contour Terminal projects, in namespace
`core`, replacing the copies of the same code that contour, endo, fastcached and tuidu each carry.

**What it is made of.** Imported, and recorded file by file in
[`.agent/reference/provenance.md`](https://github.com/contour-terminal/core-cpp/blob/master/.agent/reference/provenance.md) (see *Imported* below for each
import and what changed on the way in): contour at `6777ff05014f8ff163b071e8b0e942830119db80`
(crispy's generic half, `src/coro`, `src/net`; 110 files), endo at
`f774a210ce989e5947b8f61d715068b1dc96088c` (the generic half of `src/platform` and `src/tui`; 212
files), and fastcached at `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21` (its async and networking
layer; 103 files, plus 8 at three earlier commits). 97 files were written here.

**The merged design.** contour's coroutine and networking layer and fastcached's are one design
now, not two side by side: one `EventLoop` with a five-step turn and a six-step teardown over an
injected `IoBackend` (poll, epoll, kqueue, an I/O completion port as the Windows default, WFMO kept
one release, and a host-driven backend for WebAssembly); one park table and one deadline mechanism;
one frame-free, stop-aware socket contract with one read and one write operation per socket; one
TLS layer; one socket-error table per platform; and fastcached's ownership rules for parked work,
teardown and cancellation. The rules and the fastcached issue behind each are
[Coroutines and lifetimes](https://contour-terminal.github.io/core-cpp/design/coroutines-and-lifetimes/); the threading guarantees are
[Threading](https://contour-terminal.github.io/core-cpp/design/threading/).

**Breaking, for the projects that carried a copy.** Every entry under *Breaking* below names what
a caller changes. `tools/migrate/renames.json` and the codemods beside it apply the mechanical ones
(every rename row, and a `removed` row for every symbol that has gone), and
`.agent/guides/consumer-migration.md` is the procedure. A consumer meets the breaks of every copy
it carries, so the list is by copy first and by consumer after it; every item is an entry under
*Breaking*, where its migration is.

- **A copy of contour's `src/crispy`** (contour, and endo, which fetches it): the namespace
  (`crispy::` is `core::`); `Flags::operator&=` intersects rather than clearing, a reversal
  invisible at the call site; `FNV`'s byte-wise overload refuses a type with padding;
  `base64::decodeLength()` answers the tight bound; `readFileAsString()` answers exactly the bytes
  on disk and an empty string for a missing file; `<core/Escape.hpp>` and `readFileAsString()`
  are `[[nodiscard]]`; `Times2D::operator[]` answers the tuple `*it` yields;
  `joinHumanReadableQuoted()` takes a `std::string_view` separator.
- **A copy of contour's `src/coro`** (contour, endo, tuidu, and fastcached's `vendor/endo`): the
  namespace (`coro::` is `core::async::`); `Task`'s `operator co_await` is rvalue-only and the
  awaiter owns the frame, so a task awaited with `std::move` is empty afterwards; `result()` of a
  task that owns no frame throws `std::logic_error`; `whenAll()`/`whenAny()` take their tasks by
  rvalue; `whenAny()` resolves to `std::optional<std::size_t>`.
- **A copy of contour's `src/net`** (contour, and endo, which fetches it):
  - the loop: the namespace (`net::` is `core::net::`); `EventSource` is `IoBackend` and the loop
    is `EventLoop` over it, with a resumption one turn after readiness, a `blockOn()` that blocks,
    a destructor that frees what the loop owns, and `WaitFdAwaiter` as `WaitHandleAwaiter`;
    `FdInterest::None` mutes on every backend; a socket still parked when its loop is destroyed is
    never resumed, so sockets die first;
  - the socket: `ISocket`'s operations are awaitables rather than `Task`s, and a caller that
    stores one wraps it in `asTask`; `shutdownWrite()` returns one; a stop of the flow's own token
    throws `OperationCancelled` out of a socket operation, and a read parked when `close()` came
    reports `Cancelled`, not `BadHandle`; `isClosed()` latches the peer's EOF; `WriteQueue`
    refuses a null socket;
  - the errors: `NetErrorCode::Other` is `SystemError` and renders `"system error"`; `EPIPE` on a
    write is `SystemError`, not `ConnReset`; a TLS peer that closes without `close_notify` reads
    as `ConnReset`, not `0`;
  - TLS: `wrapTls()` and `ITlsContext::wrap()` take the loop; `generateSelfSignedCertificate()` and
    `makeSelfSignedServerContext()` take a `SelfSignedOptions`, and the **default common name is
    `"localhost"`, not `"contour-daemon"`** -- contour's `src/vthost/Daemon.cpp` calls
    `makeSelfSignedServerContext()`, so the certificate it serves changes name unless it passes
    `{ .commonName = "contour-daemon" }`;
  - the dial: `connect()` no longer resolves on the calling thread, takes its host as a
    `std::string`, throws `OperationCancelled` on the flow's stop and reports an unresolvable name
    as `AddressError`; `IListener::localPort()` is `boundPort()`.
- **A copy of endo's `src/platform`** (endo, tuidu, and fastcached's `vendor/endo`):
  `FileSystem::openWrite()` and `copyFile()` take `WriteMode` and `OverwritePolicy` rather than a
  `bool`; `testing::InMemoryFileSystem` keeps a stream across `remove()` and `rename()`, as POSIX
  does; `TestEnvironmentProvider` is in `core::platform::testing`.
- **A copy of endo's `src/tui`** (endo, tuidu, and fastcached's `vendor/endo`): `TuiRuntime` is
  composed on `core::net::EventLoop` -- constructed over a loop, destroyed before it, with the
  `runtime::EventSource` family gone -- so the `EventLoop` and `IoBackend` items above reach a
  consumer through the TUI too; the completion types are in `core::tui::completer`;
  `LanguageId::Endo` and `registerEndoHighlighter()` are gone, and endo registers its language
  with a `SyntaxHighlighterRegistry` it owns.
- **fastcached's `src/FastCache/{Async,Net}`**: the camelBack spellings (`renames.json` has every
  one); `IReactor` and its reactors as `EventLoop` and `IoBackend`s; `IAdmissionControl` as one
  `tryAdmit()` and a lease; `NetErrorCode::BadFileHandle` as `BadHandle`; `NetError::toString()`
  in contour's shape rather than `NetError(code=...)`; `TlsContext::CreateSelfSigned()` and
  `Create()` as `makeSelfSignedServerContext()` and `makeTlsServerContextFromFiles()`.

So: **contour** meets the crispy, coro and net items; **endo** meets all of contour's, because it
fetches contour's copies, and the platform and tui items; **tuidu** meets the coro, platform and
tui items; **fastcached** meets its own, and the coro, platform and tui items through
`vendor/endo`.

### Added

- **Gates that hold the merged design's rules, each watched refusing** (Task B13). Ported from
  fastcached (0708dd54) with self-tests: `core-cpp.cancel-read-declared` refuses a transport that
  inherits `ISocket::cancelRead`'s no-op, and `core-cpp.read-buffer-guard` an `ISocket::read` that
  does not call `contract::requireReadBuffer` before its first return (or delegate). New:
  `core-cpp.loop-affinity-canary.*`, one process per loop-thread-only `EventLoop` member, judged
  on the assertion's own text; `core-cpp.hostdriven-canary.closedPark`; `core-cpp.layering` now
  refuses an `#include` of a module the including module's row does not reach, and checks that the
  build and the vendoring tool read the same module table; `core-cpp.text-encoding` refuses
  invalid UTF-8, U+FFFD and double encoding in any tracked text file (core-cpp#40);
  `core-cpp.ambient-reads` refuses a direct clock or environment read outside its seam; and
  `core-cpp.tree-level-coverage` now also refuses a workflow whose `ci-ok` does not need `style`
  (core-cpp#39). `scripts/tidy-record.py` refuses a clang-tidy result that carries no instrument
  record, and the `clang-tidy` job records its build through it.

- **CI retries the toolchain installs that fail for reasons no change can cause**
  (`scripts/ci-retry.sh`, core-cpp#42): apt, the apt.llvm.org script and Homebrew, three attempts,
  each retry a warning on the log; each step still asserts what it installed.

- **Datagrams** (`<core/net/IDatagramSocket.hpp>`, `<core/net/UdpSocket.hpp>`,
  `<core/net/SharedPortDatagram.hpp>`). `openUdpSocket(bindAddress, port, BroadcastMode,
  PortSharing)` opens a UDP socket and answers WHY a bind failed, where fastcached's returned a null
  pointer; its receive buffer is the largest datagram the bound family can carry
  (`MaxIpv4DatagramPayload`, `MaxIpv6DatagramPayload`), and a datagram longer than that is dropped
  and answered `DatagramWait::MessageTooLarge` rather than handed back truncated (fastcached's
  buffer was 8 KiB); a datagram too large for the path is `NetErrorCode::MessageTooLarge` at the
  send rather than `SystemError`. `answerFromOwnAddress()` and
  `openSharedPortUdpSocket()` hear a segment on a shared port and send, and are answered, from an
  address only this node holds -- because a unicast to a shared port reaches one socket, and which
  one differs by platform.

- **Transports for threads that may block** (`<core/net/BlockingSocket.hpp>`,
  `<core/net/BlockingConnector.hpp>`, `<core/net/TcpClient.hpp>`, `<core/net/HealthProbe.hpp>`).
  `BlockingSocket` answers every verb before its awaitable exists, so `core::async::syncRun` drives
  it; its `waitReadable` blocks rather than answering `1` at once, and a non-positive
  `setReceiveDeadline` removes the bound. `BlockingConnector` is an `IConnector` over the shared dial
  flow that waits on the calling thread within its budget and arms
  `BlockingConnectorOptions::ioTimeout` before it hands the socket over. `connectTcp`, `sendAll` and
  `receiveExactly` are the one TCP client. `probeHttpStatus()` returns the status an HTTP endpoint
  answered, and `httpHealthProbe()` whether it was 200; the status line is parsed strictly, so a
  500 page mentioning "200 OK" is not healthy.

- **A deterministic socket fake, pinned to a real socket** (`<core/net/testing/InMemorySocket.hpp>`).
  `InMemorySocketPair::create()`, `InMemoryListener` and the pipe beneath them: no descriptor, no
  loop, every answer inline. **In every closed state it answers the way a loopback TCP socket does,
  and `SocketClosedStates_test.cpp` checks that** by running one table of steps over the fake and
  over a real pair: a write to a peer that has gone fails, a close over unread bytes is a reset, a
  half-close still reads. Unlike fastcached's, `waitReadable` parks, a bounded pair makes a write
  wait for the reader rather than fail with `WouldBlock`, and a second concurrent read or write trips
  the socket contract's slot guard as it does on every real socket. It is NOT
  `testing/InMemoryTransport.hpp`, whose `makeSocketPair()` is a pair of real sockets, and the two
  are kept apart on purpose: merging them would delete the difference the parity test measures.

- Test doubles: `testing::DatagramBus` (`<core/net/testing/InMemoryDatagram.hpp>`), a network
  segment in one process with scripted loss; `testing::SocketDecorator`, which forwards every verb,
  zero receive deadlines included; `testing::ParkingReadableSocket` and
  `testing::ParkingWritableSocket`, which park until the test decides and count how each parked
  operation ended.

- **One TLS layer, `core::net_tls`: fastcached's record pump behind contour's `ITlsContext` seam**
  (Task B11). `TlsSocket` now overrides the three verbs it inherited: `handshakeIfNeeded()` drives
  the handshake to completion, so an accept loop -- `core::net::serve` included -- frames its first
  byte after it; `waitReadable()` decrypts through `SSL_peek` and answers `0` for a `close_notify`
  rather than reporting the alert record as data, and parks rather than answering at once;
  `shutdownWrite()` writes and flushes `close_notify` and only then half-closes the transport, so a
  strict peer reads an orderly end rather than a truncation. New in `<core/net/Tls.hpp>`:
  `SelfSignedOptions` (common name, subject names, validity), `makeTlsServerContextFromFiles()`,
  `certificateFingerprint()` of a PEM certificate and `ITlsContext::certificateFingerprint()` of a
  context's own. `<core/net/ITlsContext.hpp>` holds the seam in `core::net` itself, with
  `wrapTls(socket, context, loop)`, which hands the socket back unchanged for a null context -- so an
  accept path compiles identically in a build without TLS. `core::net::testing::StrictTlsPeer`
  is the layer's test double: OpenSSL driven by hand, reading a FIN with no `close_notify` as the
  truncation OpenSSL 3 calls it.
- **`core-cpp.openssl-seam`, the gate for "no OpenSSL type appears in any header"** (Task B11). The
  design spec required it and nothing checked it. It refuses an OpenSSL include outside `Tls.cpp`
  and `testing/StrictTlsPeer.cpp`, and an OpenSSL type named in any header -- a forward-declared
  `struct ssl_st` included, which is what fastcached's TLS headers did. Each permitted unit must be
  seen including OpenSSL, or the row is refused as stale; `core-cpp.openssl-seam-selftest` proves
  twelve verdicts. Both are `tree-level` and run in the `style` job.

- **The dial, and the seam that keeps DNS off an event loop's thread** (`<core/net/IConnector.hpp>`,
  `<core/net/IAsyncAddressResolver.hpp>`, `<core/net/ThreadedAddressResolver.hpp>`,
  `<core/net/SocketAddress.hpp>`, `<core/net/ConnectFlow.hpp>`, `<core/net/ReadinessDial.hpp>`,
  `<core/net/KeepAlive.hpp>`, `<core/net/SocketDeadline.hpp>`, `<core/net/IAdmissionControl.hpp>`).
  `IAdmissionControl` caps concurrent connections with one atomic `tryAdmit()`, which returns an
  `AdmissionLease` whose destructor gives the slot back; `CountingAdmissionControl` fixes its cap
  at construction.
  `makeConnector(loop, resolver)` builds an `IConnector` whose sockets are pinned to `loop`;
  `connect(host, port, DialOptions)` resolves through the injected `IAsyncAddressResolver`, budgets
  the whole call, tries every candidate address in preference order and reports the last failure.
  `ThreadedAddressResolver` is the shipped resolver: a fixed pool of two threads, a bounded queue
  that is refused rather than waited on, and a fast path that never hands a LITERAL address to a
  thread at all -- so a process that only dials literals creates no thread.

  **The budget covers resolution, and so does a stop.** A lookup against a nameserver that drops
  packets does not come back for the platform's whole retry schedule -- about 30 s with glibc's
  defaults -- so `DialOptions::connectTimeout` RACES resolution against its deadline rather than
  checking it once the lookup returns, and a dial parked on a lookup ends at the deadline with
  `NetErrorCode::Timeout`. What makes that possible is a duty `IAsyncAddressResolver::resolve`
  documents: a resolver that suspends must honour the awaiting flow's stop token, resuming it on
  its loop with `core::async::OperationCancelled` and discarding the answer that arrives later.
  `ThreadedAddressResolver` does. The lookup itself cannot be interrupted -- `getaddrinfo` has no
  cancellation -- so a stop ends the WAIT and the worker finishes in its own time; the same is what
  lets `whenAny(connect(...), delay(...))` and a loop's shutdown end a dial that is resolving.

  **The seam is the deliverable, not the thread.** A test injects an `IAddressResolver` and can say
  what resolution costs and observe which thread paid for it; `InlineAddressResolver` is the
  never-suspending implementation for a caller already on a thread that may block.

- **A dial checks `SO_ERROR`, and never which callback fired.** Readiness is not success: a refused
  connect also makes a socket ready, and which of readable, writable or failed the kernel reports is
  not portable -- on Linux a failure can arrive with neither direction set, while on macOS the write
  filter fires with `EV_EOF` and the backend reports the socket WRITABLE. A dial that trusted the
  callback would hand its caller a socket whose first write fails, on one platform only.
  `ReadinessDial_test` and `Connector_test` assert a refusal reaches the caller as
  `NetErrorCode::ConnRefused` on every backend the platform builds.

- `listen(loop, ListenOptions)`, the named form of the bind, and `adoptListener(loop, handle)` for a
  listening socket this process did not create -- one inherited from a supervisor, or one a test
  bound for itself. It prepares the handle (non-blocking and close-on-exec on POSIX, a readiness
  event on Windows) and asks the kernel which port it is on, rather than taking one on trust. On
  failure the caller still owns the handle, on every platform.

- `core::tui::runtime::InputSource`, the TUI runtime's one dependency-injection seam now that the
  waiting is `core::net::EventLoop`'s: it names the handles to watch and decodes what is ready
  behind them, and has no `wait()`. `TerminalInputSource` is the production implementation over a
  `Terminal` -- header-only, with no platform body, because everything it asks is already portable
  through `TerminalInput`. `core::tui::runtime::testing::ScriptedInputSource` is the test double,
  which scripts decoding alone and can be pointed at a `core::platform::SystemPipe` so the same
  case runs over every backend a platform builds. `TuiRuntimeOptions` carries the interrupt
  wakeup, the POSIX signal fd and the escape-flush interval, and `TuiRuntime` gains `loop()` and
  `notifyAgentReady()`.

- `core::tui` has **no translation unit that chooses its platform with an `#ifdef`, and no platform
  directory under `runtime/` at all.** `runtime/PollEventSource.cpp` was the last one, and the two
  `TerminalEventSource` bodies were the last of those directories; the multiplexing all three held
  is the event loop's, on every platform. `.agent/rules/platform.md`'s rule -- an OS difference is
  an injected implementation, never an `#ifdef` in logic -- holds here by construction rather than
  by review.

- `core::async::asTask(awaitable)` (`<core/async/AsTask.hpp>`) — wraps any awaiter in a `Task`, for
  the one caller shape an awaitable cannot serve: one that must store the operation, keep it across
  a suspension point, or hand it to a combinator. It costs a coroutine frame, so it is an explicit
  call rather than an implicit conversion.

- `ctest -R socket-contract-canary` — three processes (`read-slot`, `write-slot`,
  `empty-read-buffer`) that drive a REAL socket into each of the socket contract's Debug guards and
  must die. Each is judged on a marker naming its own mode, printed immediately before the guarded
  call, and fails on a marker printed after it: a process that died on its way to the call is then
  not read as a guard that fired. They skip (77) where assertions are compiled out. Writing them
  found a defect they now guard: an operation created and never awaited left the socket's slot
  naming freed storage,
  and the next `close()` dereferenced null — in Release, where the guard that catches the usual
  spelling is compiled out.

- A syntax-check for platform sources this configuration does not otherwise compile
  (`core_cpp_add_unbuilt_source_check()` in `cmake/CoreCppHeaderSelfCheck.cmake`). `core::net`
  picks one `DefaultBackend.cpp` from five, and its `else()` arm — `posix/` — is reached only on a
  POSIX platform that is not Linux, not a BSD, not Apple, not Windows and not Emscripten, so no CI
  leg and no local preset compiled that file **at all**: a rename or a dropped include would have
  broken it silently until somebody ported core-cpp. It is now parsed for diagnostics everywhere
  else, and it is the only such arm in the tree. What a green check means is deliberately narrow
  and is written in the function's own comment: the file still **parses**, against the *host's*
  headers rather than the target's, with no link step, so it catches bit-rot and not a wrong
  implementation. The platform that takes the branch remains untested.

- **Every public header is self-contained, and the build now proves it.** Each header in a
  module's `FILE_SET HEADERS` is compiled as the first and only include of a translation unit of
  its own, so a header that needs a neighbour included first fails the build naming itself and
  what it needed. `.agent/rules/cpp-guidelines.md` has always required this and nothing enforced
  it: all eighteen hygiene rules read text, none compiled anything, so such a header passed every
  check and broke only for whoever included it first — which in a library is a consumer
  ([core-cpp#31](https://github.com/contour-terminal/core-cpp/issues/31)). The list comes from the
  build rather than a glob, so a header added later is covered without anyone remembering, and one
  a platform excludes from its file set is excluded here too. Private headers (`detail/`, `posix/`,
  `windows/` and the other platform directories) are in no file set and stay out of scope: an
  implementation header may assume its includer.

- The CMake framework: a module table that enforces the layering between modules, a dependency
  table resolved from the parent project, then `find_package`, then CPM, per-target toolchain
  tables (pedantic warnings, `CORE_CPP_WERROR`, sanitizers, coverage, clang-tidy), and no global
  state unless core-cpp is the top-level project.
- `core::testing` (Windows dialog suppression, usable without a test framework),
  `core::testing_dialogs` and `core::testing_main`, a Catch2 `main()` whose exit status is 0 when
  everything passed, 1 when anything failed or Catch2 reported an error, 77 when every test case
  skipped, and 2 when nothing ran.
- Configure, build, test and workflow presets for clang, GCC, AppleClang, MSVC and clang-cl, the
  sanitizers, clang-tidy, coverage and Tracy, and an `emscripten` preset for single-threaded
  WebAssembly whose tests run under node.
- Checks over the tree: the CMake and C++ hygiene rules with their self-test, the exit-code
  contract, and `tests/cmake/check-release.cmake`, which the release workflow runs on a tag.
- The documentation site, the API reference, the rulebook in `.agent/`, and the CI workflows.
- The module table's `PLATFORMS` column takes `any`, `native` or `wasm-subset`, and a module may
  list `SOURCES_EMSCRIPTEN`; `SOURCES_POSIX` is not compiled under Emscripten, which sets `UNIX`.
- `core::base` (namespace `core`): contract checks (`Require`, `Guarantee`), an injectable
  process environment (`core::Environment`, with `core::testing::FakeEnvironment` in
  `core::testing`), escaping, FNV hashing, type-safe `Flags`, `times()`, the password-database
  entry, string and range utilities, `Overloaded`, `Deferred`, Base64 (`core::base64`), the
  Tracy profiling macros (`CORE_ZONE_*`) and the `core::ranges::Iota`/`FoldLeft` seam. It owns the
  generated `core/Config.hpp`.
- `core::log`: categorised logging (`Category`, `Sink`, `configure()`), its sinks and formatters
  (`ScopedOutput`, `ScopedCapture`), `fatal()` and `SoftRequire()`, which report through it, and
  `isStdOutTerminal()`/`isStdErrTerminal()` — the one place the platform is asked whether a
  standard stream is a terminal, which is what decides colourisation.
- `core::cli`: the command-line parser (`core::cli::parse`, help and usage text) and the
  application scaffold `core::cli::App`.
- The Tracy dependency, 0.14.1 as contour pins it, resolved when `CORE_CPP_WITH_TRACY` is on:
  `core::base` then links `Tracy::TracyClient` and the `CORE_ZONE_*` macros record zones. A
  fetched client is built with `TRACY_ENABLE` and `TRACY_ONLY_LOCALHOST`. CI builds and tests the
  `clang-tracy` preset.
- `core::testing_main` applies the `LOG` environment variable to `core::log` before it runs the
  tests (`LOG=net` enables the `net` category and writes it to standard output), and so links
  `core::log`.
- `core::platform`, the operating-system layer: one clock seam merged from endo's, contour's and
  fastcached's (`IClock` with `now()` and a virtual no-op `refresh()`, `SteadyClock`,
  `CachedClock`, `ManualClock`, `IWallClock`, `SystemWallClock`, `ManualWallClock`,
  `WallClockRef`, `defaultSteadyClock()`, `defaultSystemWallClock()`), `Types` (`NativeHandle`,
  `isTerminal()`, ...), `PlatformError`, `Wakeup`, `SignalHandler`, `SystemPipe`, `WinsockInit`,
  `MessageQueue`, `FileSystem` and `NativeFileSystem`, `FileInfoProvider`, `EnvironmentProvider`,
  `UserPaths`, `PathUtils`, `GlobMatch`, `FileUri`, `SystemInfo` and `StringUtils`, with the test
  doubles `testing::InMemoryFileSystem`, `testing::MockFileInfoProvider` and
  `testing::TestEnvironmentProvider`, and `nativeEnvironmentProvider()` and
  `nativeFileInfoProvider()`, which give a composition root the private native implementations:
  Windows' own, and one POSIX provider each for Linux, macOS, the BSDs and Emscripten (endo's
  `LinuxFileInfoProvider`, which used nothing Linux-specific, is `PosixFileInfoProvider`).
  Under single-threaded Emscripten its row says
  `wasm-subset`: Types, PlatformError, Clock, StringUtils, PathUtils, GlobMatch, FileUri and the
  POSIX providers build, and their tests run under node.
- `core::async`, header-only and including nothing but the standard library; it links Threads,
  which is what its `StopToken` fallback's `std::mutex`, `std::condition_variable` and
  `std::this_thread::get_id()` need, and nothing at all under single-threaded Emscripten.
  fastcached's executors arrive with Task B1.
- `core::Generator<T>` in `core::base`: `std::generator` where the standard library has it and is
  not libstdc++, otherwise `core::detail::GeneratorFallback<T>`, which is tested on every
  platform. It needs only the standard library, so it lives in base rather than `core::async`,
  where `core::async::Generator` would read as an asynchronous, `co_await`-able stream.
- `core::async::StopToken`, `StopSource`, `StopCallback<F>` and the `constexpr` tag `NoStopState`
  (`<core/async/StopToken.hpp>`): `std::stop_token`, `std::stop_source`, `std::stop_callback<F>`
  and `std::nostopstate` where the standard library defines `__cpp_lib_jthread`, and otherwise
  core-cpp's implementation with the standard semantics, which keeps plain state under
  single-threaded WebAssembly. libc++ before 20 has `<stop_token>` only behind
  `-fexperimental-library` (emsdk 3.1.56's libc++ 17, FreeBSD 15's base Clang 19, AppleClang 17
  (measured in CI)), and core-cpp adds no compile flag to its consumers, so the fallback runs there,
  with real threads everywhere but WebAssembly. The configure log of a build with tests says which
  branch the toolchain takes.
  `CORE_ASYNC_FORCE_STOP_TOKEN_FALLBACK` selects the fallback everywhere; the test binary
  `core-cpp-async-fallback-test` (ctest `core-cpp.async-fallback`) is built with it, so the fallback
  is tested on every platform, ThreadSanitizer included.
- `core_cpp_add_test()` takes `NAME`, for a module's second test binary, and `DEFINITIONS`, the
  compile definitions of that binary alone.
- contour's coroutine vocabulary in `core::async`: `Task<T>`, lazy and awaited once, whose promise
  carries the `StopToken` it inherits from the awaiting coroutine; `detail::UniqueCoroHandle`;
  `OperationCancelled` and `thisCoroStopToken()` (`Cancellation.hpp`); the `Awaiter` and
  `HasStopToken` concepts (`Awaitable.hpp`); `whenAll()`, which joins `Task<void>`s and rethrows the
  first failure once all have finished; and `whenAny()`, which resolves to a
  `std::optional<std::size_t>`, the index of the first to complete, and cancels the others. Their
  tests also run over the `StopToken` fallback, and `core-cpp.async-link-smoke` links `core::async`
  alone over it, which is the link a consumer makes. A `Task`'s symmetric
  transfer is a tail call with Clang and MSVC at every optimisation level, with GCC only when it
  optimises sibling calls, and not in WebAssembly without `-mtail-call`. So awaits that complete
  synchronously grow the stack: at GCC `-O0` both a nested chain and a *loop* of 100000 of them
  overflow an 8 MiB stack, at GCC `-Og`/`-O1` the nested chain does, and under emsdk 3.1.56 the
  nested chain exceeds node's call stack. The deep-chain test is skipped under Emscripten without
  `-mtail-call`, and on GCC unless the build's optimisation level is `-O2` or better
  ([core-cpp#15](https://github.com/contour-terminal/core-cpp/issues/15)).
- `core::testing`: `ScopedTempDir`, `ScopedWorkingDirectory` and `EnvHelper` (`setTestEnv()`,
  `unsetTestEnv()`, `ScopedEnv`).
- `core::setProcessEnvironmentVariable()` and `core::unsetProcessEnvironmentVariable()` in
  `core::base`: the one writer of the process environment, in place of `setenv()`. On POSIX they
  publish a new `environ` block under `LiveEnvironment`'s lock and never free a published one, so
  a reader elsewhere never sees a block change or disappear under it.
- `core::net`, contour's event loop, sockets, TLS and HTTP server, as contour has them but for the
  namespaces and `core::platform` in place of contour's `net/platform/`: `EventLoop` over an
  injected `EventSource` (poll everywhere, epoll on Linux, kqueue on macOS and the BSDs,
  `makeDefaultEventSource()`), `ISocket` and `IListener` with `listen()`, `connect()`,
  `listenUnix()`, `connectUnix()` and `adoptFd()`, descriptor passing on POSIX,
  `AsyncBufferedReader`, `WriteQueue`, `SplitSocket`, `withTimeout()`, an HTTP/1.1 server, the
  diagnostic sink, and the test doubles `testing::ScriptedEventSource`,
  `testing::makeSocketPair()`, `testing::AllBackends` and `testing/CoroTestSupport.hpp`. Its
  error vocabulary, `NetError` and `IoResult`, is the header-only `core::net_types`, which builds
  under Emscripten too; the rest is native only until Phase B, which also replaces the
  `EventSource` API with `IoBackend`. `core::net` links `Threads::Threads` PUBLIC, because its
  headers use `std::mutex`.
- `core::net_tls` (`<core/net/Tls.hpp>`), with `CORE_CPP_WITH_TLS`: a TLS `ISocket` over any other,
  behind `ITlsContext`, in server, client (a pinned CA and a host name, or trust on first use) and
  self-signed form, and `constantTimeEquals()`. It links OpenSSL PRIVATE, and no OpenSSL type
  appears in its header.
- The OpenSSL dependency, taken from the system and never fetched, resolved when
  `CORE_CPP_WITH_TLS` is on.
- Every Linux, macOS and BSD preset turns `CORE_CPP_WITH_TLS` on, and CI installs OpenSSL where
  it builds them, so `core::net_tls` is built and tested on Linux, macOS and FreeBSD as well as in
  `cl-release-tls` on Windows. Those presets now need OpenSSL's development files.
- `core::net::EventLoop` calls its clock's `refresh()` before it computes a wait's timeout and
  after the wait returns, as `core::platform::IClock` asks of whoever owns a loop, so a
  `CachedClock` can drive it. contour's loop did not, because contour's `IClock` had no
  `refresh()`; for `SteadyClock` and `ManualClock` it does nothing.
- A module may declare further targets in the module table, each with a row of its own
  (`core_cpp_module_target()`), where its `PLATFORMS`, `WHEN` or links differ from its module's:
  a native-only module is entered under Emscripten when one of its targets builds there, and
  `core_cpp_add_test(<module> NAME <target>)` links that target and builds where it does. A
  row's `DEPS` are all its target may link, another target of the module or a module its
  module's row lists, and a row without `DEPS` links no core-cpp target; the configure refuses
  a row or a link outside that by name (`core::net_types` links nothing, `core::net_tls` only
  `core::net`).
- `core::tui_output` (`CORE_CPP_WITH_TUI`, native only), the leaf of endo's terminal UI: styled
  output and cursor, screen, scroll-region, sixel, OSC 52 and OSC 8 control through
  `TerminalOutput`, whose `writeToDestination()` a subclass overrides to retarget the stream and
  whose `isTerminal()` says what that stream is; `SyncGuard` (DEC mode 2026), which brackets the
  output it was made from; `buildSgrSequence()`; the protocol sequence constants and the DA1
  reader in `core::tui::protocols`; `CursorShape`; and the module's `Result`/`VoidResult`. It links
  `core::base` and nothing else — no libunicode, no coroutines, not even `core::platform` — so a
  program that only prints styled text takes nothing else with it, and its row in the module table
  is what refuses any other link.
- The libunicode dependency (0.9.3, `unicode::unicode`) when `CORE_CPP_WITH_TUI` is on, and stb
  (`stb_image`, `DOWNLOAD_ONLY`, pinned to a commit because stb publishes no releases) when
  `CORE_CPP_WITH_IMAGES` is on. Both are off under Emscripten. A fetched libunicode is built with
  `BUILD_SHARED_LIBS OFF`, as endo pins it: its target is linked PUBLIC from `core::tui`, so a
  consumer configured for shared libraries would otherwise get a shared libunicode behind a static
  core-cpp. A first configure with `CORE_CPP_WITH_TUI` on fetches libunicode from GitHub and
  libunicode's configure then downloads `UCD.zip` from `www.unicode.org` — core-cpp's only fetch
  outside GitHub.
- `core::tui`, endo's terminal UI (`f774a210`), native only: `TerminalInput` and `VtParser` over
  the Kitty keyboard protocol, SGR mouse reporting, bracketed paste and focus tracking;
  `Terminal`, which pairs input with output and owns the bounded query round-trips on an injected
  clock; `Buffer`, `Canvas` and the diffing `Screen` (inline, full-screen and fixed viewports);
  the components (`InputField`, `List`, `TreeTableView`, `Dialog`, `StatusBar`, `LogPanel`,
  `Spinner`, `ProgressBar`, `Tooltip`, `QuestionComponent`, the completion, command-palette and
  fuzzy-picker popups); `core::tui::completer`; `MarkdownRenderer` and `GenericSyntaxHighlighter`;
  sixel encoding, and with `CORE_CPP_WITH_IMAGES` the stb-backed loader, scaler and
  `FilesystemImageProvider`; and `core::tui::runtime`, whose `TuiRuntime` drove coroutines against
  an `EventSource` (`TerminalEventSource`, `PollEventSource`, `runModal()`, `withTimeout()`). Test
  doubles: `MockTerminalOutput`, `runtime::testing::MockEventSource` and `TestHelpers.hpp`. The
  runtime was rewritten onto `core::net::EventLoop` before release and those types are gone; see
  **Breaking** below. `runtime/TuiRuntime.hpp` and its test came from fastcached's copy
  (`5389e29a`), which carried one fix endo has not taken back: `DelayAwaiter::await_ready()` is a
  constant and an elapsed deadline is decided in `await_suspend()`, because MSVC 19.44's ARM64
  code generator loses the enclosing `try` of a `co_await` on an awaiter whose `await_ready()`
  reads the clock through a virtual `now()`. That awaiter is `core::net`'s now, and carries it.
- The global property `CORE_CPP_TARGETS`: every compiled library core-cpp built, by its real
  target name, in the order the module table declares them. A parent project that instruments its
  build reads it and applies the same sanitizers or coverage to core-cpp's code, which is what
  keeps ThreadSanitizer from reporting races between instrumented and uninstrumented code.
  Header-only targets and test binaries are not in it.
- `cmake/CoreCppVendor.cmake`, the vendoring tool of the design spec's Part I §5:
  `cmake -DMODE=sync -DREF=<tag> -DDEST=<dir> [-DREPO=<url or path>] ["-DMODULES=<a;b>"] -P ...`
  copies the file set out of git's own blobs (`-c core.autocrlf=false -c core.eol=lf`), refusing a
  CR byte, a symbolic link and a submodule, and writes a `MANIFEST` of SHA-256 hashes with LF
  endings whatever the host, because the consumer commits that file; `MODE=check` re-hashes a copy
  and refuses a hash mismatch, a missing file, an unlisted file and a manifest that is not one --
  an unparsable line, a missing `# repository` or `# ref` header, a `# commit` that is not 40
  lowercase hex digits, and a `# files` count that is absent, is not a number, is zero or disagrees
  with the lines below it -- needing no git, because a consumer runs it in its own CI. A sync
  assembles the new copy in `DEST.core-cpp-vendor-new`, a sibling of `DEST`, writes its `MANIFEST`
  there, and touches `DEST` itself only once that copy is complete; every refusal deletes the
  sibling on its way out, so a copy a refused sync found still passes its own check with nothing
  new beside it. The replacement is then two directory renames through `DEST.core-cpp-vendor-old`
  (`cmake/CoreCppVendorReplace.cmake`) rather than a file-by-file move into an emptied `DEST`, so
  whichever of the two directories exists when a sync stops -- for any reason, including being
  killed -- is a whole copy that passes its own check, and `DEST` is never half of each nor
  unfinished. A rename that fails -- on Windows an open handle, a lock or a scanner can fail one --
  puts the previous copy back and refuses; if that restore fails too the refusal names both
  directories, deletes neither, and says that either can be adopted by renaming it. A
  `DEST.core-cpp-vendor-old` left by a previous run holds the only copy of what was there, so a
  sync refuses rather than delete it to make room. It refuses a `DEST` that is not one of ours --
  a directory with files and no manifest, or a regular file where a directory belongs -- and
  it refuses what it cannot copy correctly: a `REF` that is not a tag or a full 40-character SHA,
  a local `REPO` that is not the root of its own repository, a ref whose tree is not core-cpp's,
  and a `MODULES` list that omits a module the ref's own table builds unconditionally. The last
  three are one mistake seen from three sides -- running sync with a *vendored copy's* own script,
  where `REPO` defaults to the copy's directory and git reads the consumer's repository instead.
  `tests/cmake/check-vendor-selftest.cmake` (ctest `core-cpp.vendor-selftest`, label `hygiene`)
  proves every one of those judgements by name against repositories it builds for the purpose, and
  skips rather than fails where git is absent. The file set is the spec's, plus everything else
  directly in `src/core/` -- that module's `CMakeLists.txt` and `Config.hpp.in`, without which the
  copy does not configure. File modes are outside the contract. `docs/vendoring.md` is the
  contract.
- Consumer smoke tests, one project per way core-cpp is consumed, and the `consumer-smoke` CI job
  that runs all three (`ci-ok` requires it): `tests/consumer-cpm` adds core-cpp with CPM and
  asserts that doing so changed none of its own flags, launcher or include directories, that
  `CORE_CPP_TARGETS` names every compiled library and no test binary, that no core-cpp target --
  the header-only ones included, which is where an interface-scoped usage requirement would show --
  carries a PUBLIC or INTERFACE flag, and that no test of core-cpp's was built;
  `tests/consumer-vendored` builds a vendored copy of the commit under test with
  `CORE_CPP_FETCH_DEPS=OFF`, `CORE_CPP_WITH_TUI=OFF` and `CORE_CPP_WITH_TLS=ON` inside a container
  with no network and no git, and registers the verbatim check as one of its own tests;
  `tests/consumer-wasm` builds the WebAssembly subset behind one INTERFACE library, as morph does,
  runs it under node with emsdk 3.1.56, and refuses a build in which any core-cpp target links
  threads -- read off those targets' `LINK_LIBRARIES`, because `find_package(Threads)` inside
  core-cpp creates a target the parent scope cannot see. The loopback echo and the `core::log`
  line the CPM and vendored programs share are `tests/consumer-shared/ConsumerSmoke.hpp`; each
  program keeps only what is its own.
- `cmake/portable/CompileCache.cmake` re-synced verbatim from fastcached
  `5a9dca0498f4c37c63a17270550ee51ca87ae0a3` (`cmake/portable/README.md`), fixing the nightly
  `downstream.yml` drift check. Upstream added `FASTCACHE_AUTO_INSTALL_HOST_SYSTEM` and
  `FASTCACHE_AUTO_INSTALL_HOST_PROCESSOR`: empty by default, so `_fc_auto_install_select_row()`
  still asks `CMAKE_HOST_SYSTEM_NAME`/`_PROCESSOR`, but a caller can state the host to fetch
  `fastcache-cc` for instead, which lets `scripts/check-compile-cache-autoinstall.cmake` pin a
  published platform per row rather than stopping at whichever host actually runs the check.
  `cmake/FetchTransferBound.cmake` compared identical at the same commit; no change there.

- `core::platform::NativeFileSystem` takes its rename primitive at construction:
  `RenameFunction`, `nativeRename()` and a constructor defaulting to it, so `instance()` and every
  existing caller are unchanged. It is the one filesystem call the class takes rather than makes,
  and it is injected because `rename()`'s two-hop lettercase retry only runs on a volume that
  refuses a case-only rename outright -- which ext4, APFS, NTFS and UFS all do natively, so the
  retry was unreachable from any test. It moves a consumer's entry through a temporary name and
  can leave it there when both the second hop and the rollback fail, which is not behaviour that
  may ship untested (controller ruling R53).

- `core::net::NetErrorCode` is the merged vocabulary of both lineages, so a caller of contour's
  `net::NetErrorCode` or of fastcached's `FastCache::NetErrorCode` has a code for every failure it
  used to distinguish. From fastcached it gains `AddressNotAvail` (a bind whose address is not
  available locally), `HostUnreach` and `PermissionDenied` (a low-numbered port without privileges,
  a firewall's `EACCES`) — three causes that were an unclassified `Other` here and that no caller
  could match on. **Nothing in core-cpp returns those three yet**: the errno and WSA tables that
  classify a socket failure gain their rows when fastcached's sockets and dialler are merged in
  (Tasks B6 to B8), so until then a migrated `== HostUnreach` branch compiles and is dead code. The
  codes are here now because the vocabulary is settled before the backends are rewritten on top of
  it. `core::net::isDeadlineExpiry(NetErrorCode)` joins it, also from fastcached
  (`IsDeadlineExpiry`): a deadline armed with `SO_RCVTIMEO`/`SO_SNDTIMEO`, and a poll given a
  timeout, expire as `EAGAIN`/`WouldBlock` on POSIX and as `WSAETIMEDOUT`/`Timeout` on Winsock, so
  the question is asked through one predicate over both operands rather than open-coded
  ([fastcached#824](https://github.com/LASTRADA-Software/fastcached/issues/824)). A trailing
  `NetErrorCode::Last` states how many codes there are, so a table or a test covers every one of
  them without restating the list; it is not a code, `toString()` gives it no description, and
  nothing constructs or returns it. A new code goes above it, never below — one appended after
  `Last` would satisfy both the switch and the count while every walk of `[0, Last)` missed it, so
  a test refuses that case by name. `core::net_types` still links nothing and still includes no
  `<format>`: it is what `fastcache-cc` links alone in Task C4.

- `tools/migrate/`, the tooling every consumer migration runs: `renames.json`, the rename table;
  `rewrite.py --profile contour|endo|tuidu|fastcached`, an idempotent codemod over its include,
  namespace, symbol, member and macro rows, anchored so that `net::` never matches inside
  `std::net::`, `endo::net::` or `mynet::` and so that a string literal is left alone; and
  `semantic_rename.py`, which renames a member through libclang only where the **declaration** it
  refers to is the one named, so `sock.Read(` moves where `sock` is a `FastCache::ISocket` and
  another class's `Read` does not. `check-renames.py` (ctest `core-cpp.migrate-renames`, label
  `hygiene`) holds the table to the tree: every core-cpp symbol a row names must exist in the
  delivered headers, and a row still waiting on a Phase B task must *not* exist yet, so a rename
  that forgets the table fails the build and names the row to update. The cases are stdlib
  `unittest` (ctest `core-cpp.migrate-codemods`); the `style` CI job installs libclang's Python
  bindings and fails on a skip, so the semantic pass is tested for real. None of this is part of
  the library: no target links it and no consumer builds it.

- `ruff` is pinned like clang-format and clang-tidy, and the repository's Python is `snake_case`,
  formatted *and* linted with it: `.ruff-version` states the release, `scripts/tool-versions.py`
  installs it and refuses a mismatch, `scripts/python-style.py --check` runs both halves and reports
  both before failing, and the `style` CI job runs it beside clang-format's. `ruff.toml` sets the
  line length to `.clang-format`'s `ColumnLimit`, so a Python file and the C++ beside it wrap at the
  same column and one number governs both, and it states ruff's default rule set (`E4`, `E7`, `E9`,
  `F` — undefined names, unused imports, import and statement errors) rather than inheriting it, so
  a future ruff cannot widen or narrow the gate by changing its mind about the default. Nothing
  stylistic is selected: layout is the formatter's job, and the linter never rewrites. The wrapper
  refuses any ruff but the pin, because its output changes between releases: an unpinned ruff
  reformats a file that CI then reports as unformatted, and finds one more thing on a version
  nobody chose. The `# noqa` comments are gone with it — a diagnostic-muting comment is the Python
  spelling of `NOLINT`. Nothing here enters a consumer's build, so there is no row in
  `cmake/CoreCppDependencies.cmake`.

- `core::async` gains fastcached's executor and ownership vocabulary, merged onto contour's `Task`
  (the design spec, Part I §2, item 6). New headers, all header-only and all in the WebAssembly
  subset but the last:
  - `<core/async/ParkedWork.hpp>`: `ParkedWork`, the pair of *the coroutine to resume* and *the
    chain root an executor may free if it never resumes it*, with `detail::Parked`, the container
    entry that owns the second for as long as it holds it, and `detail::unownedRootOf` /
    `detail::parkedWorkFor`, which derive the answer from the parking coroutine's own promise.
  - `<core/async/DetachedTask.hpp>`: `DetachedTask`, a coroutine started for its effects whose
    frame nobody owns -- the one shape an executor may free at teardown.
  - `<core/async/SyncRun.hpp>`: `syncRun(task)`, which drives a self-driving task to its end and
    throws `std::logic_error` rather than reading a result a still-suspended task does not have,
    and `syncRunWith(task, retrieve)`, which takes the park back first so the refusal is the whole
    of the failure.
  - `<core/async/IExecutor.hpp>`: `IExecutor`, with `submit(std::coroutine_handle<>)` (borrows) and
    `submit(ParkedWork)` (carries what may be freed). Every class deriving from it says
    `using IExecutor::submit;`, and `ParkedWork_test.cpp` asserts at compile time that
    `submit(ParkedWork {})` reaches the owning overload
    ([fastcached#1041](https://github.com/LASTRADA-Software/fastcached/issues/1041)).
  - `<core/async/ResumeOn.hpp>`: `co_await ResumeOn { executor }`, which continues the awaiting
    coroutine wherever that executor runs things.
  - `<core/async/AsyncQueue.hpp>`: `AsyncQueue<T>`, a queue one coroutine parks on and any thread
    pushes to, with `AsyncQueueOptions` (capacity and a `DropOldest`/`DropNewest` overflow policy),
    `AsyncQueuePush`, and a `pop()` that resolves to `std::optional<T>`. `push()` is
    `[[nodiscard]]`: its `AsyncQueuePush` is the only report of a drop, and a discarded one is the
    silent loss the type exists to prevent. `push()` and `close()` never resume the consumer
    inline; they hand its handle to the executor. `pop()` is stop-aware:
    a cancel from the awaiting flow's own token throws `core::async::OperationCancelled`, while an
    item already queued and a `close()` both answer first.
  - `<core/async/ThreadPoolExecutor.hpp>`: an `IExecutor` whose "somewhere else" is a fixed set of
    threads, for work that blocks. It is the one header of the module a single-threaded WebAssembly
    build does not get -- it refuses to compile there by `#error`, and is in no `FILE_SET` and in
    no test binary of that build.
- `core::async::Task<T>::release()` and `detail::UniqueCoroHandle<Promise>::release()` hand the
  owned coroutine frame to the caller.
- `core::async::CarriesUnownedRoot` (`<core/async/Awaitable.hpp>`) is the second concept a
  templated `await_suspend` reads the awaiting promise through, beside `HasStopToken`: a promise
  that carries the root of an await chain nobody owns. `Task`'s promise carries it, and so do the
  `whenAll` and `whenAny` runners, so a coroutine parked underneath a combinator still states what
  an executor may free.

- `renames.json` gains a `removed` kind, which runs the drift gate backwards: the row names a
  symbol core-cpp deleted, carries no `to` and no `target`, and `check-renames.py` asserts the
  symbol stays **absent** from the delivered headers, so a re-introduction is refused. It exists
  because a removal that changes the shape of a call, rather than just its name, must stay a compile
  error at the consumer's call site instead of becoming a codemod that rewrites it into something
  that compiles and is wrong — while the row's `note` still carries the migration instruction beside
  every other rename the same pull request applies. The schema refuses such a row that carries a
  `to`, a `target` or any `apply` but `none`, so no rewrite tool can be handed one. The first two
  rows are `core::tui::LanguageId::Endo` and `core::tui::registerEndoHighlighter()`.

- A `removed` row's `from` must be the fully qualified **core-cpp** name, and the schema now refuses
  anything else. It is the one kind whose `from` is a core-cpp name — every other kind's is the
  consumer's spelling — so `net::FdToken`, the form the neighbouring rows teach, was the natural
  mistake, and its failure was *silence*: the gate reads a bare name as a macro and a two-component
  name as a namespace nothing opens, finds neither, and reports the symbol absent. The row then
  passed for ever while naming a type sitting in the tree. `core::tui::runtime` still declares
  `FdInterest`, `FdToken`, `WaitOutcome`, `FdRegistration` and `FdRegistry` until Task B12, so the
  guard has live work to do, and all eight rows were qualified by discipline rather than by
  construction.

- A `macro` row that names only a `target.header` is checked against it: the header must still
  **name** the macro, by defining it or by testing it. Two rows are of that shape
  (`CORE_GENERATOR_FORCE_FALLBACK`, `CORE_RANGES_FORCE_FALLBACK`) and they are deliberate — the
  macro is one a *consumer* defines and core-cpp only asks about — so the assertion is "consults",
  not "defines", and a mention in a comment does not count.

- `core::net::IoBackend` (`<core/net/IoBackend.hpp>`), the readiness seam the event loop drives,
  with `makeDefaultBackend()`, `makeBackend(BackendKind)` and `preferredBackendKind()`. A backend
  DISPATCHES: `wait()` invokes the callbacks on the `ReadinessHandler`s registered with it, and
  those callbacks only enqueue — every coroutine is resumed by the loop, on the loop's thread,
  after the wait has returned. `selectReadinessCallback(handler, readiness)` is the pure rule that
  picks one callback per registration per wait and routes a hangup or error to `onError`, or, for
  a handler that has none, to whichever direction it watches; it is a free function so it is
  tested without a kernel. `setInterest()` answers `std::expected<void, NetError>`, so a kernel
  that refuses a registration is reported instead of leaving the caller parked on one it never
  made ([fastcached#1054](https://github.com/LASTRADA-Software/fastcached/issues/1054),
  [fastcached#1057](https://github.com/LASTRADA-Software/fastcached/issues/1057)), and `detach()`
  withdraws the handler from the ready batch a wait in flight is walking, which a kernel's own
  deregistration cannot do ([fastcached#475](https://github.com/LASTRADA-Software/fastcached/issues/475)).
  `wake()` is on the interface and is its one thread-safe member, so the wakeup channel belongs to
  the backend rather than to the loop. The backends are `PollBackend` (POSIX), `EpollBackend`
  (Linux), `KqueueBackend` (macOS and the BSDs) and `WfmoBackend` (Windows, `WSAEventSelect` +
  `WaitForMultipleObjects`); each header is private, and a program reaches one through the
  factories. `BackendParity_test` runs one scenario against every backend this platform builds.
- `core::net::testing::NullBackend`, which accepts registrations, reports nothing and never
  blocks — a loop driven entirely by `post`, `spawn` and timers, and what Task B4's `TestLoop`
  will be built on.
- `core::net::EventLoop::parkedWaiterCount()`, beside `pendingTimerCount()`: the same leak
  assertion for a readiness park. A flow that resumed or unwound without unregistering leaves its
  handler attached to the backend, and a count that never returns to zero is how that shows.

- `.agent/guides/consumer-migration.md` carries the byte-identity proof a consumer pull request runs
  to show a mechanical pass was mechanical: for every file the commit *modified*, re-derive the
  post-image from the pre-image by applying the substitutions the author asserts by hand — never the
  codemod's own report, and never the codemod again, or a tool that is wrong about a row is wrong
  identically on both sides and proves itself correct — and compare with whitespace stripped. It
  catches an unintended rewrite and a hand edit mixed into a codemod commit; it does not catch a
  correct rewrite to a wrong target, which is what the drift gate is for.

- `core::net::HostDrivenBackend` (`<core/net/HostDrivenBackend.hpp>`) and the `IHostScheduler`
  seam behind it: the backend for an event loop that does not own its thread. It does not block —
  there is nothing to block on inside a browser, and under single-threaded WebAssembly nothing to
  block with — so the loop is PUMPED instead. `attach` and `setInterest` answer
  `NetErrorCode::Unsupported`, `wait()` returns at once, and `wake()` and `armWakeAt(deadline)`
  ask the host for a pump through `IHostScheduler::callAfter(delay, fn, state)`, coalescing
  several requests into one and clamping a deadline already past to a zero delay. It is portable
  and is tested on every platform over `core::net::testing::ManualHostScheduler`, because a
  behaviour observable only in a node run is one nobody reads;
  `core::net::EmscriptenHostScheduler` (`emscripten_async_call`, which is the browser's
  `setTimeout`) is what `makeDefaultBackend()` uses there.
  **Filing work from outside a turn is safe on such a loop**, which is the position a DOM event
  handler, a frame callback or a TUI input path is in: `EventLoop::registerPark` asks the host for
  the turn that will reach the park — and so `addTimer`, `schedule`, `co_await loop->delay()` and
  `interruptibleSleepUntil()` all do, including from an eagerly-started `core::async::DetachedTask`
  — as do `resumeSoon` and `requestStop`. Without it the work is filed, correct, and never run: the
  host is armed at the END of a turn, so a quiescent host-driven loop has nothing coming that would
  arm it. Backends that are not host-driven are unaffected.
- `core::net` has a WebAssembly subset: its module row is `wasm-subset`, and under single-threaded
  Emscripten it builds the `IoBackend` contract, `HostDrivenBackend`, the pure logic behind them
  and the test doubles — and links no `Threads::Threads`, which would force `-pthread` and
  SharedArrayBuffer onto every consumer. The event loop, its timers and the sockets join in Tasks
  B4 and B5. `core-cpp.net_backend` is the test binary that runs everywhere, Emscripten included;
  `core-cpp.net` keeps the cases that need a loop, a socket or a descriptor.

- **Callback timers on `core::net::EventLoop`, and nothing in core-cpp polls for a deadline any
  more.** `addTimer(deadline, callback, state) -> TimerId` and `cancelTimer(TimerId) -> bool` arm
  and retire a deadline with no coroutine frame behind it. A callback timer is a park in the SAME
  table as a `co_await delay()` — one heap, one sequence counter, one never-reused id space — so
  the loop has a single answer to "when is the next deadline" and a single firing order across the
  two kinds. Step 5 of the turn queues both; step 2 runs both, which keeps the one place that
  resumes a coroutine also the one place that calls out to a timer callback. `cancelTimer`
  answering `true` means *this call prevented the callback*, which includes the window between a
  deadline firing and its callback running: an owner destroyed in that window would otherwise have
  its callback run against storage that is gone. `RunOnceResult::drained` (and so
  `testing::TestLoop::tick()`) counts what step 2 took off the ready queue — coroutines resumed
  plus timer callbacks run — because a turn that ran a callback and resumed nothing is not an idle
  turn, and `runUntilIdle()` would otherwise stop on one. It is named for what it counts rather
  than for what happens to only half of that: a timer callback is called, not resumed.
  `TimerCallback`, `ParkEntry::onCallback` and the diagnostic
  `EventLoop::pendingTimerSlotCount()` — the size of the deadline heap including the stale slots
  lazy pruning is carrying — are public with it.

- `core::net::DeadlineTimer` (`<core/net/DeadlineTimer.hpp>`): a deadline as an object, disarmed
  by `disarm()` or by destruction, and **destroyable from inside its own callback**. For a timeout
  that has to tear an operation down rather than merely stop waiting for it — a dial that only
  stopped waiting leaves the connect attempt in flight for the kernel's own retry schedule. Ported
  from fastcached, without its coroutine frame, its `shared_ptr` state or its 50ms poll interval:
  those existed because a scheduled resumption could not be taken back, and here it can.

- `core::net::interruptibleSleepUntil(loop, token, deadline)` and `core::net::WakeReason`
  (`<core/net/InterruptibleSleep.hpp>`): sleep to a deadline or until a stop token is stopped,
  whichever comes first. It parks **once** and the stop callback wakes it, where upstream slept in
  steps of `wakeBound` and re-read the token at each one. The supplied token is reported as
  `WakeReason::Cancelled`; the awaiting flow's OWN token throws `core::async::OperationCancelled`,
  as every loop awaitable does — and where they are the same token, the reported answer wins.

- `core::net::sleepUntil(EventLoop*, deadline)` and `core::net::nextWakeStep()`
  (`<core/net/SleepUntil.hpp>`). The free `sleepUntil` takes a **nullable** loop, for a caller with
  no deadline mechanism behind it (an in-memory transport): a null loop or a deadline already gone
  resolves inline, without suspending. `core::net::DelayAwaiter` gains a constructor taking
  `EventLoop*` for it; the existing `EventLoop&` one is unchanged.

- `tests/wasm/HostDrivenTimer_smoke.cpp`, run under node in the `emscripten` job on both emsdk
  versions: a `PlatformLoop` on a real host, with a coroutine `delay` and a `DeadlineTimer` parked
  on it, advanced by nothing but `emscripten_sleep` yielding to the host. `tests/consumer-wasm`
  runs the same scenario as a consumer and links `core::net`. Both are judged by their OUTPUT
  rather than their exit status, because a WebAssembly program that leaves a pending
  `emscripten_async_call` behind — which an armed deadline always does — exits 0 whatever `main`
  returned; the reasoning, and the two fixes that do not work, are in `tests/wasm/CMakeLists.txt`.

- **`core::net::IocpBackend`, the Windows completion-port backend, and the readiness bridge that
  lets one wait serve a server's sockets and a TUI's console input.** Reachable as
  `makeBackend(BackendKind::Iocp)`, and **the Windows default** since the sockets that issue
  overlapped operations on a port arrived with it (see **Changed**). The header is private, like
  every other backend's.

  A completion port reports *completions* and has no notion of "this handle is readable", so
  readiness is synthesised, and each kind of handle needs its own source: a **waitable HANDLE**
  (console input, an event, `platform::SystemPipe`'s wakeup) gets a thread-pool wait whose
  callback does nothing but `PostQueuedCompletionStatus`; **socket readability** is a zero-byte
  `WSARecv`, the Winsock idiom for "complete when data is pending, consuming nothing"; **socket
  writability** goes through `WSAEventSelect` for `FD_WRITE` and then through the first bridge.
  Where the kernel exports `NtAssociateWaitCompletionPacket` — a `GetProcAddress` probe at
  startup, never a link against `ntdll`, and its absence is an ordinary answer — the first bridge
  needs no helper thread at all. `BackendParity_test` runs the whole shared matrix against it, and
  gains a Windows case that registers `CONIN$` on **every** backend: that one wait serving both a
  console handle and a socket is the thing neither upstream had, and it is why fastcached kept a
  second coroutine runtime.

- **`ReadinessHandler::slot`** (`core::net::detail::ReadinessSlotRef`, `<core/net/detail/ReadinessSlot.hpp>`),
  and the ownership rule written beside it. A completion-based backend hands the kernel a pointer
  and gets it back on a later turn — an operation the caller has since cancelled still completes —
  so `lpOverlapped` points at a **backend-owned, refcounted** slot and never into the handler,
  which by then may be freed. The handler holds one share for the length of its registration; each
  in-flight operation holds another; a packet arriving after `detach` finds the slot retired and
  drops without reading anything of the handler's. A readiness backend leaves the field empty and
  nothing notices; an owner never reads it. The design spec declared it and Task B3 left it out,
  because a public field with no reader has no defined meaning — it **arrives with its first
  writer** rather than ahead of one.

- **`core::net::ICompletionPort`** (`<core/net/ICompletionPort.hpp>`), what a completion-based
  backend lends the sockets that sit on it, and the one place a handle is associated with a port —
  which is what makes guarantee **G4** (a SOCKET is associated with exactly one port) assertable.
  `CreateIoCompletionPort` refuses a second association with `ERROR_INVALID_PARAMETER`, which is
  also what it answers for a closed handle and half a dozen ordinary mistakes, so a caller reading
  that back would condemn a working connection; the port keeps the record itself and refuses by
  name. An owner that CLOSES a handle must call `forget()`, or the next socket handed that value
  looks already associated and is then associated with nothing. `IoBackend` gains
  `completionPort()`, defaulted to `nullptr`; it is declared on **every** platform rather than
  behind `#if defined(_WIN32)`, because a public header that changes shape per platform is one a
  consumer's build can disagree with this one about.

- **Guarantees G1 and G4 are asserted on the Windows port, and each has a canary.**
  `core-cpp.iocp-canary.g1` calls `wait()` from a second thread while another is dequeuing;
  `core-cpp.iocp-canary.g4` associates one handle with one port twice. Each is a separate program,
  because an assertion aborts the process and so cannot be provoked from inside a test case; each
  is judged by a marker it prints to `stderr` immediately before the forbidden call, and both SKIP
  where assertions are compiled out. They exist because neither violation fails
  on its own: IOCP is *designed* to be drained by many threads, and a lost association is a socket
  awaiting completions that are delivered elsewhere — a hang with nothing in any log.

- **`core::net::IocpSocket` and `core::net::IocpListener`, the Windows socket and listener whose
  operations are overlapped `WSARecv`, `WSASend` and `AcceptEx` completed by the loop's port,
  and a `ConnectEx` dial beside them.** Nothing is chosen at compile time: `listen`,
  `adoptListener`, `connect`, `makeConnector` and `testing::makeSocketPair` ask the loop
  (`EventLoop::completionPort()`, new) and hand out these over a completion port and
  `WindowsSocket`/`WindowsListener` over WFMO. AF_UNIX (`listenUnix`, `connectUnix`) stays on
  `WindowsSocket`/`WindowsListener`, which the port serves through its waitable-handle bridge. The
  headers are private; a consumer reaches them through `ISocket` and `IListener`.

  What a consumer can observe, since the kernel performs the operation and the completion reports
  what it did:
  - **a stop of the awaiting flow, a receive deadline and a dial deadline each ask the kernel for
    the operation back and let its completion answer**, so bytes already received win over a
    later stop ([fastcached#884](https://github.com/LASTRADA-Software/fastcached/issues/884)) and
    a connection the kernel made first is not thrown away;
  - **the kernel is never handed the caller's memory**: an overlapped receive lands in a buffer
    the operation owns (at most 64KiB) and is copied out when it is delivered, and an overlapped
    send is copied in (at most 256KiB per operation). So a read abandoned mid-flight -- its
    awaitable destroyed, its socket destroyed, its loop torn down -- cannot have the peer's bytes
    written into memory the caller has freed. The copy is paid only by an operation that had to
    wait;
  - **`cancelRead` on a real read settles** with whatever its receive did (its bytes, or
    `Cancelled`) on a later turn; the read SLOT is free at once, and a `waitReadable` probe is
    retired inline as on every other socket;
  - **an operation outlives its socket**: each holds itself until the port dequeues it, so a
    socket or listener destroyed mid-operation leaves the kernel writing into, and reading a
    gathered write's payload out of, storage that still exists
    ([fastcached#465](https://github.com/LASTRADA-Software/fastcached/issues/465)); an accept
    whose frame is destroyed while it waits closes the socket its AcceptEx was issued into, so
    the next client is the next accept's rather than accepted into a socket nobody owns;
  - bytes already there, and room already in the send buffer, are taken **without an operation**,
    so a read or a write costs the same number of turns as on every other socket.

  The dial's outcome is the completion's status, never `SO_ERROR`, followed by
  `SO_UPDATE_CONNECT_CONTEXT`; an accepted socket gets `SO_UPDATE_ACCEPT_CONTEXT`, without which
  `shutdownWrite` sent no FIN (fastcached#1556). `IocpListener` claims its address with
  `SO_EXCLUSIVEADDRUSE`. Ported from fastcached `Net/IocpSocket.{hpp,cpp}`, `Net/IocpDial.hpp` and
  `Net/IocpStatus.hpp` at `0708dd54`; 21 cases in `windows/IocpSocket_test.cpp` and 4 in
  `windows/IocpDial_test.cpp`, and every socket suite that runs over `BackendMatrix` now covers
  both Windows sockets.

- **`HandleKind::Completion`**, a park on an overlapped operation an owner issued on a completion
  port, and **`ICompletionPort::beginOperation`/`withdrawOperation`**, the record that lets a port
  tell an owner's packet from its own. It is how a completion reaches the loop's turn step 2 --
  the backend reports the park and the loop resumes, as for readiness -- instead of a callback
  that resumes a coroutine from inside the backend's own walk. **Every backend without a port
  refuses it by name with `Unsupported`** -- poll, epoll, kqueue, WFMO and the host-driven one --
  rather than handing an operation's address to the kernel as a descriptor, and one parity case
  holds all of them to it. `EventLoop::completionPort()` is declared on every platform and answers
  `nullptr` wherever the backend lends no port; callers branch on that, never on the preprocessor.

- **`CancelRead_test` and all four socket-contract canaries run on Windows**, over `IocpSocket`.
  The WFMO leg of `CancelRead_test` SKIPs out loud, because `WindowsSocket` does not implement
  `cancelRead`. The two write canaries now keep writing until one write stays pending (a Windows
  non-blocking send takes 8MiB at once, so one write never parked there), and SKIP with a reason if
  none does within 256MiB.

- **Three CI legs that never existed, and the gate that makes their absence fatal.** Every visible
  configure preset must now be named by a workflow or allowlisted with a written reason;
  `core-cpp.preset-coverage` (label `tree-level`, with a 15-case self-test) refuses a preset no job
  runs, an allowlist entry for a preset a job now runs, an entry for a preset that no longer
  exists, and a workflow naming a preset `CMakePresets.json` does not define.

  It was written because three presets were run by nothing, and **all three were Debug**:
  `gcc-debug`, `clangcl-debug`, and `appleclang-debug` — which was the *only* Debug configuration
  macOS had. So `NDEBUG` was defined in every macOS job and **all 30 runtime assertions in
  `src/core` were compiled out of the whole platform** — 19 of them in the shared event-loop code,
  among them the twelve `teardownIsSerialisedWithDispatch()` thread-affinity checks in
  `EventLoop.cpp` and `ReadyBatch`'s re-entrancy trap — and both canaries abstain with 77 under
  `NDEBUG`. kqueue is macOS-exclusive, so those shared checks had never once been evaluated with
  kqueue underneath them — on the platform Ruling R101 exists because of. The count is of runtime
  `assert()` only: the 122 `static_assert`s fire at compile time and `Require()`/`Guarantee()` are
  not `NDEBUG`-gated, so neither family was ever dark.

  All three legs are added: `gcc-debug` to `linux`, `appleclang-debug` to `macos`, `clangcl-debug`
  to `windows`. The LLVM-version floor on the Windows job now covers every clang-cl leg rather than
  the release one alone, or the new leg would have built with the runner's bundled clang-cl,
  silently below the project's floor of 22.

  A configuration absent from CI does not fail there — it is simply not present, and an absent gate
  reads exactly like a passing one. `.agent/rules/build-and-toolchain.md` states it as a property
  rather than a preset list, because a list there would decay the way four earlier enumerations in
  this module did.

### Deprecated

- `core::net::interruptibleSleepUntil(loop, token, deadline, wakeBound)`, the four-argument form,
  is kept for one release so a fastcached caller compiles unchanged, and **ignores `wakeBound`**.
  It named the longest uninterruptible step of a poll, and there is no poll left to bound. Drop the
  argument. It carries no `[[deprecated]]` attribute deliberately: the overload exists so a
  fastcached caller **compiles unchanged**, and the attribute under that consumer's own `-Werror`
  is exactly what would stop it doing so. What reports a migration in this project is
  `tools/migrate/renames.json` and the codemods, not the compiler, and the row is already there.

### Breaking

- **`ITlsContext::wrap()` and `wrapTls()` take the loop the socket belongs to** (Task B11, fix
  round 1): `wrap(std::move(socket), loop)`, `wrapTls(std::move(socket), context, loop)`, where
  `loop` is any `core::async::IExecutor` and outlives the socket. A TLS socket parks operations on
  itself -- a read waiting while a write drives the handshake, a write waiting for a flush in
  progress -- and a waiter is never resumed inline by whatever releases it, so the socket needs a
  loop to hand it to. Migration: pass the `EventLoop` the inner socket was made on.

- **`PosixSocket` no longer reports `EPIPE` as `NetErrorCode::ConnReset`; it is `SystemError`.**
  `EPIPE` is a write after this end's own half-close, or after a peer's FIN that the previous write
  turned into a reset -- the state Winsock reports as `WSAESHUTDOWN` or `WSAECONNABORTED`, which
  `WindowsSocket` already answered as `SystemError`. A reset is the peer closing over bytes it had
  not read (`ECONNRESET`), and a caller counts that apart from a goodbye. The two platforms disagreed
  about one state until the in-memory socket's parity test ran against both.

  Migration: a caller that treated `ConnReset` as "the peer is gone" on a WRITE should test for
  `ConnReset` or `SystemError` there, or better, stop writing once a read has seen EOF. No consumer
  branches on `ConnReset`: not contour, endo, tuidu, Lightweight or morph, and fastcached only
  produces it in its test doubles.

- **A TLS read of a transport that ended before the peer's `close_notify` is
  `NetErrorCode::ConnReset` ("peer closed without close_notify"), not a zero-byte read** (Task B11).
  `0` says the stream ended whole, so answering it for a bare FIN let a truncated stream -- cut
  short by an attacker, or by a crash -- pass as a complete one; OpenSSL 3 reading from a socket
  refuses the same thing. `close_notify` is still `0`, and `waitReadable()` answers as `read()` does.

  Migration: a caller that read `0` as "the peer is gone" also gets `ConnReset` from a TLS peer that
  closes without the alert -- which some clients do -- and should treat both as the connection's
  end. Only a caller that trusts the stream's end as the end of a MESSAGE must treat `ConnReset` as
  a message that did not arrive whole.

- **`generateSelfSignedCertificate()` and `makeSelfSignedServerContext()` take a
  `SelfSignedOptions`, and the default common name is `"localhost"`, not `"contour-daemon"`**
  (Task B11). A consumer's name had no place as a library default. The generated certificate also
  changes: a P-256 key rather than RSA-2048 (sub-millisecond rather than occasionally a second), a
  subjectAltName carrying the names -- which is what a client checks -- and a random serial, since
  a client that has seen two certificates with one issuer and serial refuses the second.

  Migrations:

  - `generateSelfSignedCertificate("contour-dev")` becomes
    `generateSelfSignedCertificate({ .commonName = "contour-dev" })`.
  - A caller relying on the old default passes `{ .commonName = "contour-daemon" }`.
  - A client pinning a certificate by host name is unaffected: the subjectAltName carries the
    common name when no other names are given.
  - fastcached's `TlsContext::CreateSelfSigned(names, validity)` becomes
    `makeSelfSignedServerContext({ .commonName = ..., .subjectNames = names, .validity = validity })`,
    and `TlsContext::Create(cert, key)` becomes `makeTlsServerContextFromFiles(cert, key)`; both
    return `std::shared_ptr<ITlsContext>` and a string reason. `tools/migrate/renames.json` has the
    rows.

- **`core::net::connect()` no longer resolves a name on the calling thread**, and callers of
  contour's `connect(loop, host, port)` inherit that without a source change. The body called
  `getaddrinfo` inline, which on an event loop is a stall of unbounded length: a lookup with a dead
  resolver is seconds, and every coroutine on that loop waits for it, including the ones with
  nothing to do with the network. It is now `makeConnector` plus `defaultAsyncResolver()`, whose
  pool starts on first use and is never touched by a dial to a literal address.

  Migrations:

  - **`host` is a `std::string`, not a `std::string_view`**, in both overloads. The task is lazy,
    so its body -- and any copy it made -- first runs when the task is awaited, by which time a
    view may name a temporary that died with the call expression:
    `auto t = connect(&loop, makeHost(), port); co_await t;` sent freed bytes to the resolver. A
    string literal or a `std::string` compiles unchanged; a `std::string_view` argument is spelled
    `std::string { view }`.
  - **A stop of the awaiting flow's own token throws `core::async::OperationCancelled`**, where
    contour's body caught it and returned `NetErrorCode::Cancelled`. That is the rule every loop
    awaitable follows -- a cancel from the flow unwinds, a cancel from the resource is a value. A
    loop such as `while (true) { auto r = co_await connect(...); if (!r && r.error().code ==
    NetErrorCode::Cancelled) break; }` now leaves by the exception instead: catch it, or let it
    unwind the flow that was stopped. `connectUnix` still returns `Cancelled` as a value until it
    moves onto the same dial (Task B9); `Sockets.hpp` documents the difference on both.
  - **A name that cannot be resolved is `NetErrorCode::AddressError`**, as contour's `connect`
    reported it and as `NetError.hpp` defines it ("address resolution or parsing failed"). So is
    an empty host, and so is `core::net::resolveFailure`. `AddressNotAvail` is a bind's code: a
    local address that is not available.
  - **Anything that assumed resolution happened INLINE is now wrong.** A dial to a name may suspend
    before it reaches a descriptor, so a caller that counted turns, or that relied on `connect`
    having touched the network by the time the first `co_await` returned, has to be re-read. A dial
    to a literal still never suspends for resolution.
  - **Injecting a resolver, a budget or keepalive** uses the new overload:
    `connect(&loop, host, port, resolver, DialOptions { .connectTimeout = …, .keepAlive = … })`,
    or `makeConnector(loop, resolver)` where one connector serves many dials.
  - **Stop the resolver before the loops it hands results back to.** `defaultAsyncResolver()` is a
    process singleton and joins at exit; a resolver a consumer owns must be stopped while the loops
    it was given are still able to run a turn, or a queued lookup's hand-back is a leaked frame.

- **fastcached's `IAdmissionControl` is one atomic `tryAdmit()` and a lease, not three calls.**
  `AllowAccept()` followed by `OnConnectionStarted()` was a check-then-act: two loops sharing a
  policy capped at 100 with 99 in flight could both be told yes, and hold 101. `tryAdmit()` decides
  and counts in one step and returns `std::optional<AdmissionLease>`; the lease's destructor is
  what `OnConnectionEnded()` was, so it runs exactly once on every path, where an unmatched end
  used to wrap the counter to `SIZE_MAX` and refuse every accept after it. `SetMax` is gone: it
  wrote a plain field other threads read, and the cap is now a constructor argument -- a reload
  builds a new policy. `renames.json` carries the rows, as `manual` because each is a reshaping
  rather than a rename.

- **`IListener::localPort()` is `IListener::boundPort()`.** Mechanical: `tools/migrate/renames.json`
  carries the row and `tools/migrate/rewrite.py` applies it. The name says what the value is -- a
  bind to port 0 means "pick a free one", so the number a caller needs is the one the kernel chose
  and not the one that was asked for. `AcceptResult` is now an alias of `SocketResult` rather than a
  second spelling of the same type; nothing a caller writes changes.

- **`core::net::ISocket::shutdownWrite()` returns `ResultAwaitable<void>`**, not `void`, and is no
  longer `noexcept` -- the shape `handshakeIfNeeded()` already has, for the same reason. A clean
  TLS half-close is a `close_notify` that has to be written and flushed before the FIN, which a
  synchronous `void` verb cannot await; forwarding to the inner socket instead sends a FIN with no
  `close_notify`, which a strict peer reads as truncation. `PosixSocket` and `WindowsSocket`
  complete it inline, and now report a failed `shutdown` as a `NetError` rather than dropping it
  (`ENOTCONN` / `WSAENOTCONN` still resolve as success). `TlsSocket` still inherits the no-op.

  It also has a stated precondition: **no write outstanding.** A decorator's half-close writes
  through the inner socket and so claims its write slot, which the Debug write-slot guard refuses
  over a parked write; there is no `cancelWrite`.

  Migration: a call in statement position, `sock->shutdownWrite();`, becomes
  `co_await sock->shutdownWrite();` (inspect or `std::ignore` the result), after awaiting any
  write in flight. Outside a coroutine, on a plain socket, `await_ready()` drives it, since it
  completes inline. An override becomes
  `[[nodiscard]] ResultAwaitable<void> shutdownWrite() override`.

- **`core::tui::runtime::TuiRuntime` is composed on `core::net::EventLoop`, and the project no
  longer carries a second scheduler.** The runtime had its own ready queue, timer min-heap, park
  slots, `pumpOnce` and blocking `EventSource`; all of it is the loop's now, and every scheduling
  member of `TuiRuntime` forwards there. What is genuinely the TUI's and stays is input semantics:
  the decoded-event buffer, `nextEvent()` / `nextEventFor()` / `nextActivity()` /
  `nextAgentReady()`, and the interrupt policy. `core::tui` links `core::net` as a result, and the
  module table's `tui` row carries it.

  Gone with the second scheduler: `runtime::EventSource`, `runtime::PollEventSource`,
  `runtime::TerminalEventSource`, `runtime::WaitFdAwaiter`, `runtime::withTimeout`,
  `runtime::testing::MockEventSource`, and the readiness vocabulary they shared --
  `FdInterest`, `hasInterest`, `FdToken`, `WaitOutcome`, `FdRegistration`, `FdRegistry`. The
  headers `runtime/EventSource.hpp`, `runtime/PollEventSource.{hpp,cpp}`,
  `runtime/WithTimeout.hpp`, `runtime/posix/PollHelpers.hpp`,
  `runtime/testing/MockEventSource.hpp` and all three `TerminalEventSource` files are deleted;
  `tools/migrate/renames.json` carries a row for each, with what to write instead.

  Migrations, in the order a caller meets them:

  - **Construction.** `TuiRuntime(EventSource&, IClock&)` becomes
    `TuiRuntime(core::net::EventLoop&, Terminal&, TuiRuntimeOptions = {})`, or
    `TuiRuntime(EventLoop&, InputSource&, …)` where the input is injected. The clock is the
    loop's, so a `ManualClock` is given to the loop rather than to the runtime. **Destroy the
    runtime before its loop, on the loop's thread**: its source flows are parked on the loop and
    name it, and the destructor is what takes them back.
  - **The agent wakeup has no handle.** Where a `core::platform::Wakeup` was passed to
    `TerminalEventSource` and waited on, a worker now calls
    `loop.post([&]{ runtime.notifyAgentReady(); })` -- the loop's own cross-thread surface. The
    `nextAgentReady()` and `nextActivity()` vocabulary is unchanged.
  - **The interrupt wakeup is `TuiRuntimeOptions::interruptWakeup`**, and the POSIX signal fd is
    `TuiRuntimeOptions::signalFd`. `core::platform::SignalHandler` records the signal and signals
    the wakeup exactly as before; what changed is that a flow parked on that handle runs the
    policy, rather than a branch inside a hand-built wait set.
  - **`runtime::withTimeout(&runtime, …)` is `core::net::withTimeout(&runtime.loop(), …)`.** The
    two were the same construction over two schedulers.
  - **`waitReadable`/`waitWritable` return `core::net::WaitHandleAwaiter`** and take an optional
    `core::net::HandleKind`. They throw `core::net::FdRegistrationFailed` where the backend
    refuses a handle, which the old awaitable flattened into `OperationCancelled` -- so a caller
    that catches only `OperationCancelled` now lets a plumbing failure escape, which is the point:
    the two were indistinguishable and are different facts.
  - **`delay`/`sleepUntil` return `core::net::DelayAwaiter`**, and `rootStopSource()`, `clock()`,
    `spawn()` and `blockOn()` are the loop's. Prefer `loop.requestStop()` over
    `rootStopSource().request_stop()`: it also unparks what it cancels.
  - **A headless runtime replaces `PollEventSource`.** An `InputSource` that reports
    `platform::InvalidHandle` for its input handle starts no input flow, and the socket work that
    used to need a headless event source belongs to `core::net::EventLoop` directly.
  - **Tests.** `runtime::testing::MockEventSource` is replaced by two doubles that are each one
    thing: `core::net::testing::ScriptedBackend` scripts readiness (or a real
    `core::platform::SystemPipe` provides it), and
    `core::tui::runtime::testing::ScriptedInputSource` scripts decoding.


- **`core::net::ISocket`'s operations are frame-free, stop-aware awaitables rather than
  `async::Task`s.** `read`, `write`, `writeVectored`, `waitReadable` and `readWithFd` return
  `core::net::ResultAwaitable<R>` (`IoAwaitable` is the byte-count spelling), and
  `handshakeIfNeeded` returns `ResultAwaitable<void>`. Awaiting one allocates no coroutine frame,
  which is what lets a server hold a parked read per connection without paying a frame per idle
  connection. New on the interface, arriving from fastcached: `writeVectored`, `waitReadable`,
  `cancelRead`, `shutdownWrite`, `setReceiveDeadline` and `handshakeIfNeeded`. Also new:
  `core::net::SocketResult`, and `core::net::contract::{requireReadBuffer, claimReadSlot,
  claimWriteSlot, assertTeardownIsSerialisedWithDispatch}` — the socket contract's guards, public
  because a transport outside this library is under the same rules.

  *Migration, for a caller that only awaits.* Nothing changes: `co_await sock.read(buffer)` works
  against an awaitable exactly as it did against a task.

  *Migration, for a caller that STORES the operation.* An awaitable is a one-shot temporary bound
  to its `co_await` expression — it cannot be held across a suspension point, put in a container or
  handed to `whenAny`, because nothing but the awaiting frame keeps it alive. Wrap it:
  `loop.blockOn(sock.read(buffer))` becomes
  `loop.blockOn(core::async::asTask(sock.read(buffer)))`, and likewise for
  `whenAny(sock.read(buffer), …)`. `core::async::asTask` (new, `<core/async/AsTask.hpp>`) costs
  exactly the frame the awaitables avoid, which is why it is a call at the site that needs one
  rather than an implicit conversion every site gets.

  *Migration, for a caller that IMPLEMENTS `ISocket`.* A transport whose operations are genuinely
  coroutines — a TLS record pump, a scripted test double — keeps its coroutine and wraps it:
  `IoAwaitable read(std::span<std::byte> b) override { return IoAwaitable { readTask(b) }; }`, where
  `readTask` is the old `Task<IoResult>` body unchanged. The awaiting flow's stop token still
  reaches it through `Task`'s own awaiter. A transport that wants the frame-free path passes an
  arm hook, a retire hook and an owner pointer instead. `writeVectored` and `readWithFd` have
  working defaults, so an existing implementation need not grow them.

  *Migration, for a caller of `readWithFd`.* Unchanged in behaviour: the base default still reads
  through `read` and reports `fd = -1`.

- **Cancellation of a socket operation now distinguishes the flow from the resource.** A stop on
  the awaiting flow's own token throws `core::async::OperationCancelled`; a cancel from the socket
  — `close()`, `cancelRead()` — resolves with `NetErrorCode::Cancelled` as a **value**.

  *What changed, precisely.* A read that was **already parked** when `close()` arrived used to
  resume, re-check the socket and report `BadHandle`; it now reports `Cancelled`. A read **issued
  after** the socket was closed still reports `BadHandle`, unchanged — the two were the same code
  before and are now distinguishable, which is the point. So a caller that branched on `BadHandle`
  to mean "somebody closed this socket under me" must add `Cancelled`; a caller that used it to mean
  "this socket is not usable" needs no change.

  *And the flow side.* A stop on the awaiting flow's own token used to surface as whatever the
  loop's `waitReadable` awaiter threw, from inside the read's retry loop; it is now
  `OperationCancelled` out of the socket operation itself. A caller that never caught it and only
  inspected the error value now has to catch it around a read it can cancel. Design spec §2 item 5.

- **`core::net::ParkEntry` gains a frameless readiness park**, and with it
  `core::net::ReadyCallback` and `core::net::ParkWake`. `ParkEntry::onReadyCallback(callback,
  state, handle, kind, interest)` files a park that calls back rather than resuming a coroutine,
  and — unlike a timer park — SURVIVES its own dispatch, so its owner can run a retry loop across
  many wakes and retire it with `unregisterPark`. It is what makes a socket operation frame-free,
  and it is a park in the same table as every other kind, so it inherits `notifyHandleClosing`,
  `requestCancel`'s generation check, `registerPark`'s host-wake arming and the turn's decision to
  enter the backend wait. Nothing existing changes shape: a `ParkEntry` built through `onDeadline`,
  `onCallback` or `onReadiness` behaves exactly as before.

  **It does NOT inherit `~EventLoop`'s teardown, and an earlier version of this entry said it
  did.** A callback park has no coroutine to resume, so teardown step 2 skips it and
  `unparkEverything` excludes it by its `!entry->parked` test — both deliberately, because calling
  it would reach an owner that is being destroyed. The consequence for a caller: **a socket
  operation still parked when its loop is destroyed is neither completed nor abandoned, and the
  awaiting coroutine is never resumed and never unwinds.** So destroy sockets before the loop they
  were created on, which is already the documented ordering for every loop-owned object.


- **`core::net::EventLoop` is the merged reactor contract: a five-step turn, a six-step teardown,
  a park table and the thread-affinity guarantees, all asserted.** It implements
  `core::async::IExecutor`, so anything that takes an executor -- `ResumeOn`, `AsyncQueue`, a
  `Task` chain -- takes a loop. What arrives with it: `run()`, `runOnce()`, `runUntilIdle()`,
  `stop()`, `submit()` and `schedule()` in both the borrowing and the owning form,
  `cancelPending()`, `resumeSoon()`, `registerPark()`, `unregisterPark()`, `requestCancel()`,
  `running()`, `isOnWorkerThread()`, `teardownIsSerialisedWithDispatch()`, `IdlePolicy`,
  `EventLoopOptions`, `RunOnceResult`, `ParkEntry`, `core::net::PlatformLoop` (which owns
  `makeDefaultBackend()`) and `core::net::testing::TestLoop` (the real loop over `NullBackend`,
  driven by hand). `ParkId` widens from an fd wait to every kind of parked work, and it is the
  generation check: ids are never reused, so a cancel request for a park that has gone resolves to
  nothing.

  Migrations, in the order a caller meets them:

  - `WaitFdAwaiter` is `WaitHandleAwaiter`, and `waitReadable`/`waitWritable` take a second,
    defaulted `HandleKind`. Call sites that wrote `auto` or `co_await` change nothing; one that
    named the type changes the name.
  - `delay()` takes a `core::platform::SteadyDuration` rather than `std::chrono::milliseconds`. A
    `5ms` argument converts; a caller that stored the parameter type changes it.
  - **`blockOn()` drives its task to completion, blocking the calling thread while the loop is
    idle, and returns only when the task finishes.** An idle turn waits on the backend rather
    than polling, so a flow that `co_await ResumeOn { pool }`s and comes back completes here —
    and a flow nothing will ever advance waits rather than burning a core
    ([core-cpp#17](https://github.com/contour-terminal/core-cpp/issues/17), which Task B12 had
    been carrying for the TUI runtime). On an `IdlePolicy::Return` loop, which is one somebody
    else drives a turn at a time, the wait cannot block and such a flow still spins;
    `testing::TestLoop` forces that policy, so drive it with `runOnce()` or `runUntilIdle()`.
  - **A coroutine resumed by readiness or by a deadline resumes one turn later**, in the next
    turn's step 2, because there is exactly one place a loop resumes and that is what makes
    guarantee G2 stateable. A test that counted waits, or that used `blockOn(trivialTask())` as
    "pump once", counts differently now; `runOnce()` is what drives one turn.
  - **`~EventLoop` frees what the loop owns and resumes what it borrows.** A `DetachedTask` parked
    on a loop that is destroyed is FREED, not run on: it carries no stop token, so resuming it
    would not cancel it, it would run the rest of its body on a loop that is going away
    ([fastcached#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)). A flow whose
    frame a `Task` owns is resumed, observes the stop and unwinds, as before.
  - `EventLoop::run()` and `blockOn()` are **precondition violations on a host-driven loop** and
    assert. They are compiled under WebAssembly rather than removed, so the mistake is an abort
    with a message rather than a link error in a consumer's build.
  - `IoBackend` gains `setPump(HostCallback, void*)`, defaulted to a no-op beside `isHostDriven()`
    and `armWakeAt()`. A backend outside this repository need not implement it; a host-driven one
    that wants a loop to pump must.
  - **Six loop-thread-only members now assert their thread affinity** — `spawn`, `resumeSoon`,
    `requestStop`, `registerPark`, `unregisterPark`, `wakeReasonOf`, plus `cancelPending` and
    `notifyHandleClosing`. They mutate the loop's own containers with no lock and no inbound
    queue to hand to, so a call from a second thread while another drives tears a `std::list` or
    rehashes a map underneath a turn. `spawn` is the one to check first when migrating from
    `submit`: it looks like it and is not. `core-cpp.loop-affinity-canary.<member>` proves each
    one fires. `resumeSoon`'s documentation previously named thread-pool callbacks among its
    callers, which the assert aborts — use `submit(async::ParkedWork)`, the same operation with
    the cross-thread hand-off.
  - `FdRegistrationFailed` gains a `NetError reason` member carrying what the backend refused
    with. `attach()` and `setInterest()` both return the kernel's reason so it is never swallowed,
    and the loop was flattening both to `bool`: a consumer debugging descriptor exhaustion could
    not tell it from a filter the kernel would not arm. Nothing constructs the type with
    arguments, so no call site changes; a `catch` that wants the reason reads `.reason`.
  - **`~EventLoop` DROPS borrowed work still waiting in the inbound queue**, and this is now
    stated rather than left to be discovered. What the loop owns is freed and what it borrows is
    resumed — in the ready queue and the park table, which hold work a turn has accepted. A
    submission the inbound queue still holds is an offer no turn took up, and the loop cannot tell
    what a borrowed `std::coroutine_handle<>` names: a suspended flow that would unwind and a
    never-started lazy `Task` that would RUN are the same type, and `ResumeOn::await_resume()` is
    noexcept, so resuming one runs its body against a loop that is being destroyed rather than
    unwinding it. The cost is real and worth planning around: a cross-thread `ResumeOn { loop }`
    whose loop dies before the next turn leaves its awaiting flow suspended forever. **Run one
    more turn before destroying a loop other threads have been handing work to.** Posts are
    dropped for the same reason, and owned chains are still freed rather than resumed.
  - **A readiness park keeps its handle registration until the park itself is taken.** A park
    whose waiter had been queued but not yet resumed was invisible to
    `notifyHandleClosing()` for the turn in between, so its kernel registration was detached
    after the close rather than before it — against a descriptor number the kernel may already
    have reassigned. `parkedWaiterCount()` counts those parks again, which is what its
    documentation always claimed.

  Consumer impact: contour, endo and tuidu all construct an `EventLoop`. The rename table
  (`tools/migrate/renames.json`) carries `net::WaitFdAwaiter`, and fastcached's `IReactor`,
  `PlatformReactor`, `TestReactor` and their members are marked delivered.

- `core::platform::testing::InMemoryFileSystem` models a file's lifetime the way POSIX does, where
  it used to hand each stream a private copy. A stream now survives `remove()` of its file and
  follows it across `rename()`, `openRead()` sees writes that land after it was opened, and
  `copyFile()` onto an open destination overwrites in place rather than detaching the stream.
  Migration: a test that relied on a read stream holding a snapshot of the file it opened must
  read the file before the write, or re-open it after. The divergences that remain between this
  fake and `NativeFileSystem` are listed in
  [core-cpp#27](https://github.com/contour-terminal/core-cpp/issues/27).
- `core::tui`'s completion types move to `core::tui::completer`, the namespace their directory
  names, as `src/core/tui/runtime/` already gives `core::tui::runtime`. endo's TUI is one flat
  `namespace tui` and the import kept that, which core-cpp's namespace-equals-directory rule does
  not allow; it was invisible until Task A11 fixed the hygiene rule that only looked at the first
  directory segment. Migration, for each of `Completer`, `CompletionConfig`, `CompletionProvider`,
  `CompletionItem`, `FuzzyMatch`, `FuzzyConfig`, `FuzzyMatchResult`, `SmartCaseMatch` and
  `SmartCaseConfig`: `core::tui::Completer` becomes `core::tui::completer::Completer`, and so on.
  The include paths do not change. Recorded here rather than under **Changed** because this file's
  preamble puts every API break under **Breaking** with a migration note
  ([core-cpp#30](https://github.com/contour-terminal/core-cpp/issues/30)).
- `core::async::whenAny()` resolves to `std::optional<std::size_t>` rather than to a `std::size_t`
  that was `core::async::detail::WhenAnyNoWinner` (`SIZE_MAX`) when nothing won. The sentinel was
  part of the documented public result but lived in `detail::`, so handling the empty case meant
  reaching into `detail::`, and a caller who forgot the check indexed a container at `SIZE_MAX`.
  Migration: `auto const i = co_await whenAny(...);` becomes
  `auto const i = co_await whenAny(...); if (i) use(*i);`, and any `== detail::WhenAnyNoWinner`
  becomes `!i.has_value()`. Nothing outside this repository reads the result yet.
- `core::async::whenAll()` and `whenAny()`'s variadic overloads take their tasks by rvalue. The
  constraint was written over `std::remove_cvref_t`, so an lvalue `Task<void>` satisfied it and
  then failed inside `std::vector::push_back` on `Task`'s deleted copy constructor. An lvalue or a
  `const` rvalue is now "no matching overload" at the call. Migration: `whenAll(std::move(task))`,
  which is what every call already had to do to compile.
- `core::platform::FileSystem::openWrite()` takes a `core::platform::WriteMode` and `copyFile()` a
  `core::platform::OverwritePolicy`, in place of the `bool` each took before. A `bool` in an API is
  an anonymous enum whose two values are named after their representation rather than their meaning
  (`.agent/rules/design-principles.md`), and `FileSystem.hpp` is public API for every consumer, so
  this costs nothing now and would be a break once one of them passes `true`. Migration:
  `openWrite(p, true)` becomes `openWrite(p, WriteMode::Append)` and `openWrite(p, false)` becomes
  `openWrite(p, WriteMode::Truncate)`; `copyFile(a, b, true)` becomes
  `copyFile(a, b, OverwritePolicy::Replace)` and `copyFile(a, b, false)` becomes
  `copyFile(a, b, OverwritePolicy::Refuse)`. The defaults are unchanged, so a call that took the
  default needs no edit; an implementation of the interface outside core-cpp mirrors the two
  signatures.
- `core::platform::testing::TestEnvironmentProvider` opens the namespace its directory names,
  alongside its neighbours `InMemoryFileSystem` and `MockFileInfoProvider`; it used to open
  `core::platform`. Migration: spell it `core::platform::testing::TestEnvironmentProvider`.

- `core::Flags::operator&=` intersects instead of clearing, which silently reverses what it
  answers. It called `disable()`, so `f &= X::A` kept everything except `A` while
  `f = f & Flags { X::A }` kept only `A`: the compound operator computed the complement of its
  binary form. Nothing in contour, endo, tuidu or morph uses it, so nothing has to change today,
  but the reversal is invisible at the call site — it compiles either way. Migration: a caller
  that wanted the old meaning spells it `f.disable(X::A)`. An overload taking a `Flags` was added
  too, so the pair is symmetric with `operator|` and `operator|=`.
- `core::FNV`'s byte-wise overload takes only a type with unique object representations, which
  narrows what compiles. It accepted any trivially copyable type and walked its object
  representation, padding included, so two objects with equal members hashed differently
  depending on what their padding held. It now rejects any type with padding bits — a struct with
  interior padding, and `float` and `double`, whose representations have padding bit patterns.
  Migration: hash the members one at a time, or pass
  `std::bit_cast<std::array<unsigned char, sizeof(T)>>(value)`, which is what the overload does
  for the types it still accepts. No consumer instantiates it with such a type: contour's and
  endo's `FNV` uses all go through the `char`, `uint8_t` or `string_view` overloads.
- `core::base64::decodeLength()` answers a different number for the same input: the size of the
  base64 prefix, where it used to size from the whole input including padding and any trailing
  junk. Migration: none for a caller that used it to reserve a buffer for `decode()`, which is
  what it is for — the answer is still an upper bound, just a tight one. A caller that relied on
  the old over-estimate for something else wants its own arithmetic. endo sizes an image buffer
  with it (`GeminiProvider.cpp`) and was over-allocating.
- `core::readFileAsString()` answers a different string for the same file: exactly the bytes on
  disk. It sized from `file_size()` and read in text mode, so on Windows CRLF translation
  delivered fewer bytes than it had reserved and the shortfall stayed behind as trailing NULs;
  and it narrowed the path through `path::string()`, which cannot represent every name a
  filesystem accepts and throws on Windows for the ones it cannot. A missing file now answers
  empty rather than throwing, as its documentation said all along. Migration: a caller that
  trimmed trailing NULs off the result can stop; one that caught `std::filesystem::filesystem_error`
  for a missing path checks for an empty string instead. contour reads a CA certificate and a
  forced-DPI file through it.
- Every function in `<core/Escape.hpp>` — `escape()` in all three of its spellings,
  `escapeMarkdown()` in both of its, and `unescape()` — plus `core::readFileAsString()`,
  `core::detail::Times::operator[]` and `core::detail::Times2D::operator[]` are `[[nodiscard]]`.
  Discarding any of them is a bug: none has an effect other than its return value. A consumer that does so and builds with `-Werror` stops building. Migration: use the
  result, or cast it to `void` at the one call site that means to throw it away. The whole header
  rather than the one overload that changed behaviour, because the surprise would be the
  inconsistency — `escape(text)` is the spelling most likely to be called for its return value
  alone.

- `core::net::ISocket::isClosed()` answers what its documentation has always said: true once
  `close()` was called **or** a read observed the peer's EOF. Neither `PosixSocket` nor
  `WindowsSocket` latched the second half, so a consumer polling a connection whose peer had hung
  up was told for ever that it was still open, and `SplitSocket::isClosed()` ("closed once either
  half is") inherited that. The contract is latched rather than narrowed, because the latched
  version is the one callers need. `TlsSocket` latches its own EOF too -- a `close_notify` ends the
  session whether or not the inner transport is still open. The latch is a flag of its own, so a
  peer that shut only its write side leaves `read()` and `write()` working exactly as before.
  Migration: a caller that used `isClosed()` as "did I close this myself" asks its own bookkeeping
  instead; one that polled it to drop dead connections now gets the answer it wanted.
- `core::net::FdInterest::None` mutes a registration on every `EventSource` backend, as its
  documentation says ("mute the fd without detaching it"). The Windows wait and kqueue already
  reported nothing for such a registration; poll(2) and epoll reported `POLLHUP`/`POLLERR`
  (`EPOLLHUP`/`EPOLLERR`) for it whatever interest was asked for, so a muted descriptor still woke
  its flow -- and on epoll it did so on every wait, since those bits are level-triggered, spinning
  the pump. A muted registration still counts as attached and is still found by `detach()`.
  Migration: a caller that attached with `None` and relied on being woken when the descriptor died
  attaches with `Read`, which reports HUP/ERR as read-readiness by design.
- `core::net::WriteQueue`'s constructor throws `std::invalid_argument` for a null socket rather
  than accepting it. `ITlsContext::wrap()` is documented to return null when it cannot allocate,
  and a queue built on that null constructed cleanly and crashed later in `close()` -- which is
  `noexcept`, so the failure landed at teardown, far from the call that caused it, and could not be
  reported at all. A constructed object is usable (`.agent/rules/design-principles.md`). Migration:
  check `wrap()`'s result and drop the connection instead of queueing onto nothing.
- `core::detail::Times2D::operator[]` answers the same type its `value_type` declares: a
  `std::tuple` of both coordinates, in the order iteration yields them (the inner range advances
  fastest). It answered the inner coordinate alone, so subscripting and iterating disagreed on
  what an element of a `Times2D` even is; `operator[]` changed rather than `value_type`, because
  the tuple is what `*it` already yielded and what the existing case asserts. Migration: a caller
  that wanted the inner coordinate alone takes it out of the tuple — `std::get<1>(grid[i])`, or
  `auto const [outer, inner] = grid[i];`. Nothing in core-cpp or in contour, endo, tuidu or morph
  subscripts a `Times2D`. While there: `Times::size()` and `Times::operator[]`, which nothing had
  ever instantiated, spell out the conversions their arithmetic implies instead of letting the
  compiler narrow silently.
- `core::joinHumanReadableQuoted()`'s separator is a `std::string_view` rather than a deduced
  template parameter, so the `= ", "` default it declares can be taken: `joinHumanReadableQuoted(xs)`
  did not compile before. Migration: a caller that passed something other than a string formats it
  itself — the old signature rendered the separator with `std::format`, so an `int` or a `char`
  was accepted and now is not. Nothing calls it yet, in core-cpp or in any consumer.
- `core::net::NetErrorCode::Other` is `SystemError`, and `NetErrorCode::BadFileHandle` is
  `BadHandle`. The merged enumeration takes one spelling per meaning, and these are the two the
  spec's rename map names: fastcached's `SystemError` says what the code is (an OS error nothing
  classified further — read `NetError::systemCode`) where contour's `Other` said only what it is
  not, and contour's `BadHandle` covers the Windows `HANDLE` and the waitable handle that
  fastcached's `BadFileHandle` did not name. `NetError`'s default code is `SystemError`, as it was
  `Other`. Migration, for a contour or Lightweight caller:
  `sed -i 's/NetErrorCode::Other/NetErrorCode::SystemError/g'`; for a fastcached caller:
  `sed -i 's/NetErrorCode::BadFileHandle/NetErrorCode::BadHandle/g'`. Both rows are in
  `tools/migrate/renames.json`. 53 call sites moved inside core-cpp, nearly all of them
  `makeNetError(Other, errno, …)`.
- `toString(core::net::NetErrorCode::SystemError)` is `"system error"`, where contour's
  `toString(Other)` was `"network error"`. The description follows the code's name, and both change
  in the same release. Migration: a log filter or a test matching the exact text `network error`
  matches `system error` instead; nothing else in the rendering changed.
- `core::net::NetError::toString()` renders contour's shape for both lineages —
  `connection reset (recv) [errno 104]` — where fastcached's `NetError::ToString()` rendered
  `NetError(code=9 system=104 context=recv)`. A log line's shape is API for anyone grepping their
  logs, and fastcached's is the one that loses: its `code=` is a position in an enumeration this
  release renumbered, so an old line and a new one that read alike would mean different codes, and
  a reader needs the header open to decode either. Dropping `std::format` also keeps `<format>` out
  of `core::net_types`, which links nothing and which `fastcache-cc` will link alone. Migration for
  a fastcached caller: `ToString()` is `toString()`; `ToStringView(code)`, which gave the
  enumerator's name (`"Eof"`), is `core::net::toString(code)`, which gives the description
  (`"end of stream"`) — a caller that wanted the identifier must map it itself. Anything parsing
  `NetError(code=…)` out of a log reads the words instead, and the OS number is still
  `[errno <n>]`.

- `core::tui` no longer ships one consumer's language. `LanguageId::Endo`,
  `registerEndoHighlighter()`, the `.endo` row of `ExtensionLanguageTable` and the `endo` row of
  `FenceTagLanguageTable` are gone; an application teaches `core::tui` its own language through
  `core::tui::SyntaxHighlighterRegistry` instead — a registry it constructs, fills and passes to
  whatever renders the text, rather than a process-wide callback core-cpp holds on its behalf
  ([core-cpp#24](https://github.com/contour-terminal/core-cpp/issues/24)). Nothing about the
  built-in languages changed and a registry answers for them too, so a consumer that uses only
  those recompiles unchanged: every new parameter is trailing and defaults to "the built-ins
  alone". Two exceptions to that, both narrow: code that takes the **address** of
  `highlightLine`, `detectLanguageFromExtension`, `detectLanguageFromFenceTag` or
  `detectLanguageFromPath` sees a changed function type, because a default argument is not part
  of one; and a consumer that registers an extension or fence tag core-cpp later adds as a
  **built-in** will find `registerLanguage()` refusing it with `TokenInUse` after that upgrade —
  registering a token core-cpp might one day ship is a forward-compatibility risk the refusal
  makes loud rather than silent.
  `LanguageId` gained a trailing `Last` — not a language, but how many there are, which anchors
  the new `BuiltinLanguageTable` — and the ids a registry issues begin at
  `core::tui::FirstRegisteredLanguageId` (128).
  `FilenameLanguageTable`, the well-known-file-name table `detectLanguageFromPath()` consults
  first, moves from an anonymous namespace in the `.cpp` into the header beside the other two, so
  that all three of the module's built-in tables are public and pinned by the same golden test;
  it is the table that carried a consumer's `-format` dotfile, and it was the one nothing guarded.
  Migration, for the one consumer that registered a language:

  ```cpp
  // was: a process-wide callback, and a closed enumerator naming one application's language.
  core::tui::registerEndoHighlighter(highlightEndoLine);
  auto const language = core::tui::LanguageId::Endo;

  // is: one registry the application owns, filled once at startup and injected. Hold exactly one
  // per program unless you keep each id with the registry that issued it -- see below.
  auto highlighters = core::tui::SyntaxHighlighterRegistry {};
  auto const registered = highlighters.registerLanguage({
      .name = "endo",
      .extensions = { ".endo" },
      .fenceTags = { "endo" },
      .highlight = highlightEndoLine,
  });
  // std::expected<LanguageId, LanguageRegistrationFailure>; *registered replaces LanguageId::Endo.

  // and each entry point takes the registry, as a trailing argument defaulting to nullptr:
  auto renderer = core::tui::MarkdownRenderer { output, theme, &highlighters };
  auto const styled = core::tui::StyledText::fromMarkdown(text, width, &theme, &highlighters);
  auto const detected = core::tui::detectLanguageFromPath(path, &highlighters);
  auto const [map, next] = core::tui::highlightLine(line, detected, state, &highlighters);
  ```

  `registerLanguage()` refuses a name, extension or fence tag another language already claims —
  built-in or registered, including a well-known file name that would shadow the extension — and
  refuses a token that could never match at all: an extension without its leading dot, or an
  empty extension or fence tag (`LanguageRegistrationError::MalformedToken`). It refuses rather
  than shadows, because replacing would repoint an id already issued and its holder would then
  get a wrong answer that looks right. A refused definition leaves the registry exactly as it
  was, and `LanguageRegistrationFailure` names the token at fault.

  A registered `LanguageId` **belongs to the registry that issued it.** Ids are dense from
  `FirstRegisteredLanguageId` in registration order and carry nothing that identifies their
  registry, so passing one to a different registry is a precondition violation — the contract a
  `std::vector::iterator` has with its container. If that registry issued an id in the same
  position, the line is highlighted as *its* language, silently and wrongly; only an id past the
  end of it gives plain text. A program that holds one registry, which is the shape this is
  designed for, cannot hit it. Built-in ids are not issued by anybody and *are* portable: they
  mean the same language in any registry and in none.

  `tools/migrate/renames.json` carries both removals as `kind: "removed"` rows, which assert the
  symbols stay absent rather than rewriting anything: the call shape changes, so a mechanical
  rewrite would produce code that compiles into the wrong thing, and a compile error at
  `LanguageId::Endo` is the better signal.
- `core::async::Task<T>`'s awaiter OWNS the task it awaits. `operator co_await` is rvalue-qualified
  and now moves the frame out of the `Task` value into the awaiter, which holds it across the
  suspension and destroys it at the end of the `co_await` expression; the `Task` that produced it is
  empty afterwards. Until now the awaiter borrowed, and the `Task` value freed the frame on scope
  exit. Migration: `co_await someTask()` is unchanged, since a temporary was already freed at the
  end of that expression. Code that awaited a named local with `co_await std::move(task)` and then
  read `task.handle()`, `task.done()` or `task.result()` must stop: the frame is gone and the name
  holds nothing. The change is what lets an executor free an abandoned chain from its root, because
  ownership in a `Task` chain now runs strictly downward.
- `core::async::Task<T>::result()` and its awaiter's `await_resume()` throw `std::logic_error` for a
  task that owns no coroutine frame, where they used to answer with a default-constructed `T`;
  `Task<void>`'s equivalents throw there too, where they used to return silently. `done()` is true
  for a default-constructed, moved-from or released task as well as for a completed one, so
  `if (task.done()) task.result();` reaches this, and a default-constructed value is one the
  coroutine never produced. With the `T {}` gone, `T` no longer has to be default-constructible.
  Migration: a driver that asks for a result checks that it still owns a frame (`task.handle()`),
  not only that `done()` is true.

- `core::net::IoBackend` replaces `core::net::EventSource`, and the shape changes with the name:
  a backend invokes the callbacks on a `ReadinessHandler` the caller registers, where an event
  source returned two vectors of `FdToken`s for the caller to look up. `EventLoop` takes an
  `IoBackend&`, and the post self-pipe it used to own is gone — `post()` calls
  `IoBackend::wake()`, which every backend provides, so the loop's constructor no longer throws
  and a backend's does when its wakeup channel cannot be created.

  Migration, for contour, endo and tuidu, which all have callers. Every row is in
  `tools/migrate/renames.json`:

  | Was | Is |
  |---|---|
  | `EventSource` | `IoBackend` |
  | `<core/net/EventSource.hpp>`, `<core/net/DefaultEventSource.hpp>`, `<core/net/PollEventSource.hpp>` | `<core/net/IoBackend.hpp>` |
  | `FdInterest`, `FdInterest::None` | `Interest`, `Interest::None` (still "mute the handle without detaching it", and now that on every backend) |
  | `makeDefaultEventSource()`, `makeEventSource(EventSourceKind)`, `preferredEventSourceKind()` | `makeDefaultBackend()`, `makeBackend(BackendKind)`, `preferredBackendKind()` |
  | `EventSourceKind` | `BackendKind`, which gains `Iocp`, `Wfmo`, `HostDriven`, `Scripted`, `Null` and a `Last` sentinel |
  | `PollEventSource`, `EpollEventSource`, `KqueueEventSource` | `PollBackend` and `WfmoBackend` (contour's one file, split along its `#ifdef`), `EpollBackend`, `KqueueBackend` — all private; reach one through the factories |
  | `testing::ScriptedEventSource` | `testing::ScriptedBackend`, scripting readiness against a `HandlerId` handed out in attach order |
  | `testing::AllBackends`, `testing::Backend` | `testing::BackendMatrix`, `testing::BackendUnderTest` (`<core/net/testing/BackendMatrix.hpp>`) |
  | `source.attach(fd, interest)` → `FdToken` | `backend.attach(handler)` then `backend.setInterest(handler, interest)`, each `std::expected<void, NetError>` |
  | `source.detach(token)` | `backend.detach(handler)` |
  | `source.wait(timeoutMs)` → `WaitOutcome` | `backend.wait(std::optional<SteadyDuration>)` → `WaitResult`, having already dispatched |

  `FdToken`, `WaitOutcome`, `FdRegistry` and `FdRegistration` are gone with no replacement: a
  handler's ADDRESS is its registration's identity. `EventLoop` keeps an id of its own for its
  parks, `core::net::ParkId`, which `registerFdWaiter()` and `unregisterFdWaiter()` now take;
  Task B4 widens it over every kind of parked work.

  Two behavioural differences a caller can see. `attach()` no longer carries an interest, because
  kqueue has no "register with no filters" operation and so cannot say whether the kernel accepted
  the descriptor — only `setInterest()` can, and that is where a refusal is reported. And a
  registration is serviced by at most ONE callback per wait, because a callback may leave the
  object its handler is embedded in ready to be freed; level triggering reports whatever was
  skipped on the next wait.

### Changed
- **`PosixSocket`, `WindowsSocket` and the POSIX dial classify errors through the one socket-error
  table** (Task B13), as the datagram, blocking and completion-port transports already did. A socket
  error the private switches reported as `SystemError` now has its category: `ECONNREFUSED` is
  `ConnRefused`, `EHOSTUNREACH`/`ENETUNREACH` `HostUnreach`, `EADDRNOTAVAIL` `AddressNotAvail`,
  `EACCES` and `EPERM` `PermissionDenied`, `ETIMEDOUT` `Timeout`, `EAFNOSUPPORT` and
  `EPROTONOSUPPORT` `Unsupported`, and the Winsock equivalents likewise. `EPIPE` stays
  `SystemError`. A caller that matched `SystemError` for one of those should match its category.
  Two rows change more than a category, both as the completion-port socket already had them:
  `ETIMEDOUT` on a `PosixSocket` read -- a connection keepalive or `TCP_USER_TIMEOUT` declared
  dead -- is `Timeout`, so `isDeadlineExpiry()` is true for it as for `setReceiveDeadline`'s own
  expiry, and only `systemCode` tells them apart; a caller that retries on `isDeadlineExpiry()`
  reads a dead connection once more before its EOF. And `EINTR` (`WSAEINTR`) is `Cancelled`,
  including from a non-blocking `connect()`, where POSIX says the connect carries on.

- **`<core/net/ISocket.hpp>` no longer includes the park table**: `ParkId` has its own header,
  `<core/net/detail/ParkId.hpp>` (core-cpp#43). A translation unit that includes `ISocket.hpp`
  preprocesses 113,503 lines rather than 130,784. A consumer that named `ParkEntry` or
  `TimerCallback` through `ISocket.hpp` alone includes `<core/net/EventLoop.hpp>`.

- `core::homeResolvedPath()` takes its input by `std::string const&` rather than by value:
  `std::filesystem::path` has no `std::string&&` constructor on Windows, so the move was a copy
  there. Source-compatible.

- `core::platform::InvalidHandle` is declared `void* const` on Windows, which is the type it always
  had (`NativeHandle const` read as a pointer to const and was not one).

- **Every TLS context sets `SSL_OP_NO_RENEGOTIATION`** (Task B11): a TLS 1.2 peer that
  renegotiates now fails. Renegotiation is the one way a write can need a read after the handshake,
  and a socket with a read already parked has no second read slot for it. TLS 1.3 has none.
- `AsyncBufferedReader` is neither copyable nor movable, since a parked refill writes into a
  member buffer that must keep its address (Task B10 review).
- **`core::net::serve` awaits each connection's `handshakeIfNeeded()` before reading a request**
  (Task B10), and drops a connection whose handshake fails without reading from it or answering it.
  A plaintext socket completes the verb inline, so nothing changes for one; a negotiating transport
  has finished negotiating before the server frames a byte. The server still closes every
  connection through its destructor with no lingering close -- a refusal written over an unread
  request body can be destroyed by the reset that close sends
  ([core-cpp#35](https://github.com/contour-terminal/core-cpp/issues/35)), and that is recorded on
  `serve` rather than fixed here.
- **`AsyncBufferedReader` refills through a member buffer rather than a coroutine-frame array**
  (Task B10), so a refill no longer costs a heap allocation of more than 4 KiB for its frame, and it
  asserts `contract::requireReadBuffer` on the span it hands the socket. `sizeof(AsyncBufferedReader)`
  grows by 4 KiB accordingly.
- **The Windows default backend is the I/O completion port.** `preferredBackendKind()` answers
  `BackendKind::Iocp` and `makeDefaultBackend()` builds an `IocpBackend`, so every Windows loop
  made the default way -- `PlatformLoop` included -- now completes socket operations on a port
  and hands out `IocpSocket`/`IocpListener`. It scales past WFMO's 64-handle wait, and it serves
  a console handle and a socket from one wait. **To get the old backend back**, construct it by
  name: `core::net::makeBackend(core::net::BackendKind::Wfmo)`, and give that backend to your
  `EventLoop` -- the socket factories ask the loop, so they hand out `WindowsSocket` and
  `WindowsListener` again with no other change. `makeDefaultBackend()` also falls back to WFMO when
  the port itself cannot be created. `BackendKind::Wfmo` is kept for one release and then removed
  ([core-cpp#6](https://github.com/contour-terminal/core-cpp/issues/6)); say so there if you need
  it longer.
- **`check-cmake-hygiene` reports how many files it *checked*, and refuses a count it cannot
  reconcile.** The gate printed the number of files it *found*, which is not the number any rule ran
  over: the kind dispatch skips a file silently, so a defect there shrinks the checked set without
  moving the number and the gate goes on reporting success over less and less. It now holds the
  dispatch's own tally against an independent recount taken with a different CMake primitive, and
  fails naming both when they disagree. On this tree the message changes from `447 file(s) under
  <root> are clean` to `checked 445 of 447 file(s)`; the two it does not check are a `README.md` and
  a `.clang-tidy`, which no rule reaches. A guard against zero would not have caught the failure
  this answers — `scripts/check-upstream-drift.py` once reported 350 rows where there were 351,
  because a substring match swallowed one and its total was printed rather than checked.
- **`core::net::selectReadinessCallback` routes a failure to the watched direction, and
  `Readiness::Failed` is documented best-effort.** The function returns exactly one callback, and
  it used to return `onError` whenever the kernel reported a failure — so a peer hangup on a socket
  with **unread bytes still in it** (`POLLIN|POLLHUP` on poll and epoll) would have returned
  `onError` alone the moment a handler set that field, the reader would never have been woken, and
  those bytes would never have been read. kqueue and `WfmoBackend` report the same hangup as
  readable and were always right. `onError` now takes a failure only when no watched direction
  accompanies it, which is the `POLLERR`-only failed connect it exists for. Nothing shipped with
  the old order and `EventLoop` sets `onError = nullptr`, so no consumer can observe the change;
  Task B6 is the first code that would have. The portable guarantee, now pinned by
  `BackendParity_test` on every backend, is that **a peer hangup wakes the direction the handler
  watches** — a caller is woken, calls `read()` or `write()`, and learns what happened from that.
  `Readiness::Failed` itself is a hint and must not be branched on for correctness: the backends
  disagree and are each right to, since `EV_EOF` on a kqueue read filter is an ordinary
  `shutdown(WR)` rather than an error.
- `cmake/portable/CompileCache.cmake` is re-synced from fastcached
  `f6ec49f3446b8bc121eba82c64cde2de759e774a`, and `cmake/FetchTransferBound.cmake`'s pin moves to
  the same commit, where its content is unchanged. The whole delta is one diagnostic: with
  `FASTCACHE_AUTO_START=ON`, a daemon that exits immediately now has its first line of output
  printed beside the exit status, so `(127)` reads as the missing shared library it is rather than
  as "not found" for a binary this module has just staged and knows the path of
  ([fastcached#1538](https://github.com/LASTRADA-Software/fastcached/issues/1538)). Launcher
  selection is untouched: a fresh configure on Windows (clang-cl) and in WSL (clang) still
  resolves to fastcache-cc, read off `build.ninja` rather than `CMakeCache.txt`.
- `core::async::whenAll` and `whenAny` are one runner, one join state and one awaiter,
  parameterised by a policy (`<core/async/Join.hpp>`, all of it `core::async::detail`). The two
  combinators had ~200 lines of near-identical coroutine, latch and start-phase code, differing in
  one step: what a child finishing does to the shared state. That step, the token each child
  observes and what the awaiting coroutine resumes with are what `WhenAll.hpp` and `WhenAny.hpp`
  still hold. No public name changes, and no behaviour does: `whenAll` still surfaces the first
  escape from any child and cancels nobody, `whenAny` still latches the first child to *complete*
  and unwinds the rest. Two things the collapse settled by making them one source: what escaped a
  child's task is recorded once, in the runner promise, where `whenAll`'s wrapper used to catch it
  a second time in its own body; and `whenAll`'s join state is reference-counted like `whenAny`'s,
  so the lifetime rule that keeps a stop state alive across its own `request_stop()` has one
  spelling rather than two.

- `core::async::whenAny()` reports a child that completed even when the awaiting flow's own token
  is stopped afterwards. It threw `OperationCancelled` whenever that token was stopped, so
  `whenAny(readSocket(), timeout())` whose read had completed and consumed bytes lost them if the
  cancellation landed before the last loser unwound; `.agent/rules/async-and-net.md` says the
  opposite, that a receive which already completed with bytes wins. It now throws only where no
  child completed at all. A child that *swallows* its `OperationCancelled` and returns counts as
  one that completed, because nothing can tell the two apart: a loser must let the cancellation
  out, which is what `whenAny`'s contract already asked of it.
- `core::async` links `Threads::Threads` (interface), on the same condition `core::base` uses, so
  a consumer writing `target_link_libraries(app PRIVATE core::async)` links what
  `<core/async/StopToken.hpp>`'s fallback needs. It linked nothing, which failed wherever pthread
  is a library of its own and the fallback branch is taken — libc++ before 20 without
  `-fexperimental-library`, so FreeBSD 15 and AppleClang 17. A single-threaded Emscripten build
  still links nothing.
- `.clang-tidy`'s `readability-identifier-naming` no longer exempts `request_stop`,
  `stop_requested`, `stop_possible` and `get_token` from the *function* naming rule: they are
  members of `std::stop_token` and friends, which `core::async`'s fallback spells as the standard
  does, and a free function of one of those names is not a standard-library hook. The
  `ClassMethod` style is gone with its duplicate of that ~800-character regex; with no
  `ClassMethod` style configured, a static member function falls through to the `Method` style,
  which says the same thing.

### Fixed

- **`EventLoop::cancelPending()` takes a waiter queued after readiness together with its park**
  (core-cpp#41). A readiness-dispatched waiter is in the ready queue while its park stays filed and
  attached until `await_resume` runs; the ready-queue branch returned before the branch that
  detaches, so the caller was handed a frame the backend still held a handler for. `~TuiRuntime`
  no longer resumes each taken-back flow to work around it; it destroys the frame.

- **`WindowsSocket` implements `cancelRead()`** instead of inheriting the no-op that cannot retire a
  parked read: a parked `read` or `waitReadable` on the WFMO backend's socket is taken back and
  completed with `NetErrorCode::Cancelled` inline, as on POSIX. `CancelRead_test` runs on the WFMO
  backend, and `TlsCancelRead_test` on every platform.

- **58 clang-tidy findings in Windows-only code**, which no CI leg analyses: comparisons against
  `INVALID_SOCKET` (now `detail::InvalidSocket`, one `SOCKET`-typed constant), redundant casts,
  member initialisers, ranges, a 32-bit multiply that widened, and a const that blocked a move.

- **Four defects in the TLS socket's handshake and flush gates** (Task B11, fix round 1), each with a
  case in `TlsLifetime_test.cpp` that failed before the fix:
  - **Destroying a TLS socket with an operation parked in its handshake or flush was a double
    free** (`double free or corruption` in a plain Debug build): the members were destroyed before
    the inner socket, whose destructor unwinds the parked driver into a gate that was already gone.
    The gates are now shared with their waiters, the destructor abandons them and the inner socket
    before freeing the session, and every flow parked on the socket unwinds.
  - **Releasing a gate resumed its waiters inline**, so a waiter whose flow dropped the socket left
    the next waiter to be resumed onto the freed one (SIGSEGV). Waiters are handed to the loop.
  - **A gate waiter ignored its stop token**, so a read that lost a `withTimeout` race while
    waiting on the handshake came back when it completed and read the peer's first bytes into the
    caller's abandoned buffer. It now unwinds when its token is stopped.
  - **`cancelRead()` during the handshake poisoned the socket**: the retired inner read was stored
    as a sticky handshake failure, and where a write drove the handshake it retired the WRITE's
    inner read. A cancelled handshake is no longer sticky, and `cancelRead` retires only the read
    direction: a read waiting on the handshake gate, or an inner read a read is parked on.
- `makeTlsClientContext()` trusts every certificate in its CA PEM, not only the first; a
  self-signed certificate's validity is refused when not positive, and one beyond about 68 years
  no longer wraps into the past on Windows, where `long` is 32 bits.

- **A TLS read beside a parked TLS write no longer puts a second write into the inner socket**
  (Task B11). A write larger than one flush chunk leaves ciphertext queued in OpenSSL's BIO while
  its first chunk parks; a read reaching `WANT_READ` then flushed that remainder itself -- a second
  operation in the inner socket's single write slot, which a real socket answers by dropping the
  parked one, and ciphertext out of order either way. One flush runs at a time: a write waits for
  one in progress, a read leaves the bytes to it. Found by counting writes at a gated inner socket
  (`TlsSocket_test`, `maxInFlight` 2 before, 1 after).
- **A healthy TLS connection no longer fails for another connection's OpenSSL error** (Task B11).
  `SSL_get_error` reads the thread's error queue, which every connection on a loop shares, and the
  record pump never cleared it: a stale entry turned a routine `WANT_READ` into `SSL_ERROR_SSL`.
  `ERR_clear_error()` now precedes every classified call, as it did in fastcached.
- **A server certificate chain is served whole** (Task B11). `makeTlsServerContext()` read only the
  first certificate of `certPem`, which its documentation called a chain, so a named certificate
  lost its intermediates and a client that could not build the chain to its anchor failed the
  handshake.

- **`SplitSocket::close()` no longer reads a destroyed object when a retirement drops its owner**
  (Task B10). It closed its read half and then its write half, and closing the read half completes
  a parked read -- which resumes a coroutine that may own the `SplitSocket` and destroy it, both
  halves included, before `close()` returns. The write half was then closed through a freed
  `unique_ptr`: a SIGSEGV in a plain debug build, not merely under a sanitizer. Ordering the two
  calls cannot fix it, because whichever runs first can do that, so `close()` now checks a liveness
  token between them and returns if the socket is gone; the other half's parked operation was
  abandoned by its destructor, which is what a destroyed socket does. `SocketDecorator_test`
  reproduces it on every POSIX backend.

- **The TUI runtime's four deferred defects, all closed by composing it on `core::net::EventLoop`
  (Task B12).** They were found reviewing Task A7's import from endo and deferred here because this
  task deletes or rewrites the code they live in; nothing shipped with them.
  - [core-cpp#16](https://github.com/contour-terminal/core-cpp/issues/16): **a cancelled `delay`
    left its entry in the runtime's timer heap.** Its `await_resume` reset the stop-callback and
    unregistered nothing, so a `whenAny` loser's heap entry outlived the frame it named and the
    next pass over the heap called `done()` on freed storage. Reproduced as a SIGSEGV in a plain
    debug build, not merely under a sanitizer. The loop's own awaitable unregisters its park on
    every resume, ready or cancelled, and the regression case asserts `pendingTimerCount()` and
    `pendingTimerSlotCount()` are both zero afterwards.
  - [core-cpp#17](https://github.com/contour-terminal/core-cpp/issues/17): **`blockOn()` spun at
    full CPU when nothing the runtime knew about was parked.** `pumpOnce()` returned without
    waiting and `blockOn` looped on it unconditionally. `EventLoop::blockOn` waits on the backend
    instead. An outcome test cannot separate a loop that slept from one that burned a core, so the
    case asserts the ARGUMENT: every wait of an idle turn is indefinite, never zero.
  - [core-cpp#18](https://github.com/contour-terminal/core-cpp/issues/18): **a cancelled input
    awaiter stranded the runtime's single waiter slot.** `NextInputEventAwaiter`,
    `NextEventForAwaiter` and `NextActivityAwaiter` armed no stop-callback, so a cancelled waiter
    stayed parked until input happened to arrive -- after EOF on stdin, never -- and the next flow
    to ask for input found the slot taken. All four input awaiters now arm one, and a cancelled
    waiter releases its slot and its deadline. The case asserts the argument again: a `whenAny`
    whose loser is parked on input must resolve with **no wait at all**.
  - [core-cpp#19](https://github.com/contour-terminal/core-cpp/issues/19): **the Windows TUI event
    source failed hard past 64 wait handles.** It called `WaitForMultipleObjects` with an unchecked
    count and mapped the refusal onto `interrupted`, so a consumer watching ~60 descriptors saw the
    whole TUI unwind indistinguishably from Ctrl+C; `PollEventSource` had the same limit and span
    instead. Both files are deleted, and `core::net`'s Windows backend sweeps its set in chunks
    (`detail/WaitChunking.hpp`). Held by a case that parks 70 concurrent handle waits and requires
    every one to resolve.

- **`~TuiRuntime` handled two states of a source flow and there were three.** A `core::async::Task`
  is lazy, so between the constructor and the first turn every source flow sits at its initial
  suspend point with its body not yet entered — and the destructor resumed it, which ran that body
  from the top and parked it on the loop, after which the frame was destroyed underneath the park.
  `~EventLoop` then resumed freed storage. Each flow's loop now guards on the teardown flag
  **before** its first `co_await`, so a flow started during teardown returns without parking, and
  the destructor's comment enumerates all three states rather than branching on two.

  Reachable by constructing a runtime and destroying it with no turn in between — an error-return
  path, or construction and destruction inside one turn. **No case in the suite reached it**,
  because every one drives the loop first, which is why seven configurations and both sanitizers
  were green over it. A case that constructs and destroys with no turn now holds it, over all four
  source flows; reverting any one flow's guard fails it.

- **Destroying a `TuiRuntime` from inside its own interrupt handler is refused in a debug build**
  instead of destroying a running frame. The handler runs inline inside the source flow that
  observed the interrupt, so a handler that destroyed the runtime destroyed that flow's frame, and
  the handler's own `std::function`, while both were executing; AddressSanitizer reports it as a
  heap-use-after-free in the flow's resumption. It is a precondition rather than a case the
  destructor can handle, so `~TuiRuntime` now asserts it, `setInterruptHandler` documents it, and
  the remedy is to `post()` the teardown to the loop, after which the flow is parked and the
  destructor takes it back. The same holds for an `InputSource` member reached from a source flow.

- **A timed input wait could silently lose its timeout.** `releaseInputWaiter` retired
  `_inputDeadline` unconditionally, but a waiter's deadline is retired where it leaves the slot, so
  by the time a queued waiter's `await_resume` ran the slot could already hold a *different* flow
  and its timer. `nextEventFor`/`nextActivity` then waited forever on a deadline they had asked for
  and never got. The deadline is now retired only in the branch where the slot still holds the
  resuming waiter. Reachable through the public API alone: a timed waiter woken by a focus report
  while a sibling's `delay` expires in the same turn, which a regression case now builds.

- **A focus change no longer closes an open modal.** The runtime woke its input waiter for any
  non-input activity, including a dispatched focus report -- but a `nextEvent()` awaiter can only
  yield an event or throw, so it threw `OperationCancelled`, and `runModal` catches that and
  returns `std::nullopt`. Which waiters may be resumed with nothing is now stated as
  `core::tui::runtime::InputWake` rather than inferred: only a waiter that can *say* nothing
  happened (`nextEventFor`, `nextActivity`) is told so.

- **A lone Escape keypress is delivered.** Disambiguating a bare `ESC` from the start of an arrow
  key needs a clock, and the old runtime only called the parser's timeout hook when its multiplexed
  wait timed out -- which, for a flow parked on `nextEvent()` with no timer pending, meant an
  indefinite wait and a hook that was never called at all. The runtime now arms
  `TuiRuntimeOptions::escapeFlush` (50ms by default) after any read that decoded to nothing, and
  flushes when it elapses.


- **`scripts/clang-format.py` no longer destroys a file it was handed.** The extension filter
  applied only to what `--all` discovered, never to a path the caller named, so
  `clang-format.py x.cmake` handed a CMake file to clang-format, which parses its input as C++
  whatever the name is, rewrote it, and reported `1 file(s) formatted`. `--check` was worse than
  blind: it **failed** the pristine file and **passed** the mangled one, so it pointed a caller at
  the damage and then certified it. Both scripts now refuse a path that is not theirs, by name and
  before either tool is looked up — refused rather than skipped, because a silent skip leaves a
  caller believing a file they named was formatted when nothing touched it.
  `scripts/python-style.py` had the same shape with a milder effect (ruff honours the extension and
  leaves the file alone, but reported it as formatted), and is closed the same way. Both are now
  held by `core-cpp.format-scripts-selftest`, which proves each refuses every foreign shape by name
  and accepts its own — the absence of exactly that test is why this survived.
- **`core::async`: a use-after-free when a chain parks again while an earlier park is being
  released.** `detail::claimOn()` armed the chain's abandon state and then took its claim as two
  separate atomic stores, so a concurrent `AbandonClaim` release could observe a state that never
  existed as a whole — armed by the new park, count zero because that park had not been counted yet
  — conclude the chain was abandoned, and destroy a coroutine frame that was live. It fires
  wherever several children of one detached flow park on an executor at once, which is exactly what
  `whenAll` and `whenAny` over a `ThreadPoolExecutor` do: measured at 36 crashes in 1920 runs of the
  async suite at 32-way concurrency, and at 15 and 14 for the two combinators individually. The
  park count and the armed flag are now one atomic word, with claim-and-arm and
  decrement-and-claim-the-destroy each a single compare-exchange, so *armed* is never observable
  without the claim that accompanies it. No API changed.

- A module-table row whose `WHEN` names an undeclared variable is refused where it is declared,
  instead of silently removing its target. `core_cpp_row_builds()` evaluates
  `if(when AND NOT ${when})`, so `WHEN CORE_CPP_WITH_TSL` expanded to `NOT` an undefined variable —
  true — and the row stopped building: the target went, every test `core_cpp_add_test()` registers
  against it went with it, and nothing said so. It configured clean, built clean, and a module was
  simply not there. The check is `DEFINED` rather than "is an `option()`", because
  `CORE_CPP_USE_THREADS` is a plain `set()` and is already used as a `WHEN`, so an `option()`-only
  rule would refuse a condition this build uses today; a typo is undefined by construction, which is
  exactly what is now refused. Five scenarios in `core-cpp.layering` cover it, including the two that
  must still configure — a declared option and a plain variable — because a check that refused every
  `WHEN` would also "catch" the misspelling.

- `tests/cmake/check-layering.cmake` includes `CoreCppOptions.cmake` before the module table, in the
  order the real build uses. Its scenarios read a table whose rows carry `WHEN` conditions naming
  those options while never declaring them; that was invisible until the check above started
  refusing an undefined `WHEN`, at which point every scenario failed on the real `net_tls` row.

- `core::platform::testing::InMemoryFileSystem` keeps a name the platform's narrow encoding
  cannot spell in *both* directions. The keys were made UTF-8 earlier in this release, but twelve
  sites turned a key back into a path through `std::filesystem::path`'s narrow constructor -- the
  ANSI code page on Windows -- so `listDirectory()`, `walkDirectoryRecursive()`,
  `weaklyCanonical()`, the walk's sort key and the parent-key derivations handed back a path that
  no longer named the entry it came from, and a symlink's target was narrowed on the way in as
  well. One helper now spells the way out, as `normalizePath()` spells the way in.
- `core::platform::testing::InMemoryFileSystem`'s streams no longer point into the file map. A
  `writeFile()` through the filesystem reallocated the string under an open stream, and `remove()`
  or `rename()` took the entry away from under it -- a use-after-free in each case, on the fake
  every consumer's tests are written against. The content is shared now, and the read-write stream
  caches no pointer into it, so a file that changes behind a stream is read from where it lives.
- `core::platform::testing::InMemoryFileSystem`'s streams support `unget()` and `putback()`, which
  set `badbit` while the buffer kept no get area for `std::streambuf` to satisfy a put-back from.
  `unget()` and a `putback()` of the character just read now answer as `std::ifstream` and
  `std::fstream` do. A `putback()` of a character the file does *not* hold is a case the standard
  leaves open -- only one put-back is guaranteed at all, and a different character is expressly
  permitted to fail ([streambuf.virt.pback]). libstdc++ and MSVC accept it; libc++ refuses, so
  macOS and FreeBSD differ from Linux and Windows. The fake accepts it, since a memory buffer with
  an exact position can always satisfy one, and keeps the character in a slot of its own rather
  than writing it to the file. Where it is deliberately more permissive than a real stream is
  listed in [core-cpp#27](https://github.com/contour-terminal/core-cpp/issues/27).
- `core::async::whenAny()` no longer runs the rest of a `request_stop()` on freed memory. Its
  parent→child cancel bridge requested stop on a `StopSource` that the awaiter held as a member;
  a child awaitable that resumes its coroutine from inside its own stop callback — how every
  runtime awaitable delivers cancellation — makes the losers unwind there and then, the last of
  them transfer to the awaiting coroutine, and that frame unwind, destroying the awaiter and with
  it the source whose `request_stop()` is still on the stack. The race state is now held by
  `shared_ptr` and every call into it that can run foreign code holds a reference for that call.
  This was a use-after-free wherever `StopToken` is `std::stop_token`, whose state a raw pointer
  reaches; core-cpp's fallback survived it only because its `request_stop()` happens to hold a
  `shared_ptr` copy of the state.
- `tests/cmake/check-cmake-hygiene.cmake`'s namespace gate had two holes. It checked only the
  *first* namespace a file declares, although its rule is that every segment of every namespace is
  lowercase, so `namespace core::async { namespace Detail { ... } }` passed clean. And it derived
  the expected namespace from the first directory segment under `src/core/` alone, so a file in
  `src/core/platform/testing/` declaring `core::platform` passed although the rule is namespace =
  directory. The expected namespace now follows the whole path, with the platform and
  private-detail directories (`posix/`, `windows/`, `linux/`, `bsd/`, `darwin/`, `emscripten/`,
  `detail/`) skipped as layout — exactly the ones `core_cpp_add_module()` holds private headers
  in, and nothing else. The one thing in the tree the deeper rule found,
  `src/core/tui/completer/` declaring `core::tui`, is fixed rather than exempted: see **Breaking**
  ([core-cpp#30](https://github.com/contour-terminal/core-cpp/issues/30)).
- `Task_test.cpp`'s deep-chain case skips on GCC unless the build's optimisation level is known to
  make symmetric transfer a tail call. It keyed on `__OPTIMIZE__`, which GCC defines at `-Og` and
  `-O1` as well, where the 100000-frame chain overflows the stack and kills the process, taking
  every other case in the binary with it — so a build outside core-cpp's presets lost the binary
  rather than getting a red. `src/core/async/CMakeLists.txt` now reads the level off the build's
  own flags and says in the configure log which it decided.
- `<core/async/WhenAll.hpp>` includes `<type_traits>`, which it names; `<core/async/Awaitable.hpp>`
  no longer includes `<utility>`, which it does not; `Task_test.cpp` includes `<stdexcept>` rather
  than relying on Catch2 for it, and not `<string>`, which it does not use.
- `core::cli`'s `--help` no longer reads past the text it is laying out. `wordWrapped()` computed
  the room left on the line as `margin - cursor + 1` in unsigned arithmetic, with a `<= 0` guard
  below it that is dead for an unsigned type; `printOptions()` sets the cursor to the option
  column, so an option column wider than the terminal — an 80-column terminal and an option whose
  rendered text exceeds 65 characters, a narrower terminal, or a pty reporting `ws_col == 0` —
  wrapped it to about 4294967295 and indexed the help text far past its end. It also read
  `text[SIZE_MAX]` for a help text beginning with a line feed, and returned an empty chunk for a
  word longer than the line, which made the caller loop forever. The options column now accounts
  for the verbatim placeholder as well, so a placeholder longer than the longest option no longer
  underflows its padding into a string of about four billion spaces (the `assert` above it is
  compiled out under NDEBUG), and the hyperlink scan's `isalpha()` widens through `unsigned char`,
  which is what it is defined for. The wrapper also advances its index by what a chunk consumed
  rather than by what it emitted: the two differ whenever a chunk is trimmed, and a space before
  a line feed left the trimmed space in front of the index for a skip loop that skips line feeds
  and not spaces, so the same empty chunk came back for ever and `--help` never returned.
  Two rendering changes come with this, both visible to anyone diffing `--help` output. A line
  whose text reaches exactly to the margin is no longer broken onto a second line. And trailing
  spaces before a line feed no longer produce a wrapped line each: the old renderer consumed them
  one per turn and emitted a line break plus a continuation indent for every one of them, so a
  help text reading `First line. ` + line feed + `Second line.` rendered as three lines where its
  author wrote two, and `abc` + three spaces + line feed + `xyz` as five. They are consumed
  together now.
- `core::cli::App` keeps the contracts it documents. `installLogging()` assigned the replacement
  over the member holding the previous output, so the previous `ScopedOutput` was destroyed after
  the new one had installed itself: its destructor restores every category to the sink it
  snapshotted, so a second call silently sent every later log line back to the console and left
  each category holding a reference into a destroyed sink. It releases the previous output first
  now — which means a destination that then fails to open leaves logging on the console rather
  than on whatever was installed before; the caller is told, and has nothing to fall back to
  either way. `reparseParameters()` and `parseParametersForTesting()`, both documented "false on
  failure", catch what `cli::parse()` throws rather than letting it escape a function whose
  contract is a bool (`cli::parse()`'s declaration now states what it throws;
  [core-cpp#13](https://github.com/contour-terminal/core-cpp/issues/13) converts this API to
  `std::expected` at the end of the plan). `screenWidth()` rejects a reported width of 0.
  `listDebugTags()` sorts a copy rather than the process-wide category registry, whose order is
  its construction order.
- `core::log` asks the platform whether a standard stream is a terminal, on Windows too.
  `ScopedOutput`'s private `isStdErrTty()` returned `true` unconditionally there, so a redirected
  standard error received SGR escapes — against the header's own contract — and
  `core::cli::App`'s `helpStyle()` and `customizeLogStoreOutput()` each carried a second copy of
  the same branch for standard output. All three now call `core::log::isStdOutTerminal()` or
  `isStdErrTerminal()`, which `core::log` implements once per platform in
  `src/core/log/posix/TerminalQuery.cpp` and `src/core/log/windows/TerminalQuery.cpp` — an
  operating-system difference is an implementation, never an `#ifdef` inside the decision
  (`.agent/rules/platform.md`). The module's other one, the process id the `[PID]` field prints,
  went the same way (`posix/ProcessId.cpp`, `windows/ProcessId.cpp`, declared in the private
  `detail/ProcessId.hpp`), so `core::log` has no `#ifdef` in its logic left.
- `core::escape()` and `core::unescape()` round-trip again. 0x7E was outside the printable range,
  so `~` came out as a numeric escape; `escape()` writes a quote as `\"` and `unescape()`
  re-emitted it as `\"`; and an octal escape is three digits of which only those below `\100`
  begin with a zero, but the reader keyed the sequence on `'0'`, so `\101` and everything above it
  came back as literal text. The reader now opens an octal sequence on any octal digit and
  consumes exactly three, which reads the `\0dd` form it used to accept identically (a
  leading zero is octal-neutral). One reading did change: `\1` through `\7` used to come back as
  the two literal characters and now open a three-digit octal run. That is correct for anything
  `escape()` produced, which is what `unescape()` is for; hand-written or third-party escaped text
  that meant a literal backslash before a digit has to spell the backslash `\\`.
- `core::FNV`'s byte-wise overload reads the bytes with `std::bit_cast` rather than a
  `reinterpret_cast` through the object representation, which no constant evaluation may do — so
  the `constexpr` on the overload can now be taken up. (What it accepts narrowed too; see
  **Breaking**.)
- `core::base64::decode()`'s index lambda captures its 256-byte table by reference; by value it
  copied the whole table on every call.
- `core::Utils` stays inside the bounds it is given. `splitKeyValuePairs()` rebuilt its last
  segment with the length-less `std::string_view(char const*)` constructor, which calls `strlen()`:
  it read past the view (AddressSanitizer reports a heap-buffer-overflow) and returned whatever
  followed as part of the value. `toLower()`/`toUpper()` passed a plain `char` to
  `tolower()`/`toupper()`, undefined for any byte with the high bit set — every continuation byte
  of a UTF-8 sequence, and `cli::about::registerProjects()` sorts project titles through them; a
  character wider than a byte goes to `towlower()`/`towupper()` rather than being truncated into
  the narrow functions' domain. (`readFileAsString()` is fixed too; because it answers
  differently, its entry is under **Breaking**.) `eachElement()`'s end iterator was
  `max + 1` computed in `int` and cast back, which for a type narrower than `int` wraps onto
  `begin()` — so the range was empty — and for one as wide as `int` overflows. Windows'
  `threadName()` resized by `len - 1` with `len == 0` on a failed conversion, which threw
  `length_error` before the `LocalFree()` below it ran.
- `core::tui` carries no consumer's name in the code it runs. Beyond the OSC 8 hyperlink id
  below, `detectLanguageFromPath()`'s well-known-filename table no longer has a row for endo's
  `.endo-format`, so that name now answers `LanguageId::None`; the table keeps only names that
  are well known beyond one project, and a consumer that wants its own configuration file
  highlighted passes the language to `highlightLine()` itself. The default theme's path-gradient
  colours and the fuzzy matcher's worked example no longer describe themselves in terms of one
  application either. (`LanguageId::Endo`, `registerEndoHighlighter()` and the `.endo` and `endo`
  token rows were the same finding, left then for a design decision; they are removed under
  **Breaking** above, together with the registration seam that replaces them.)
- `core::tui`'s assembly highlighter no longer overruns a stack buffer. Its three scanners
  lowercased an identifier, a `%register` or a `.directive` into a 64-character array through a
  helper that took a bare `char*` and wrote `src.size()` bytes; the four other call sites bounded
  the copy themselves and these did not, so a token longer than 64 characters in any rendered
  ```` ```asm ```` fence smashed the caller's frame. The helper now takes a `std::span<char>` and
  returns an oversized identifier unchanged, so the bound is in one place.
- `core::tui`'s three dialogs draw their frame where their text is. Each built its `Rect` as
  `{ .x = startRow, .y = startCol }`, but `Rect::x` is the left column and `Rect::y` the top row,
  while `putString()` takes `(row, col)`; the border and the interior fill therefore landed at the
  transposed position and the contents outside them. The two coincide only on a canvas where the
  dialog is centred at the same offset in both axes, which is why nothing caught it.
- `core::tui::InputDialog::render()` no longer throws on a terminal narrower than its own border.
  `dialogWidth = min(config.width, termCols - 4)` and `inputWidth = dialogWidth - 4` had no floor,
  and the negative width reached `substr()` as a huge `std::size_t`, throwing `std::out_of_range`
  out of a `render()` no caller expects to throw. All three dialogs clamp both to zero.
- `core::tui::Buffer::addHyperlink()` mints the OSC 8 `id=` as the bare hash of the URI. endo's
  copy prefixed it `endo-`, so every consumer's hyperlinks carried another project's name on the
  wire. A behaviour change for anything that reads the id back: it is now `1f2e` where it was
  `endo-1f2e`.
- `core::tui::completer::Completer::addProvider()` sorts stably, so providers of equal priority -- which is
  every provider that does not set one -- keep the order they were registered in.
  `gatherCompletions()` drops a later duplicate by text, so an unstable sort let the standard
  library decide which provider's item a user saw.
- `core::tui::VtParser`'s three sequence buffers are bounded. A bracketed paste, a CSI parameter
  string and a DCS payload each grew for as long as bytes kept arriving without the terminator
  that ends the sequence, and `timeout()` resolves only a bare Escape, so a `ESC[200~` whose
  `ESC[201~` never came grew the process without limit from untrusted bytes on stdin. The caps
  are the new public `VtParser::MaxPasteLength` (4 MiB), `MaxCsiParamLength` (256) and
  `MaxDcsLength` (64 KiB); past one, the parser returns to Ground, emitting the collected text
  for a paste and dropping the other two, which are malformed at that length. Each cap bounds what
  the sequence CARRIES: the terminator's own bytes (`ESC[201~`, `ESC \`) pass through the same
  buffer on their way in, and their room is reserved above the cap, so a paste of exactly
  `MaxPasteLength` bytes and a DCS payload of exactly `MaxDcsLength` bytes still end at their own
  terminator instead of being cut a few bytes into it. `MaxCsiParamLength` needs no such
  reservation: a CSI's final byte is dispatched, never collected.
- `core::tui`'s POSIX SIGWINCH handler saves and restores `errno`, reaches its `TerminalInput`
  through a lock-free `std::atomic` rather than a plain pointer, and cannot block. The write end
  of the resize self-pipe was left blocking (only the read end was made non-blocking), so a pipe
  nobody had drained stalled `::write()` inside a signal context; and a resize arriving between a
  failed syscall and the mainline's `errno` check overwrote the value that check was about to
  read.
- `core::tui::SyncGuard` flushes at both ends of the region, whichever way the guard was made.
  Anything composed inside the region and still buffered was emitted after `CSI ?2026l` and so
  applied outside it -- `Screen::flush()`'s `applyCursorShape()` is the live case -- and
  move-assignment, which ends a region the same way, flushes too. The flush on the way *in* is the
  constructor's rather than `TerminalOutput::syncGuard()`'s, so the natural RAII spelling
  `auto guard = SyncGuard { output };` no longer emits previously buffered bytes inside the
  region it is opening.
- `core::tui::SyncGuard` writes its begin and end sequences (DEC mode 2026) through the
  `TerminalOutput` it brackets, so they follow that output's `writeToDestination()` wherever its
  bytes go. endo's guard wrote them to the process's standard output whatever the output was
  (`src/tui/platform/TerminalOutput.cpp:355-359` at `f774a210`), which put the frame's begin and
  end on a stream that never saw the frame's contents, and left a retargeted output's own stream
  unsynchronised. The guard therefore carries no native handle, and `<core/tui/TerminalOutput.hpp>`
  no longer declares a `void*` handle alias under `_WIN32`. `TerminalOutput::isTerminal()` is new
  beside it: whether the destination is a terminal, answered by the operating system for the
  default one and by the subclass for a retargeted one.
- `core::nextPowerOfTwo()` rounds a 16-, 32- or 64-bit value up to a power of two. crispy's, which
  it was imported from, compared the type's width in bytes against bit counts and so smeared only
  the eight bits below the highest set one: 257 became 511, and 0x10001 became 0x1fe01.
- `core::LiveEnvironment` on Windows reads a variable set to the empty string as set, as it does
  on POSIX; it read as unset.
- `core::Generator` is the same type in every translation unit. endo's, which it was
  imported from, tested `__cpp_lib_generator` before including anything, so whether it was
  `std::generator` depended on what the including file had included first, and a virtual function
  returning one (`FileSystem::walkDirectoryRecursive`) could have two return types in one program.
- `core::platform::SystemPipe` never blocks: both POSIX ends are non-blocking and close-on-exec,
  a write into a full channel reports done, and `send()` uses `MSG_NOSIGNAL`. endo's copy blocked;
  contour's, which an event loop's `post()` uses, already did this.
- `core::platform::SystemPipe::read()` returns a `ChannelResult`, which tells the bytes read, an
  empty channel and the end of the stream apart; only a failed read is a `PlatformError`. endo's
  and contour's copies returned a count, 0 for the end of the stream, and failed a read of an
  empty non-blocking channel with the same error as a broken one.

- `core::platform::testing::InMemoryFileSystem`'s read-write stream stops handing out stale
  pointers. It cached the get-area pointers into the `std::string` that holds the file and then
  appended to that same string on every write, so a write that grew it past its capacity left every
  one of those pointers naming freed memory -- a heap-use-after-free on the next read. The same
  append also ignored where the stream stood, so `openReadWrite()` could never overwrite in place
  the way the `std::fstream` behind `NativeFileSystem` does; it now carries one position for reading
  and writing, as `std::filebuf` has, and implements `seekoff()`/`seekpos()`.
- `core::platform::MessageQueue` guards its wakeup pointer like every other member. `setWakeup()`
  wrote it with no lock while `push()` and `shutdown()` read and dereferenced it from another
  thread, so a teardown that cleared the pointer could be missed and leave `push()` signalling a
  destroyed `Wakeup`. Registration takes the queue's mutex now, and the signalling happens under it,
  so once `setWakeup(nullptr)` returns nothing is still inside `signal()`.
- `core::platform::SignalHandler::restore()` deregisters the interrupt wakeup as well as the
  callback. It left `interruptWakeup` pointing at the `Wakeup` the caller was about to destroy, and
  Linux's `processSignalFd()`, the SIGINT handler elsewhere and the Windows console control handler
  all reach it through that pointer.
- `core::platform`'s Windows `EnvironmentProvider` reads the environment through
  `core::LiveEnvironment`, and writes it through `core::setProcessEnvironmentVariable()` and
  `core::unsetProcessEnvironmentVariable()`, as the POSIX one already did. Its own
  `GetEnvironmentVariableA()` call could not tell an empty value from a missing name, so it reported
  a variable set to `""` as unset while the other reader of the same Win32 block reported `""`; it
  also ignored the buffer-too-small return and allocated 32 KiB per lookup.
- `core::testing::setTestEnv()` sets an empty value instead of removing the variable. On Windows
  `_putenv_s(name, "")` removes it, so `setTestEnv(name, "")` and `unsetTestEnv(name)` were the same
  call and `ScopedEnv` could not put back a variable whose previous value was empty -- and the
  environment is process-global, so the loss crossed into every later test.
- `core::platform::SystemPipe`'s never-stall guarantee holds on Windows too. Only the read socket
  was made non-blocking, so a producer that outran the loop parked in `send()` indefinitely; the
  write socket is non-blocking now and `write()` answers `WSAEWOULDBLOCK` as done, the way the POSIX
  branch answers `EAGAIN`. `write()` also clamps the byte count to `INT_MAX`, as `read()` already
  did, so a count past it can no longer go negative or wrap into a short write reported as a full
  one. And `makeLoopbackPair()` compares the two ends' addresses and retries: `accept()` returns
  whoever connected, and between the `listen()` and the `accept()` any local process can take the
  ephemeral port, leaving a "pair" whose ends are not each other's.
- `core::platform::FileSystem::isExecutableFile()` classifies a symlink by what it points at. On
  POSIX it accepted any symlink and then read the followed target's permissions, so a symlink to a
  directory was reported as executable on the directory's own search bit -- against the
  declaration's "Directories always return false". A PATH lookup that trusted it ran the directory
  and failed with `EACCES` instead of trying the next entry.
- `core::platform::globMatchFilename()` reaches its bracket arm for the character it exists to
  match. The literal arm was tested first, so `globMatchFilename("[", "[[]")` -- POSIX's own way to
  spell a literal bracket -- answered false. A `[` that no `]` closes stays the literal `[` that
  `fnmatch(3)` reads it as.
- `core::platform::stripTrailingSeparator()` and `isCaseOnlyRename()` keep a spelling the platform's
  native narrow encoding cannot hold. Both went through `path::generic_string()`, which on Windows
  narrows to the ANSI code page: MSVC throws on a path it cannot spell, and where it does not throw
  it substitutes, so two distinct paths come back as one. `InMemoryFileSystem` keys its whole file
  map on the first of them, so one file answered for another.
  `NativeFileSystem::createTempFile()` had the same problem twice, and is `wchar_t` end to end on
  Windows now.
- `core::platform::NativeFileSystem::createDirectory()` names an existing directory rather than
  reporting "No such file or directory", the diagnosis for the other way it fails; and `rename()`
  reports the two-hop recase's own error instead of the first attempt's, and says where a failed
  rollback left the entry.

- `core::net`'s HTTP head parser ends the head at the first blank line, whichever terminator
  produced it. An empty line inside the header block was skipped with a `continue`, so
  `"GET / HTTP/1.1\n\nHost: evil\r\nContent-Length: 0"` parsed as ONE request carrying those
  headers while a front-end that honours a bare LF as a line terminator -- which RFC 9112 2.2
  permits, and which this parser itself does for every other line -- read it as two. That is the
  request-smuggling desync of RFC 9112 11.2, in the parser that already refuses `Transfer-Encoding`
  and a conflicting `Content-Length` for the same reason. The message is refused rather than
  re-framed: the bytes behind the blank line were already consumed as part of the head block, so a
  `Content-Length` read before it would index into the wrong place.
- `core::net`'s Windows listener no longer stops accepting for good. `accept()` called
  `WSAResetEvent` on the shared readiness event before parking; a client connecting between the
  `::accept()` that returned `WSAEWOULDBLOCK` and that reset leaves `FD_ACCEPT` recorded and the
  event signalled, and the reset then cleared the event while the record stood -- Winsock raises a
  recorded indication only once, so the coroutine parked for ever and the listener went silent, for
  that connection and every one after it. The indication is consumed with `WSAEnumNetworkEvents`
  instead, which clears both in one step and says what it took, so a connection from that window is
  accepted rather than lost. `WindowsSocket::latchNetworkEvents` already did this for the two
  directions that share a connected socket's event; both now go through one
  `core::net::consumeNetworkEvents`.
- `core::net`'s TLS wrapper checks both `BIO_new` results before handing them to `SSL_set_bio`.
  A failed allocation produced a non-null socket whose first read or write dereferenced null --
  breaking `ITlsContext::wrap()`'s own documented "null on allocation failure", one line below the
  checked `SSL_new`. The failure path also releases the `SSL` it had already created.
- `core::net`'s TLS `flushOut()` reports a failed flush instead of success. Its `BIO_read <= 0`
  branch is reachable only after `BIO_ctrl_pending` said bytes WERE queued, so it meant a failed
  write BIO, and calling that "nothing to flush" dropped ciphertext silently: the handshake then
  waited for a peer response to a flight that was never written, and both ends hung until an outer
  timeout.
- `core::net::PosixSocket::write()` handles a zero-length return instead of reading a stale
  `errno`. Only a positive return was consumed, so a zero fell through to an `errno` no call in the
  loop had set -- and depending on that leftover value the loop spun on an already-writable socket,
  retried for ever, or reported a failure that never happened. `errno` is now captured immediately
  after the syscall, as `read()` already handled its own zero (a clean EOF) first.
- `core::net::AsyncBufferedReader::readUntil()` rescans the buffer when the delimiter changes. The
  scan offset was reset only when the scanner KIND changed, but "no match can begin before here" is
  a statement about the bytes that scan was looking for: after a `readUntil("\r\n\r\n")` returned
  early, a following `readUntil("X")` resumed near the buffer's end and reported EOF for an `X` the
  reader was already holding.
- `core::net`'s POSIX listeners create their socket close-on-exec atomically, through the
  `makeStreamSocket()` helper `connect()` and `connectUnix()` already use, instead of a bare
  `::socket()` with the flags applied after `listen()`. A fork and exec from another thread in that
  window inherited the listening descriptor and kept the port -- or the AF_UNIX socket file --
  claimed after the daemon exited.
- `core::net::testing::ScriptedEventSource::detach()` is idempotent, as `EventSource` documents and
  every real backend behaves. It counted detach CALLS rather than live registrations, and the loop
  genuinely detaches twice on normal paths (`notifyHandleClosing` then `unregisterFdWaiter`;
  `requeueForCancellation` and `wakeAllWaiters` before `await_resume`), so a second detach of one
  token cancelled out another token's registration and `attachedCount()` under-reported -- a future
  leak assertion against this source would have passed on a registration that never went away.
- `src/core/net/EventLoop.cpp` includes `<stdexcept>` for the `std::runtime_error` it throws, which
  compiled only through a transitive include.
- `core::net`'s own tests: a failing `REQUIRE` in a `whenAll` arm fails the case instead of hanging
  it (`whenAll` cancels no sibling, and the sibling was parked in `accept()` with nobody left to
  close the listener -- `.agent/rules/testing.md`); the descriptor-exhaustion case restores the
  process-wide `RLIMIT_NOFILE` through a scope guard, so a throw in between can no longer leave
  every later case in the binary running squeezed; and the TLS cases check `makeSocketPair()`
  before dereferencing it, so a loopback failure is a test failure rather than undefined behaviour.

- `core::net`'s HTTP head parser rejects whitespace between a field name and its colon, which
  RFC 9112 5.1 makes a MUST for a server, instead of trimming it away. It is the same class as the
  bare-LF blank line above: a front-end that trims "Host :" back to "Host" and a server that
  rejects it (or the reverse) do not agree on what the message says, and the lenient half of that
  disagreement is the one that lets a header through under a name the other end never saw. A field
  name is a token, so whitespace anywhere in it -- and an empty name -- is refused; whitespace
  AFTER the colon is still padding a recipient removes, so `Host:  \texample \t` is unchanged.
- `core::net`'s own tests bound the waits that can hang rather than fail. The sequential-accept
  guard for the Windows listener would itself have parked for ever on the defect it guards -- so
  ctest reported "Timeout" after 1500 seconds and named nothing -- and now fails inside its budget
  with the count it waited for. Every test `core_cpp_add_test` registers is now bounded -- 300
  seconds unless a `TIMEOUT` says otherwise, which the two net binaries tighten to 120 and the cli
  binary to 60 -- so a wait somebody forgets to bound is named in five minutes instead of ctest's
  1500-second silence. And the sibling half of the `whenAll`
  sweep is closed: an arm that gave up early without stopping the sibling parked in `accept()`
  turned a red into a hang just as an assertion there would, at five sites (two loopback client
  flows, the AF_UNIX probe, and the two TLS cases whose server runs on another thread, where the
  hang landed on `std::thread::join`).

### Imported

Each file was read as a git blob at the commit named, and none contains a CR byte. Every row of
[`.agent/reference/provenance.md`](https://github.com/contour-terminal/core-cpp/blob/master/.agent/reference/provenance.md)
that names an upstream falls under exactly one row below; that table has the per-file record.

| From | Commit | What |
|---|---|---|
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | `f6ec49f3446b8bc121eba82c64cde2de759e774a` | `cmake/portable/CompileCache.cmake` and `cmake/FetchTransferBound.cmake`, verbatim |
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | `eb9c9c68da8fadfd43b0b36366919cb462689f48` | the bounded bootstrap download in `cmake/CPM.cmake`; the Windows error-popup suppression, merged into `SuppressWindowsDialogs`; the hook-name `IgnoredRegexp` of `.clang-tidy` |
| [contour](https://github.com/contour-terminal/contour) | `6777ff05014f8ff163b071e8b0e942830119db80` | `.clang-format` and `.clang-tidy`, adapted; two copies of `SuppressWindowsDialogs`, merged; `LICENSE` |
| [endo](https://github.com/contour-terminal/endo) | `f774a210ce989e5947b8f61d715068b1dc96088c` | `SuppressWindowsDialogsAtStartup.cpp`, `WindowsDialogCanary.cpp`, the CPM 0.40.8 pin; a copy of `SuppressWindowsDialogs`, merged; `.github/clang-tidy-matcher.json` |
| [contour](https://github.com/contour-terminal/contour) | `6777ff05014f8ff163b071e8b0e942830119db80` | crispy's generic half, `src/crispy/{Assert,Base64,Deferred,Defines,Environment,Escape,FNV,Flags,Overloaded,Times,UserInfo,Utils}` as `core` (`core::base`), `{LogStore,LogSink}` as `core::log`, `{CLI,App}` as `core::cli`, and `testing/Environment.hpp` as `core::testing`, with their tests (`Base64`, `CLI`, `Environment`, `LogSink`, `Times`, `Utils`); `fatal()` and `SoftRequire()` moved from `Assert.hpp` to `core/log/Assert.hpp`; `gsl::not_null` replaced by a reference |
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | `ee71f868547712892b7d9a2ebff60d49c496e25c` | `src/FastCache/Core/{Profiling,Ranges}.hpp` as `core/{Profiling,Ranges}.hpp` (`FC_*` as `CORE_*`, `FastCache::Ranges` as `core::ranges`), with `Profiling_test.cpp` and `Ranges_test.cpp` |
| [endo](https://github.com/contour-terminal/endo) | `f774a210ce989e5947b8f61d715068b1dc96088c` | `src/testing/{ScopedTempDir,ScopedWorkingDirectory,EnvHelper}.hpp` and `ScopedTempDir_test.cpp` as `core::testing`; `EnvHelper` writes through `core::setProcessEnvironmentVariable()` and reads through `core::LiveEnvironment` on POSIX, not `setenv()`/`getenv()` |
| [endo](https://github.com/contour-terminal/endo) | `f774a210ce989e5947b8f61d715068b1dc96088c` | the generic half of `src/platform` as `core::platform` (Types, PlatformError, Clock, Wakeup, SignalHandler, SystemPipe, WinsockInit, MessageQueue, FileSystem, NativeFileSystem, FileInfoProvider, EnvironmentProvider, UserPaths, PathUtils, GlobMatch, FileUri, SystemInfo, StringUtils, their `posix/`, `linux/` and `windows/` implementations and `testing/` doubles), with their tests (`WindowsPlatform_test.cpp` split into `PathUtils_test`, `Types_test` and `UserPaths_test`); `Generator.hpp` as `core::base` (`core::Generator`; Task A5b moved it out of `core::async`, which it needs nothing of). Process, Pipe, WaitResult, ProcessProvider, ProjectFileTree, InstallPaths and InterruptThrottle stay in endo; the `namespace endo` compatibility aliases were not imported |
| [endo](https://github.com/contour-terminal/endo) | `f774a210ce989e5947b8f61d715068b1dc96088c` | `src/tui` as `core::tui` and `core::tui_output`: 151 files, with their tests (`src/tui/completer` as `core::tui::completer`, core-cpp#30; `src/tui/platform` split into `posix/` and `windows/` implementations, the OS state an opaque `NativeState` so no public header includes `<windows.h>` or `<termios.h>`, Ruling R41; `src/tui/runtime` rewritten by Task B12 onto `core::net::EventLoop`, `EventSource` and `MockEventSource` replaced by an input adapter and `ScriptedInputSource`). The byte-composing half is `core::tui_output`, which links `core::base` alone, and every byte goes through `writeToDestination()`. On the way in: `LanguageId::Endo` and `registerEndoHighlighter()` removed, the OSC 8 `id=` no longer prefixed `endo-`, the parser's paste, CSI and DCS buffers bounded (`MaxPasteLength`, `MaxCsiParamLength`, `MaxDcsLength`), `toLowerInto()` bounded by a `std::span`, `Rect` coordinates un-transposed in the dialogs, `FilesystemImageProvider` split out behind `CORE_CPP_WITH_IMAGES`, the SIGWINCH handler saving `errno`, `NOLINT`s and warning pragmas removed, and private constants renamed to CamelCase |
| [contour](https://github.com/contour-terminal/contour) | `6777ff05014f8ff163b071e8b0e942830119db80` | `src/net/platform/Clock.hpp`, merged into `core/platform/Clock.hpp`; `src/net/platform/SystemPipe.{hpp,cpp}`, whose non-blocking behaviour is merged into `core/platform/SystemPipe`; `src/net/platform/WinsockInit.{hpp,cpp}`, identical to endo's |
| [contour](https://github.com/contour-terminal/contour) | `6777ff05014f8ff163b071e8b0e942830119db80` | `src/coro/{Awaitable,Cancellation,Task,UniqueCoroHandle,WhenAll,WhenAny}.hpp` and `{Task,WhenAll,WhenAny}_test.cpp` as `core::async`, `coro::` renamed `core::async::`; the `std::stop_token` aliases of `Cancellation.hpp` moved to `StopToken.hpp`, whose fallback replaces their `#error`; no `NOLINT`; two locals renamed for `-Wshadow`; two `WhenAny_test.cpp` helpers compiled only where the case using them is. `test_main.cpp` was not imported (`core::testing_main` replaces it), and `testing/SuppressWindowsDialogs.hpp` had been merged into `core::testing` already |
| [contour](https://github.com/contour-terminal/contour) | `6777ff05014f8ff163b071e8b0e942830119db80` | `src/net` as `core::net`, `core::net_types` and `core::net_tls`, `net::` renamed `core::net::` and `coro::` `core::async::`, with its tests but `test_main.cpp`; `net/platform/{Clock,NativeHandle,SystemPipe,WinsockInit}` replaced by `core::platform`, whose `SystemPipe::read()` returns a `ChannelResult`; `platform/PeerAddress.hpp` moved to `detail/` and `platform/WindowsLoopback.*` to `windows/`, so that no `core::net::platform` namespace hides `core::platform`; platform code in platform subdirectories (epoll in `linux/`, kqueue in `bsd/`, `PollEventSource.cpp` split into `posix/` and `windows/`, `WaitChunking.hpp` in `detail/`); `NetError` split out of `IoResult.hpp` into `NetError.hpp`; `testing/TempDir.hpp` not imported (`core::testing::ScopedTempDir`); no `NOLINT`; the C-style `for` loops written as range-`for`s and `while`s; one lambda parameter renamed for GCC's `-Wshadow`, and a `CMSG_FIRSTHDR()` result checked for GCC's `-Wnull-dereference`; the TLS test makes its client context before its server thread starts |
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | `b461e8b6d367ed22e4bf2935717fa59360a64b7d` | `src/FastCache/Core/Clock.hpp`, merged into `core/platform/Clock.hpp` in camelBack (`Now`/`Refresh` as `now`/`refresh`, `TimePoint`/`Duration` as `SteadyTimePoint`/`SteadyDuration`); `Clock_test.cpp` and `WallClockRef_test.cpp`, merged into `core/platform/Clock_test.cpp` |
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21` | `src/FastCache/Async/{ParkedWork,IExecutor,ResumeOn,ThreadPoolExecutor,AsyncQueue}.{hpp,cpp}` and their tests as `core::async`, `FastCache::` renamed `core::async::` and `Detail::` `detail::`; `DetachedTask` and `SyncRun`/`SyncRunWith` out of `Task.hpp` into `DetachedTask.hpp` and `SyncRun.hpp` (Ruling R66), and the rest of that file merged into contour's `Task.hpp`; `ThreadPoolExecutor.cpp` inlined into its header (Ruling R65), over `std::thread` rather than `std::jthread`; `IReactor` replaced by `IExecutor` in `AsyncQueue` and `ParkedWork_test.cpp`, whose reactor-driven cases belong to Task B4 |
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21` | `src/FastCache/Net/{IDatagramSocket,UdpSocket,SharedPortDatagram,InMemoryDatagram,BlockingSocket,BlockingConnector,TcpClient,HealthProbe}` and their tests, `InMemoryTransport` as `testing/InMemorySocket` (core-cpp's `testing/InMemoryTransport.hpp` is contour's real socket pair), `SocketClosedStates_test.cpp`, and `src/tests/{DatagramPayload,SocketDecorator}.hpp` as `core::net::testing`, camelBack; `UdpSocket.cpp` split into `posix/` and `windows/`; `BlockingListener`, `Detail::AcceptRaw` and `FailingReadSocket` not imported; `HealthProbe_test.cpp` rewritten against `core::net::serve` |
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21` | the rest of `src/FastCache/Net` and the reactor half of `src/FastCache/Async`, merged into `core::net` (61 files; with the row above and the `core::async` row, the 103 of this commit): the reactors (`EpollReactor`, `IocpReactor`, `TestReactor`, `PlatformReactor`) become `IoBackend`s under one `EventLoop` (`TestLoop`, `PlatformLoop`, `detail::ReadyBatch`, `detail::WorkerIdentity`), with `DeadlineTimer`, `InterruptibleSleep` and `SleepUntil`; `ISocket`'s awaitable and read slot as `IoAwaitable` and `SocketContract`, with the empty-read-buffer canary; the dial (`ConnectFlow`, `IConnector`, `ReactorDial` as `ReadinessDial`, `IocpDial`, `SocketAddress`, `ThreadedAddressResolver`, `IAsyncAddressResolver`, `IAdmissionControl`, `KeepAlive`, `SocketDeadline`); `TlsWrap` as `ITlsContext` and the TLS tests, with `testing::StrictTlsPeer`; the IOCP socket (`IocpSocket`, `IocpStatus` as `IocpOperation`); and the contract tests (`CancelRead`, `WaitReadable`, `AcceptedHalfClose`, `ReactorSocket`, `SocketDecorator`, `ClockRefresh`, `LoopTeardown`). camelBack throughout; `.agent/reference/provenance.md` has each file's own delta |
| [fastcached](https://github.com/LASTRADA-Software/fastcached) | `5389e29a5eeca9c2319f43757bd7d6d0ac1c1a13` | `vendor/endo/tui/runtime/TuiRuntime.hpp` and its test: endo `f774a210` plus fastcached `6483abd8`, which decides an elapsed `DelayAwaiter` deadline in `await_suspend()` because MSVC 19.44's ARM64 code generator loses the `try` around a `co_await` whose `await_ready()` reads a virtual clock. Task B12 then composed the class on `core::net::EventLoop` |

The rulebook and CI configuration adapt text from fastcached at
`b5ded89c5ae6ba5b45337335ce774c5ae6986d65`, contour and endo at the commits above, Lightweight at
`f57dc2e0704d885a3c642a63675873919fc2d128` and tuidu at
`30107fbab72310fde5db89e7882eab288f6b541e`; `NOTICE` lists the files.
