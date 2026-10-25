# Native Redesign — Step 6.5 (Moments: Finished Elsewhere) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling). Line numbers are at HEAD `661b6029`, before 1c (in progress), 1d, 2, 3a–3f,
4b, 5a and 5b land. Where this step uses code they add, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before 6.5 executes.

**Goal:** On macOS, a command that ran 10 s or more while its pane was unfocused raises a real Notification Center
notification when Contour runs from an `.app` with a bundle identifier — a click focuses the pane and scrolls to its
block, and it chimes only when the bell sound is on — and a tray message otherwise; its tab carries the Done or
Error badge until it is seen.

**What already exists** (and stays): the decision and the words (`platform/FinishNotification.hpp`,
`config/FinishNotificationConfig.hpp`: `when: unfocused`, `min_duration: 10`), the delivery
(`TerminalSession::deliverFinishNotice`, `TerminalSession.cpp:2328-2354`), `TrayNotifier` on every non-Linux host
and `FreeDesktopNotifier` on Linux (`platform/Notifier.cpp:56-75`), the Done/Error badge
(`window/TabAttention.hpp`, `TabAttention::noteFinished`, Step 1c, commit `661b6029`), and the click that reveals the
session and scrolls to the block (Step 3f R-o, Task 12).

**Architecture:**
- **One centre per app, one notifier per session.** `platform::IUserNotificationCenter` is the app-wide seam
  (post, withdraw, route a click back by request id), with a null implementation inline. `UserNotificationNotifier`
  is a `Notifier` per session over it, prefixing every request id with the session, so two sessions' OSC 99
  identifiers never meet.
- **The macOS centre is gated.** `CocoaUserNotificationCenter.mm` (ARC) is built only when `appBundleOf(path, id)`
  says the process runs from an `.app` with a bundle identifier: `+[UNUserNotificationCenter currentNotificationCenter]`
  raises outside one. Everywhere else, and in every `TestApp`, the centre is the null one and the session keeps
  today's `TrayNotifier` (Linux keeps FreeDesktop or the portal).
- **Authorization is a pure gate.** `AuthorizationGate` decides whether a post asks, waits, goes out or is dropped,
  so the queue while the user answers the prompt is tested headless.
- **Sound is data on the notification.** `vtbackend::NotificationSound` on `DesktopNotification`: a finish notice
  chimes only when the profile's bell sound is on; FreeDesktop maps `Silent` to its `suppress-sound` hint.

**Tech Stack:** C++23, Qt 6.11, Objective-C++ (UserNotifications, AppKit) on Apple, Catch2 v3, CMake build tree
`out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.5 (`:822`): "A
notification for a command that ran 10 s or more while unfocused, plus the Done or Error badge"; seam "macOS
`UNUserNotificationCenter` `.mm`, only when running from an `.app` with a bundle identifier (otherwise
`TrayNotifier`)". It binds with what it cites: §1 principles (`:28-64`; 4, the OS through interfaces), §2.6 testing
(`:206-226`), §5 (`:844-854`). The canvas is <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; local copies in
`.superpowers/design/canvas/project/`: `moments` board 8 ("Past 10 s and out of view: a system notification (click
focuses the pane and scrolls to the block) plus a ✓ / ✗ badge on the tab until it is seen. Sound only if the bell
sound is on"; "cmake finished" · "~/repo/contour · 2m 14s · exit 0"), `daemon` board 5 (the badge slot), `blocks`.
The plans index is `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

## Status board and handoff (every step)

This run is tracked on a status board: <https://claude.ai/artifact/YZUVcC1vdaWaLGJLsdS2AX>. The
owner asked for the link to live here so that any agent continuing the work can find the board and
keep it current. The page is kept with `/status-board` (the `contour-workflows` plugin) on the machine doing the work; its source is local to that working tree and never committed.
- **Keep it up to date** with `/status-board update`, which republishes to the link above
  (`/status-board` remembers the link per working tree). The board belongs to the owner's account, so a
  session signed in as the owner can read and publish it; the design canvas, the owner's copy of
  Yaraslau's, is in the same account.
- **Every agent that continues this work updates the board.** Do it
  in the same turn as each state change: a step or task started, in review, committed, landed,
  blocked or unblocked, a plan written, or a ruling.
- A different worktree has a board of its own. Never write to another worktree's board.
  Report to the session that owns the board instead.
- If this worktree has no board, ask the owner rather than creating a second board.

## Global Constraints

- **The spec, verbatim (§3.16 row 6.5):** "A notification for a command that ran 10 s or more while unfocused, plus
  the Done or Error badge"; "macOS `UNUserNotificationCenter` `.mm`, only when running from an `.app` with a bundle
  identifier (otherwise `TrayNotifier`)". Canvas: "click focuses the pane and scrolls to the block", "Sound only if
  the bell sound is on".
- **Binding rulings** (ledgers in `docs/superpowers/ledgers/`, `.superpowers/sdd/*/progress.md`):
  - Every OS service is an interface with a null implementation, built through `platform::AppearanceServices`
    (`AppearanceServices.hpp`), so every `TestApp` gets the null one through `nullAppearanceServices()` and
    `appearanceServicesWith(…)` names the new field (1b R-a / Review Focus 1: a test must never reach a native
    backend).
  - AppKit and UserNotifications: every call is documented `@pre` main thread; the `.mm` is ARC (`-fobjc-arc`),
    listed in the `if(APPLE)` blocks of `src/contour/platform/CMakeLists.txt:83-99` and of `contour_gui_test`
    (`src/contour/CMakeLists.txt:838-849`); no integer-to-object-pointer cast (1b F7); the Apple factory lives in the
    `.mm`, the non-Apple one in a `.cpp` wholly inside `#ifndef __APPLE__` (the `PlatformAppearance.cpp` shape).
  - A `[cocoa]` test that needs the cocoa platform skips under offscreen (1b F8); `[display]` tests are opt-in.
  - `enum class` over `bool`, zero the default; `std::expected` for fallible calls. The OSC 99 struct's existing
    `bool`s are a wire boundary (AGENT.md's serialization carve-out) and are not touched here.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** data-driven (the gate's transitions, the notifier selection, the sound rule are
  tables); dependency injection; configuration at construction; Doxygen; zero warnings and no `NOLINT`.
- **Build** with every core; the tree is configured with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds at once.
- **Lint and format** before each commit: `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug
  --extra-arg=-isysroot$(xcrun --show-sdk-path) <touched .cpp files>`; `clang-format -i` (llvm@22) on touched
  `.cpp`/`.hpp`/`.mm`; `editorconfig-checker` on touched `CMakeLists.txt`.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** both null `AppearanceServices` factories and
  every table row name every field.
- **Qt-free stays Qt-free:** `vtbackend`, `platform/AppBundle.hpp` and `AuthorizationGate` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task, staging only that task's files by name, each message ending with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, never `Co-Authored-By`.

## Review Focus

1. **The bare binary, a test run, or `contour` from a shell crashes in `currentNotificationCenter`.** Expected: the
   class is never touched unless the main bundle is an `.app` with an identifier; the test binary gets the null centre
   and keeps the tray. Pinned in Task 2 (`[appbundle]` and the `[cocoa]` case) and Task 3 (`TestApp`).
2. **Two sessions' OSC 99 notifications share an identifier (`i=1`)**, so one replaces the other, or a click focuses
   the wrong pane. Expected: request ids carry the session; a click reaches only its own session. Pinned in Task 1.
3. **A click on a notification whose session is gone**, or one left in Notification Center by a previous run.
   Expected: nothing happens and nothing crashes; a closing session withdraws its notifications; the centre removes a
   previous run's at start. Pinned in Task 1 (a destroyed notifier's subscription) and Task 2.
4. **The permission prompt.** A prompt at every launch, or the first finish notices lost while the prompt is up.
   Expected: authorization is asked on the first post, once; posts made meanwhile go out when it is granted (at most
   16 held); after a refusal nothing is raised and the badge still marks the tab. Pinned in Task 2
   (`AuthorizationGate`).
5. **Sound when the bell is off, or twice with `action: notify_bell`.** Expected: a finish notice chimes only when
   the profile's bell sound is not `off` and its volume is above 0, and is silent under `notify_bell`, where the bell
   is the sound. Pinned in Task 1.

Also pinned, beyond the five: the badge end to end — a hidden tab's 12-s command gives Done, `exit 2` gives Error,
seeing the tab clears it (Task 3); Linux's FreeDesktop args carry `suppress-sound` for a silent notice (Task 1).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. The gate.** `appBundleOf(bundlePath, bundleIdentifier)` is `Present` when the path ends in `.app` (case
  sensitive, after trailing slashes) and the identifier is non-empty. The `.mm` reads `NSBundle.mainBundle` for both.
  A Homebrew symlink into the bundle still resolves to the bundle (NSBundle follows the executable's real path).
- **R-b. One centre, request ids by session.** The centre is created once per app; each session's
  `UserNotificationNotifier` subscribes with the prefix `contour-session-{id}/` and posts `{prefix}{oscIdentifier}`.
  A click's request id is routed to the subscription whose prefix it starts with, as the OSC identifier after it; no
  subscription, no action. A notifier's destructor unsubscribes and withdraws what it posted.
- **R-c. Authorization (owner).** Asked on the first post (`UNAuthorizationOptionAlert | UNAuthorizationOptionSound`),
  never at launch. While pending, posts are held (`AuthorizationHoldLimit` = 16, the oldest dropped) and sent when
  granted; refused, every later post is dropped and logged once, with no tray fallback. A later change in System
  Settings is read at the next launch (`getNotificationSettings` at construction sets the gate's start state).
- **R-d. Sound (owner).** `enum class NotificationSound : uint8_t { DesktopDefault = 0, Silent, Chime };` on
  `vtbackend::DesktopNotification`. OSC 99 leaves `DesktopDefault` (Cocoa: the default sound; FreeDesktop and the
  portal: unchanged). A finish notice is `Chime` when its action is `Notify` and the bell is on (`bell.sound != "off"`
  and `bell.volume > 0`), else `Silent`. FreeDesktop sends `suppress-sound: true` for `Silent`; Cocoa sets no sound for
  `Silent` and `UNNotificationSound.defaultSound` otherwise.
- **R-e. Presentation while Contour is frontmost.** "Unfocused" includes another Contour window being key, so the
  delegate's `willPresentNotification` answers banner and list (and sound when the content has one).
- **R-f. Withdrawal.** `close(identifier)` removes the request from both delivered and pending; at construction the
  centre removes every delivered request of this app (a previous run's sessions are gone).
- **R-g. A click.** `didReceiveNotificationResponse` with the default action is dispatched to the main queue, routed
  per R-b, and emitted as the session's `actionInvoked(oscIdentifier)`; Step 3f's handler reveals the session and
  scrolls to the block. AppKit activates the app on the click by itself.
- **R-h. Windows and Linux are unchanged.** Windows keeps the tray (Step 5a adds no toast backend); Linux keeps
  FreeDesktop or the portal. The selection is a table (`selectSessionNotifier`).

### Non-goals

- Notification actions (buttons), grouping by tab (`threadIdentifier` is set to the session prefix, nothing more),
  a Windows toast backend, changing when a finish notifies.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/vtbackend/vt/DesktopNotification.hpp` (modify) | `NotificationSound` | 1 |
| `src/contour/platform/UserNotificationCenter.hpp` (create) | `IUserNotificationCenter`, request, subscription, null centre, `AuthorizationGate` | 1, 2 |
| `src/contour/platform/UserNotificationNotifier.{hpp,cpp}` (create) | The per-session `Notifier` over the centre | 1 |
| `src/contour/platform/DBusNotificationTransport.cpp` (modify) | `suppress-sound` | 1 |
| `src/contour/session/TerminalSession.cpp` (modify) | The finish notice's sound | 1 |
| `src/contour/test/RecordingUserNotificationCenter.hpp` (create) | The test double | 1 |
| `src/contour/platform/AppBundle.hpp` (create), `CocoaUserNotificationCenter.mm` (create), `UserNotificationCenter.cpp` (create), `CMakeLists.txt` (modify) | The gate and the macOS centre | 2 |
| `src/contour/platform/AppearanceServices.hpp`, `Notifier.{hpp,cpp}`, `ContourGuiApp.{hpp,cpp}`, `session/TerminalSession.{hpp,cpp}`, `test/AppearanceFixtures.hpp` (modify) | Wiring | 3 |

---

### Task 1: Sound on a notification, and a session's notifier over a centre

**Files:**
- Modify: `src/vtbackend/vt/DesktopNotification.hpp` — `NotificationSound` and the member
  `NotificationSound sound = NotificationSound::DesktopDefault;` after `urgency`.
- Create: `src/contour/platform/UserNotificationCenter.hpp` (list in `contour_platform`).
- Create: `src/contour/platform/UserNotificationNotifier.hpp`, `.cpp` (list in `contour_platform`).
- Modify: `src/contour/platform/DBusNotificationTransport.cpp` — `buildFreedesktopNotifyArguments`
  (`DBusNotificationTransport.hpp:59`) adds the hint for `Silent`.
- Modify: `src/contour/session/TerminalSession.cpp` — `deliverFinishNotice` (`:2339-2343`) sets `notification.sound`
  per R-d; the rule is a pure function in `platform/FinishNotification.hpp`.
- Create: `src/contour/test/RecordingUserNotificationCenter.hpp`.
- Test: `src/contour/platform/UserNotificationNotifier_test.cpp` (in `contour_gui_test`: a `QObject`),
  `src/contour/platform/FinishNotification_test.cpp`, `src/contour/platform/FreeDesktopNotifier_test.cpp`,
  `src/contour/session/TerminalSession_test.cpp`.

**Interfaces:**
- Produces (`platform/UserNotificationCenter.hpp`, namespace `contour::platform`):
  - `struct UserNotificationRequest { std::string requestId {}; std::string title {}; std::string body {}; vtbackend::NotificationSound sound = vtbackend::NotificationSound::DesktopDefault; std::string threadId {}; bool operator==(UserNotificationRequest const&) const = default; };`
  - `using UserNotificationActivated = std::function<void(std::string_view oscIdentifier)>;`
  - `class UserNotificationSubscription` — move-only; its destructor unsubscribes; built only by a centre.
  - `class IUserNotificationCenter { public: virtual ~IUserNotificationCenter() = default; virtual void post(UserNotificationRequest request) = 0; virtual void withdraw(std::string const& requestId) = 0; [[nodiscard]] virtual UserNotificationSubscription subscribe(std::string prefix, UserNotificationActivated onActivated) = 0; [[nodiscard]] virtual UserNotificationAvailability availability() const noexcept = 0; };`
    with `enum class UserNotificationAvailability : uint8_t { Unavailable = 0, Available };`. `post` and `withdraw` are
    `@pre` main thread and never block.
  - `class NullUserNotificationCenter final: public IUserNotificationCenter` (inline): `Unavailable`; posts nothing.
  - `inline constexpr std::string_view SessionRequestPrefix = "contour-session-";`
    `[[nodiscard]] std::string sessionRequestPrefix(int sessionId);` → `"contour-session-7/"`.
- Produces (`platform/UserNotificationNotifier.hpp`):
  `class UserNotificationNotifier final: public Notifier { public: UserNotificationNotifier(IUserNotificationCenter& center, int sessionId, QObject* parent = nullptr); void notify(vtbackend::DesktopNotification const& notification) override; void close(std::string const& identifier) override; };`
  — subscribes in its constructor (activation → `emit actionInvoked(identifier)`, through a `QPointer` guard),
  posts `{prefix}{identifier}`, tracks what it posted, withdraws it all in its destructor. An application's
  notification is titled `tr("Application Message: %1")` as `TrayNotifier` titles it (`Notifier.cpp:22-36`).
- Produces (`platform/FinishNotification.hpp`):
  `[[nodiscard]] constexpr vtbackend::NotificationSound finishNoticeSound(config::NotifyAction action, BellAudibility bell) noexcept;`
  with `enum class BellAudibility : uint8_t { Muted = 0, Audible };` and
  `[[nodiscard]] inline BellAudibility bellAudibilityOf(std::string_view sound, float volume) noexcept;` (`"off"` or a
  volume ≤ 0 → `Muted`).
- Produces (`test/RecordingUserNotificationCenter.hpp`): `posted`, `withdrawn`, `availability` settable, and
  `void activate(std::string const& requestId);` routing as the real centre does.

- [ ] **Step 1: Write the failing tests.**
  - `FinishNotification_test.cpp` (`[notification]`, in `contour_test`): `finishNoticeSound(Notify, Audible) == Chime`;
    `(Notify, Muted) == Silent`; (Review Focus 5) `(NotifyBell, Audible) == Silent`; `bellAudibilityOf("off", 1.0) ==
    Muted`, `("default", 0.0) == Muted`, `("default", 0.5) == Audible`.
  - `UserNotificationNotifier_test.cpp` (`[usernotifications]`), over the recording centre:
    - a notifier for session 7 posting OSC id `"1"` posts request id `"contour-session-7/1"`, the title and body, and
      `threadId == "contour-session-7/"`.
    - (Review Focus 2) notifiers for sessions 7 and 8 both post id `"1"` → two requests; `activate("contour-session-8/1")`
      fires `actionInvoked("1")` on session 8's notifier only.
    - `close("1")` withdraws `"contour-session-7/1"`.
    - (Review Focus 3) destroying session 7's notifier withdraws its requests and unsubscribes; a later
      `activate("contour-session-7/1")` does nothing.
    - an application's notification (not `FinishNotificationIdPrefix`) is titled `"Application Message: …"`.
  - `FreeDesktopNotifier_test.cpp`: `buildFreedesktopNotifyArguments` of a `Silent` notification has
    `hints["suppress-sound"] == true`; of `DesktopDefault` has no such hint.
  - `TerminalSession_test.cpp` (`[contour][session][notification]`), extending "a finished command raises one finish
    notification" (`:9104`): with the profile's bell `default` at volume 1 the recording notifier saw `sound == Chime`;
    with `bell.sound: off`, `Silent`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors on `NotificationSound` and `UserNotificationCenter.hpp`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[notification]"
out/appleclang-debug/src/contour/contour_gui_test "[usernotifications],[contour][notification],[contour][session][notification]"
out/appleclang-debug/src/vtbackend/vtbackend_test "[DesktopNotification]"
```

  Expected: PASS (the field's default keeps
  every OSC 99 case unchanged).
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: a notification says whether it makes a sound; a session's notifier over one app-wide centre" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 2: The gate and the macOS centre

**Files:**
- Create: `src/contour/platform/AppBundle.hpp` (Qt-free); test `src/contour/platform/AppBundle_test.cpp` in
  `contour_test`.
- Modify: `src/contour/platform/UserNotificationCenter.hpp` — `AuthorizationGate`, and the factories below.
- Create: `src/contour/platform/UserNotificationCenter.cpp` — the non-Apple `makeUserNotificationCenter()` and
  `currentAppBundle()`, wholly inside `#ifndef __APPLE__`; test `src/contour/platform/UserNotificationCenter_test.cpp`
  in `contour_test` (the gate).
- Create: `src/contour/platform/CocoaUserNotificationCenter.mm` — the Apple factory, `currentAppBundle()` and
  `CocoaUserNotificationCenter`; test `src/contour/platform/CocoaUserNotificationCenter_test.mm` in `contour_gui_test`.
- Modify: `src/contour/platform/CMakeLists.txt` — the `.mm` in the `if(APPLE)` sources and the `-fobjc-arc` list,
  `target_link_libraries(contour_platform PUBLIC "-framework UserNotifications")`; `src/contour/CMakeLists.txt` — the
  test `.mm` in both `APPLE` lists of `contour_gui_test`.

**Interfaces:**
- Produces (`platform/AppBundle.hpp`): `enum class AppBundle : uint8_t { Absent = 0, Present };`
  `[[nodiscard]] constexpr AppBundle appBundleOf(std::string_view bundlePath, std::string_view bundleIdentifier) noexcept;` — R-a.
- Produces (`platform/UserNotificationCenter.hpp`):
  - `enum class AuthorizationState : uint8_t { Unasked = 0, Pending, Granted, Denied };`
    `enum class GateAction : uint8_t { Ask = 0, Hold, Send, Drop };`
    `inline constexpr std::size_t AuthorizationHoldLimit = 16;`
  - `class AuthorizationGate { public: explicit AuthorizationGate(AuthorizationState start); [[nodiscard]] GateAction admit(UserNotificationRequest request); [[nodiscard]] std::vector<UserNotificationRequest> answer(AuthorizationState answer); void forget(std::string const& requestId); [[nodiscard]] AuthorizationState state() const noexcept; };`
    — `Unasked` → `Ask` (and the request is held), `Pending` → `Hold` (bounded), `Granted` → `Send`, `Denied` → `Drop`;
    `answer(Granted)` returns the held requests in order; `answer(Denied)` returns none; `forget` drops a held one.
  - `[[nodiscard]] std::unique_ptr<IUserNotificationCenter> makeUserNotificationCenter();` — Apple: a
    `CocoaUserNotificationCenter` when `currentAppBundle() == AppBundle::Present`, else the null centre; elsewhere the
    null centre. `[[nodiscard]] AppBundle currentAppBundle();` — `Absent` off Apple.
- Produces (`CocoaUserNotificationCenter`, in the `.mm` only): construction reads `getNotificationSettings` for the
  gate's start state, removes this app's delivered notifications (R-f), and installs a delegate (an ARC object held
  strongly by the centre; the delegate holds a `std::weak_ptr` to the routes, so a click after the centre is gone does
  nothing). `post` runs the gate; `Ask` calls `requestAuthorizationWithOptions:` and dispatches its answer to the main
  queue, into `answer`, then adds the released requests. A request becomes `UNMutableNotificationContent` (title, body,
  `threadIdentifier`, sound per R-d) in a `UNNotificationRequest` with no trigger. The delegate implements R-e and R-g.
  Every method is documented `@pre` main thread.

- [ ] **Step 1: Write the failing tests.**
  - `AppBundle_test.cpp` (`[appbundle]`): `appBundleOf("/Applications/Contour.app", "org.contourterminal.Contour") ==
    Present`; `("/Applications/Contour.app/", id) == Present`; (Review Focus 1) `("/Users/me/out/src/contour", id)`,
    `("/Applications/Contour.app", "")`, `("/Applications/Contour.APP.bak", id)` → `Absent`.
  - `UserNotificationCenter_test.cpp` (`[usernotifications]`), Review Focus 4: from `Unasked`, the first `admit` is `Ask`,
    the next two `Hold`; `answer(Granted)` returns all three in order and a later `admit` is `Send`; from `Pending`,
    `AuthorizationHoldLimit + 2` admits keep the newest 16; `answer(Denied)` returns none and every later `admit` is
    `Drop`; `forget` of a held id removes it from what `answer(Granted)` returns.
  - `CocoaUserNotificationCenter_test.mm` (`[cocoa][usernotifications]`): the test binary is no `.app`, so
    `currentAppBundle() == Absent` and `makeUserNotificationCenter()->availability() == Unavailable` — the case that
    keeps Review Focus 1 honest; it never touches `UNUserNotificationCenter`. Skips unless the platform is `cocoa`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `platform/AppBundle.hpp` not found.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[appbundle],[usernotifications]"
out/appleclang-debug/src/contour/contour_gui_test "[cocoa][usernotifications]"
```

  Expected: PASS (the `[cocoa]` case skips offscreen and runs on this Mac's display).
- [ ] **Step 5: Lint, format and commit** the files above by name (`editorconfig-checker` on both `CMakeLists.txt`):
  `git commit -m "contour: on macOS, Notification Center when running from an app bundle, asked for on first use" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 3: Every session's notices through the centre

> **Depends on Step 1c:** the badge (`TabAttention::noteFinished`, `pooledAttention`, the `[manager][tabbadge]` cases).
> **Depends on Step 3f:** the click scrolls to the block (3f Task 12's `actionInvoked` handler). **Depends on Steps 5a
> and 5b:** any fields they added to `AppearanceServices` (named in both null factories).

**Files:**
- Modify: `src/contour/platform/AppearanceServices.hpp` — the field
  `std::function<std::unique_ptr<IUserNotificationCenter>()> userNotifications = makeUserNotificationCenter;` and the
  null factory's entry; `src/contour/test/AppearanceFixtures.hpp` — `appearanceServicesWith(…)` names it.
- Modify: `src/contour/platform/Notifier.hpp`, `.cpp` — the selection below; `makeDesktopNotifier` takes the centre.
- Modify: `src/contour/ContourGuiApp.hpp`, `.cpp` — builds the centre once, after the `QGuiApplication` exists;
  `[[nodiscard]] platform::IUserNotificationCenter& userNotificationCenter() noexcept;`.
- Modify: `src/contour/session/TerminalSession.cpp` — the default notifier (`:398-409`) passes
  `app.userNotificationCenter()` and the session's id.
- Test: `src/contour/platform/Notifier_test.cpp`, `src/contour/session/TerminalSession_test.cpp`,
  `src/contour/session/TerminalSessionManager_test.cpp`, `src/contour/ContourGuiApp_test.cpp` or the fixture's own test.

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces (`platform/Notifier.hpp`):
  - `enum class SessionNotifierKind : uint8_t { Tray = 0, Desktop, UserNotifications };`
  - `[[nodiscard]] constexpr SessionNotifierKind selectSessionNotifier(NotifierHost host, UserNotificationAvailability centre) noexcept;`
    with `enum class NotifierHost : uint8_t { Other = 0, Linux, MacOS };` — Linux → `Desktop`; MacOS with `Available`
    → `UserNotifications`; everything else → `Tray`.
  - `[[nodiscard]] std::unique_ptr<Notifier> makeDesktopNotifier(std::chrono::milliseconds closeDelay, TrayNotifier::ShowMessage const& showTrayMessage, IUserNotificationCenter& centre, int sessionId);`
    — dispatches on `selectSessionNotifier(currentNotifierHost(), centre.availability())`; the two existing overloads
    stay for their callers (`ContourGuiApp.cpp:810`).

- [ ] **Step 1: Write the failing tests.**
  - `Notifier_test.cpp` (`[contour][notification][notifier]`, in `contour_gui_test`): `selectSessionNotifier` over every host × availability.
  - `TerminalSession_test.cpp` (`[contour][session][notification][manager]`): with `appearanceServicesWith(…)` giving a
    `RecordingUserNotificationCenter` that reports `Available`, and the host forced to `MacOS` through the selection's
    input (a `TestApp` option), a 12-s command in a background session posts one request
    `"contour-session-{id}/contour:command-finished:{id}"`; `activate(it)` brings the session's tab forward and its
    block's head into the viewport (Step 3f's assertions at `:9277`).
  - `TerminalSessionManager_test.cpp` (`[manager][tabbadge]`), the badge end to end: a hidden tab's command of 12 s that
    exits 0 → the tab's badge is `Done`; one that exits 2 → `Error`; activating the tab → `None`; a 3-s command → no
    notification and no finished badge.
  - (Review Focus 1) "TestApp's centre is the null one": a default `TestApp`'s
    `userNotificationCenter().availability() == Unavailable`, and its sessions' finish notices still reach the tray
    path the existing tests assert.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `userNotifications` in `AppearanceServices`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[notification]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][notification],[contour][session][notification],[manager][tabbadge],[manager],[usernotifications]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: finish notices and application notifications go to Notification Center on macOS" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 4: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts`), apply its
  findings, re-run the filters of every task it touched, and commit once with the Signed-off-by line, staging only
  the files it changed.
- [ ] **Step 2: Every filter once more.**

```bash
out/appleclang-debug/src/contour/contour_test "[notification],[appbundle],[usernotifications]"
out/appleclang-debug/src/contour/contour_gui_test "[usernotifications],[contour][notification],[contour][session][notification],[manager][tabbadge],[cocoa][usernotifications]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, including the Terminal row** (the Step 6.1 plan's Task 6 Step 3 loop). This step adds no
  surface (the notification is the OS's, the badge is Step 1c's row); expected: the count is unchanged and the badge
  rows look as after 1c.
- [ ] **Step 4: Owner manual checks** (batched for Phase 6; list them in the ledger, do not wait), from
  `out/appleclang-debug/src/contour/contour.app` (`open` it): run `sleep 12; true` in a background tab and switch to
  another app → the first time, macOS asks for permission; allow → a banner "✓ sleep finished" with the directory and
  duration, a chime; click it → Contour comes forward on that tab with the block's head in view; `sleep 12; false` →
  "✗ sleep failed (exit 1)" and the tab's ✗ badge until you look at it; `bell.sound: off` → no chime; deny in System
  Settings › Notifications and relaunch → no banner, the badge still shows; copy
  `contour.app/Contents/MacOS/contour` out of the bundle and run it → a tray message instead, no crash.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release: "Shows
  finished-command and application notifications in Notification Center on macOS when Contour runs from its app
  bundle; clicking one brings the pane forward at its command." and "Plays the notification sound only when the
  profile's bell sound is on."
- [ ] **Step 6:** Mark Step 6.5 done in the plans index's step table and *Last update*, and on the board with
  `/status-board update`. Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-5.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none; one centre per app; a post is asynchronous.
  - **Risk:** medium on macOS (a new framework, a permission prompt on first use, the delegate's threading); none on
    Windows; on Linux one hint for silent notices.
  - **Coverage:** the gate, the bundle check, the per-session routing, the sound rule, the selection table, the
    badge end to end; the Cocoa adapter itself only by the owner checks (the test binary has no bundle).
- [ ] **Step 7:** Run `/code-review` on the whole step and address every finding.

---

## Owner decisions

The run proceeds on each recommended default.

1. **A refused permission raises nothing (R-c).** *Default: respect it; the badge still marks the tab.* The
   alternative falls back to the tray, which on macOS is the same Notification Center the user just refused.
2. **Authorization is asked on the first post (R-c).** *Default: yes, never at launch.*
3. **The sound rule (R-d).** *Default: chime only for `notify` with the bell on; silent under `notify_bell`.*
4. **Linux silent notices send `suppress-sound` (R-d).** *Default: yes, so "sound only if the bell sound is on" holds
   there too.* The alternative leaves Linux to the server's default.
5. **A previous run's notifications are removed at start (R-f).** *Default: yes; their sessions are gone.*

## Spec gaps found

- The ≥ 10 s / unfocused notification and the Done/Error badge already exist (`FinishNotificationConfig`, Step 1c,
  Step 3f R-o); this step adds the macOS backend, the sound rule and the end-to-end tests.
- "Otherwise `TrayNotifier`" is silent on a refused permission (owner 1) and on when to ask (owner 2).
- "Sound only if the bell sound is on" is the canvas's, not §3.16's; it touches Linux too (owner 4).
- The badge marks only a session that is off screen (`TabAttention::noteFinished` takes `Hidden`), while the
  notification follows `when: unfocused`; a visible pane in an unfocused window gets the notification but no badge,
  which matches "until it is seen".
