// SPDX-License-Identifier: Apache-2.0
//
// The shell-integration scripts, run by the shells they are written for.
//
// Each shell found on PATH is started on a real PTY with its embedded script sourced -- the very
// bytes `contour generate integration` hands out -- and a command line holding two spaces, both
// kinds of quote, a ';', a '%' and characters of two, three and four UTF-8 bytes is typed at its
// prompt. Every byte the shell writes goes through a Terminal, which also answers whatever the shell
// asks of its terminal on the way (fish's DA1, .NET's cursor-position report), as Contour would. Two
// witnesses then have to agree: the OSC 133 sequences in the byte stream, and the command block the
// Terminal recorded from them.
//
// A shell that is not installed SKIPs. CI installs bash, zsh, fish and tcsh and fails when any of them
// skipped ("verify the shell-integration tests actually ran", build.yml). The POSIX shells are not
// run on Windows, where `bash` on PATH may be WSL's launcher.

#include <contour/cli/ShellIntegration.hpp>

#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <core/Environment.hpp>
#include <core/Escape.hpp>
#include <core/Utils.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/Daemon.hpp>

using namespace std::string_view_literals;

namespace
{

/// The terminal's size, for the PTY and for the Terminal reading it alike.
constexpr auto TerminalSize =
    vtbackend::PageSize { .lines = vtbackend::LineCount(24), .columns = vtbackend::ColumnCount(80) };

/// How long a shell may take, all told, before the test ends it: on Windows a read blocks until the
/// pseudoconsole has something to say, and only the shell's end makes it return.
constexpr auto Deadline = std::chrono::seconds { 60 };

#ifdef __APPLE__
/// A UTF-8 locale the runner has: macOS ships no C.UTF-8.
constexpr auto Utf8Locale = "en_US.UTF-8"sv;
#else
/// A UTF-8 locale the runner has.
constexpr auto Utf8Locale = "C.UTF-8"sv;
#endif

/// The command line typed at the POSIX shells' prompts: two spaces, both kinds of quote, the ';' that
/// separates OSC 133's parameters, a '%', and characters of two, three and four UTF-8 bytes.
constexpr auto PosixCommand = "echo \"a  b;c\" 'd\"e' 50% \xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80"sv;

/// One OSC 133 sequence found in a byte stream.
struct Mark
{
    char kind {};       ///< A, B, C or D.
    std::string params; ///< What follows "<kind>;", e.g. "cmdline_url=ls" or "0"; empty for none.
};

/// @return Every OSC 133 sequence in @p bytes, in order, whichever terminator (ST or BEL) ends it.
///         One the end of @p bytes cuts off is not counted.
[[nodiscard]] std::vector<Mark> osc133Marks(std::string_view bytes)
{
    constexpr auto Introducer = "\x1b]133;"sv;
    auto marks = std::vector<Mark> {};
    for (auto const piece: std::views::split(bytes, Introducer) | std::views::drop(1))
    {
        auto const body = std::string_view { piece.begin(), piece.end() };
        auto const end = body.find_first_of("\x07\x1b"sv);
        if (end == std::string_view::npos || end == 0)
            continue;
        auto const payload = body.substr(0, end);
        marks.push_back(Mark { .kind = payload.front(),
                               .params = std::string { payload.size() > 2 ? payload.substr(2) : ""sv } });
    }
    return marks;
}

/// @return The kinds of @p marks as one string, e.g. "ABCDAB".
[[nodiscard]] std::string kindsOf(std::vector<Mark> const& marks)
{
    return std::ranges::to<std::string>(marks | std::views::transform(&Mark::kind));
}

/// How one shell is started for these tests: with its integration script sourced, and nothing of
/// the user's -- a private HOME holds the start-up file that sources it.
struct ShellLaunch
{
    std::string shell;       ///< The integration table's name for it.
    std::string program;     ///< The executable looked up on PATH.
    std::string scriptFile;  ///< Where in HOME the script goes; PowerShell runs only *.ps1.
    std::string startupFile; ///< A start-up file written into HOME, by relative path; empty for none.
    std::string startupText; ///< Its contents. ${SCRIPT} and ${HOME} stand for those paths.
    std::vector<std::string> arguments; ///< The command-line arguments, with the same substitutions.
    std::string command;                ///< The command line typed at the prompt.
    vtbackend::CommandLineSource commandLineSource {}; ///< How the terminal learns that command line.
};

/// @return Every shell the integration scripts target, and how each is started here.
[[nodiscard]] std::vector<ShellLaunch>& shellLaunches()
{
    static auto launches = std::vector<ShellLaunch> {
        ShellLaunch {
            .shell = "bash",
            .program = "bash",
            .scriptFile = "shell-integration.bash",
            .startupFile = ".bashrc",
            .startupText = "PS1='$ '\nsource '${SCRIPT}'\n",
            .arguments = { "--noprofile", "--rcfile", "${HOME}/.bashrc", "-i" },
            .command = std::string { PosixCommand },
            .commandLineSource = vtbackend::CommandLineSource::Reported,
        },
    };
    return launches;
}

/// @return The row for @p shell.
[[nodiscard]] ShellLaunch const& launchFor(std::string_view shell)
{
    auto const& launches = shellLaunches();
    auto const row = std::ranges::find(launches, shell, &ShellLaunch::shell);
    REQUIRE(row != launches.end());
    return *row;
}

/// @return Whether @p candidate is a file this process may execute.
[[nodiscard]] bool isExecutableFile(std::filesystem::path const& candidate)
{
#ifdef _WIN32
    // Windows keeps no execute permission: a regular file is as executable as it gets.
    constexpr auto ExecutePermission = std::filesystem::perms::none;
#else
    constexpr auto ExecutePermission = std::filesystem::perms::owner_exec;
#endif
    auto error = std::error_code {};
    auto const status = std::filesystem::status(candidate, error);
    return !error && std::filesystem::is_regular_file(status)
           && (status.permissions() & ExecutePermission) == ExecutePermission;
}

/// @return Where @p program is on PATH, or nullopt when it is not.
[[nodiscard]] std::optional<std::filesystem::path> findOnPath(std::string_view program)
{
    auto const searchPath = core::defaultEnvironment().get("PATH").value_or(std::string {});
    auto const resolved = vthost::resolveExecutablePath(program, searchPath, isExecutableFile);
    if (resolved == program)
        return std::nullopt;
    return std::filesystem::path { resolved };
}

/// Writes @p contents to @p path, creating the directories above it.
void writeFile(std::filesystem::path const& path, std::string_view contents)
{
    std::filesystem::create_directories(path.parent_path());
    auto out = std::ofstream { path, std::ios::binary };
    REQUIRE(out.good());
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/// @return The environment every shell here runs in: its own HOME, and with it its own history and
///         start-up files; a UTF-8 locale; a TERM every shell knows.
[[nodiscard]] vtpty::Process::Environment isolatedEnvironment(std::string const& home)
{
    return vtpty::Process::Environment {
        { "HOME", home },
        { "ZDOTDIR", home },
        { "XDG_CONFIG_HOME", home + "/.config" },
        { "XDG_DATA_HOME", home + "/.local/share" },
        { "HISTFILE", home + "/.history" },
        { "BASH_SILENCE_DEPRECATION_WARNING", "1" },
        { "LANG", std::string { Utf8Locale } },
        { "LC_ALL", std::string { Utf8Locale } },
        { "TERM", "xterm-256color" },
    };
}

/// @return What the shell must not inherit from whoever runs the tests: a file every bash (BASH_ENV)
///         or sh (ENV) sources, and prompt and history settings that would decide what it marks.
[[nodiscard]] std::vector<std::string> userShellSettings()
{
    return { "BASH_ENV", "ENV", "PROMPT_COMMAND", "PS1", "HISTCONTROL", "HISTIGNORE" };
}

/// Runs an action on a thread of its own once a timeout has passed, unless it is destroyed first.
///
/// std::thread rather than std::jthread: libc++ before 20 -- Apple's among them -- offers jthread
/// and stop_token only behind -fexperimental-library.
class Watchdog
{
  public:
    /// Starts the countdown.
    /// @param timeout How long to wait before acting.
    /// @param onTimeout What to do once @p timeout has passed, on the watchdog's thread.
    Watchdog(std::chrono::milliseconds timeout, std::function<void()> onTimeout):
        _thread { [this, timeout, onTimeout = std::move(onTimeout)] {
            auto lock = std::unique_lock { _mutex };
            if (_wakeup.wait_for(lock, timeout, [this] { return _state == State::Disarmed; }))
                return;
            lock.unlock();
            onTimeout();
        } }
    {
    }

    Watchdog(Watchdog const&) = delete;
    Watchdog& operator=(Watchdog const&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;

    /// Disarms the watchdog, and waits for its thread -- and an action already under way -- to end.
    ~Watchdog()
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _state = State::Disarmed;
        }
        _wakeup.notify_one();
        _thread.join();
    }

  private:
    /// Whether the action is still to run once the timeout has passed.
    enum class State : uint8_t
    {
        Armed = 0,
        Disarmed,
    };

    std::mutex _mutex;
    std::condition_variable _wakeup;
    State _state = State::Armed;
    std::thread _thread; // last: started once everything it reads exists
};

/// A shell started per a ShellLaunch on a real PTY, whose output a Terminal reads -- and answers -- as
/// Contour would.
class ShellSession
{
  public:
    /// Writes the script and the start-up file into a private HOME and starts the shell there.
    /// @param launch How to start it.
    /// @param program Where its executable was found.
    ShellSession(ShellLaunch const& launch, std::filesystem::path const& program):
        _home { "contour-shell-integration" },
        _process { prepare(launch, program, _home),
                   vtpty::createPty(TerminalSize, std::nullopt),
                   /*escapeSandbox=*/false,
                   std::make_shared<vtpty::NoPlacement>() },
        _terminal { TerminalSize },
        _watchdog { Deadline, [this] { _process.terminate(vtpty::Process::TerminationHint::Hangup); } }
    {
        REQUIRE(_process.start().has_value());
    }

    ShellSession(ShellSession const&) = delete;
    ShellSession& operator=(ShellSession const&) = delete;
    ShellSession(ShellSession&&) = delete;
    ShellSession& operator=(ShellSession&&) = delete;

    /// Ends the shell -- every shell here exits on a hangup -- and reaps it, whether or not the test
    /// passed: ~Process would otherwise wait for a shell nobody asked to end.
    ~ShellSession()
    {
        _process.terminate(vtpty::Process::TerminationHint::Hangup);
        std::ignore = _process.wait();
    }

    /// Reads until @p done holds for everything the shell wrote so far, feeding each byte through the
    /// terminal and sending back whatever the terminal answers; fails the test when it never does.
    /// @param done Whether what the shell wrote so far is enough.
    /// @param what What is being waited for, for the failure message.
    void pumpUntil(std::function<bool(std::string_view)> const& done, std::string_view what)
    {
        auto seen = size_t { 0 };
        std::ignore = vtpty::testing::readUntil(_process, [&](std::string_view readSoFar) {
            auto const fresh = readSoFar.substr(seen);
            seen = readSoFar.size();
            _transcript += fresh;
            _terminal.writeToScreen(fresh);
            _terminal.terminal.flushInput();
            if (!_terminal.replyData().empty())
            {
                std::ignore = _process.write(_terminal.replyData());
                _terminal.resetReplyData();
            }
            return done(_transcript);
        });
        INFO("waiting for: " << what);
        INFO("what the shell wrote: " << core::escape(_transcript));
        REQUIRE(done(_transcript));
    }

    /// Types @p text at the shell, as a keyboard would.
    void type(std::string_view text) { REQUIRE(_process.write(text) == static_cast<int>(text.size())); }

    /// @return Everything the shell wrote so far.
    [[nodiscard]] std::string const& transcript() const noexcept { return _transcript; }

    /// @return The terminal that read it.
    [[nodiscard]] vtbackend::MockTerm<>& terminal() noexcept { return _terminal; }

  private:
    /// Puts the script and the start-up file into @p home.
    /// @return How to start the shell there.
    [[nodiscard]] static vtpty::Process::ExecInfo prepare(
        ShellLaunch const& launch,
        [[maybe_unused]] std::filesystem::path const& program,
        core::testing::ScopedTempDir const& home)
    {
        auto const homeText = home.string();
        auto const scriptText = (home / launch.scriptFile).generic_string();
        auto const substitute = [&](std::string_view text) {
            return core::replaceVariables(text, [&](std::string_view name) -> std::string const& {
                return name == "SCRIPT" ? scriptText : homeText;
            });
        };

        auto const script = contour::cli::shellIntegrationScript(launch.shell);
        REQUIRE(script.has_value());
        writeFile(home / launch.scriptFile, *script);
        if (!launch.startupFile.empty())
            writeFile(home / launch.startupFile, substitute(launch.startupText));

#ifdef _WIN32
        // CreateProcess searches PATH itself, and Process_win32 does not quote a program path, which
        // "C:\Program Files\PowerShell\7\pwsh.exe" would need.
        auto const& programText = launch.program;
#else
        auto const programText = program.string();
#endif
        auto exec = vtpty::Process::ExecInfo { .program = programText,
                                               .arguments = {},
                                               .workingDirectory = home.path(),
                                               .env = isolatedEnvironment(homeText),
                                               .removedEnvironment = userShellSettings() };
        for (auto const& argument: launch.arguments)
            exec.arguments.push_back(substitute(argument));
        return exec;
    }

    core::testing::ScopedTempDir _home;
    vtpty::Process _process;
    vtbackend::MockTerm<> _terminal;
    std::string _transcript;
    Watchdog _watchdog; // last: disarmed and joined before anything it touches goes away
};

/// @return @p launch's shell, started; SKIPs when it is not installed.
[[nodiscard]] std::unique_ptr<ShellSession> start(ShellLaunch const& launch)
{
#ifdef _WIN32
    if (launch.shell != "pwsh")
        SKIP(launch.shell << " is not run on Windows");
#endif
    auto const program = findOnPath(launch.program);
    if (!program)
        SKIP(launch.program << " is not installed");
    return std::make_unique<ShellSession>(launch, *program);
}

/// Waits until the shell has drawn a prompt to its end (OSC 133;B) after @p from bytes of transcript.
void awaitPromptEnd(ShellSession& session, size_t from = 0)
{
    session.pumpUntil(
        [from](std::string_view text) { return kindsOf(osc133Marks(text.substr(from))).contains('B'); },
        "the end of a prompt (OSC 133;B)");
}

/// Types @p launch's command at the prompt the shell shows, and checks what it told the terminal from
/// then on: exactly one C carrying the command line, exactly one D with exit status 0, and the next
/// prompt's A and B, in that order -- repaints of the waiting prompt (A, B) aside -- and the command
/// block the terminal recorded from them.
void checkCommandCycle(ShellSession& session, ShellLaunch const& launch)
{
    auto const typedAt = session.transcript().size();
    session.type(launch.command + "\r");
    session.pumpUntil(
        [typedAt](std::string_view text) {
            auto const kinds = kindsOf(osc133Marks(text.substr(typedAt)));
            auto const finished = kinds.find('D');
            return finished != std::string::npos && kinds.find('B', finished) != std::string::npos;
        },
        "the command's end (OSC 133;D) and the next prompt's end (OSC 133;B)");

    auto const transcript = std::string_view { session.transcript() };
    INFO("what the shell wrote: " << core::escape(transcript));
    auto const marks = osc133Marks(transcript.substr(typedAt));
    auto const kinds = kindsOf(marks);
    auto const commandStart = kinds.find('C');
    REQUIRE(commandStart != std::string::npos);
    CHECK(kinds.substr(0, commandStart).find_first_not_of("AB") == std::string::npos);
    REQUIRE(kinds.substr(commandStart) == "CDAB");
    CHECK(marks[commandStart + 1].params == "0");

    if (launch.commandLineSource == vtbackend::CommandLineSource::Reported)
    {
        constexpr auto Key = "cmdline_url="sv;
        auto const params = std::string_view { marks[commandStart].params };
        REQUIRE(params.starts_with(Key));
        CHECK(core::unescapeURL(params.substr(Key.size())) == launch.command);
    }

    auto const* const record = session.terminal().terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine == launch.command);
    CHECK(record->commandLineSource == launch.commandLineSource);
    CHECK(record->exitCode == std::optional { 0 });
}

/// What a script's percent-encoder must make of a command line: one %XX per UTF-8 byte for everything
/// but A-Z a-z 0-9 / . _ ~ -, upper-case hex -- byte for byte what fish's own encoder produces.
constexpr auto EncodingCases = std::array {
    std::pair { "ls"sv, "ls"sv },
    std::pair { "a b"sv, "a%20b"sv },
    std::pair { "A-Z_a.z~/09"sv, "A-Z_a.z~/09"sv },
    std::pair { "x;y=z"sv, "x%3By%3Dz"sv },
    std::pair { "50%"sv, "50%25"sv },
    std::pair { "\"'$`\\"sv, "%22%27%24%60%5C"sv },
    std::pair { "\xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80"sv, "%C3%BC%E2%82%AC%F0%9F%98%80"sv },
    std::pair { "a\nb\tc"sv, "a%0Ab%09c"sv },
    std::pair { "\x1b]0;x\x07"sv, "%1B%5D0%3Bx%07"sv },
    std::pair { "\xff\xfe"sv, "%FF%FE"sv },
};

/// Runs @p launch's shell until it has printed a line ending in ']' -- a script function's result in
/// brackets, which the caller's arguments ask for. SKIPs as start() does, so call it outside an
/// assertion: Catch2 reports a SKIP inside one as a failure.
/// @param launch The shell's row, its arguments replaced by the caller.
/// @return What the shell printed.
[[nodiscard]] std::string printedBy(ShellLaunch const& launch)
{
    auto const session = start(launch);
    session->pumpUntil([](std::string_view text) { return text.contains("]\r\n"); }, "the bracketed result");
    return session->transcript();
}

} // namespace

TEST_CASE("osc133Marks reads both terminators and skips a cut-off sequence", "[shell-integration]")
{
    auto const marks = osc133Marks("x\x1b]133;A\x07y\x1b]133;D;0\x1b\\z\x1b]133;C;cmdline_url=l");
    CHECK(kindsOf(marks) == "AD");
    CHECK(marks[1].params == "0");
}

TEST_CASE("bash integration reports prompts, the command line and its end",
          "[shell-integration][shells][bash]")
{
    auto const session = start(launchFor("bash"));
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launchFor("bash"));
}

TEST_CASE("bash integration sourced twice still marks everything once", "[shell-integration][shells][bash]")
{
    auto launch = launchFor("bash");
    launch.startupText += "source '${SCRIPT}'\n";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launch);
}

TEST_CASE("bash integration installs its hooks beside a bash-preexec loaded before it",
          "[shell-integration][shells][bash]")
{
    // atuin, starship and others bring a bash-preexec of their own, which a .bashrc may source first.
    // Its duplicate-inclusion guard then returns from the copy the script carries -- and from anything
    // that follows that copy. The one loaded first here is that copy and nothing else, cut out of the
    // script from its first line to its last, the only `fi;` at the start of a line; the prompt says
    // it loaded.
    auto launch = launchFor("bash");
    launch.startupText = "PS1='$ '\n"
                         "sed -n '/^# bash-preexec.sh --/,/^fi;$/p' '${SCRIPT}' >'${HOME}/bash-preexec.sh'\n"
                         "source '${HOME}/bash-preexec.sh'\n"
                         "[[ -n $bash_preexec_imported ]] && PS1='preloaded$ '\n"
                         "source '${SCRIPT}'\n";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    CHECK(session->transcript().contains("preloaded$ "));
    checkCommandCycle(*session, launch);
}

TEST_CASE("bash integration survives a prompt framework that rebuilds PS1 later",
          "[shell-integration][shells][bash]")
{
    // A framework sourced after the integration, rebuilding PS1 in a precmd hook of its own the way
    // starship and oh-my-bash do. The mark moves behind it, which counts from the second prompt on.
    auto launch = launchFor("bash");
    launch.startupText += "__framework_prompt() { PS1='fw$ '; }\nprecmd_functions+=(__framework_prompt)\n";
    auto const session = start(launch);
    session->pumpUntil([](std::string_view text) { return text.contains("fw$ "); }, "the framework's prompt");
    auto const emptyLineAt = session->transcript().size();
    session->type("\r");
    awaitPromptEnd(*session, emptyLineAt);
    checkCommandCycle(*session, launch);
}

TEST_CASE("bash integration percent-encodes the command line byte by byte",
          "[shell-integration][shells][bash]")
{
    for (auto const& [input, encoded]: EncodingCases)
    {
        INFO("input: " << core::escape(input));
        auto launch = launchFor("bash");
        launch.arguments = { "--noprofile",
                             "--norc",
                             "-c",
                             "source \"$1\"; printf '[%s]\\n' \"$(__contour_percent_encode \"$2\")\"",
                             "bash",
                             "${SCRIPT}",
                             std::string { input } };
        auto const printed = printedBy(launch);
        CHECK(printed.contains(std::format("[{}]", encoded)));
    }
}
