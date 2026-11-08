// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ErrnoText.hpp>
#include <vtpty/OwnedFd.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/SpawnTrampoline.hpp>
#include <vtpty/UnixUtils.hpp>

#include <array>
#include <bit>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifdef __APPLE__
    #include <mach-o/dyld.h>
#endif

#ifdef __FreeBSD__
    #include <sys/sysctl.h>
#endif

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <spawn.h>
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

    /// Where a session's program finds the writer of its stdout fast pipe.
    constexpr auto StdoutFastPipeFd = 3;

    /// The size of the length that precedes the encoded spec on the spec socket.
    constexpr auto SpecLengthSize = sizeof(std::uint32_t);

    /// The longest spec the trampoline accepts; a length above it is garbage, not a spec.
    constexpr auto MaxSpecLength = std::uint32_t { 256U * 1024U * 1024U };

// {{{ parent side
#ifdef MSG_NOSIGNAL
    constexpr auto NoSigPipeSendFlags = MSG_NOSIGNAL;
#else
    constexpr auto NoSigPipeSendFlags = 0; // makeGate() set SO_NOSIGPIPE on the socket instead
#endif

    /// @return @p payload preceded by its length, a little-endian u32: what the trampoline reads.
    [[nodiscard]] std::string framed(std::string const& payload)
    {
        auto const length = static_cast<std::uint32_t>(payload.size());
        auto frame = std::string {};
        frame.reserve(SpecLengthSize + payload.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, SpecLengthSize))
            frame.push_back(static_cast<char>((length >> (8U * index)) & 0xFFU));
        frame += payload;
        return frame;
    }

    /// Sends @p bytes in full on @p socket, which raises no SIGPIPE.
    [[nodiscard]] std::expected<void, std::error_code> sendAll(int socket, std::string_view bytes) noexcept
    {
        while (!bytes.empty())
        {
            auto const sent = ::send(socket, bytes.data(), bytes.size(), NoSigPipeSendFlags);
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent <= 0)
                return std::unexpected(sent < 0 ? util::lastError()
                                                : std::make_error_code(std::errc::broken_pipe));
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
        return {};
    }

    /// posix_spawn_file_actions_t, destroyed when it goes.
    class FileActions
    {
      public:
        FileActions() noexcept: _error { ::posix_spawn_file_actions_init(&_actions) } {}
        ~FileActions()
        {
            if (_error == 0)
                ::posix_spawn_file_actions_destroy(&_actions);
        }
        FileActions(FileActions const&) = delete;
        FileActions& operator=(FileActions const&) = delete;
        FileActions(FileActions&&) = delete;
        FileActions& operator=(FileActions&&) = delete;

        /// @return The error init reported, or 0.
        [[nodiscard]] int error() const noexcept { return _error; }
        [[nodiscard]] posix_spawn_file_actions_t* get() noexcept { return &_actions; }

      private:
        posix_spawn_file_actions_t _actions {};
        int _error;
    };

    /// posix_spawnattr_t, destroyed when it goes.
    class SpawnAttributes
    {
      public:
        SpawnAttributes() noexcept: _error { ::posix_spawnattr_init(&_attributes) } {}
        ~SpawnAttributes()
        {
            if (_error == 0)
                ::posix_spawnattr_destroy(&_attributes);
        }
        SpawnAttributes(SpawnAttributes const&) = delete;
        SpawnAttributes& operator=(SpawnAttributes const&) = delete;
        SpawnAttributes(SpawnAttributes&&) = delete;
        SpawnAttributes& operator=(SpawnAttributes&&) = delete;

        /// @return The error init reported, or 0.
        [[nodiscard]] int error() const noexcept { return _error; }
        [[nodiscard]] posix_spawnattr_t* get() noexcept { return &_attributes; }

      private:
        posix_spawnattr_t _attributes {};
        int _error;
    };

    /// @return @p error, a posix_spawn*() result, as an error code.
    [[nodiscard]] std::error_code spawnError(int error) noexcept
    {
        return std::error_code { error, std::generic_category() };
    }

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

    /// @return The slots a spec of @p mode cannot do without.
    [[nodiscard]] constexpr TrampolineSlots requiredSlots(SpawnMode mode) noexcept
    {
        switch (mode)
        {
            // Without its terminal the program would stay in this process's session, on its stdio.
            case SpawnMode::Terminal: return TrampolineSlots {}.with(TrampolineSlot::Terminal);
            case SpawnMode::Detached: return TrampolineSlots {};
        }
        return TrampolineSlots {};
    }

    /// Waits for @p pid to end, retrying on EINTR.
    void reap(pid_t pid) noexcept
    {
        auto status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    // }}}

    // {{{ trampoline side
    /// Writes @p text to @p fd in full, retrying on short writes and EINTR.
    void writeAll(int fd, std::string_view text) noexcept
    {
        while (!text.empty())
        {
            auto const written = ::write(fd, text.data(), text.size());
            if (written > 0)
                text.remove_prefix(static_cast<std::size_t>(written));
            else if (written < 0 && errno == EINTR)
                continue;
            else
                break;
        }
    }

    /// Reports @p message on stderr and ends the trampoline with @p status.
    [[noreturn]] void abandon(TrampolineExit status, std::string_view message) noexcept
    {
        writeAll(STDERR_FILENO, std::format("contour: spawn trampoline: {}\n", message));
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
        auto const prefix = readExactly(SpawnSpecFd, SpecLengthSize);
        if (!prefix)
            abandon(TrampolineExit::SpecUnreadable, "no spawn spec arrived");
        auto length = std::uint32_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, SpecLengthSize))
            length |=
                static_cast<std::uint32_t>(static_cast<unsigned char>((*prefix)[index])) << (8U * index);
        if (length > MaxSpecLength)
            abandon(TrampolineExit::SpecUnreadable,
                    std::format("a spawn spec of {} bytes is refused", length));
        auto const payload = readExactly(SpawnSpecFd, length);
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
            writeAll(fd.get(), std::to_string(*value));
    }

    /// Parks on the gate slot, if there is one, until the parent's placement releases it; then closes it.
    void awaitPlacement(SpawnSpec const& spec) noexcept
    {
        if (auto const gate = slotFd(spec, TrampolineSlot::Gate))
        {
            awaitGateRelease(*gate);
            ::close(*gate);
        }
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
    [[nodiscard]] constexpr int failureStatus(FailureReport report) noexcept
    {
        switch (report)
        {
            case FailureReport::Silent: return 127;
            case FailureReport::OnTerminal: return EXIT_FAILURE;
        }
        return EXIT_FAILURE;
    }

    /// Writes @p text onto the program's terminal, where failures are reported there.
    void report(SpawnSpec const& spec, std::string_view text) noexcept
    {
        if (spec.failureReport == FailureReport::OnTerminal)
            writeAll(STDOUT_FILENO, text);
    }

    /// @return The text for @p error.
    [[nodiscard]] std::string errorText(int error)
    {
        auto buffer = std::array<char, 256> {};
        return std::string { errnoText(error, buffer) };
    }

    /// The program on a terminal, as a session's shell: Process::start()'s child.
    [[noreturn]] void runOnTerminal(SpawnSpec& spec)
    {
        writeOwnOomScoreAdjust(spec.oomScoreAdjust);
        awaitPlacement(spec);

        if (auto const terminal = slotFd(spec, TrampolineSlot::Terminal))
        {
            if (spec.terminalSettings == TerminalSettings::Configure)
                util::applyTerminalSettings(*terminal, util::constructTerminalSettings(*terminal));
            // A terminal it cannot make the controlling one is still the program's best chance at one.
            std::ignore = util::loginTerminal(*terminal, spec.controllingTerminal);
            ::close(*terminal);
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

        if (!spec.workingDirectory.empty() && ::chdir(spec.workingDirectory.c_str()) != 0)
        {
            auto const failure = errno;
            report(spec,
                   std::format("Failed to chdir to \"{}\". {}\n", spec.workingDirectory, errorText(failure)));
            ::_exit(failureStatus(spec.failureReport));
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
        report(spec,
               std::format("\r\033[31;1mFailed to spawn {}\033[m\r\nTrying login shell: {}\n",
                           program.image,
                           spec.fallbackText));
        if (!spec.fallbackArgv.empty())
        {
            auto fallbackArgv = pointersTo(spec.fallbackArgv);
            ::execvp(fallbackArgv[0], fallbackArgv.data());
            failure = errno;
        }

        // Bad luck.
        report(spec, std::format("\r\nOut of luck. {}\r\n\n", errorText(failure)));
        ::_exit(failureStatus(spec.failureReport));
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
            // Parked first, as a session's shell is. @see ProcessPlacement.
            writeOwnOomScoreAdjust(spec.oomScoreAdjust);
            awaitPlacement(spec);
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
            writeAll(*gate, std::string_view { placed.data(), placed.size() });
        }
        ::_exit(program > 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    /// Runs this process as the trampoline: reads the spec, and starts its program.
    [[noreturn]] void runTrampoline()
    {
        // posix_spawn() reset SIGPIPE to its default, where a child forked from the GUI or the daemon kept
        // their SIG_IGN until just before its exec. Ignored again until then: a write to a channel whose
        // parent end has gone -- the settle byte, the detached program's pid -- fails with EPIPE, as it did,
        // rather than ending the trampoline before the program runs. Each mode restores the default for
        // the program.
        ::signal(SIGPIPE, SIG_IGN);
        auto spec = readSpec();
        ::close(SpawnSpecFd);
        closeStrangers(spec);
        switch (spec.mode)
        {
            case SpawnMode::Terminal: runOnTerminal(spec);
            case SpawnMode::Detached: runDetached(spec);
        }
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
                    std::format("{} is not a protocol this executable speaks ({}{}); was it replaced by "
                                "another version while running? Restart it.",
                                arguments[1],
                                SpawnTrampolineMarker,
                                SpawnTrampolineProtocolVersion));
        case TrampolineInvocation::Supported: runTrampoline();
    }
}

void runSpawnTrampolineIfRequested(int argc, char* const* argv) noexcept
{
    runSpawnTrampolineIfRequested(argc, static_cast<char const* const*>(argv));
}

std::expected<std::string, std::error_code> selfExecutablePath()
{
#ifdef __linux__
    // Resolved by the spawned process, which still runs this image: the running file, even where an upgrade
    // replaced or removed it on disk.
    return std::string { "/proc/self/exe" };
#elifdef __APPLE__
    auto size = std::uint32_t { 0 };
    std::ignore = ::_NSGetExecutablePath(nullptr, &size);
    auto path = std::string(size, '\0');
    if (::_NSGetExecutablePath(path.data(), &size) != 0)
        return std::unexpected(std::make_error_code(std::errc::filename_too_long));
    path.resize(std::string_view { path.c_str() }.size());
    auto error = std::error_code {};
    auto canonical = std::filesystem::canonical(path, error);
    if (error)
        return std::unexpected(error);
    return canonical.string();
#elifdef __FreeBSD__
    auto name = std::array { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    auto size = std::size_t { 0 };
    if (::sysctl(name.data(), name.size(), nullptr, &size, nullptr, 0) != 0)
        return std::unexpected(util::lastError());
    auto path = std::string(size, '\0');
    if (::sysctl(name.data(), name.size(), path.data(), &size, nullptr, 0) != 0)
        return std::unexpected(util::lastError());
    path.resize(std::string_view { path.c_str() }.size());
    return path;
#else
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
#endif
}

std::expected<pid_t, std::error_code> spawnTrampoline(SpawnSpec spec, std::span<InheritedFd const> fds)
{
    return selfExecutablePath().and_then(
        [&](std::string const& self) { return spawnTrampolineImage(self, std::move(spec), fds); });
}

std::expected<pid_t, std::error_code> spawnTrampolineImage(std::string const& image,
                                                           SpawnSpec spec,
                                                           std::span<InheritedFd const> fds)
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
    auto const required = requiredSlots(spec.mode);
    if (spec.argv.empty() || (spec.inheritedSlots.raw() & required.raw()) != required.raw())
        return invalid;
    auto const frame = framed(encodeSpawnSpec(spec));

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

    auto actions = FileActions {};
    if (actions.error() != 0)
        return std::unexpected(spawnError(actions.error()));
    for (auto const& [copy, target]: mappings)
        if (auto const error = ::posix_spawn_file_actions_adddup2(actions.get(), copy.get(), target);
            error != 0)
            return std::unexpected(spawnError(error));

    // Whatever this process blocks or ignores (a daemon blocks its shutdown signals, a GUI ignores SIGPIPE)
    // is not the trampoline's to inherit, nor its program's.
    auto attributes = SpawnAttributes {};
    if (attributes.error() != 0)
        return std::unexpected(spawnError(attributes.error()));
    auto none = sigset_t {};
    sigemptyset(&none);
    auto catchable = sigset_t {};
    sigfillset(&catchable);
    sigdelset(&catchable, SIGKILL);
    sigdelset(&catchable, SIGSTOP);
    ::posix_spawnattr_setsigmask(attributes.get(), &none);
    ::posix_spawnattr_setsigdefault(attributes.get(), &catchable);
    ::posix_spawnattr_setflags(attributes.get(), POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    auto arguments = std::array {
        std::string { TrampolineProgramName },
        std::format("{}{}", SpawnTrampolineMarker, SpawnTrampolineProtocolVersion),
    };
    auto argv = std::array<char*, 3> { arguments[0].data(), arguments[1].data(), nullptr };

    auto pid = pid_t {};
    auto const spawned =
        ::posix_spawn(&pid, image.c_str(), actions.get(), attributes.get(), argv.data(), environ);
    mappings.clear();
    channel->childEnd.reset();
    if (spawned != 0)
        return std::unexpected(spawnError(spawned));

    // Written after the spawn, not before: a spec larger than the socket's buffer is read as it is written.
    if (auto sent = sendAll(channel->parentEnd.get(), frame); !sent)
    {
        channel->parentEnd.reset(); // end-of-file, for a trampoline still reading
        reap(pid);
        return std::unexpected(sent.error());
    }
    return pid;
}

} // namespace vtpty
