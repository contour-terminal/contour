// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/PosixPorts.hpp>

#include <vtpty/UnixUtils.hpp>

#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <deque>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <vthost/keeper/DescriptorSocket.hpp>
#include <vthost/keeper/Frame.hpp>
#include <vthost/keeper/Kqueue.hpp>

#ifdef __linux__
    #include <sys/prctl.h>
    #include <sys/signalfd.h>
    #include <sys/syscall.h>
#endif

namespace vthost::keeper
{

namespace
{
    // {{{ the process's limits and signals

    /// The most bytes a channel queues for a daemon that does not read before it is dropped: four frames of
    /// the largest size, more than any burst the keeper sends (an inventory) needs.
    constexpr auto MaxQueuedBytes = 4 * MaxFrameBodySize;

    /// The most reads one wait() makes from a live daemon's channel, so that a flood cannot starve the rest.
    constexpr auto MaxReadsPerWait = 16;

    /// How large one read from a channel is.
    constexpr auto ReadSize = std::size_t { 64 } * 1024;

    /// The status a forked child ends with when it could not exec.
    constexpr auto ExecFailedStatus = 127;

    /// What a signal the keeper receives means to it.
    enum class SignalMeaning : uint8_t
    {
        Stop = 0,   ///< End: forwarded to the daemon, which shuts down.
        ChildEnded, ///< A child changed state: look at the processes that have no process handle.
    };

    struct ReceivedSignal
    {
        int number;
        SignalMeaning meaning;
    };

    /// The signals the keeper receives through its event source. SIGHUP is a stop, as for a `contour daemon`
    /// in the foreground today: the terminal it was started from went away.
    constexpr auto ReceivedSignals = std::array {
        ReceivedSignal { .number = SIGTERM, .meaning = SignalMeaning::Stop },
        ReceivedSignal { .number = SIGINT, .meaning = SignalMeaning::Stop },
        ReceivedSignal { .number = SIGHUP, .meaning = SignalMeaning::Stop },
        ReceivedSignal { .number = SIGCHLD, .meaning = SignalMeaning::ChildEnded },
    };

    /// The platform's signal for each KeeperSignal, in the enum's order.
    constexpr auto SignalNumbers = std::array { SIGTERM, SIGKILL, SIGABRT, SIGCONT };

    [[nodiscard]] std::error_code lastError() noexcept
    {
        return { errno, std::system_category() };
    }

    /// Marks @p fd close-on-exec, where it could not be opened so.
    void setCloseOnExec(int fd) noexcept
    {
        ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }

    /// @return The descriptors this process has open, counted from its descriptor listing; or by asking for
    ///         every number below @p limit where there is no listing.
    [[nodiscard]] std::size_t countOpenDescriptors(rlim_t limit)
    {
#ifdef __linux__
        auto const listing = std::filesystem::path { "/proc/self/fd" };
#else
        auto const listing = std::filesystem::path { "/dev/fd" };
#endif
        auto error = std::error_code {};
        auto entries = std::filesystem::directory_iterator { listing, error };
        if (!error)
        {
            // Less the one the listing itself holds open.
            auto const count =
                static_cast<std::size_t>(std::distance(entries, std::filesystem::directory_iterator {}));
            return count > 0 ? count - 1 : 0;
        }
        constexpr auto SweepBound = rlim_t { 65'536 };
        auto const bound = static_cast<int>(std::min(limit, SweepBound));
        return static_cast<std::size_t>(std::ranges::count_if(
            std::views::iota(0, bound), [](int fd) { return ::fcntl(fd, F_GETFD) != -1; }));
    }

    // }}}
    // {{{ the descriptor budget

    /// Raises the soft descriptor limit to the hard one while it lives, and counts what is left of it.
    class PosixDescriptorBudget final: public DescriptorBudget
    {
      public:
        PosixDescriptorBudget() noexcept: _original { currentLimit() }
        {
            auto raised = _original;
            raised.rlim_cur = raised.rlim_max;
#ifdef __APPLE__
            // macOS refuses a soft limit above OPEN_MAX, however high the hard one.
            raised.rlim_cur = std::min<rlim_t>(raised.rlim_cur, OPEN_MAX);
#endif
            if (::setrlimit(RLIMIT_NOFILE, &raised) == 0)
                _current = raised.rlim_cur;
        }

        PosixDescriptorBudget(PosixDescriptorBudget const&) = delete;
        PosixDescriptorBudget& operator=(PosixDescriptorBudget const&) = delete;
        PosixDescriptorBudget(PosixDescriptorBudget&&) = delete;
        PosixDescriptorBudget& operator=(PosixDescriptorBudget&&) = delete;

        ~PosixDescriptorBudget() override { ::setrlimit(RLIMIT_NOFILE, &_original); }

        [[nodiscard]] std::size_t available() const override
        {
            auto const open = countOpenDescriptors(_current);
            return _current > open ? static_cast<std::size_t>(_current - open) : 0;
        }

        /// @return The limit this process started with, which every child is given back.
        [[nodiscard]] rlimit const& original() const noexcept { return _original; }

      private:
        [[nodiscard]] static rlimit currentLimit() noexcept
        {
            auto limit = rlimit {};
            ::getrlimit(RLIMIT_NOFILE, &limit);
            return limit;
        }

        rlimit _original;
        rlim_t _current = _original.rlim_cur;
    };

    // }}}
    // {{{ forking a child

    /// Whether a forked child ends when the keeper does.
    enum class ParentDeath : uint8_t
    {
        Survive = 0, ///< A shell: it outlives the keeper's own end only as long as its PTY does.
        Terminate,   ///< The daemon: SIGTERM on the keeper's death (PR_SET_PDEATHSIG), Linux only.
    };

    /// What a forked child needs to become its program; all of it prepared before the fork, so that the
    /// child calls nothing but system calls.
    struct ChildPlan
    {
        char const* path = nullptr;   ///< The program, for execve().
        int executable = -1;          ///< The program, opened O_PATH, for execveat() (Linux).
        char* const* argv = nullptr;  ///< Null-terminated.
        char* const* envp = nullptr;  ///< Null-terminated.
        std::span<int const> sources; ///< This process's descriptors...
        std::span<int const> targets; ///< ...and the numbers they get in the child.
        std::span<int> moved;         ///< Scratch, one per source.
        int errorPipe = -1;           ///< The child writes its errno here when the exec fails.
        int descriptorLimit = 0;      ///< One above the highest descriptor that may be open.
        rlimit limit {};              ///< The descriptor limit the child gets.
        ParentDeath parentDeath = ParentDeath::Survive;
        pid_t parent = 0;
    };

    /// Reports @p error through the plan's error pipe and ends the child.
    [[noreturn]] void failChild(int errorPipe, int error) noexcept
    {
        std::ignore = ::write(errorPipe, &error, sizeof(error));
        ::_exit(ExecFailedStatus);
    }

    /// In the forked child: resets what it inherited and must not, installs its descriptor table, execs.
    [[noreturn]] void runChild(ChildPlan const& plan) noexcept
    {
        // A clean signal state: the keeper blocks the signals it reads, and whatever spawned the keeper may
        // have ignored others.
        // Dispositions first, as posix_spawn does: a signal the mask held back must not meet a handler
        // the keeper's image no longer has once it is unblocked.
        for (auto const signal: std::views::iota(1, NSIG))
            if (signal != SIGKILL && signal != SIGSTOP)
                ::signal(signal, SIG_DFL);
        auto none = sigset_t {};
        sigemptyset(&none);
        ::pthread_sigmask(SIG_SETMASK, &none, nullptr);

#ifdef __linux__
        if (plan.parentDeath == ParentDeath::Terminate)
        {
            // Valid because the keeper is single-threaded: the signal follows the forking thread's end.
            ::prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (::getppid() != plan.parent)
                ::_exit(ExecFailedStatus);
        }
#endif

        // Every source, and the two descriptors the child keeps for itself, move above the highest target
        // first, so that installing one target never overwrites a source still to be installed.
        auto const top = plan.targets.empty() ? -1 : std::ranges::max(plan.targets);
        auto const errorSlot = top + 1;
        auto const executableSlot = top + 2;
        auto const floor = top + 3;
        auto const errorPipe = ::fcntl(plan.errorPipe, F_DUPFD_CLOEXEC, floor);
        if (errorPipe < 0)
            failChild(plan.errorPipe, errno);
        auto const executable = plan.executable >= 0 ? ::fcntl(plan.executable, F_DUPFD_CLOEXEC, floor) : -1;
        if (plan.executable >= 0 && executable < 0)
            failChild(errorPipe, errno);
        for (auto&& [source, moved]: std::views::zip(plan.sources, plan.moved))
        {
            moved = ::fcntl(source, F_DUPFD_CLOEXEC, floor);
            if (moved < 0)
                failChild(errorPipe, errno);
        }
        for (auto&& [moved, target]: std::views::zip(plan.moved, plan.targets))
            if (::dup2(moved, target) < 0)
                failChild(errorPipe, errno);
        if (::dup2(errorPipe, errorSlot) < 0)
            failChild(errorPipe, errno);
        setCloseOnExec(errorSlot);
        if (executable >= 0)
        {
            if (::dup2(executable, executableSlot) < 0)
                failChild(errorSlot, errno);
            setCloseOnExec(executableSlot);
        }
        // Exactly the table: nothing below the highest target that is not one, nothing above the two slots.
        for (auto const fd: std::views::iota(0, errorSlot))
            if (!std::ranges::contains(plan.targets, fd))
                ::close(fd);
        vtpty::util::closeDescriptorsAbove(executable >= 0 ? executableSlot : errorSlot,
                                           plan.descriptorLimit);

        ::setrlimit(RLIMIT_NOFILE, &plan.limit);
#ifdef __linux__
        if (executable >= 0)
            ::syscall(SYS_execveat, executableSlot, "", plan.argv, plan.envp, AT_EMPTY_PATH);
        else
            ::execve(plan.path, plan.argv, plan.envp);
#else
        ::execve(plan.path, plan.argv, plan.envp);
#endif
        failChild(errorSlot, errno);
    }

    /// @return A null-terminated array of pointers into @p strings, which must outlive it.
    [[nodiscard]] std::vector<char*> pointersTo(std::vector<std::string>& strings)
    {
        auto pointers = std::ranges::to<std::vector>(
            strings | std::views::transform([](std::string& s) { return s.data(); }));
        pointers.push_back(nullptr);
        return pointers;
    }

    [[nodiscard]] SpawnError spawnErrorOf(int error) noexcept
    {
        return error == EMFILE || error == ENFILE ? SpawnError::TooManyDescriptors : SpawnError::ForkFailed;
    }

    /// How a forked child names its program to the kernel.
    enum class ExecBy : uint8_t
    {
        Path = 0,   ///< execve(path): a program named by path, which may be a `#!` script.
        Descriptor, ///< execveat(fd, "", AT_EMPTY_PATH) on the program opened before the fork (Linux; the
                    ///< daemon image, or the spawn trampoline the daemon sent, always ELF). A script cannot
                    ///< run so: its descriptor is close-on-exec.
    };

    /// What a child is started as.
    struct ChildRequest
    {
        std::string const& path;
        std::vector<std::string> argv;
        std::span<int const> sources;
        std::span<int const> targets;
        ParentDeath parentDeath = ParentDeath::Survive;
        ExecBy execBy = ExecBy::Path;
        /// For ExecBy::Descriptor: the program, opened already (it stays the caller's); none to open the
        /// path.
        std::optional<int> executable = std::nullopt;
    };

    /// The pipe a forked child reports a failed exec through.
    struct ErrorPipe
    {
        UniqueFd reader;
        UniqueFd writer;
    };

    /// Forks and execs children, with the environment and the descriptor limit the keeper started with.
    class ChildForker
    {
      public:
        ChildForker(std::vector<std::string> environment, rlimit limit):
            _environment { std::move(environment) }, _envp { pointersTo(_environment) }, _limit { limit }
        {
        }

        ChildForker(ChildForker const&) = delete;
        ChildForker& operator=(ChildForker const&) = delete;
        ChildForker(ChildForker&&) = delete;
        ChildForker& operator=(ChildForker&&) = delete;
        ~ChildForker() = default;

        /// Forks @p request's program and waits until it runs: until its exec succeeded or failed.
        /// @return The child's pid; or why it does not run.
        [[nodiscard]] std::expected<pid_t, SpawnError> start(ChildRequest request) const
        {
            auto const opened = request.execBy == ExecBy::Descriptor && !request.executable
                                    ? openExecutable(request.path)
                                    : UniqueFd {};
            auto const executable =
                request.execBy == ExecBy::Descriptor ? request.executable.value_or(opened.get()) : -1;
#ifdef __linux__
            if (request.execBy == ExecBy::Descriptor && executable < 0)
                return std::unexpected { SpawnError::ExecutableUnusable };
#else
            // No execveat() here: only a path can be run.
            if (request.executable)
                return std::unexpected { SpawnError::ExecutableUnusable };
#endif
            auto errorPipe = makeErrorPipe();
            if (!errorPipe)
                return std::unexpected { spawnErrorOf(errorPipe.error().value()) };
            auto& reader = errorPipe->reader;
            auto& writer = errorPipe->writer;

            auto argv = pointersTo(request.argv);
            auto moved = std::vector<int>(request.sources.size(), -1);
            auto const plan = ChildPlan {
                .path = request.path.c_str(),
                .executable = executable,
                .argv = argv.data(),
                .envp = _envp.data(),
                .sources = request.sources,
                .targets = request.targets,
                .moved = moved,
                .errorPipe = writer.get(),
                .descriptorLimit = vtpty::util::openDescriptorLimit(),
                .limit = _limit,
                .parentDeath = request.parentDeath,
                .parent = ::getpid(),
            };

            auto const pid = ::fork();
            if (pid < 0)
                return std::unexpected { spawnErrorOf(errno) };
            if (pid == 0)
                runChild(plan);

            writer.reset();
            auto childError = 0;
            auto got = ssize_t { 0 };
            do
                got = ::read(reader.get(), &childError, sizeof(childError));
            while (got < 0 && errno == EINTR);
            if (got == 0)
                return pid;
            // It never ran: reap it here, by its pid, and report what stopped it.
            auto status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
            return std::unexpected { SpawnError::ExecutableUnusable };
        }

      private:
        /// @return The program, opened to be run by descriptor where the platform can (Linux); none
        /// otherwise.
        [[nodiscard]] static UniqueFd openExecutable([[maybe_unused]] std::string const& path) noexcept
        {
#ifdef __linux__
            return UniqueFd { ::open(path.c_str(), O_PATH | O_CLOEXEC) };
#else
            return UniqueFd {};
#endif
        }

        /// @return A close-on-exec pipe; or why none could be made.
        [[nodiscard]] static std::expected<ErrorPipe, std::error_code> makeErrorPipe() noexcept
        {
            auto ends = std::array { -1, -1 };
#ifdef __linux__
            if (::pipe2(ends.data(), O_CLOEXEC) != 0)
                return std::unexpected { lastError() };
#else
            if (::pipe(ends.data()) != 0)
                return std::unexpected { lastError() };
            for (auto const fd: ends)
                setCloseOnExec(fd);
#endif
            return ErrorPipe { .reader = UniqueFd { ends[0] }, .writer = UniqueFd { ends[1] } };
        }

        std::vector<std::string> _environment;
        std::vector<char*> _envp; ///< Into _environment.
        rlimit _limit;
    };

    // }}}
    // {{{ the channel

    /// The keeper's end of a daemon's channel: an AF_UNIX stream socket, framed (@see Frame.hpp).
    class PosixChannel final: public Channel
    {
      public:
        /// How far the link still goes.
        enum class Link : uint8_t
        {
            Open = 0,
            WriteClosed, ///< The peer is gone for sends (EPIPE, ECONNRESET); what it wrote is still read.
            Ended,       ///< Read to its end, broken, or dropped: reported as ChannelClosed.
        };

        /// How far one receive() reads.
        enum class Drain : uint8_t
        {
            Bounded = 0, ///< At most MaxReadsPerWait reads: the daemon may still be writing.
            ToEnd,       ///< Until nothing is left: the daemon ended, its frames must all be read first.
        };

        explicit PosixChannel(UniqueFd socket): _socket { std::move(socket) }, _readBuffer(ReadSize)
        {
            ::fcntl(_socket.get(), F_SETFL, ::fcntl(_socket.get(), F_GETFL) | O_NONBLOCK);
            setCloseOnExec(_socket.get());
#ifdef SO_NOSIGPIPE
            auto const on = 1;
            ::setsockopt(_socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        }

        PosixChannel(PosixChannel const&) = delete;
        PosixChannel& operator=(PosixChannel const&) = delete;
        PosixChannel(PosixChannel&&) = delete;
        PosixChannel& operator=(PosixChannel&&) = delete;

        ~PosixChannel() override { closePending(); }

        void send(Frame frame) override
        {
            if (_link != Link::Open)
                return;
            auto bytes = encodeFrame(frame);
            auto offset = std::size_t { 0 };
            if (_queue.empty())
            {
                // The common case: the socket takes it all now, and the caller's descriptors with it.
                auto const sent = sendChunk(bytes, frame.fds);
                if (!sent)
                {
                    fail(sent.error());
                    return;
                }
                offset = *sent;
                if (offset == bytes.size())
                    return;
            }
            auto outgoing = Outgoing { .bytes = std::move(bytes), .sent = offset, .fds = {} };
            if (offset == 0)
                for (auto const fd: frame.fds)
                {
                    // Duplicated now, so that the caller may close its own once send() returned.
                    auto copy = UniqueFd { ::fcntl(fd, F_DUPFD_CLOEXEC, 0) };
                    if (!copy.isOpen())
                    {
                        end();
                        return;
                    }
                    outgoing.fds.push_back(std::move(copy));
                }
            _queuedBytes += outgoing.bytes.size() - offset;
            _queue.push_back(std::move(outgoing));
            if (_queuedBytes > MaxQueuedBytes)
                end(); // It does not read.
        }

        [[nodiscard]] int pollHandle() const override { return _socket.get(); }

        /// @return Whether frames wait to be sent once the socket takes more.
        [[nodiscard]] bool wantsToWrite() const noexcept { return _link == Link::Open && !_queue.empty(); }

        /// @return Whether the link ended: read to its end, broken, or dropped.
        [[nodiscard]] Link link() const noexcept { return _link; }

        /// Sends what is queued, as far as the socket takes it.
        void flush()
        {
            while (_link == Link::Open && !_queue.empty())
            {
                auto& front = _queue.front();
                auto const fds =
                    front.sent == 0
                        ? std::ranges::to<std::vector>(front.fds | std::views::transform(&UniqueFd::get))
                        : std::vector<int> {};
                auto const sent = sendChunk(std::span { front.bytes }.subspan(front.sent), fds);
                if (!sent)
                {
                    fail(sent.error());
                    return;
                }
                if (*sent == 0)
                    return;
                front.fds.clear();
                front.sent += *sent;
                _queuedBytes -= *sent;
                if (front.sent == front.bytes.size())
                    _queue.pop_front();
            }
        }

        /// Reads what arrived, and appends the frames it completes to @p frames.
        /// @return The link's state after the reads: still Open, WriteClosed (still read), or Ended.
        [[nodiscard]] Link receive(std::vector<Frame>& frames, Drain drain)
        {
            auto reads = 0;
            while (_link != Link::Ended && (drain == Drain::ToEnd || reads++ < MaxReadsPerWait))
            {
                auto fds = std::vector<int> {};
                auto const got = readOnce(fds);
                if (!got)
                    break; // Nothing more for now.
                if (*got == 0)
                {
                    closeAll(fds); // Descriptors with no byte have no frame to belong to.
                    end();
                    break;
                }
                _assembler.feed(std::span { _readBuffer }.first(*got), fds);
                while (auto frame = _assembler.next())
                    frames.push_back(std::move(*frame));
                if (_assembler.error())
                    end();
            }
            return _link;
        }

      private:
        struct Outgoing
        {
            std::vector<uint8_t> bytes;
            std::size_t sent = 0;
            std::vector<UniqueFd> fds; ///< Ride with the first byte; none once it went.
        };

        void closePending() { closeAll(_assembler.takePendingFds()); }

        /// Stops sending after @p failure: the queue goes. A peer that is gone keeps its frames readable to
        /// the end of the link, so that a Shutdown it wrote before it exited still arrives.
        void fail(SendError failure)
        {
            if (failure == SendError::Broken)
            {
                end();
                return;
            }
            _link = Link::WriteClosed;
            _queue.clear();
            _queuedBytes = 0;
        }

        /// Ends the link: nothing more is sent, and the frames it never completed are dropped.
        void end()
        {
            _link = Link::Ended;
            _queue.clear();
            _queuedBytes = 0;
            closePending();
        }

        /// Sends @p bytes with @p fds on the first, in one sendmsg, without blocking.
        /// @return How many bytes went (0 when the socket takes nothing now); or why the send failed.
        [[nodiscard]] std::expected<std::size_t, SendError> sendChunk(std::span<uint8_t const> bytes,
                                                                      std::span<int const> fds) const
        {
            auto const runs = std::array { bytes };
            return sendWithDescriptors(_socket.get(), runs, fds);
        }

        /// Reads once into _readBuffer, collecting the descriptors that came with it into @p fds.
        /// @return How many bytes arrived, 0 at the end of the link; nothing when nothing is there now. A
        ///         broken link, or a truncated descriptor list, ends the link.
        [[nodiscard]] std::optional<std::size_t> readOnce(std::vector<int>& fds)
        {
            auto received = receiveWithDescriptors(_socket.get(), _readBuffer);
            if (!received)
            {
                if (received.error() == ReceiveError::WouldBlock)
                    return std::nullopt;
                return std::size_t { 0 }; // Broken, or descriptors lost: an end like any other.
            }
            fds = std::move(received->fds);
            return received->bytes;
        }

        UniqueFd _socket;
        std::vector<uint8_t> _readBuffer;
        FrameAssembler _assembler;
        std::deque<Outgoing> _queue;
        std::size_t _queuedBytes = 0;
        Link _link = Link::Open;
    };

    // }}}
    // {{{ the launcher, the spawner and the signaller

    class PosixDaemonLauncher final: public DaemonLauncher
    {
      public:
        PosixDaemonLauncher(std::shared_ptr<ChildForker const> forker, std::vector<std::string> arguments):
            _forker { std::move(forker) }, _arguments { std::move(arguments) }
        {
        }

        [[nodiscard]] std::expected<LaunchedDaemon, SpawnError> launch(
            std::filesystem::path const& image) override
        {
            auto ends = std::array { -1, -1 };
#ifdef SOCK_CLOEXEC
            auto const created = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends.data()) == 0;
#else
            auto const created = ::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0;
#endif
            if (!created)
                return std::unexpected { spawnErrorOf(errno) };
            auto keeperEnd = UniqueFd { ends[0] };
            auto const daemonEnd = UniqueFd { ends[1] };
            setCloseOnExec(keeperEnd.get());
            setCloseOnExec(daemonEnd.get());

            // The daemon inherits the keeper's standard streams (a `--stdio` daemon serves on them) and finds
            // its channel on DaemonChannelDescriptor.
            auto sources = std::vector<int> {};
            auto targets = std::vector<int> {};
            for (auto const stream: { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO })
                if (::fcntl(stream, F_GETFD) != -1)
                {
                    sources.push_back(stream);
                    targets.push_back(stream);
                }
            sources.push_back(daemonEnd.get());
            targets.push_back(DaemonChannelDescriptor);

            auto const path = image.string();
            auto argv = std::vector { path };
            argv.insert(argv.end(), _arguments.begin(), _arguments.end());
            auto const pid = _forker->start({ .path = path,
                                              .argv = std::move(argv),
                                              .sources = sources,
                                              .targets = targets,
                                              .parentDeath = ParentDeath::Terminate,
                                              .execBy = ExecBy::Descriptor });
            if (!pid)
                return std::unexpected { pid.error() };
            return LaunchedDaemon { .pid = *pid, .channel = makePosixChannel(std::move(keeperEnd)) };
        }

      private:
        std::shared_ptr<ChildForker const> _forker;
        std::vector<std::string> _arguments;
    };

    /// @return A pidfd for @p pid where the kernel has them and @p watch allows; none otherwise.
    [[nodiscard]] UniqueFd openProcessHandle([[maybe_unused]] int pid, ProcessWatch watch) noexcept
    {
#if defined(__linux__) && defined(SYS_pidfd_open)
        if (watch == ProcessWatch::ProcessHandle)
            return UniqueFd { static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)) };
#else
        std::ignore = watch;
#endif
        return UniqueFd {};
    }

    class PosixShellSpawner final: public ShellSpawner
    {
      public:
        PosixShellSpawner(std::shared_ptr<ChildForker const> forker, ProcessWatch watch):
            _forker { std::move(forker) }, _watch { watch }
        {
        }

        [[nodiscard]] std::expected<SpawnedChild, SpawnError> spawn(Spawn const& request,
                                                                    std::span<int const> descriptors,
                                                                    std::optional<int> executable) override
        {
            if (request.argv.empty() || descriptors.size() != request.childFds.size())
                return std::unexpected { SpawnError::Unknown };
            auto targets = std::vector<int> {};
            for (auto const target: request.childFds)
            {
                if (target > static_cast<uint32_t>(INT_MAX) || std::ranges::contains(targets, target))
                    return std::unexpected { SpawnError::Unknown };
                targets.push_back(static_cast<int>(target));
            }
            auto const pid = _forker->start({ .path = request.executablePath,
                                              .argv = request.argv,
                                              .sources = descriptors,
                                              .targets = targets,
                                              .parentDeath = ParentDeath::Survive,
                                              .execBy = executable ? ExecBy::Descriptor : ExecBy::Path,
                                              .executable = executable });
            if (!pid)
                return std::unexpected { pid.error() };
            return SpawnedChild { .pid = *pid, .processHandle = openProcessHandle(*pid, _watch) };
        }

      private:
        std::shared_ptr<ChildForker const> _forker;
        ProcessWatch _watch;
    };

    class PosixSignaller final: public Signaller
    {
      public:
        void send(int pid, KeeperSignal signal) override
        {
            // A process that is gone already needs no signal.
            std::ignore = ::kill(pid, SignalNumbers.at(std::to_underlying(signal)));
        }
    };

    // }}}
    // {{{ the event source

    /// @return Whether @p info tells a process's end (it exited, or a signal ended it), not a stop or a
    /// resume.
    [[nodiscard]] bool hasEnded(siginfo_t const& info) noexcept
    {
        return info.si_code == CLD_EXITED || info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED;
    }

    /// @return How @p info's process ended.
    [[nodiscard]] WireExitStatus exitStatusOf(siginfo_t const& info) noexcept
    {
        if (info.si_code == CLD_EXITED)
            return { .kind = ExitKind::Normal, .value = info.si_status };
        return { .kind = ExitKind::Signal, .value = info.si_status };
    }

#ifdef __linux__
    /// waitid()'s id type for a pidfd: an enumerator of glibc's, not a macro, and not in older headers.
    constexpr auto PidfdIdType = static_cast<idtype_t>(3);
#endif

    /// Receives the keeper's signals as a descriptor: a signalfd (Linux) or a kqueue (macOS, the BSDs). The
    /// signals are blocked (Linux) or ignored (kqueue) while it lives, so they arrive only through it.
    class SignalWatch
    {
      public:
        /// @return The watch; or why it could not be set up.
        [[nodiscard]] static std::expected<std::unique_ptr<SignalWatch>, std::error_code> create()
        {
            auto watch = std::unique_ptr<SignalWatch> { new SignalWatch() };
            if (!watch->_handle.isOpen())
                return std::unexpected { watch->_error };
            return watch;
        }

        SignalWatch(SignalWatch const&) = delete;
        SignalWatch& operator=(SignalWatch const&) = delete;
        SignalWatch(SignalWatch&&) = delete;
        SignalWatch& operator=(SignalWatch&&) = delete;

        ~SignalWatch()
        {
            // What arrived and was not read must not be delivered once the signals are restored.
            std::ignore = take();
            // The actions first, the mask last: a signal unblocked while still SIG_DFL would end the process.
            for (auto&& [signal, previous]: std::views::zip(ReceivedSignals, _previousActions))
                ::sigaction(signal.number, &previous, nullptr);
#ifdef __linux__
            ::pthread_sigmask(SIG_SETMASK, &_previousMask, nullptr);
#endif
        }

        [[nodiscard]] int handle() const noexcept { return _handle.get(); }

        /// @return The meanings of the signals that arrived since the last take(), without waiting.
        [[nodiscard]] std::vector<SignalMeaning> take()
        {
            auto meanings = std::vector<SignalMeaning> {};
            auto const meaningOf = [&](int number) {
                auto const found = std::ranges::find(ReceivedSignals, number, &ReceivedSignal::number);
                if (found != ReceivedSignals.end())
                    meanings.push_back(found->meaning);
            };
#ifdef __linux__
            auto info = signalfd_siginfo {};
            while (::read(_handle.get(), &info, sizeof(info)) == sizeof(info))
                meaningOf(static_cast<int>(info.ssi_signo));
#elifdef CONTOUR_KEEPER_KQUEUE
            struct kevent event {};
            auto const none = timespec {};
            while (::kevent(_handle.get(), nullptr, 0, &event, 1, &none) == 1)
                meaningOf(static_cast<int>(event.ident));
#endif
            return meanings;
        }

      private:
        SignalWatch()
        {
#ifdef __linux__
            auto set = sigset_t {};
            sigemptyset(&set);
            for (auto const& signal: ReceivedSignals)
                sigaddset(&set, signal.number);
            // Blocked before SIGCHLD and the rest go to SIG_DFL, so that none can end the keeper in between.
            ::pthread_sigmask(SIG_BLOCK, &set, &_previousMask);
#endif
            struct sigaction defaults {};
            defaults.sa_handler = SIG_DFL;
            for (auto&& [signal, previous]: std::views::zip(ReceivedSignals, _previousActions))
            {
                // SIGCHLD at SIG_IGN would reap every child behind the keeper's back.
                auto action = defaults;
#ifdef CONTOUR_KEEPER_KQUEUE
                if (signal.meaning == SignalMeaning::Stop)
                    action.sa_handler =
                        SIG_IGN; // kqueue sees it; the default action must not end the keeper.
#endif
                ::sigaction(signal.number, &action, &previous);
            }
#ifdef __linux__
            _handle = UniqueFd { ::signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC) };
            if (!_handle.isOpen())
                _error = lastError();
#elifdef CONTOUR_KEEPER_KQUEUE
            _handle = UniqueFd { ::kqueue() };
            if (!_handle.isOpen())
            {
                _error = lastError();
                return;
            }
            setCloseOnExec(_handle.get());
            for (auto const& signal: ReceivedSignals)
            {
                struct kevent change {};
                EV_SET(&change, signal.number, EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
                if (::kevent(_handle.get(), &change, 1, nullptr, 0, nullptr) != 0)
                {
                    _error = lastError();
                    _handle.reset();
                    return;
                }
            }
#else
            _error = std::make_error_code(std::errc::not_supported); // Neither a signalfd nor a kqueue.
#endif
        }

        UniqueFd _handle;
        std::error_code _error;
        std::array<struct sigaction, ReceivedSignals.size()> _previousActions {};
#ifdef __linux__
        sigset_t _previousMask {};
#endif
    };

    class PosixEventSource final: public EventSource
    {
      public:
        PosixEventSource(core::platform::IClock const& clock,
                         ProcessWatch watch,
                         std::unique_ptr<SignalWatch> signals):
            _clock { clock }, _watch { watch }, _signals { std::move(signals) }
        {
        }

        void watchChannel(Channel& channel, DaemonGeneration generation) override
        {
            auto* posix = dynamic_cast<PosixChannel*>(&channel);
            if (posix == nullptr)
                return; // Not one of these ports': nothing to watch it with.
            _channels.push_back({ .channel = posix, .generation = generation, .close = Report::Pending });
        }

        void unwatchChannel(Channel& channel) override
        {
            std::erase_if(_channels,
                          [&](WatchedChannel const& watched) { return watched.channel == &channel; });
        }

        void watchPid(WatchedProcess process) override
        {
            // The spawn's pidfd where it opened one: one pidfd per child, not two.
            auto owned = process.processHandle >= 0 ? UniqueFd {} : openProcessHandle(process.pid, _watch);
            auto const handle = process.processHandle >= 0 ? process.processHandle : owned.get();
            if (handle < 0)
                _isFallbackUnchecked = true; // It may have ended before SIGCHLD was looked for.
            _pids.push_back({ .pid = process.pid,
                              .role = process.role,
                              .owned = std::move(owned),
                              .handle = handle,
                              .end = Report::Pending });
        }

        void reap(int pid) override
        {
            auto const found = std::ranges::find(_pids, pid, &WatchedPid::pid);
            if (found == _pids.end())
                return;
            // A zombie by contract: reaped at once, never waited for.
            auto info = siginfo_t {};
            while (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG) < 0 && errno == EINTR)
                ;
            _pids.erase(found);
        }

        [[nodiscard]] std::vector<Event> wait(core::platform::SteadyTimePoint deadline) override
        {
            auto pollSet = std::vector<::pollfd> {};
            pollSet.push_back({ .fd = _signals->handle(), .events = POLLIN, .revents = 0 });
            for (auto const& watched: _channels | std::views::filter(&WatchedChannel::isOpen))
            {
                auto const writes = watched.channel->wantsToWrite() ? POLLOUT : 0;
                pollSet.push_back({ .fd = watched.channel->pollHandle(),
                                    .events = static_cast<short>(POLLIN | writes),
                                    .revents = 0 });
            }
            for (auto const& watched: _pids | std::views::filter(&WatchedPid::isPolled))
                pollSet.push_back({ .fd = watched.handle, .events = POLLIN, .revents = 0 });

            auto const ready = ::poll(pollSet.data(), pollSet.size(), timeoutUntil(deadline));
            if (ready < 0)
                return {}; // EINTR: the keeper waits again.

            auto stops = 0;
            auto childSignalled = std::exchange(_isFallbackUnchecked, false);
            for (auto const meaning: _signals->take())
            {
                if (meaning == SignalMeaning::Stop)
                    ++stops;
                else
                    childSignalled = true;
            }

            // Which processes ended, first: their channels are then read to the end before the ends are
            // reported (@see EventSource).
            auto ended = std::vector<Event> {};
            auto drain = PosixChannel::Drain::Bounded;
            auto const revents = [&](int fd) {
                auto const found = std::ranges::find(pollSet, fd, &::pollfd::fd);
                return found != pollSet.end() ? found->revents : short { 0 };
            };
            for (auto& watched: _pids)
            {
                if (watched.end == Report::Reported)
                    continue;
                if (watched.handle >= 0 ? revents(watched.handle) != 0 : childSignalled)
                    if (auto status = endOf(watched))
                    {
                        ended.push_back({ .kind = EventKind::Ended,
                                          .frame = std::nullopt,
                                          .pid = watched.pid,
                                          .status = *status,
                                          .generation = {} });
                        // Only the daemon's end reads its channel to the end: a shell's leaves the read
                        // bounded, so that a daemon writing on cannot hold the keeper in this wait.
                        if (watched.role == WatchedRole::Daemon)
                            drain = PosixChannel::Drain::ToEnd;
                    }
            }

            auto events = std::vector<Event> {};
            for (auto& watched: _channels | std::views::filter(&WatchedChannel::isOpen))
            {
                auto const flags = revents(watched.channel->pollHandle());
                if ((flags & POLLOUT) != 0)
                    watched.channel->flush();
                auto frames = std::vector<Frame> {};
                auto link = watched.channel->link();
                if ((flags & (POLLIN | POLLHUP | POLLERR)) != 0 || drain == PosixChannel::Drain::ToEnd)
                    link = watched.channel->receive(frames, drain);
                for (auto& frame: frames)
                    events.push_back({ .kind = EventKind::Frame,
                                       .frame = std::move(frame),
                                       .pid = 0,
                                       .status = {},
                                       .generation = watched.generation });
                if (link == PosixChannel::Link::Ended)
                {
                    watched.close = Report::Reported;
                    events.push_back({ .kind = EventKind::ChannelClosed,
                                       .frame = std::nullopt,
                                       .pid = 0,
                                       .status = {},
                                       .generation = watched.generation });
                }
            }
            for ([[maybe_unused]] auto const stop: std::views::iota(0, stops))
                events.push_back({ .kind = EventKind::StopSignal,
                                   .frame = std::nullopt,
                                   .pid = 0,
                                   .status = {},
                                   .generation = {} });
            std::ranges::move(ended, std::back_inserter(events));

            if (events.empty() && _clock.now() >= deadline)
                events.push_back({ .kind = EventKind::Timer,
                                   .frame = std::nullopt,
                                   .pid = 0,
                                   .status = {},
                                   .generation = {} });
            return events;
        }

      private:
        /// Whether an end has been reported.
        enum class Report : uint8_t
        {
            Pending = 0,
            Reported,
        };

        struct WatchedChannel
        {
            PosixChannel* channel = nullptr;
            DaemonGeneration generation {};
            Report close = Report::Pending;

            [[nodiscard]] bool isOpen() const noexcept { return close == Report::Pending; }
        };

        struct WatchedPid
        {
            int pid = 0;
            WatchedRole role = WatchedRole::Session;
            UniqueFd owned;  ///< The pidfd opened here; none where the spawn's is borrowed, or there is none.
            int handle = -1; ///< Its pidfd, owned or borrowed; -1 where it is watched through SIGCHLD.
            Report end = Report::Pending;

            [[nodiscard]] bool isPolled() const noexcept { return end == Report::Pending && handle >= 0; }
        };

        /// @return The poll() timeout that ends at @p deadline, rounded up to whole milliseconds; 0 when a
        ///         channel that broke on a send still has its close to report.
        [[nodiscard]] int timeoutUntil(core::platform::SteadyTimePoint deadline) const
        {
            auto const brokenUnreported = std::ranges::any_of(_channels, [](WatchedChannel const& watched) {
                return watched.isOpen() && watched.channel->link() == PosixChannel::Link::Ended;
            });
            if (brokenUnreported || _isFallbackUnchecked)
                return 0;
            auto const left = std::chrono::ceil<std::chrono::milliseconds>(deadline - _clock.now());
            return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(left.count(), 0, INT_MAX));
        }

        /// Looks whether @p watched ended, leaving it a zombie for the keeper to reap (reap()).
        /// @return How it ended; or nothing while it runs.
        [[nodiscard]] std::optional<WireExitStatus> endOf(WatchedPid& watched) const
        {
            auto const options = WEXITED | WNOHANG | WNOWAIT;
            auto info = siginfo_t {};
            auto result = 0;
            do
            {
#ifdef __linux__
                result = watched.handle >= 0
                             ? ::waitid(PidfdIdType, static_cast<id_t>(watched.handle), &info, options)
                             : ::waitid(P_PID, static_cast<id_t>(watched.pid), &info, options);
#else
                result = ::waitid(P_PID, static_cast<id_t>(watched.pid), &info, options);
#endif
            } while (result < 0 && errno == EINTR);
            if (result < 0)
            {
                // Not a child of this process any more (reaped elsewhere): ended, how is lost.
                watched.end = Report::Reported;
                return WireExitStatus {};
            }
            // Only an end counts: a stop reported here -- as macOS's waitid() appears to do, though asked for
            // WEXITED alone -- is none, and the stopped daemon is left to the heartbeat to judge.
            if (info.si_pid == 0 || !hasEnded(info))
                return std::nullopt;
            watched.end = Report::Reported;
            return exitStatusOf(info);
        }

        core::platform::IClock const& _clock;
        ProcessWatch _watch;
        std::unique_ptr<SignalWatch> _signals;
        std::vector<WatchedChannel> _channels;
        std::vector<WatchedPid> _pids;
        bool _isFallbackUnchecked = false; ///< A pid without a handle was added since the last wait().
    };

    // }}}

    class PosixLabelLock final: public LabelLock
    {
      public:
        explicit PosixLabelLock(UniqueFd file): _file { std::move(file) } {}

      private:
        UniqueFd _file;
    };

} // namespace

KeeperPorts PosixKeeperPorts::ports() const
{
    return { .launcher = *launcher,
             .spawner = *spawner,
             .events = *events,
             .signaller = *signaller,
             .budget = *budget,
             .clock = *clock };
}

std::expected<PosixKeeperPorts, std::error_code> makePosixKeeperPorts(KeeperPortsConfig config)
{
    auto signals = SignalWatch::create();
    if (!signals)
        return std::unexpected { signals.error() };
    auto budget = std::make_unique<PosixDescriptorBudget>();
    auto clock = std::make_unique<core::platform::SteadyClock>();
    auto forker = std::make_shared<ChildForker>(std::move(config.environment), budget->original());
    auto events = std::make_unique<PosixEventSource>(*clock, config.processWatch, std::move(*signals));
    return PosixKeeperPorts {
        .clock = std::move(clock),
        .budget = std::move(budget),
        .events = std::move(events),
        .launcher = std::make_unique<PosixDaemonLauncher>(forker, std::move(config.daemonArguments)),
        .spawner = std::make_unique<PosixShellSpawner>(forker, config.processWatch),
        .signaller = std::make_unique<PosixSignaller>(),
    };
}

std::unique_ptr<Channel> makePosixChannel(UniqueFd socket)
{
    return std::make_unique<PosixChannel>(std::move(socket));
}

std::filesystem::path labelLockPath(std::filesystem::path const& socketPath)
{
    auto path = socketPath;
    path += ".keeper.lock";
    return path;
}

std::expected<std::unique_ptr<LabelLock>, LockError> lockLabel(std::filesystem::path const& socketPath)
{
    auto file = UniqueFd { ::open(labelLockPath(socketPath).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600) };
    if (!file.isOpen())
        return std::unexpected { LockError::Unopenable };
    auto locked = 0;
    do
        locked = ::flock(file.get(), LOCK_EX | LOCK_NB);
    while (locked != 0 && errno == EINTR);
    if (locked != 0)
        return std::unexpected { errno == EWOULDBLOCK ? LockError::Held : LockError::Unopenable };
    return std::make_unique<PosixLabelLock>(std::move(file));
}

} // namespace vthost::keeper
