// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SpawnTrampoline.hpp>

#ifndef _WIN32

    #include <vtpty/ExecutablePath.hpp>
    #include <vtpty/OwnedFd.hpp>
    #include <vtpty/PosixSpawn.hpp>
    #include <vtpty/ProcessPlacement.hpp>
    #include <vtpty/SpawnSpec.hpp>
    #include <vtpty/UnixUtils.hpp>
    #include <vtpty/test/ProcessTesting.hpp>

    #include <core/Utils.hpp>

    #include <catch2/catch_test_macros.hpp>
    #include <catch2/generators/catch_generators.hpp>

    #include <algorithm>
    #include <array>
    #include <bit>
    #include <cerrno>
    #include <chrono>
    #include <csignal>
    #include <cstddef>
    #include <cstdint>
    #include <filesystem>
    #include <format>
    #include <fstream>
    #include <functional>
    #include <iterator>
    #include <ranges>
    #include <set>
    #include <string>
    #include <string_view>
    #include <thread>
    #include <tuple>
    #include <utility>
    #include <variant>
    #include <vector>

    #ifdef __APPLE__
        #include <util.h>
    #elifdef __FreeBSD__
        #include <libutil.h>
    #else
        #include <pty.h>
    #endif

    #include <sys/wait.h>

    #include <fcntl.h>
    #include <poll.h>
    #include <spawn.h>
    #include <unistd.h>

using namespace std::string_literals;
using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace
{

/// How long a test waits for anything a trampoline is to do.
constexpr auto Patience = std::chrono::seconds { 10 };

/// How long a test waits to see that something does not happen.
constexpr auto Glance = std::chrono::milliseconds { 200 };

/// A pseudo-terminal pair, as a session's shell runs on.
struct TestTerminal
{
    vtpty::OwnedFd master;
    vtpty::OwnedFd slave;
    std::string slaveName;
};

TestTerminal openTerminal()
{
    auto master = -1;
    auto slave = -1;
    REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
    auto name = std::array<char, 256> {};
    REQUIRE(::ttyname_r(slave, name.data(), name.size()) == 0);
    auto terminal = TestTerminal { .master = vtpty::OwnedFd { master },
                                   .slave = vtpty::OwnedFd { slave },
                                   .slaveName = name.data() };
    REQUIRE(vtpty::util::setCloseOnExec(terminal.master.get()).has_value());
    REQUIRE(vtpty::util::setCloseOnExec(terminal.slave.get()).has_value());
    return terminal;
}

/// Reads @p fd until @p done holds for what arrived, end-of-file (EIO, for a terminal master), or Patience.
std::string readUntil(int fd, std::function<bool(std::string_view)> const& done)
{
    auto text = std::string {};
    auto const deadline = std::chrono::steady_clock::now() + Patience;
    while (!done(text))
    {
        auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
            break;
        auto ready = pollfd { .fd = fd, .events = POLLIN, .revents = 0 };
        auto const polled = ::poll(&ready, 1, static_cast<int>(left.count()));
        if (polled < 0 && errno == EINTR)
            continue;
        if (polled <= 0)
            break;
        auto buffer = std::array<char, 4096> {};
        auto const length = ::read(fd, buffer.data(), buffer.size());
        if (length < 0 && errno == EINTR)
            continue;
        if (length <= 0)
            break;
        text.append(buffer.data(), static_cast<std::size_t>(length));
    }
    return text;
}

/// @return Everything @p fd yields until end-of-file or Patience.
std::string readAll(int fd)
{
    return readUntil(fd, [](std::string_view) { return false; });
}

/// @return Whether @p fd has anything to read within @p wait.
bool readable(int fd, std::chrono::milliseconds wait)
{
    auto ready = pollfd { .fd = fd, .events = POLLIN, .revents = 0 };
    return ::poll(&ready, 1, static_cast<int>(wait.count())) == 1;
}

/// Lets the child parked on @p gate go.
void release(vtpty::Gate const& gate)
{
    REQUIRE(::write(gate.parentEnd.get(), "r", 1) == 1);
}

/// @return @p pid's wait status, once it has ended.
int waitFor(pid_t pid)
{
    auto const status = vtpty::util::waitForExit(pid);
    REQUIRE(status.has_value());
    return *status;
}

/// @return Whether @p status says the process exited with @p code.
bool exitedWith(int status, int code)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == code;
}

vtpty::Gate gate()
{
    auto made = vtpty::makeGate();
    REQUIRE(made.has_value());
    return std::move(*made);
}

/// @return A spec running @p argv on the terminal slot, as a session's shell runs.
vtpty::SpawnSpec terminalSpec(std::vector<std::string> argv)
{
    return vtpty::SpawnSpec { .argv = std::move(argv),
                              .launch = vtpty::TerminalLaunch {
                                  .controllingTerminal = vtpty::ControllingTerminal::Acquire,
                                  .failureReport = vtpty::FailureReport::OnTerminal,
                              } };
}

/// @return The terminal launch of @p spec.
vtpty::TerminalLaunch& launchOf(vtpty::SpawnSpec& spec)
{
    return std::get<vtpty::TerminalLaunch>(spec.launch);
}

/// A program spawned through the trampoline onto a terminal of its own, parked on its gate.
struct TerminalChild
{
    TestTerminal terminal;
    vtpty::Gate gate;
    pid_t pid;
};

/// Spawns @p spec onto a fresh terminal, with a gate, and @p extra descriptors besides.
TerminalChild spawnOnTerminal(vtpty::SpawnSpec spec, std::vector<vtpty::InheritedFd> extra = {})
{
    auto terminal = openTerminal();
    auto parking = gate();
    auto fds = std::vector<vtpty::InheritedFd> {
        { .slot = vtpty::TrampolineSlot::Gate, .fd = parking.childEnd.get() },
        { .slot = vtpty::TrampolineSlot::Terminal, .fd = terminal.slave.get() },
    };
    fds.insert(fds.end(), extra.begin(), extra.end());
    auto const pid = vtpty::spawnTrampoline(std::move(spec), fds, vtpty::TrampolineEnvironment::Inherited);
    REQUIRE(pid.has_value());
    parking.childEnd.reset();
    terminal.slave.reset();
    return TerminalChild { .terminal = std::move(terminal), .gate = std::move(parking), .pid = *pid };
}

/// @return Whether descriptor @p fd is open in this process.
bool isOpen(int fd)
{
    return ::fcntl(fd, F_GETFD) != -1;
}

/// Restores a signal's disposition when it goes.
class SignalDisposition
{
  public:
    SignalDisposition(int signo, void (*handler)(int)):
        _signo { signo }, _previous { ::signal(signo, handler) }
    {
    }
    ~SignalDisposition() { ::signal(_signo, _previous); }
    SignalDisposition(SignalDisposition const&) = delete;
    SignalDisposition& operator=(SignalDisposition const&) = delete;
    SignalDisposition(SignalDisposition&&) = delete;
    SignalDisposition& operator=(SignalDisposition&&) = delete;

  private:
    int _signo;
    void (*_previous)(int);
};

/// @return An environment of @p count variables BIG0, BIG1, ... of @p size bytes of 'x' each.
std::vector<std::string> bigEnvironment(std::size_t count, std::size_t size)
{
    auto environment = std::vector<std::string> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        environment.push_back(std::format("BIG{}={}", index, std::string(size, 'x')));
    return environment;
}

} // namespace

TEST_CASE("spawnTrampoline.terminal mode runs the program on the terminal", "[vtpty][trampoline]")
{
    auto child = spawnOnTerminal(terminalSpec({ "/bin/sh", "-c", "printf ok; tty" }));
    release(child.gate);

    auto const name = child.terminal.slaveName;
    auto const output =
        readUntil(child.terminal.master.get(), [&](std::string_view text) { return text.contains(name); });
    CHECK(output.contains("ok"));
    CHECK(output.contains(name));
    CHECK(exitedWith(waitFor(child.pid), 0));
}

TEST_CASE("spawnTrampoline.spawns while the lowest free descriptor is a slot's number", "[vtpty][trampoline]")
{
    // What the spawn opens for itself lands on the lowest free number -- on Linux the descriptor of the image
    // it execs. Where that is a number the spawn dup2()s a slot (or the spec) onto, the dup2() replaces it
    // before the exec reads the path, unless it is kept above them, as the copies are. A runner that starts
    // the suite with few descriptors open is such a case. Here the test's own descriptors are moved out of
    // the way, the lowest such number left free and every number below it taken.
    static constexpr auto Raised = 64;
    auto const raise = [](vtpty::OwnedFd& fd) {
        fd = vtpty::OwnedFd { ::fcntl(fd.get(), F_DUPFD_CLOEXEC, Raised) };
    };
    auto terminal = openTerminal();
    auto parking = gate();
    raise(terminal.master);
    raise(terminal.slave);
    raise(parking.parentEnd);
    raise(parking.childEnd);
    auto const targets = std::array { vtpty::SpawnSpecFd,
                                      vtpty::trampolineFd(vtpty::TrampolineSlot::Gate),
                                      vtpty::trampolineFd(vtpty::TrampolineSlot::Terminal) };
    auto const target = std::ranges::find_if(targets, [](int fd) { return !isOpen(fd); });
    if (target == targets.end())
        SKIP("every descriptor the spawn maps onto is taken in this process");
    auto fillers = std::vector<vtpty::OwnedFd> {};
    for (auto const fd: std::views::iota(0, *target))
        if (!isOpen(fd))
            fillers.emplace_back(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    auto const fds = std::array {
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Gate, .fd = parking.childEnd.get() },
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Terminal, .fd = terminal.slave.get() },
    };
    auto const pid = vtpty::spawnTrampoline(
        terminalSpec({ "/bin/sh", "-c", "printf ok" }), fds, vtpty::TrampolineEnvironment::Inherited);
    fillers.clear();
    REQUIRE(pid.has_value());
    parking.childEnd.reset();
    terminal.slave.reset();
    release(parking);

    auto const output =
        readUntil(terminal.master.get(), [](std::string_view text) { return text.contains("ok"); });
    CHECK(output.contains("ok"));
    CHECK(exitedWith(waitFor(*pid), 0));
}

TEST_CASE("spawnTrampoline.parks until the gate is released", "[vtpty][trampoline]")
{
    auto const marker = fs::temp_directory_path() / std::format("vtpty-trampoline-parked-{}", ::getpid());
    fs::remove(marker);
    auto child =
        spawnOnTerminal(terminalSpec({ "/bin/sh", "-c", std::format("touch '{}'", marker.string()) }));

    std::this_thread::sleep_for(Glance);
    CHECK_FALSE(fs::exists(marker));
    #ifdef __linux__
    // Parked in the trampoline: still this very executable, not yet the program.
    CHECK(fs::read_symlink(std::format("/proc/{}/exe", child.pid)) == fs::read_symlink("/proc/self/exe"));
    #endif

    release(child.gate);
    CHECK(exitedWith(waitFor(child.pid), 0));
    CHECK(fs::exists(marker));
    fs::remove(marker);
}

TEST_CASE("spawnTrampoline.settles once the terminal is set up", "[vtpty][trampoline]")
{
    auto settle = gate();
    auto child = spawnOnTerminal(terminalSpec({ "/bin/sh", "-c", "exit 0" }),
                                 { { .slot = vtpty::TrampolineSlot::Settle, .fd = settle.childEnd.get() } });
    settle.childEnd.reset();

    CHECK_FALSE(readable(settle.parentEnd.get(), Glance));

    release(child.gate);
    REQUIRE(readable(settle.parentEnd.get(), Patience));
    auto byte = char {};
    CHECK(::read(settle.parentEnd.get(), &byte, 1) == 1);
    CHECK(byte == 's');
    CHECK(exitedWith(waitFor(child.pid), 0));
}

TEST_CASE("spawnTrampoline.installs the given environment", "[vtpty][trampoline]")
{
    auto spec = terminalSpec({ "/bin/sh", "-c", "printf '%s|' \"$FOO\"; /usr/bin/env; printf 'done|'" });
    spec.environment = std::vector<std::string> { "FOO=bar" };
    auto child = spawnOnTerminal(std::move(spec));
    release(child.gate);

    auto const output =
        readUntil(child.terminal.master.get(), [](std::string_view text) { return text.contains("done|"); });
    CHECK(output.starts_with("bar|"));
    CHECK(output.contains("FOO=bar"));
    CHECK_FALSE(output.contains("HOME="));
    CHECK(exitedWith(waitFor(child.pid), 0));
}

TEST_CASE("spawnTrampoline.falls back to the login shell", "[vtpty][trampoline]")
{
    auto spec = terminalSpec({ "/nonexistent" });
    launchOf(spec).fallbackArgv = { "/bin/sh", "-c", "echo fallback" };
    auto child = spawnOnTerminal(std::move(spec));
    release(child.gate);

    auto const output = readUntil(child.terminal.master.get(),
                                  [](std::string_view text) { return text.contains("fallback\r\n"); });
    CHECK(output.contains("Failed to spawn /nonexistent"));
    CHECK(output.contains(R"(Trying login shell: "/bin/sh" "-c" "echo fallback")"));
    CHECK(output.contains("fallback\r\n"));
    CHECK(exitedWith(waitFor(child.pid), 0));
}

TEST_CASE("spawnTrampoline.reports what failed where nothing could be spawned", "[vtpty][trampoline]")
{
    auto child = spawnOnTerminal(terminalSpec({ "/nonexistent" }));
    release(child.gate);

    auto const output = readAll(child.terminal.master.get());
    CHECK(output.contains("Failed to spawn /nonexistent"));
    CHECK(output.contains("Out of luck. "));
    CHECK(exitedWith(waitFor(child.pid), EXIT_FAILURE));
}

TEST_CASE("spawnTrampoline.reports a working directory it cannot enter", "[vtpty][trampoline]")
{
    auto spec = terminalSpec({ "/bin/sh", "-c", "exit 0" });
    launchOf(spec).workingDirectory = "/nonexistent-directory";
    auto child = spawnOnTerminal(std::move(spec));
    release(child.gate);

    CHECK(readAll(child.terminal.master.get()).contains("Failed to chdir to \"/nonexistent-directory\". "));
    CHECK(exitedWith(waitFor(child.pid), EXIT_FAILURE));
}

TEST_CASE("spawnTrampoline.a settle channel nobody listens to does not stop the program",
          "[vtpty][trampoline]")
{
    // The parent's end gone before the settle byte is written, as when a session is closed while its child is
    // released but not yet settled: the write fails, and the program runs all the same.
    auto settle = gate();
    auto child = spawnOnTerminal(terminalSpec({ "/bin/sh", "-c", "printf ran" }),
                                 { { .slot = vtpty::TrampolineSlot::Settle, .fd = settle.childEnd.get() } });
    settle.childEnd.reset();
    settle.parentEnd.reset();
    release(child.gate);

    CHECK(readUntil(child.terminal.master.get(), [](std::string_view text) {
              return text.contains("ran");
          }).contains("ran"));
    CHECK(exitedWith(waitFor(child.pid), 0));
}

TEST_CASE("spawnTrampoline.refuses a spec it cannot carry out", "[vtpty][trampoline]")
{
    auto terminal = openTerminal();
    auto parking = gate();
    auto const gateFd =
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Gate, .fd = parking.childEnd.get() };
    auto const terminalFd =
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Terminal, .fd = terminal.slave.get() };
    auto const invalid = std::make_error_code(std::errc::invalid_argument);

    SECTION("terminal mode without a terminal")
    {
        auto const fds = std::array { gateFd };
        auto const pid = vtpty::spawnTrampoline(
            terminalSpec({ "/bin/sh", "-c", "exit 0" }), fds, vtpty::TrampolineEnvironment::Inherited);
        REQUIRE_FALSE(pid.has_value());
        CHECK(pid.error() == invalid);
    }
    SECTION("an empty argv")
    {
        auto const fds = std::array { gateFd, terminalFd };
        auto const pid =
            vtpty::spawnTrampoline(terminalSpec({}), fds, vtpty::TrampolineEnvironment::Inherited);
        REQUIRE_FALSE(pid.has_value());
        CHECK(pid.error() == invalid);
    }
    SECTION("a slot passed twice")
    {
        auto const fds = std::array { gateFd, terminalFd, gateFd };
        auto const pid = vtpty::spawnTrampoline(
            terminalSpec({ "/bin/sh", "-c", "exit 0" }), fds, vtpty::TrampolineEnvironment::Inherited);
        REQUIRE_FALSE(pid.has_value());
        CHECK(pid.error() == invalid);
    }
    SECTION("the program's environment for the trampoline, where the spec carries none")
    {
        auto const fds = std::array { gateFd, terminalFd };
        auto const pid = vtpty::spawnTrampoline(
            terminalSpec({ "/bin/sh", "-c", "exit 0" }), fds, vtpty::TrampolineEnvironment::Program);
        REQUIRE_FALSE(pid.has_value());
        CHECK(pid.error() == invalid);
    }
}

TEST_CASE("spawnTrampoline.detached mode reports the program's pid", "[vtpty][trampoline]")
{
    auto parking = gate();
    auto spec =
        vtpty::SpawnSpec { .argv = { "/bin/sh", "-c", "exit 0" }, .launch = vtpty::DetachedLaunch {} };
    auto const fds = std::array {
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Gate, .fd = parking.childEnd.get() },
    };
    auto const trampoline =
        vtpty::spawnTrampoline(std::move(spec), fds, vtpty::TrampolineEnvironment::Inherited);
    REQUIRE(trampoline.has_value());
    parking.childEnd.reset();
    CHECK(exitedWith(waitFor(*trampoline), EXIT_SUCCESS));

    REQUIRE(readable(parking.parentEnd.get(), Patience));
    auto received = std::array<char, sizeof(pid_t)> {};
    REQUIRE(::read(parking.parentEnd.get(), received.data(), received.size())
            == static_cast<ssize_t>(received.size()));
    auto const program = std::bit_cast<pid_t>(received);
    CHECK(program > 0);
    CHECK(program != *trampoline);
    errno = 0;
    CHECK(::waitpid(program, nullptr, WNOHANG) == -1);
    CHECK(errno == ECHILD);
    release(parking);
}

TEST_CASE("spawnTrampoline.a detached program runs only on the release byte", "[vtpty][trampoline]")
{
    auto const marker = fs::temp_directory_path() / std::format("vtpty-trampoline-detached-{}", ::getpid());
    fs::remove(marker);
    auto parking = gate();
    auto spec =
        vtpty::SpawnSpec { .argv = { "/bin/sh", "-c", std::format("echo ran > '{}'", marker.string()) },
                           .launch = vtpty::DetachedLaunch {} };
    auto const fds = std::array {
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Gate, .fd = parking.childEnd.get() },
    };
    auto const trampoline =
        vtpty::spawnTrampoline(std::move(spec), fds, vtpty::TrampolineEnvironment::Inherited);
    REQUIRE(trampoline.has_value());
    parking.childEnd.reset();
    CHECK(exitedWith(waitFor(*trampoline), EXIT_SUCCESS));
    REQUIRE(readable(parking.parentEnd.get(), Patience));
    auto received = std::array<char, sizeof(pid_t)> {};
    REQUIRE(::read(parking.parentEnd.get(), received.data(), received.size())
            == static_cast<ssize_t>(received.size()));
    auto const program = std::bit_cast<pid_t>(received);
    REQUIRE(program > 0);

    auto const released = GENERATE(false, true);
    INFO("released: " << released);
    if (released)
        release(parking);
    parking.parentEnd.reset(); // end-of-file, after the byte or instead of it

    // Not this process's child, so not waited for: gone once a signal no longer reaches it.
    auto const deadline = std::chrono::steady_clock::now() + Patience;
    while (::kill(program, 0) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    CHECK(::kill(program, 0) == -1);
    CHECK(fs::exists(marker) == released);
    fs::remove(marker);
}

TEST_CASE("spawnTrampoline.slots survive colliding source descriptors", "[vtpty][trampoline]")
{
    // Each source sits on the number the other's slot takes: dup2()ed one after the other, in place, the
    // first would overwrite the second's source.
    auto parking = gate();
    auto settle = gate();
    auto const gateSource = vtpty::trampolineFd(vtpty::TrampolineSlot::Settle);
    auto const settleSource = vtpty::trampolineFd(vtpty::TrampolineSlot::Gate);
    REQUIRE(gateSource == 5);
    REQUIRE(settleSource == 4);
    // Every end moved high first, freeing the low numbers the test is to put the sources on.
    auto const moveHigh = [](vtpty::OwnedFd& fd) {
        auto high = vtpty::OwnedFd { ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 100) };
        REQUIRE(high.isOpen());
        fd = std::move(high);
    };
    for (auto* const end: { &parking.childEnd, &parking.parentEnd, &settle.childEnd, &settle.parentEnd })
        moveHigh(*end);
    REQUIRE_FALSE(isOpen(gateSource));
    REQUIRE_FALSE(isOpen(settleSource));
    REQUIRE(::dup2(parking.childEnd.get(), gateSource) == gateSource);
    parking.childEnd = vtpty::OwnedFd { gateSource };
    REQUIRE(::dup2(settle.childEnd.get(), settleSource) == settleSource);
    settle.childEnd = vtpty::OwnedFd { settleSource };
    REQUIRE(vtpty::util::setCloseOnExec(gateSource).has_value());
    REQUIRE(vtpty::util::setCloseOnExec(settleSource).has_value());

    auto terminal = openTerminal();
    auto const fds = std::array {
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Gate, .fd = gateSource },
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Settle, .fd = settleSource },
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Terminal, .fd = terminal.slave.get() },
    };
    auto const pid = vtpty::spawnTrampoline(
        terminalSpec({ "/bin/sh", "-c", "exit 0" }), fds, vtpty::TrampolineEnvironment::Inherited);
    REQUIRE(pid.has_value());
    parking.childEnd.reset();
    settle.childEnd.reset();
    terminal.slave.reset();

    CHECK_FALSE(readable(settle.parentEnd.get(), Glance)); // Parked on the gate, not on the settle channel.
    release(parking);
    REQUIRE(readable(settle.parentEnd.get(), Patience));
    auto byte = char {};
    CHECK(::read(settle.parentEnd.get(), &byte, 1) == 1);
    CHECK(byte == 's');
    CHECK(exitedWith(waitFor(*pid), 0));
}

    #ifdef __linux__
TEST_CASE("spawnTrampoline.an unused slot number holding a stray descriptor is ignored and closed",
          "[vtpty][trampoline]")
{
    auto const strayNumber = vtpty::trampolineFd(vtpty::TrampolineSlot::Input);
    REQUIRE_FALSE(isOpen(strayNumber));
    auto const nullDevice = vtpty::OwnedFd { ::open("/dev/null", O_RDONLY) }; // inherited: no O_CLOEXEC
    REQUIRE(nullDevice.isOpen());
    REQUIRE(::dup2(nullDevice.get(), strayNumber) == strayNumber);
    auto const stray = vtpty::OwnedFd { strayNumber };

    auto child = spawnOnTerminal(terminalSpec({ "/bin/sh", "-c", "exit 0" }));

    // Looked at while it is parked, before the program's own sweep closes everything above stderr: only the
    // slots the spec names are open above it -- the gate and the terminal -- not the stray, nor the spec.
    auto const kept = std::set { vtpty::trampolineFd(vtpty::TrampolineSlot::Gate),
                                 vtpty::trampolineFd(vtpty::TrampolineSlot::Terminal) };
    auto const aboveStderr = [&] {
        auto open = std::set<int> {};
        for (auto const& entry: fs::directory_iterator { std::format("/proc/{}/fd", child.pid) })
            if (auto const fd = std::stoi(entry.path().filename().string()); fd > STDERR_FILENO)
                open.insert(fd);
        return open;
    };
    auto const deadline = std::chrono::steady_clock::now() + Patience;
    auto open = aboveStderr();
    while (open != kept && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
        open = aboveStderr();
    }
    CHECK(open == kept);

    release(child.gate);
    CHECK(exitedWith(waitFor(child.pid), 0));
}
    #endif

TEST_CASE("spawnTrampoline.an environment beyond the socket buffer arrives verbatim", "[vtpty][trampoline]")
{
    // Variables of 64 KiB, as Linux takes no single string over 128 KiB (MAX_ARG_STRLEN), filling half of
    // what the program's own exec accepts in all (ARG_MAX: 1 MiB on macOS), and at most 1 MiB: either way
    // several times what a socket buffers, so the spec cannot arrive in one write.
    constexpr auto Size = std::size_t { 64 } * 1024;
    constexpr auto Ceiling = std::size_t { 1024 } * 1024;
    auto const argMax = ::sysconf(_SC_ARG_MAX);
    REQUIRE(argMax > 0);
    auto const count = std::min(Ceiling, static_cast<std::size_t>(argMax) / 2) / Size;
    REQUIRE(count * Size >= std::size_t { 128 } * 1024);
    auto sum = std::string { "0" };
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        sum += std::format(" + ${{#BIG{}}}", index);
    auto spec = terminalSpec({ "/bin/sh", "-c", std::format("printf 'total=%s|' $(({}))", sum) });
    spec.environment = bigEnvironment(count, Size);
    auto child = spawnOnTerminal(std::move(spec));
    release(child.gate);

    auto const output =
        readUntil(child.terminal.master.get(), [](std::string_view text) { return text.contains("|"); });
    CHECK(output == std::format("total={}|", count * Size));
    CHECK(exitedWith(waitFor(child.pid), 0));
}

TEST_CASE("spawnTrampoline.an unsupported version exits 121", "[vtpty][trampoline]")
{
    auto const self = vtpty::selfExecutablePath();
    REQUIRE(self.has_value());
    auto name = "contour-spawn"s;
    auto marker = std::format("{}{}", vtpty::SpawnTrampolineMarker, 99);
    auto argv = std::array<char*, 3> { name.data(), marker.data(), nullptr };
    auto pid = pid_t {};
    REQUIRE(
        ::posix_spawn(&pid, self->c_str(), nullptr, nullptr, argv.data(), vtpty::util::processEnvironment())
        == 0);
    CHECK(exitedWith(waitFor(pid), static_cast<int>(vtpty::TrampolineExit::UnsupportedVersion)));
}

TEST_CASE("spawnTrampoline.a refused spec is reported on the terminal slot", "[vtpty][trampoline]")
{
    auto const self = vtpty::selfExecutablePath();
    REQUIRE(self.has_value());
    auto terminal = openTerminal();
    auto const nullDevice = vtpty::OwnedFd { ::open("/dev/null", O_RDONLY | O_CLOEXEC) };
    REQUIRE(nullDevice.isOpen());
    // Both sources sit above every target, so neither dup2 overwrites the other's source: an inherited
    // descriptor (ctest hands one down on fd 3) can otherwise put the slave where the spec is read from.
    auto slave = vtpty::OwnedFd { ::fcntl(terminal.slave.get(), F_DUPFD_CLOEXEC, 100) };
    auto const endOfFile = vtpty::OwnedFd { ::fcntl(nullDevice.get(), F_DUPFD_CLOEXEC, 100) };
    REQUIRE(slave.isOpen());
    REQUIRE(endOfFile.isOpen());
    auto actions = vtpty::SpawnFileActions::create();
    REQUIRE(actions.has_value());
    REQUIRE(::posix_spawn_file_actions_adddup2(
                actions->get(), slave.get(), vtpty::trampolineFd(vtpty::TrampolineSlot::Terminal))
            == 0);
    // End-of-file where the spec is to arrive.
    REQUIRE(::posix_spawn_file_actions_adddup2(actions->get(), endOfFile.get(), vtpty::SpawnSpecFd) == 0);

    struct Refusal
    {
        std::uint32_t version;
        vtpty::TrampolineExit status;
        std::string_view says;
    };
    auto const refusal = GENERATE(Refusal { .version = 99U,
                                            .status = vtpty::TrampolineExit::UnsupportedVersion,
                                            .says = "must be restarted" },
                                  Refusal { .version = vtpty::SpawnTrampolineProtocolVersion,
                                            .status = vtpty::TrampolineExit::SpecUnreadable,
                                            .says = "no spawn spec arrived" });
    INFO("version: " << refusal.version);
    auto name = "contour-spawn"s;
    auto marker = std::format("{}{}", vtpty::SpawnTrampolineMarker, refusal.version);
    auto argv = std::array<char*, 3> { name.data(), marker.data(), nullptr };
    auto pid = pid_t {};
    REQUIRE(::posix_spawn(
                &pid, self->c_str(), actions->get(), nullptr, argv.data(), vtpty::util::processEnvironment())
            == 0);
    terminal.slave.reset();
    slave.reset();

    // On its own lines: the terminal is not configured yet, so its ONLCR may make each "\r\n" "\r\r\n".
    auto const output = readUntil(terminal.master.get(),
                                  [](std::string_view text) { return std::ranges::count(text, '\n') >= 2; });
    CHECK(output.contains("\ncontour: "));
    CHECK(output.contains(refusal.says));
    CHECK(output.ends_with("\n"));
    CHECK(exitedWith(waitFor(pid), static_cast<int>(refusal.status)));
}

TEST_CASE("spawnTrampoline.an image without the trampoline entry fails fast", "[vtpty][trampoline]")
{
    // A SIGPIPE from writing the spec to a process that never reads it would end this test process.
    auto const pipeSignal = SignalDisposition { SIGPIPE, SIG_DFL };
    auto parking = gate();
    auto spec = terminalSpec({ "/bin/sh", "-c", "exit 0" });
    spec.environment = bigEnvironment(16, std::size_t { 64 } * 1024); // More than any socket buffer holds.
    auto const fds = std::array {
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Gate, .fd = parking.childEnd.get() },
    };

    auto const start = std::chrono::steady_clock::now();
    auto const pid = vtpty::spawnTrampolineImage(
        "/bin/true", std::move(spec), fds, vtpty::TrampolineEnvironment::Inherited);
    if (pid.has_value())
        CHECK(exitedWith(waitFor(*pid), 0));
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("spawnOnNewTerminal.places the program before it runs", "[vtpty][trampoline][placement]")
{
    auto placement = vtpty::testing::ReleasingPlacement {};
    auto program = vtpty::spawnOnNewTerminal(
        terminalSpec({ "/bin/sh", "-c", "printf ran" }), placement, vtpty::ProcessRole::TmuxClient);
    REQUIRE(program.has_value());
    CHECK(placement.pids() == std::vector<int> { program->pid });
    CHECK(readUntil(program->master.get(), [](std::string_view text) {
              return text.contains("ran");
          }).contains("ran"));
    CHECK(exitedWith(waitFor(program->pid), 0));
}

    #ifdef __linux__
TEST_CASE("spawnOnNewTerminal.gives the program the placement's OOM score adjustment", "[vtpty][trampoline]")
{
    // Raised, not lowered: any process may make itself a likelier victim.
    auto placement = vtpty::NoPlacement { 543 };
    auto program = vtpty::spawnOnNewTerminal(
        terminalSpec({ "/bin/sh", "-c", "printf 'adj=%s|' $(cat /proc/self/oom_score_adj)" }),
        placement,
        vtpty::ProcessRole::Session);
    REQUIRE(program.has_value());
    CHECK(readUntil(program->master.get(), [](std::string_view text) {
              return text.contains("|");
          }).contains("adj=543|"));
    CHECK(exitedWith(waitFor(program->pid), 0));
}
    #endif

TEST_CASE("spawnOnNewTerminal.refuses a detached spec", "[vtpty][trampoline]")
{
    auto placement = vtpty::testing::ReleasingPlacement {};
    auto const program = vtpty::spawnOnNewTerminal(
        vtpty::SpawnSpec { .argv = { "/bin/sh", "-c", "exit 0" }, .launch = vtpty::DetachedLaunch {} },
        placement,
        vtpty::ProcessRole::Program);
    REQUIRE_FALSE(program.has_value());
    CHECK(program.error() == std::make_error_code(std::errc::invalid_argument));
    CHECK(placement.pids().empty());
}

TEST_CASE("spawnTrampoline.a terminal it cannot control is still the program's", "[vtpty][trampoline]")
{
    // The terminal is the controlling one of a first program's session already, so the second program's
    // TIOCSCTTY fails: its standard streams are on the terminal all the same, not on the trampoline's.
    auto first = spawnOnTerminal(terminalSpec({ "/bin/sh", "-c", "printf first; exec sleep 30" }));
    release(first.gate);
    REQUIRE(readUntil(first.terminal.master.get(), [](std::string_view text) {
                return text.contains("first");
            }).contains("first"));

    auto const slave =
        vtpty::OwnedFd { ::open(first.terminal.slaveName.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC) };
    REQUIRE(slave.isOpen());
    auto parking = gate();
    auto const fds = std::array {
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Gate, .fd = parking.childEnd.get() },
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Terminal, .fd = slave.get() },
    };
    auto const second = vtpty::spawnTrampoline(
        terminalSpec({ "/bin/sh", "-c", "printf second" }), fds, vtpty::TrampolineEnvironment::Inherited);
    REQUIRE(second.has_value());
    parking.childEnd.reset();
    release(parking);

    CHECK(readUntil(first.terminal.master.get(), [](std::string_view text) {
              return text.contains("second");
          }).contains("second"));
    CHECK(exitedWith(waitFor(*second), 0));
    ::kill(first.pid, SIGKILL);
    std::ignore = waitFor(first.pid);
}

    #ifdef __linux__
namespace
{
/// Whether this build runs under ThreadSanitizer, whose runtime starts a thread of its own in every process.
constexpr auto UnderThreadSanitizer =
        #ifdef __SANITIZE_THREAD__
    true;
        #elifdef __has_feature
            #if __has_feature(thread_sanitizer)
    true;
            #else
    false;
            #endif
        #else
    false;
        #endif

/// @return The value of @p key in /proc/@p pid/status ("Threads", say); empty where there is none.
std::string procStatus(pid_t pid, std::string_view key)
{
    auto status = std::ifstream { std::format("/proc/{}/status", pid) };
    auto line = std::string {};
    auto const prefix = std::format("{}:", key);
    while (std::getline(status, line))
        if (line.starts_with(prefix))
            return std::string { core::trimLeft(std::string_view { line }.substr(prefix.size())) };
    return {};
}
} // namespace

TEST_CASE("spawnTrampoline.parks single-threaded, under its own name", "[vtpty][trampoline]")
{
    // The point of the trampoline: whatever the spawning process runs beside, the process that sets the
    // program up has one thread, so nothing it calls can find a lock another thread held at the spawn.
    if (UnderThreadSanitizer)
        SKIP("ThreadSanitizer's runtime starts a thread in every process");
    auto child = spawnOnTerminal(terminalSpec({ "/bin/sh", "-c", "exit 0" }));
    // Named once it runs as the trampoline, which is how a test knows it got that far.
    auto const deadline = std::chrono::steady_clock::now() + Patience;
    while (procStatus(child.pid, "Name") != "contour-spawn" && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    std::this_thread::sleep_for(Glance); // parked on its gate by now

    CHECK(procStatus(child.pid, "Name") == "contour-spawn");
    CHECK(procStatus(child.pid, "Threads") == "1");

    release(child.gate);
    CHECK(exitedWith(waitFor(child.pid), 0));
}

TEST_CASE("spawnTrampoline.runs under the program's environment where asked to", "[vtpty][trampoline]")
{
    // What the trampoline itself was spawned under, read while it is parked, before any exec installs one.
    // Polled: posix_spawn() returns once the exec has let go of this process's memory, which is before the
    // kernel has laid the new image's environment out.
    auto const environmentOf = [](pid_t pid) {
        auto const deadline = std::chrono::steady_clock::now() + Patience;
        auto environment = std::string {};
        while (environment.empty() && std::chrono::steady_clock::now() < deadline)
        {
            auto in = std::ifstream { std::format("/proc/{}/environ", pid), std::ios::binary };
            environment.assign(std::istreambuf_iterator<char> { in }, {});
            if (environment.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
        }
        return environment;
    };
    auto terminal = openTerminal();
    auto parking = gate();
    auto const fds = std::array {
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Gate, .fd = parking.childEnd.get() },
        vtpty::InheritedFd { .slot = vtpty::TrampolineSlot::Terminal, .fd = terminal.slave.get() },
    };
    auto spec = terminalSpec({ "/bin/sh", "-c", "exit 0" });
    spec.environment = std::vector<std::string> { "ONLY=given" };

    auto const pid = vtpty::spawnTrampoline(std::move(spec), fds, vtpty::TrampolineEnvironment::Program);
    REQUIRE(pid.has_value());
    parking.childEnd.reset();
    CHECK(environmentOf(*pid) == std::string_view { "ONLY=given\0", 11 });

    release(parking);
    CHECK(exitedWith(waitFor(*pid), 0));
}
    #endif

TEST_CASE("PosixSpawnObject.create", "[vtpty][trampoline]")
{
    // Made initialised or not at all; a move keeps the very object, which POSIX does not let be copied.
    auto actions = vtpty::SpawnFileActions::create();
    REQUIRE(actions.has_value());
    auto* const object = actions->get();
    REQUIRE(object != nullptr);
    CHECK(::posix_spawn_file_actions_addclose(object, 42) == 0);
    auto moved = std::move(*actions);
    CHECK(moved.get() == object);
    CHECK(vtpty::SpawnAttributes::create().has_value());
}

TEST_CASE("selfExecutablePath.its errors say what is missing", "[vtpty][trampoline]")
{
    CHECK(vtpty::makeErrorCode(vtpty::SelfExecutableError::NoProcFileSystem).message().contains("/proc"));
    CHECK(vtpty::makeErrorCode(vtpty::SelfExecutableError::NoWayToName)
              .message()
              .contains("no way to name the running executable"));
}

TEST_CASE("selfExecutablePath", "[vtpty][trampoline]")
{
    auto const self = vtpty::selfExecutablePath();
    REQUIRE(self.has_value());
    #ifdef __linux__
    CHECK(*self == "/proc/self/exe");
    #else
    CHECK(fs::path { *self }.is_absolute());
    CHECK(fs::exists(*self));
    #endif
}

#endif // !_WIN32
