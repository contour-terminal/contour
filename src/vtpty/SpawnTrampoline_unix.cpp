// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ErrnoText.hpp>
#include <vtpty/OwnedFd.hpp>
#include <vtpty/PosixSpawn.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/SpawnTrampoline.hpp>
#include <vtpty/UnixUtils.hpp>

#include <core/Overloaded.hpp>
#include <core/Utils.hpp>

#include <array>
#include <bit>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#ifdef __APPLE__
    #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
    #include <libutil.h>
#else
    #include <pty.h>
#endif

#ifdef __linux__
    #include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

// POSIX requires no header to declare `environ`; see Process_unix.cpp for why each platform is spelled out.
#ifdef __APPLE__
    #include <crt_externs.h>
    #define environ (*_NSGetEnviron())
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" char** environ;
#endif

using namespace std::string_view_literals;

namespace vtpty
{

namespace
{
    /// The name the trampoline runs under, as ps shows it until the program's exec.
    constexpr auto TrampolineProgramName = "contour-spawn"sv;

    /// The lowest number the parent copies a source descriptor to before mapping it onto its slot: above
    /// every slot, so that no copy sits on a number another copy is dup2()ed onto.
    constexpr auto FirstTemporaryFd = 10;
    static_assert(trampolineFd(static_cast<TrampolineSlot>(TrampolineSlotCount - 1)) < FirstTemporaryFd);

    /// How long spawnPlaced() waits, in milliseconds, for a detached program's trampoline to name the
    /// program: no time at all once it has reaped the trampoline, which sent the name before it exited.
    constexpr auto DetachedHandoverTimeout = 5000;

    /// The longest spec the trampoline accepts, which it allocates up front: a length above it is garbage,
    /// not a spec. Well above the 2 MiB exec() takes on Linux under the default 8 MiB stack limit, a quarter
    /// of which argv and the environment may fill together.
    constexpr auto MaxSpecLength = std::uint32_t { 16U * 1024U * 1024U };

    // {{{ parent side
    /// A source descriptor's close-on-exec copy, and the number it takes in the trampoline.
    struct SlotMapping
    {
        OwnedFd copy;
        int target;
    };

    /// @return A close-on-exec copy of @p source, numbered FirstTemporaryFd or above, mapped onto @p target.
    [[nodiscard]] std::expected<SlotMapping, std::error_code> mapping(int source, int target) noexcept
    {
        auto copy = OwnedFd { ::fcntl(source, F_DUPFD_CLOEXEC, FirstTemporaryFd) };
        if (!copy.isOpen())
            return std::unexpected(util::lastError());
        return SlotMapping { .copy = std::move(copy), .target = target };
    }

    /// @return The slots a launch cannot do without.
    [[nodiscard]] constexpr TrampolineSlots requiredSlots(TerminalLaunch const& /*launch*/) noexcept
    {
        // Without its terminal the program would stay in this process's session, on its stdio.
        return TrampolineSlots {}.with(TrampolineSlot::Terminal);
    }

    [[nodiscard]] constexpr TrampolineSlots requiredSlots(DetachedLaunch const& /*launch*/) noexcept
    {
        return TrampolineSlots {};
    }

    /// Waits for a detached program's trampoline to end, and reads the program's pid from the gate it was
    /// written into.
    /// @param trampoline The trampoline, a child of this process.
    /// @param gate The parent's end of the program's gate.
    /// @return The program's pid; or why it is not known.
    [[nodiscard]] std::expected<pid_t, std::error_code> awaitDetachedProgram(pid_t trampoline, int gate)
    {
        if (auto const status = util::waitForExit(trampoline); status && WIFEXITED(*status))
            switch (static_cast<TrampolineExit>(WEXITSTATUS(*status)))
            {
                case TrampolineExit::UnsupportedVersion:
                    return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));
                case TrampolineExit::SpecUnreadable:
                    return std::unexpected(std::make_error_code(std::errc::bad_message));
            }
        // Once the trampoline is reaped, what it sent is there to read, or never comes. Waited for all the
        // same, briefly, should a SIGCHLD handler have reaped it first; a program left waiting on the gate
        // then sees its end close without the release byte on the way out, and exits without running.
        auto ready = pollfd { .fd = gate, .events = POLLIN, .revents = 0 };
        while (::poll(&ready, 1, DetachedHandoverTimeout) < 0 && errno == EINTR)
            ;
        auto received = std::array<char, sizeof(pid_t)> {};
        auto const length = ::recv(gate, received.data(), received.size(), MSG_DONTWAIT);
        auto const pid = std::bit_cast<pid_t>(received);
        if (std::cmp_not_equal(length, received.size()) || pid == 0)
            return std::unexpected(std::make_error_code(std::errc::no_child_process));
        if (pid < 0)
            return std::unexpected(std::error_code { -pid, std::generic_category() });
        return pid;
    }
    // }}}

    // {{{ trampoline side
    /// Reports @p message on stderr and ends the trampoline with @p status. Where the Terminal slot holds a
    /// terminal -- a session's tab, which would otherwise just show its shell gone -- it is reported there
    /// too. Asked of the descriptor rather than of the spec, which may be what could not be read.
    [[noreturn]] void abandon(TrampolineExit status, std::string_view message) noexcept
    {
        std::ignore = util::writeAll(STDERR_FILENO,
                                     util::DescriptorKind::Stream,
                                     std::format("contour: spawn trampoline: {}\n", message));
        if (auto const terminal = trampolineFd(TrampolineSlot::Terminal); ::isatty(terminal) == 1)
            std::ignore = util::writeAll(
                terminal, util::DescriptorKind::Stream, std::format("\r\ncontour: {}\r\n", message));
        ::_exit(static_cast<int>(status));
    }

    /// Reads exactly @p size bytes from @p fd.
    [[nodiscard]] std::optional<std::string> readExactly(int fd, std::size_t size)
    {
        auto bytes = std::string(size, '\0');
        auto filled = std::size_t { 0 };
        while (filled < size)
        {
            auto const got = ::read(fd, bytes.data() + filled, size - filled);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                return std::nullopt;
            filled += static_cast<std::size_t>(got);
        }
        return bytes;
    }

    /// @return The spec the parent wrote to SpawnSpecFd: its length, then that many bytes.
    [[nodiscard]] SpawnSpec readSpec()
    {
        auto const prefix = readExactly(SpawnSpecFd, SpawnSpecLengthSize);
        auto const length =
            prefix ? decodeSpawnSpecLength(*prefix) : std::unexpected { SpawnSpecError::Truncated };
        if (!length)
            abandon(TrampolineExit::SpecUnreadable, "no spawn spec arrived");
        if (*length > MaxSpecLength)
            abandon(TrampolineExit::SpecUnreadable,
                    std::format("a spawn spec of {} bytes is refused", *length));
        auto const payload = readExactly(SpawnSpecFd, *length);
        if (!payload)
            abandon(TrampolineExit::SpecUnreadable, "the spawn spec arrived incomplete");
        auto spec = decodeSpawnSpec(*payload);
        if (!spec)
            abandon(TrampolineExit::SpecUnreadable,
                    std::format("the spawn spec is malformed (error {})", static_cast<int>(spec.error())));
        return std::move(*spec);
    }

    /// @return The slot's descriptor, where the parent handed one over.
    [[nodiscard]] std::optional<int> slotFd(SpawnSpec const& spec, TrampolineSlot slot) noexcept
    {
        if (!spec.inheritedSlots.has(slot))
            return std::nullopt;
        return trampolineFd(slot);
    }

    /// Closes every descriptor above stderr but the slots @p spec names: what the parent leaked on a slot
    /// number it did not fill is no slot.
    void closeStrangers(SpawnSpec const& spec) noexcept
    {
        constexpr auto HighestSlotFd = trampolineFd(static_cast<TrampolineSlot>(TrampolineSlotCount - 1));
        util::closeDescriptorsAbove(HighestSlotFd, util::openDescriptorLimit());
        for (auto const index: std::views::iota(std::uint8_t { 0 }, TrampolineSlotCount))
        {
            auto const slot = static_cast<TrampolineSlot>(index);
            if (!spec.inheritedSlots.has(slot))
                ::close(trampolineFd(slot));
        }
    }

    /// Writes @p value to this process's own oom_score_adj, where there is one; a failure is ignored.
    void writeOwnOomScoreAdjust(std::optional<int> value) noexcept
    {
        if (!value)
            return;
        auto const fd = OwnedFd { ::open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC) };
        if (fd.isOpen())
            std::ignore = util::writeAll(fd.get(), util::DescriptorKind::Stream, std::to_string(*value));
    }

    /// Parks on the gate slot, if there is one, until the parent's placement releases it; then closes it.
    /// @return How the wait ended; Released where there is no gate to wait on.
    [[nodiscard]] GateRelease awaitPlacement(SpawnSpec const& spec) noexcept
    {
        auto const gate = slotFd(spec, TrampolineSlot::Gate);
        if (!gate)
            return GateRelease::Released;
        auto const release = awaitGateRelease(*gate);
        ::close(*gate);
        return release;
    }

    /// @return Pointers to @p strings, then a null pointer: the layout exec() takes.
    [[nodiscard]] std::vector<char*> pointersTo(std::vector<std::string>& strings)
    {
        auto pointers = std::vector<char*> {};
        pointers.reserve(strings.size() + 1);
        for (auto& entry: strings)
            pointers.push_back(entry.data());
        pointers.push_back(nullptr);
        return pointers;
    }

    /// The program's argv and environment, as exec() takes them; backed by the spec.
    struct ProgramImage
    {
        std::string image;
        std::vector<char*> argv;
        std::optional<std::vector<char*>> envp;
    };

    [[nodiscard]] ProgramImage programImage(SpawnSpec& spec)
    {
        auto image = spec.image.empty() ? spec.argv.front() : spec.image;
        auto envp = spec.environment.transform([](auto& environment) { return pointersTo(environment); });
        return ProgramImage { .image = std::move(image),
                              .argv = pointersTo(spec.argv),
                              .envp = std::move(envp) };
    }

    /// Execs @p program as @p lookup says, under its environment (or this process's).
    /// @return The errno of the failure; exec() returns only on one.
    [[nodiscard]] int execute(ProgramImage& program, ExecLookup lookup) noexcept
    {
        if (program.envp)
            environ = program.envp->data();
        switch (lookup)
        {
            case ExecLookup::ExactPath: ::execv(program.image.c_str(), program.argv.data()); break;
            case ExecLookup::SearchPath: ::execvp(program.image.c_str(), program.argv.data()); break;
        }
        return errno;
    }

    /// @return The status a trampoline ends with when the program cannot run.
    [[nodiscard]] constexpr int failureStatus(FailureExit exit) noexcept
    {
        switch (exit)
        {
            case FailureExit::Failure: return EXIT_FAILURE;
            case FailureExit::NotFound: return 127;
        }
        return EXIT_FAILURE;
    }

    /// Writes @p text onto the program's terminal, where failures are reported there.
    void report(TerminalLaunch const& launch, std::string_view text) noexcept
    {
        if (launch.failureReport == FailureReport::OnTerminal)
            std::ignore = util::writeAll(STDOUT_FILENO, util::DescriptorKind::Stream, text);
    }

    /// @return The text for @p error.
    [[nodiscard]] std::string errorText(int error)
    {
        auto buffer = std::array<char, 256> {};
        return std::string { errnoText(error, buffer) };
    }

    /// The program on a terminal, as a session's shell: Process::start()'s child.
    [[noreturn]] void runOnTerminal(SpawnSpec& spec, TerminalLaunch& launch)
    {
        writeOwnOomScoreAdjust(spec.oomScoreAdjust);
        // A gate closed without the byte lets the program run all the same: spawnPlaced() hands every
        // session's trampoline to a ParkedChild, which sends it, so only a parent gone before that closes the
        // gate silently -- and its terminal's master went with it.
        std::ignore = awaitPlacement(spec);

        if (auto const terminal = slotFd(spec, TrampolineSlot::Terminal))
        {
            if (launch.terminalSettings == TerminalSettings::Configure)
                util::configureTerminal(*terminal);
            // A terminal it cannot make the controlling one is still the program's standard streams, and
            // its best chance at one: loginTerminal() puts them on it either way, so the failure is ignored.
            std::ignore = util::loginTerminal(*terminal, launch.controllingTerminal);
            ::close(*terminal);
        }

        // Entered before the settle is said: until then the parent names the directory the child is about to
        // enter (Process::workingDirectory()), and from then on the child's own, which is this one already.
        if (!launch.workingDirectory.empty() && ::chdir(launch.workingDirectory.c_str()) != 0)
        {
            auto const failure = errno;
            report(
                launch,
                std::format("Failed to chdir to \"{}\". {}\n", launch.workingDirectory, errorText(failure)));
            ::_exit(failureStatus(launch.failureExit));
        }

        // Its terminal's queues are flushed: what the parent writes from now on reaches the program. Said
        // with a byte, not only the close: a helper another thread of the parent forks holds the parent's
        // copy of this end on, and would keep the parent's end from reading end-of-file for as long as it
        // lives.
        if (auto const settle = slotFd(spec, TrampolineSlot::Settle))
        {
            std::ignore = ::write(*settle, "s", 1);
            ::close(*settle);
        }

        auto keepFd = STDERR_FILENO;
        if (auto const fastPipe = slotFd(spec, TrampolineSlot::FastPipe))
        {
            util::saveDup2(*fastPipe, StdoutFastPipeFd);
            ::close(*fastPipe);
            keepFd = StdoutFastPipeFd;
        }
        util::closeDescriptorsAbove(keepFd, util::openDescriptorLimit());

        // Reset signal(s) to default that may have been changed in the parent process.
        ::signal(SIGPIPE, SIG_DFL);

        auto program = programImage(spec);
        auto failure = execute(program, spec.lookup);

        // Fallback: Try login shell.
        report(launch,
               std::format("\r\033[31;1mFailed to spawn {}\033[m\r\nTrying login shell: {}\n",
                           program.image,
                           core::joinHumanReadableQuoted(launch.fallbackArgv, " ")));
        if (!launch.fallbackArgv.empty())
        {
            auto fallbackArgv = pointersTo(launch.fallbackArgv);
            ::execvp(fallbackArgv[0], fallbackArgv.data());
            failure = errno;
        }

        // Bad luck.
        report(launch, std::format("\r\nOut of luck. {}\r\n\n", errorText(failure)));
        ::_exit(failureStatus(launch.failureExit));
    }

    /// The program on its own, in a session of its own: startDetached()'s two levels, with this process as
    /// the intermediate one.
    [[noreturn]] void runDetached(SpawnSpec& spec)
    {
        // A session of its own, the program's process, and gone: the program is left to init, or to the
        // nearest subreaper, to wait for -- not to the parent.
        ::setsid();
        auto const program = ::fork();
        auto const forkError = errno;
        if (program == 0)
        {
            // Parked first, as a session's shell is. @see ProcessPlacement. A gate closed without the byte is
            // a parent that gave up on the program -- its pid never reached it, so the caller was told the
            // spawn failed (awaitDetachedProgram()) -- and the program never runs, rather than run unplaced
            // and unknown to it.
            writeOwnOomScoreAdjust(spec.oomScoreAdjust);
            if (awaitPlacement(spec) == GateRelease::Abandoned)
                ::_exit(127);
            if (auto const input = slotFd(spec, TrampolineSlot::Input))
                util::saveDup2(*input, STDIN_FILENO);
            if (auto const output = slotFd(spec, TrampolineSlot::Output))
            {
                util::saveDup2(*output, STDOUT_FILENO);
                util::saveDup2(*output, STDERR_FILENO);
            }
            util::closeDescriptorsAbove(STDERR_FILENO, util::openDescriptorLimit());
            // Neither what the caller ignores nor what its thread blocks -- the daemon blocks SIGINT and
            // SIGTERM -- carries over into the program, as exec() would carry both.
            ::signal(SIGPIPE, SIG_DFL);
            auto noSignals = sigset_t {};
            sigemptyset(&noSignals);
            ::pthread_sigmask(SIG_SETMASK, &noSignals, nullptr);
            auto image = programImage(spec);
            std::ignore = execute(image, spec.lookup);
            ::_exit(127);
        }
        // Which process to place travels over the gate: written into the end the program waits on, it arrives
        // at the end the parent holds. A failed fork sends its errno, negated.
        if (auto const gate = slotFd(spec, TrampolineSlot::Gate))
        {
            auto const placed =
                std::bit_cast<std::array<char, sizeof(pid_t)>>(program > 0 ? program : -forkError);
            std::ignore = util::writeAll(
                *gate, util::DescriptorKind::Socket, std::string_view { placed.data(), placed.size() });
        }
        ::_exit(program > 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    /// Runs this process as the trampoline: reads the spec, and starts its program.
    [[noreturn]] void runTrampoline()
    {
#ifdef __linux__
        // Named as its argv[0] says rather than the number of the /proc/self/fd entry it was spawned from, so
        // that ps, top and the OOM killer's report say what the parked process is.
        auto const name = std::string { TrampolineProgramName };
        ::prctl(PR_SET_NAME, name.c_str());
#endif
        // posix_spawn() reset SIGPIPE to its default, where a child forked from the GUI or the daemon kept
        // their SIG_IGN until just before its exec. Ignored again until then: a write to a channel whose
        // parent end has gone -- the settle byte, the detached program's pid -- fails with EPIPE, as it did,
        // rather than ending the trampoline before the program runs. Each mode restores the default for
        // the program.
        ::signal(SIGPIPE, SIG_IGN);
        auto spec = readSpec();
        ::close(SpawnSpecFd);
        closeStrangers(spec);
        std::visit(core::Overloaded {
                       [&](TerminalLaunch& launch) { runOnTerminal(spec, launch); },
                       [&](DetachedLaunch const&) { runDetached(spec); },
                   },
                   spec.launch);
        abandon(TrampolineExit::SpecUnreadable, "the spawn spec names no known mode");
    }
    // }}}
} // namespace

void runSpawnTrampolineIfRequested(int argc, char const* const* argv) noexcept
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    switch (classifyInvocation(arguments))
    {
        case TrampolineInvocation::NotRequested: return;
        case TrampolineInvocation::UnsupportedVersion:
            abandon(TrampolineExit::UnsupportedVersion,
                    std::format("Contour was updated while it was running, and must be restarted to start "
                                "new programs: it asked for {}, which this executable, speaking {}{}, does "
                                "not understand.",
                                arguments[1],
                                SpawnTrampolineMarker,
                                SpawnTrampolineProtocolVersion));
        case TrampolineInvocation::Supported: runTrampoline();
    }
}

std::expected<pid_t, std::error_code> spawnTrampoline(SpawnSpec spec,
                                                      std::span<InheritedFd const> fds,
                                                      TrampolineEnvironment environment)
{
    return selfExecutablePath().and_then(
        [&](std::string const& self) -> std::expected<pid_t, std::error_code> {
#ifdef __linux__
            // Through a descriptor of the running image rather than the /proc/self/exe link itself: the same
            // file, replaced or removed on disk or not. valgrind runs this process from its own tool, which
            // an exec of the link starts ("You cannot run 'contour-spawn' directly"), but it opens the link
            // onto the image it runs. O_PATH, as an image may be executable without being readable; kept open
            // until posix_spawn() returns, by which time the trampoline's exec has resolved it. Numbered
            // above every slot, as the copies are: opened on the lowest free number, it may be a slot's,
            // which the spawn dup2()s the slot onto before the exec reads the path.
            auto const opened = OwnedFd { ::open(self.c_str(), O_PATH | O_CLOEXEC) };
            if (!opened.isOpen())
                return std::unexpected(util::lastError());
            auto const image = OwnedFd { ::fcntl(opened.get(), F_DUPFD_CLOEXEC, FirstTemporaryFd) };
            if (!image.isOpen())
                return std::unexpected(util::lastError());
            return spawnTrampolineImage(
                std::format("/proc/self/fd/{}", image.get()), std::move(spec), fds, environment);
#else
            return spawnTrampolineImage(self, std::move(spec), fds, environment);
#endif
        });
}

std::expected<pid_t, std::error_code> spawnTrampolineImage(std::string const& image,
                                                           SpawnSpec spec,
                                                           std::span<InheritedFd const> fds,
                                                           TrampolineEnvironment environment)
{
    auto const invalid = std::unexpected(std::make_error_code(std::errc::invalid_argument));
    spec.inheritedSlots = TrampolineSlots {};
    for (auto const& inherited: fds)
    {
        if (spec.inheritedSlots.has(inherited.slot))
            return invalid;
        spec.inheritedSlots = spec.inheritedSlots.with(inherited.slot);
    }
    // Refused here, where the caller hears why, rather than by a trampoline that could only exit.
    auto const required = std::visit([](auto const& launch) { return requiredSlots(launch); }, spec.launch);
    if (spec.argv.empty() || (spec.inheritedSlots.raw() & required.raw()) != required.raw()
        || (environment == TrampolineEnvironment::Program && !spec.environment))
        return invalid;
    auto const frame = encodeFramedSpawnSpec(spec);

    auto channel = makeGate();
    if (!channel)
        return std::unexpected(channel.error());

    // Every source is copied above the slot numbers first, and the slots are filled from the copies: a
    // source sitting on another slot's number is then never overwritten before it is read.
    auto mappings = std::vector<SlotMapping> {};
    mappings.reserve(fds.size() + 1);
    auto mapped = mapping(channel->childEnd.get(), SpawnSpecFd);
    if (!mapped)
        return std::unexpected(mapped.error());
    mappings.push_back(std::move(*mapped));
    for (auto const& inherited: fds)
    {
        mapped = mapping(inherited.fd, trampolineFd(inherited.slot));
        if (!mapped)
            return std::unexpected(mapped.error());
        mappings.push_back(std::move(*mapped));
    }

    auto actions = SpawnFileActions::create();
    if (!actions)
        return std::unexpected(actions.error());
    for (auto const& [copy, target]: mappings)
        if (auto const error = ::posix_spawn_file_actions_adddup2(actions->get(), copy.get(), target);
            error != 0)
            return std::unexpected(spawnError(error));

    // Not the trampoline's to inherit, nor its program's.
    auto attributes = SpawnAttributes::create();
    if (!attributes)
        return std::unexpected(attributes.error());
    if (auto const reset = resetSignals(*attributes); !reset)
        return std::unexpected(reset.error());

    auto arguments = std::array {
        std::string { TrampolineProgramName },
        std::format("{}{}", SpawnTrampolineMarker, SpawnTrampolineProtocolVersion),
    };
    auto argv = std::array<char*, 3> { arguments[0].data(), arguments[1].data(), nullptr };

    // Under the program's environment, the spawn reads nothing of this process's, which another thread may
    // be changing (DetachedCommand::environment).
    auto envp = environment == TrampolineEnvironment::Program
                    ? std::optional { pointersTo(*spec.environment) }
                    : std::nullopt;

    auto pid = pid_t {};
    auto const spawned = ::posix_spawn(
        &pid, image.c_str(), actions->get(), attributes->get(), argv.data(), envp ? envp->data() : environ);
    mappings.clear();
    channel->childEnd.reset();
    if (spawned != 0)
        return std::unexpected(spawnError(spawned));

    // Written after the spawn, not before: a spec larger than the socket's buffer is read as it is written.
    if (auto sent = util::writeAll(channel->parentEnd.get(), util::DescriptorKind::Socket, frame); !sent)
    {
        channel->parentEnd.reset(); // end-of-file, for a trampoline still reading
        std::ignore = util::waitForExit(pid);
        return std::unexpected(sent.error());
    }
    return pid;
}

std::expected<pid_t, std::error_code> spawnPlaced(SpawnSpec spec,
                                                  std::span<InheritedFd const> fds,
                                                  ProcessPlacement& placement,
                                                  ProcessRole role,
                                                  TrampolineEnvironment environment)
{
    // The gate parks the program between its spawn and its exec() until the placement has moved it into its
    // own resource domain, so everything it forks from then on is born there too. @see ProcessPlacement.
    auto gate = makeGate();
    if (!gate)
        return std::unexpected(gate.error());
    spec.oomScoreAdjust = placement.childOomScoreAdjust();
    auto const detached = std::holds_alternative<DetachedLaunch>(spec.launch);
    auto withGate = std::vector<InheritedFd>(fds.begin(), fds.end());
    withGate.push_back({ .slot = TrampolineSlot::Gate, .fd = gate->childEnd.get() });

    auto const trampoline = spawnTrampoline(std::move(spec), withGate, environment);
    if (!trampoline)
        return std::unexpected(trampoline.error());
    gate->childEnd.reset();

    // A detached program's trampoline starts a session of its own, forks the program, tells the gate the
    // program's pid, and exits: the program is left to init, or to the nearest subreaper, to wait for -- not
    // to this process. Not this process's child, yet its pid still names it: it waits on the gate, and only a
    // signal from elsewhere could end it before the pidfd is opened.
    auto const program = detached ? awaitDetachedProgram(*trampoline, gate->parentEnd.get())
                                  : std::expected<pid_t, std::error_code> { *trampoline };
    if (!program)
        return std::unexpected(program.error());
    placement.placeThenRelease(
        ParkedChild { *program, openPidFd(*program), std::move(gate->parentEnd), role });
    return *program;
}

std::expected<TerminalProgram, std::error_code> spawnOnNewTerminal(SpawnSpec spec,
                                                                   ProcessPlacement& placement,
                                                                   ProcessRole role)
{
    if (!std::holds_alternative<TerminalLaunch>(spec.launch))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    auto masterFd = -1;
    auto slaveFd = -1;
    if (::openpty(&masterFd, &slaveFd, nullptr, nullptr, nullptr) < 0)
        return std::unexpected(util::lastError());
    auto master = OwnedFd { masterFd };
    auto const slave = OwnedFd { slaveFd };
    if (auto const closing =
            util::setCloseOnExec(master.get()).and_then([&] { return util::setCloseOnExec(slave.get()); });
        !closing)
        return std::unexpected(closing.error());

    auto const fds = std::array { InheritedFd { .slot = TrampolineSlot::Terminal, .fd = slave.get() } };
    return spawnPlaced(std::move(spec), fds, placement, role, TrampolineEnvironment::Inherited)
        .transform([&](pid_t pid) { return TerminalProgram { .pid = pid, .master = std::move(master) }; });
}

} // namespace vtpty
