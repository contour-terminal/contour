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
// run on Windows, where `bash` on PATH may be WSL's launcher; PowerShell runs wherever it is found, on
// Windows through ConPTY.

#include <contour/cli/ShellIntegration.hpp>

#include <vtbackend/core/LineFlags.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/ProgramLookup.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <core/Environment.hpp>
#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/PathUtils.hpp>
#include <core/platform/SystemInfo.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace
{

/// The terminal's size, for the PTY and for the Terminal reading it alike.
constexpr auto TerminalSize =
    vtbackend::PageSize { .lines = vtbackend::LineCount(24), .columns = vtbackend::ColumnCount(80) };

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

/// The command line typed at tcsh's prompt: PosixCommand without its four-byte character. tcsh's line
/// editor draws a character above U+FFFF as text ("\U+1F600", NLSClassify() in its tc.nls.c), and the
/// terminal reads tcsh's command line back off the screen.
constexpr auto TcshCommand = "echo \"a  b;c\" 'd\"e' 50% \xc3\xbc\xe2\x82\xac"sv;

/// The command line typed at PowerShell's prompt: the same characters as PosixCommand.
constexpr auto PwshCommand = "Write-Output \"a  b;c\" 'd\"e' 50% \xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80"sv;

#ifdef _WIN32
/// PowerShell's executable, as PATH has it.
constexpr auto PwshProgram = "pwsh.exe"sv;
/// A native program that exits 3, as typed at PowerShell's prompt.
constexpr auto ExitThreeCommand = "cmd /d /c exit 3"sv;
#else
/// PowerShell's executable, as PATH has it.
constexpr auto PwshProgram = "pwsh"sv;
/// A native program that exits 3, as typed at PowerShell's prompt.
constexpr auto ExitThreeCommand = "sh -c 'exit 3'"sv;
#endif

/// What the PowerShell start-up file holds before the script: PSReadLine kept away from the user's
/// history file, neither writing to it nor predicting from it; and a prompt of its own, as the other
/// shells get one, which -- unlike PowerShell's -- names no directory, and tells by its '!' whether the
/// command before it failed.
constexpr auto PwshStartup = "Set-PSReadLineOption -HistorySaveStyle SaveNothing -PredictionSource None\n"
                             "function prompt { if ($?) { 'PS> ' } else { 'PS!> ' } }\n"sv;

/// One OSC sequence found in a byte stream.
struct OscPayload
{
    std::string_view text; ///< What follows the sequence's introducer, up to its terminator.
    char terminator {};    ///< BEL, or the ESC that opens ST -- or any other sequence, which ends it too.
};

/// @return Every sequence in @p bytes that opens with @p introducer, in order, whichever terminator (ST or
///         BEL) ends it. One the end of @p bytes cuts off is not counted.
[[nodiscard]] std::vector<OscPayload> oscPayloads(std::string_view bytes, std::string_view introducer)
{
    auto payloads = std::vector<OscPayload> {};
    for (auto const piece: std::views::split(bytes, introducer) | std::views::drop(1))
    {
        auto const body = std::string_view { piece.begin(), piece.end() };
        auto const end = body.find_first_of("\x07\x1b"sv);
        if (end != std::string_view::npos)
            payloads.push_back(OscPayload { .text = body.substr(0, end), .terminator = body[end] });
    }
    return payloads;
}

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
    auto marks = std::vector<Mark> {};
    for (auto const& payload: oscPayloads(bytes, "\x1b]133;"sv))
    {
        auto const text = payload.text;
        if (text.empty())
            continue;
        marks.push_back(
            Mark { .kind = text.front(), .params = std::string { text.size() > 2 ? text.substr(2) : ""sv } });
    }
    return marks;
}

/// @return The kinds of @p marks as one string, e.g. "ABCDAB".
[[nodiscard]] std::string kindsOf(std::vector<Mark> const& marks)
{
    return std::ranges::to<std::string>(marks | std::views::transform(&Mark::kind));
}

/// @return Whether @p text holds every one of @p pieces, each somewhere after the one before it.
[[nodiscard]] bool holdsInOrder(std::string_view text, std::span<std::string_view const> pieces)
{
    auto from = std::size_t { 0 };
    for (auto const piece: pieces)
    {
        auto const at = text.find(piece, from);
        if (at == std::string_view::npos)
            return false;
        from = at + piece.size();
    }
    return true;
}

/// How one shell is started for these tests: with its integration script sourced, and nothing of
/// the user's -- a private HOME holds the start-up file that sources it.
struct ShellLaunch
{
    std::string shell;                  ///< The integration table's name for it.
    std::string program;                ///< The executable looked up on PATH.
    std::string scriptFile;             ///< Where in HOME the script goes; PowerShell runs only *.ps1.
    std::string startupFile;            ///< A start-up file written into HOME, by relative path.
    std::string startupText;            ///< Its contents. ${SCRIPT} and ${HOME} stand for those paths.
    std::vector<std::string> arguments; ///< The command-line arguments, with the same substitutions.
    std::string command;                ///< The command line typed at the prompt.
    vtbackend::CommandLineSource commandLineSource {}; ///< How the terminal learns that command line.
    /// The command line that puts the shell into AwkwardDirectory, whose name the shell expands from "cwd*".
    std::string enterDirectory = "cd cwd*";
};

/// @return Every shell the integration scripts target, and how each is started here.
[[nodiscard]] std::vector<ShellLaunch> const& shellLaunches()
{
    static auto const launches = std::vector<ShellLaunch> {
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
        ShellLaunch {
            .shell = "zsh",
            .program = "zsh",
            .scriptFile = "shell-integration.zsh",
            .startupFile = ".zshrc",
            .startupText = "PROMPT='%# '\nsource '${SCRIPT}'\n",
            .arguments = { "-d", "-i" }, // -d: no /etc/zshrc; ZDOTDIR points zsh at HOME's .zshrc
            .command = std::string { PosixCommand },
            .commandLineSource = vtbackend::CommandLineSource::Reported,
        },
        ShellLaunch {
            .shell = "fish",
            .program = "fish",
            .scriptFile = "shell-integration.fish",
            .startupFile = ".config/fish/config.fish", // XDG_CONFIG_HOME is HOME/.config
            .startupText =
                "set -g fish_greeting\nfunction fish_prompt\n    printf '> '\nend\nsource '${SCRIPT}'\n",
            .arguments = { "-i" },
            .command = std::string { PosixCommand },
            .commandLineSource = vtbackend::CommandLineSource::Reported,
        },
        ShellLaunch {
            .shell = "tcsh",
            .program = "tcsh",
            .scriptFile = "shell-integration.tcsh",
            .startupFile = ".tcshrc",
            .startupText = "set prompt = \"%# \"\nsource '${SCRIPT}'\n",
            .arguments = { "-i" },
            .command = std::string { TcshCommand },
            .commandLineSource = vtbackend::CommandLineSource::Recovered, // tcsh cannot report it
            // tcsh 6.24.14 and later cannot cd into a name holding a character above U+FFFF, which
            // AwkwardDirectory does: its cd hands the system call the name cut at that character. So
            // sh enters the directory, and a new tcsh starts in it.
            .enterDirectory = "sh -c 'cd cwd* && exec tcsh -i'",
        },
        ShellLaunch {
            .shell = "pwsh",
            .program = std::string { PwshProgram },
            .scriptFile = "shell-integration.ps1", // PowerShell runs only *.ps1 files
            .startupFile = "startup.ps1",
            .startupText = std::string { PwshStartup } + ". '${SCRIPT}'\n",
            .arguments = { "-NoLogo", "-NoProfile", "-NoExit", "-Command", ". '${HOME}/startup.ps1'" },
            .command = std::string { PwshCommand },
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

/// @return Where @p program is on PATH, or nullopt when it is not.
[[nodiscard]] std::optional<std::filesystem::path> findOnPath(std::string_view program)
{
    auto const searchPath = core::defaultEnvironment().get("PATH").value_or(std::string {});
    auto const resolved =
        vtpty::resolveExecutablePath(program, searchPath, core::platform::NativeFileSystem::instance());
    if (resolved == program)
        return std::nullopt;
    return core::pathFromTrustedUtf8(resolved);
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
///         start-up files; a UTF-8 locale; a TERM every shell knows; and a stale HOSTNAME and HOST.
[[nodiscard]] vtpty::Process::Environment isolatedEnvironment(std::string const& home)
{
    return vtpty::Process::Environment {
        { "HOME", home },
        // A HOSTNAME and a HOST naming another machine, as a login carried over from one leaves them: bash
        // keeps the one and zsh the other over gethostname(2), and an integration that reported either would
        // make every directory a foreign one. (tcsh sets HOST itself, whatever it inherits.)
        { "HOSTNAME", "stale-hostname.invalid" },
        { "HOST", "stale-hostname.invalid" },
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
///         or sh (ENV) sources, prompt and history settings that would decide what it marks -- the
///         cases about history settings set their own -- and the names of the terminal it runs in, for
///         which a fish before 4.0 reports the working directory itself (TERM_PROGRAM, VTE_VERSION).
[[nodiscard]] std::vector<std::string> userShellSettings()
{
    return { "BASH_ENV",    "ENV",        "PROMPT_COMMAND", "PS1",
             "HISTCONTROL", "HISTIGNORE", "TERM_PROGRAM",   "VTE_VERSION" };
}

/// One step of ending a shell: how it is asked, and how long it gets to end before the next step.
struct TeardownStep
{
    vtpty::Process::TerminationHint hint {}; ///< How the shell is asked to end.
    std::chrono::milliseconds grace {};      ///< How long it then has.
};

/// How a shell is ended, in order. The hangup's grace covers a PowerShell still starting .NET.
constexpr auto TeardownSteps = std::array {
    TeardownStep { .hint = vtpty::Process::TerminationHint::Hangup, .grace = std::chrono::seconds { 5 } },
    TeardownStep { .hint = vtpty::Process::TerminationHint::Normal, .grace = std::chrono::seconds { 2 } },
    TeardownStep { .hint = vtpty::Process::TerminationHint::Kill, .grace = std::chrono::seconds { 2 } },
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
        _process { vtpty::testing::unplacedProcess(prepare(launch, program, _home), TerminalSize) },
        _terminal { TerminalSize }
    {
        REQUIRE(_process.start().has_value());
    }

    ShellSession(ShellSession const&) = delete;
    ShellSession& operator=(ShellSession const&) = delete;
    ShellSession(ShellSession&&) = delete;
    ShellSession& operator=(ShellSession&&) = delete;

    /// Ends the shell and reaps it, whether or not the test passed: ~Process would otherwise wait for a
    /// shell nobody asked to end. As a closed terminal does: the PTY closes, which ends a shell reading
    /// it, and a hangup follows. A shell that does neither -- one ignoring the hangup while it runs a
    /// command, or deferring it to a point it never reaches -- is then terminated, and at last killed,
    /// each after a grace period, so teardown never waits unbounded.
    ~ShellSession()
    {
        _process.close();
        for (auto const& [hint, grace]: TeardownSteps)
        {
            _process.terminate(hint);
            if (endsWithin(grace))
                return;
        }
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

    /// @return The shell's private HOME, which it starts in.
    [[nodiscard]] std::filesystem::path const& home() const noexcept { return _home.path(); }

  private:
    /// @return Whether the shell has ended -- and been reaped -- within @p grace.
    [[nodiscard]] bool endsWithin(std::chrono::milliseconds grace) const
    {
        constexpr auto PollInterval = std::chrono::milliseconds { 10 };
        auto const& clock = core::platform::defaultSteadyClock();
        auto const deadline = clock.now() + grace;
        while (!_process.checkStatus().has_value())
        {
            if (clock.now() >= deadline)
                return false;
            std::this_thread::sleep_for(PollInterval);
        }
        return true;
    }

    /// Puts the script and the start-up file into @p home.
    /// @return How to start the shell there.
    [[nodiscard]] static vtpty::Process::ExecInfo prepare(ShellLaunch const& launch,
                                                          std::filesystem::path const& program,
                                                          core::testing::ScopedTempDir const& home)
    {
        auto const homeText = home.string();
        auto const scriptText = core::platform::normalizePath(home / launch.scriptFile);
        auto const substitute = [&](std::string_view text) {
            return core::replaceVariables(text, [&](std::string_view name) -> std::string const& {
                return name == "SCRIPT" ? scriptText : homeText;
            });
        };

        auto const script = contour::cli::shellIntegrationScript(launch.shell);
        REQUIRE(script.has_value());
        writeFile(home / launch.scriptFile, *script);
        writeFile(home / launch.startupFile, substitute(launch.startupText));

        // The path found, as a user's configuration names it -- on Windows a path with a space in it,
        // "C:\Program Files\PowerShell\7\pwsh.exe".
        auto exec = vtpty::Process::ExecInfo { .program = core::pathToUtf8(program),
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

/// Waits until the shell has drawn its first prompt to its end, and checks that nothing before it said a
/// command had started: tcsh runs postcmd before every line of a file it sources.
void awaitFirstPromptEnd(ShellSession& session)
{
    awaitPromptEnd(session);
    CHECK_FALSE(kindsOf(osc133Marks(session.transcript())).contains('C'));
}

/// Types @p command at the prompt the shell shows, and waits for the command's end (OSC 133;D) and the
/// next prompt's end (OSC 133;B).
/// @param session The shell, at a prompt.
/// @param command What to type, without the Return that runs it.
/// @return Where in the transcript the command was typed.
size_t runCommand(ShellSession& session, std::string const& command)
{
    auto const typedAt = session.transcript().size();
    session.type(command + "\r");
    session.pumpUntil(
        [typedAt](std::string_view text) {
            auto const kinds = kindsOf(osc133Marks(text.substr(typedAt)));
            auto const finished = kinds.find('D');
            return finished != std::string::npos && kinds.find('B', finished) != std::string::npos;
        },
        "the command's end (OSC 133;D) and the next prompt's end (OSC 133;B)");
    return typedAt;
}

/// Types @p launch's command at the prompt the shell shows, and checks what it told the terminal from
/// then on: exactly one C carrying the command line, exactly one D with @p exitCode, and the next
/// prompt's A and B, in that order -- repaints of the waiting prompt (A, B) aside -- and the command
/// block the terminal recorded from them, its output starting below the prompt.
/// @param session  The shell, at a prompt.
/// @param launch   Its row, whose command is typed.
/// @param exitCode The exit status the command ends with.
void checkCommandCycle(ShellSession& session, ShellLaunch const& launch, int exitCode = 0)
{
    auto const typedAt = runCommand(session, launch.command);

    auto const transcript = std::string_view { session.transcript() };
    INFO("what the shell wrote: " << core::escape(transcript));
    auto const marks = osc133Marks(transcript.substr(typedAt));
    auto const kinds = kindsOf(marks);
    auto const commandStart = kinds.find('C');
    REQUIRE(commandStart != std::string::npos);
    CHECK(kinds.substr(0, commandStart).find_first_not_of("AB") == std::string::npos);
    REQUIRE(kinds.substr(commandStart) == "CDAB");
    CHECK(marks[commandStart + 1].params == std::to_string(exitCode));

    if (launch.commandLineSource == vtbackend::CommandLineSource::Reported)
    {
        constexpr auto Key = "cmdline_url="sv;
        auto const params = std::string_view { marks[commandStart].params };
        REQUIRE(params.starts_with(Key));
        CHECK(core::unescapeURL(params.substr(Key.size())) == launch.command);
    }
    else
        CHECK(marks[commandStart].params.empty());

    auto const* const record = session.terminal().terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == launch.command);
    CHECK(record->commandLineSource == launch.commandLineSource);
    CHECK(record->exitCode == std::optional { exitCode });

    // The command's output starts on the line below its one-line prompt: C went out after the newline
    // that ended the typed line, not before it. (The prompt line itself may carry an OutputStart: that of
    // a command before it that printed nothing.)
    auto const& screen = session.terminal().terminal.currentScreen();
    auto const promptLine = vtbackend::headOffsetOf(screen.grid(), *record);
    REQUIRE(promptLine.has_value());
    CHECK(screen.lineFlagsAt(*promptLine + vtbackend::LineOffset(1))
              .contains(vtbackend::LineFlag::OutputStart));
}

/// A character of four UTF-8 bytes, U+1F600, which the awkward directory's name holds.
constexpr auto FourByteCharacter = "\xf0\x9f\x98\x80"sv;

#ifdef _WIN32
/// A directory name no OSC 7 may carry raw, as far as Windows allows one: a space; "%41", which reads as
/// "A" once decoded; ';', which separates OSC parameters, '#', which opens a URL's fragment, and the
/// brackets and quote PowerShell reads specially; characters of two, three and four UTF-8 bytes. The
/// control characters the POSIX name holds Windows does not allow in a name.
constexpr auto AwkwardDirectory = "cwd a%41;#'[x]\xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80z"sv;
#else
/// A directory name no OSC 7 may carry raw: a space; "%41", which reads as "A" once decoded; characters
/// of two, three and four UTF-8 bytes; and an OSC 2 behind an ESC, which -- sent raw -- ends OSC 7 early
/// and retitles the terminal.
constexpr auto AwkwardDirectory = "cwd a%41\xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80\x1b]2;pwned\x07z"sv;
#endif

/// Whether @p session's shell can hold a four-byte UTF-8 character in a string: whether it can change into a
/// directory named with one.
/// @param session The shell, at a prompt in its HOME; it is left there.
/// @return Whether it can.
[[nodiscard]] bool holdsFourByteCharacters(ShellSession& session)
{
    auto const probe = std::string { "probe" } + std::string { FourByteCharacter };
    REQUIRE(std::filesystem::create_directory(session.home() / core::pathFromTrustedUtf8(probe)));
    auto const typedAt = runCommand(session, "cd probe* && cd ..");
    auto const marks = osc133Marks(std::string_view { session.transcript() }.substr(typedAt));
    auto const finished = std::ranges::find(marks, 'D', &Mark::kind);
    REQUIRE(finished != marks.end());
    return finished->params == "0";
}

/// Changes @p launch's shell into AwkwardDirectory, and checks that the terminal learned from OSC 7
/// exactly where the next command runs -- on this machine -- and nothing else from the name.
void checkWorkingDirectoryReport(ShellLaunch const& launch)
{
    auto const session = start(launch);
    // The host every shell names in its file:// URL, as Contour asks for it too.
    session->terminal().identity.hostname = core::platform::hostName();
    awaitPromptEnd(*session);

    // A shell that cannot hold a four-byte UTF-8 character in a string cannot change into a directory named
    // with one, whatever its integration would make of the name: tcsh 6.24.14 and 6.24.15 cut a string short
    // at one in a UTF-8 locale (the tcsh mailing list, "UTF-8 character problems", February 2025). Asked
    // first, so that such a shell is still tested on everything else the name holds -- and says so.
    auto name = std::string { AwkwardDirectory };
    if (!holdsFourByteCharacters(*session))
    {
        WARN(launch.shell
             << " cannot hold a four-byte UTF-8 character in a string: its directory leaves it out");
        name.erase(name.find(FourByteCharacter), FourByteCharacter.size());
    }

    // Named as UTF-8 on every platform: Windows reads a narrow path in its ANSI code page. And expected in
    // the form a file:// URL's path takes there too: "C:/Users/...", with forward slashes.
    auto const directory = session->home() / core::pathFromTrustedUtf8(name);
    REQUIRE(std::filesystem::create_directory(directory));
    auto const expected = core::platform::normalizePath(std::filesystem::canonical(directory));

    auto changeDirectory = launch;
    changeDirectory.command = launch.enterDirectory;
    checkCommandCycle(*session, changeDirectory);
    // Each a command of its own: bash sends no command line for one typed twice in a row.
    auto const nextRunsIn = [&](std::string command) {
        auto next = launch;
        next.command = std::move(command);
        checkCommandCycle(*session, next);
        auto const* const record = session->terminal().terminal.commandBlocks().lastFinished();
        REQUIRE(record != nullptr);
        return record->workingDirectory;
    };

    // A shell may name the directory the way it reached it, through a symbolic link: tcsh starts from HOME
    // as spelled, which on macOS lies under /var, a link to /private/var. Resolved, the name must still be
    // exactly the directory made here.
    auto const resolved = [](std::string const& path) {
        return core::platform::normalizePath(
            std::filesystem::weakly_canonical(core::pathFromTrustedUtf8(path)));
    };
    auto const here = nextRunsIn("echo here");
    CHECK(resolved(here.path) == expected);
    CHECK(here.locality == vtbackend::ContextLocality::Local);
    CHECK_FALSE(session->terminal().terminal.windowTitle().contains("pwned"));

    // The converse: to a terminal on another machine, the very same URL names a directory over there.
    session->terminal().identity.hostname = "elsewhere.invalid";
    auto const there = nextRunsIn("echo there");
    CHECK(resolved(there.path) == expected);
    CHECK(there.locality == vtbackend::ContextLocality::Foreign);

    // fish 4.0 and later report the directory themselves, ending OSC 7 in BEL where the scripts end it in
    // ST: one of the two speaks, never both.
    auto const reports = oscPayloads(session->transcript(), "\x1b]7;"sv);
    auto const byBel = [](OscPayload const& report) {
        return report.terminator == '\x07';
    };
    CHECK((std::ranges::all_of(reports, byBel) || std::ranges::none_of(reports, byBel)));
}

#ifndef _WIN32
/// Changes @p launch's shell into a directory through a symbolic link, and checks that the terminal learned
/// from OSC 7 the link's path -- the logical one, which the shell shows and `cd` extends -- rather than the
/// path of the directory it leads to. (Not on Windows, where creating a symbolic link takes a privilege.)
void checkSymlinkedDirectoryReport(ShellLaunch const& launch)
{
    auto const session = start(launch);
    session->terminal().identity.hostname = core::platform::hostName();
    // HOME with every symbolic link above it resolved, as the kernel names it.
    auto const home = std::filesystem::canonical(session->home());
    auto const target = home / "target directory";
    auto const link = home / "link directory";
    REQUIRE(std::filesystem::create_directory(target));
    auto error = std::error_code {};
    std::filesystem::create_directory_symlink(target, link, error);
    REQUIRE_FALSE(error);
    awaitPromptEnd(*session);

    auto changeDirectory = launch;
    changeDirectory.command = "cd link*";
    checkCommandCycle(*session, changeDirectory);
    auto next = launch;
    next.command = "echo here";
    checkCommandCycle(*session, next);

    auto const* const record = session->terminal().terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    // A shell may spell the directories above the link the way it reached them: tcsh starts from HOME as
    // spelled, which on macOS lies under /var, a link to /private/var, where the others ask the kernel. Those
    // resolved, the path must still end in the link rather than in the directory it leads to.
    auto const reported = core::pathFromTrustedUtf8(record->workingDirectory.path);
    auto const resolvedAbove = core::platform::normalizePath(
        std::filesystem::weakly_canonical(reported.parent_path()) / reported.filename());
    CHECK(resolvedAbove == core::platform::normalizePath(link));
    CHECK(resolvedAbove != core::platform::normalizePath(target));
    CHECK(record->workingDirectory.locality == vtbackend::ContextLocality::Local);
}
#endif

/// A working directory longer than the longest command line a script sends. It goes out whole, or not at
/// all: cut, it would name another directory.
constexpr auto LongPathBytes = [] {
    auto path = std::array<char, 5000> {};
    std::ranges::fill(path, 'd');
    path.front() = '/';
    return path;
}();

/// LongPathBytes as text.
constexpr auto LongPath = std::string_view { LongPathBytes.data(), LongPathBytes.size() };

/// What a script's percent-encoder must make of a command line: one %XX per UTF-8 byte for everything
/// but A-Z a-z 0-9 / . _ ~ -, upper-case hex -- byte for byte what fish's own encoder produces. Uncut, as
/// the working directory is encoded.
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
    std::pair { LongPath, LongPath },
};

/// @return The results in @p text, in order: what stands between the last '[' and the ']' that ends a line
///         -- a script function's result, in brackets, which no result holds itself.
[[nodiscard]] std::vector<std::string> bracketedResults(std::string_view text)
{
    auto results = std::vector<std::string> {};
    for (auto const piece: std::views::split(text, '\n'))
    {
        auto line = std::string_view { piece.begin(), piece.end() };
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (!line.ends_with(']'))
            continue;
        if (auto const open = line.rfind('['); open != std::string_view::npos)
            results.emplace_back(line.substr(open + 1, line.size() - open - 2));
    }
    return results;
}

/// Runs @p launch's shell -- once, for a whole table -- until it has printed @p count results, each a script
/// function's on a line of its own, in brackets, as the caller's arguments ask for. SKIPs as start() does,
/// so call it outside an assertion: Catch2 reports a SKIP inside one as a failure.
/// @param launch The shell's row, its arguments replaced by the caller.
/// @param count  How many results the arguments ask for.
/// @return The results, in order.
[[nodiscard]] std::vector<std::string> resultsPrintedBy(ShellLaunch const& launch, size_t count)
{
    auto const session = start(launch);
    session->pumpUntil([count](std::string_view text) { return bracketedResults(text).size() >= count; },
                       "every bracketed result");
    return bracketedResults(session->transcript());
}

/// The arguments that make a shell print, for each of the inputs in turn, "[<result>]" on a line of its own.
using ArgumentsFor = std::function<std::vector<std::string>(std::vector<std::string> const& inputs)>;

/// Checks @p shell's percent-encoder against EncodingCases, in one run of the shell.
/// @param shell        The shell's row name.
/// @param argumentsFor How to make the shell print each input's encoding.
/// @param handedOver   Whether an input is one this shell can be handed at all.
void checkPercentEncoding(std::string_view shell,
                          ArgumentsFor const& argumentsFor,
                          std::function<bool(std::string_view)> const& handedOver)
{
    auto inputs = std::vector<std::string> {};
    auto expected = std::vector<std::string> {};
    for (auto const& [input, encoded]: EncodingCases)
    {
        if (!handedOver(input))
            continue;
        inputs.emplace_back(input);
        expected.emplace_back(encoded);
    }
    auto launch = launchFor(shell);
    launch.arguments = argumentsFor(inputs);
    auto const printed = resultsPrintedBy(launch, inputs.size());
    REQUIRE(printed.size() == inputs.size());
    for (auto const& [input, encoded, result]: std::views::zip(inputs, expected, printed))
    {
        INFO("input: " << core::escape(input));
        CHECK(result == encoded);
    }
}

/// @return true: a POSIX shell can be handed any input, as an argument, byte for byte.
[[nodiscard]] bool everyInput(std::string_view /*input*/)
{
    return true;
}

/// @return @p head followed by @p tail.
[[nodiscard]] std::vector<std::string> concatenated(std::vector<std::string> head,
                                                    std::vector<std::string> const& tail)
{
    std::ranges::copy(tail, std::back_inserter(head));
    return head;
}

/// Checks that @p shell's prompt-end mark survives a prompt framework, sourced after the integration, that
/// rebuilds the prompt in a precmd hook of its own. The mark moves behind it, which counts from the second
/// prompt on.
/// @param shell           The shell's row name.
/// @param framework       The start-up lines that install the framework's hook.
/// @param frameworkPrompt What the framework's prompt shows.
void checkSurvivesPromptRebuild(std::string_view shell,
                                std::string_view framework,
                                std::string_view frameworkPrompt)
{
    auto launch = launchFor(shell);
    launch.startupText += framework;
    auto const session = start(launch);
    session->pumpUntil([frameworkPrompt](std::string_view text) { return text.contains(frameworkPrompt); },
                       "the framework's prompt");
    auto const emptyLineAt = session->transcript().size();
    session->type("\r");
    awaitPromptEnd(*session, emptyLineAt);
    checkCommandCycle(*session, launch);
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
    // The way starship and oh-my-bash do it.
    checkSurvivesPromptRebuild(
        "bash", "__framework_prompt() { PS1='fw$ '; }\nprecmd_functions+=(__framework_prompt)\n", "fw$ ");
}

namespace
{

/// Runs, in bash started with @p startup appended to its start-up file, each of @p before -- which history
/// records, and whose command lines bash reports -- and then the POSIX command line, which it does not
/// record. bash-preexec reads a command line back from history, where it then finds the one before: so
/// bash must send no command line for it, and the terminal reads it off the screen.
/// @param startup The start-up lines that keep history from recording the command.
/// @param before  The commands typed first.
void checkUnrecordedCommandReadOffTheScreen(std::string_view startup,
                                            std::vector<std::string_view> const& before)
{
    auto launch = launchFor("bash");
    launch.startupText += startup;
    auto const session = start(launch);
    awaitPromptEnd(*session);
    for (auto const command: before)
    {
        auto recorded = launch;
        recorded.command = command;
        checkCommandCycle(*session, recorded);
    }
    auto unrecorded = launch;
    unrecorded.commandLineSource = vtbackend::CommandLineSource::Recovered;
    checkCommandCycle(*session, unrecorded);
}

} // namespace

TEST_CASE("bash integration reports no command line that history did not record",
          "[shell-integration][shells][bash]")
{
    SECTION("HISTIGNORE")
    {
        checkUnrecordedCommandReadOffTheScreen("HISTIGNORE='echo *'\n", { "true" });
    }
    SECTION("set +o history")
    {
        checkUnrecordedCommandReadOffTheScreen("", { "true", "set +o history" });
    }
    SECTION("history -d, deleting the entry it made itself")
    {
        // The second command deletes itself, the second entry: history's last entry is the first command
        // again, which the third, unrecorded, must not be reported as.
        checkUnrecordedCommandReadOffTheScreen("HISTIGNORE='echo *'\n", { "true", "history -d 2" });
    }
    SECTION("HISTSIZE=0")
    {
        // No history at all: still a C, and so a D.
        checkUnrecordedCommandReadOffTheScreen("HISTSIZE=0\n", {});
    }
}

TEST_CASE("bash integration percent-encodes the command line byte by byte",
          "[shell-integration][shells][bash]")
{
    auto const argumentsFor = [](std::vector<std::string> const& inputs) {
        return concatenated(
            {
                "--noprofile",
                "--norc",
                "-c",
                R"(source "$1"; shift; for text; do __contour_percent_encode "$text"; printf '[%s]\n' "$__contour_encoded"; done)",
                "bash",
                "${SCRIPT}",
            },
            inputs);
    };
    checkPercentEncoding("bash", argumentsFor, everyInput);
}

TEST_CASE("bash integration reports the working directory as a file URL", "[shell-integration][shells][bash]")
{
    checkWorkingDirectoryReport(launchFor("bash"));
}

#ifndef _WIN32
TEST_CASE("bash integration reports a directory entered through a symbolic link by the link's path",
          "[shell-integration][shells][bash]")
{
    checkSymlinkedDirectoryReport(launchFor("bash"));
}
#endif

TEST_CASE("the shell harness ends a shell that ignores a hangup", "[shell-integration][shells][bash]")
{
    // As a system rc file's `trap '' HUP` makes it, and busy with a command rather than reading the
    // terminal, so that the closed PTY does not end it either. An interactive bash ignores SIGTERM as well,
    // so only a kill ends this one: the session's teardown must get there, and in bounded time.
    auto launch = launchFor("bash");
    launch.startupText += "trap '' HUP\n";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launch);
    auto const typedAt = session->transcript().size();
    session->type("while :; do sleep 1; done\r");
    session->pumpUntil(
        [typedAt](std::string_view text) { return kindsOf(osc133Marks(text.substr(typedAt))).contains('C'); },
        "the command's start (OSC 133;C)");
}

TEST_CASE("zsh integration reports prompts, the command line and its end", "[shell-integration][shells][zsh]")
{
    auto const session = start(launchFor("zsh"));
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launchFor("zsh"));
}

TEST_CASE("zsh integration survives a theme that rebuilds PROMPT later", "[shell-integration][shells][zsh]")
{
    // The way powerlevel10k and most oh-my-zsh themes do it.
    checkSurvivesPromptRebuild(
        "zsh", "_framework_prompt() { PROMPT='fw> ' }\nadd-zsh-hook precmd _framework_prompt\n", "fw> ");
}

TEST_CASE("zsh integration percent-encodes the command line byte by byte", "[shell-integration][shells][zsh]")
{
    auto const argumentsFor = [](std::vector<std::string> const& inputs) {
        return concatenated(
            {
                "-f",
                "-c",
                R"(source "$1"; shift; for text in "$@"; do _contour_percent_encode "$text"; print -r -- "[$REPLY]"; done)",
                "zsh",
                "${SCRIPT}",
            },
            inputs);
    };
    checkPercentEncoding("zsh", argumentsFor, everyInput);
}

TEST_CASE("zsh integration reports the working directory as a file URL", "[shell-integration][shells][zsh]")
{
    checkWorkingDirectoryReport(launchFor("zsh"));
}

#ifndef _WIN32
TEST_CASE("zsh integration reports a directory entered through a symbolic link by the link's path",
          "[shell-integration][shells][zsh]")
{
    checkSymlinkedDirectoryReport(launchFor("zsh"));
}
#endif

TEST_CASE("fish integration reports prompts, the command line and its end",
          "[shell-integration][shells][fish]")
{
    // Whichever fish this is: a fish that marks prompts itself must not get a second A, C or D, and
    // one that does not must get all four -- checkCommandCycle() demands exactly CDAB either way.
    auto const session = start(launchFor("fish"));
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launchFor("fish"));
}

TEST_CASE("fish integration marks the end of a prompt defined after it was sourced",
          "[shell-integration][shells][fish]")
{
    auto launch = launchFor("fish");
    launch.startupText =
        "set -g fish_greeting\nsource '${SCRIPT}'\nfunction fish_prompt\n    printf 'fw> '\nend\n";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launch);
}

TEST_CASE("fish integration marks the end of a prompt chosen while it runs",
          "[shell-integration][shells][fish]")
{
    // The command typed replaces the wrapped fish_prompt, as `fish_config prompt choose` does, so the
    // prompt that follows it -- whose B the cycle waits for -- is the new one.
    auto launch = launchFor("fish");
    launch.command = "function fish_prompt; printf '%s> ' new; end";
    auto const session = start(launch);
    awaitPromptEnd(*session);
    auto const typedAt = session->transcript().size();
    checkCommandCycle(*session, launch);
    CHECK(std::string_view { session->transcript() }.substr(typedAt).contains("new> "));
}

TEST_CASE("fish integration marks everything itself once fish's own marks are turned off",
          "[shell-integration][shells][fish]")
{
    // no-mark-prompt silences fish 4.0.6 and later; an older fish ignores a feature it does not know.
    auto launch = launchFor("fish");
    launch.arguments = { "--features", "no-mark-prompt", "-i" };
    auto const session = start(launch);
    awaitPromptEnd(*session);
    checkCommandCycle(*session, launch);
}

TEST_CASE("fish integration leaves to fish the marks fish sends itself", "[shell-integration][shells][fish]")
{
    struct Case
    {
        std::string_view version;
        std::string_view markPromptStatus; ///< `status test-feature mark-prompt`: 0 on, 1 off, 2 unknown.
        std::string_view native;           ///< What __contour_native_marks must print, space-joined.
    };
    constexpr auto Cases = std::array {
        Case { "3.7.1", "2", "" },
        Case { "4.0b1", "2", "A C D cwd" },
        Case { "4.0.0", "2", "A C D cwd" },
        Case { "4.0.1", "2", "A C D cmdline cwd" },
        Case { "4.0.6", "1", "cwd" },
        Case { "4.2.1", "0", "A C D cmdline cwd" },
        Case { "4.3.0", "0", "A C D cmdline B cwd" },
        Case { "4.3.0-12-gabcdef0", "0", "A C D cmdline B cwd" },
        Case { "4.10.0", "0", "A C D cmdline B cwd" },
    };
    // One fish for the whole table: the script, then each version and status in turn.
    auto launch = launchFor("fish");
    launch.arguments = {
        "-c",
        "source $argv[1]\n"
        "for i in (seq 2 2 (count $argv))\n"
        "    set -l marks (__contour_native_marks $argv[$i] $argv[(math $i + 1)])\n"
        "    echo \"[$marks]\"\n"
        "end",
        "${SCRIPT}",
    };
    for (auto const& testCase: Cases)
    {
        launch.arguments.emplace_back(testCase.version);
        launch.arguments.emplace_back(testCase.markPromptStatus);
    }
    auto const printed = resultsPrintedBy(launch, Cases.size());
    REQUIRE(printed.size() == Cases.size());
    for (auto const& [testCase, result]: std::views::zip(Cases, printed))
    {
        INFO("fish " << testCase.version << ", mark-prompt status " << testCase.markPromptStatus);
        CHECK(result == testCase.native);
    }
}

TEST_CASE("fish integration reports the working directory as a file URL", "[shell-integration][shells][fish]")
{
    // Whichever fish this is: 4.0 and later report it themselves, and the script must not as well. fish's
    // own window title names the directory raw (fish_title, prompt_pwd), which retitles the terminal from
    // a name like this one; that is fish's to fix, so it is silenced here.
    auto launch = launchFor("fish");
    launch.startupText += "function fish_title\nend\n";
    checkWorkingDirectoryReport(launch);
}

#ifndef _WIN32
TEST_CASE("fish integration reports a directory entered through a symbolic link by the link's path",
          "[shell-integration][shells][fish]")
{
    checkSymlinkedDirectoryReport(launchFor("fish"));
}
#endif

TEST_CASE("tcsh integration ends its prompt where typing begins", "[shell-integration][shells][tcsh]")
{
    // tcsh reports no command line, so the terminal reads it back off the screen from the ;B column
    // on: the record only matches the typed command if that column is exact.
    auto const session = start(launchFor("tcsh"));
    awaitFirstPromptEnd(*session);
    checkCommandCycle(*session, launchFor("tcsh"));
}

TEST_CASE("tcsh integration sourced twice still marks the prompt once", "[shell-integration][shells][tcsh]")
{
    auto launch = launchFor("tcsh");
    launch.startupText += "source '${SCRIPT}'\n";
    auto const session = start(launch);
    awaitFirstPromptEnd(*session);
    checkCommandCycle(*session, launch);
}

TEST_CASE("tcsh integration reports the working directory as a file URL", "[shell-integration][shells][tcsh]")
{
    checkWorkingDirectoryReport(launchFor("tcsh"));
}

#ifndef _WIN32
TEST_CASE("tcsh integration reports a directory entered through a symbolic link by the link's path",
          "[shell-integration][shells][tcsh]")
{
    checkSymlinkedDirectoryReport(launchFor("tcsh"));
}
#endif

TEST_CASE("tcsh integration sends no working directory once it is gone", "[shell-integration][shells][tcsh]")
{
    // pwd has no path to tell then, and says so on stderr: neither an OSC 7 with an empty path nor that
    // complaint, shown at every prompt, may reach the terminal.
    auto const launch = launchFor("tcsh");
    auto const session = start(launch);
    auto const host = core::platform::hostName();
    session->terminal().identity.hostname = host;
    REQUIRE(std::filesystem::create_directory(session->home() / "gone"));
    awaitPromptEnd(*session);

    auto const typeCommand = [&](std::string command) {
        auto next = launch;
        next.command = std::move(command);
        checkCommandCycle(*session, next);
    };
    typeCommand("cd gone");
    auto const removedAt = session->transcript().size();
    typeCommand("rmdir ../gone");
    typeCommand("echo here");

    auto const afterRemoval = std::string_view { session->transcript() }.substr(removedAt);
    INFO("what the shell wrote: " << core::escape(afterRemoval));
    CHECK_FALSE(afterRemoval.contains("pwd:"));
    for (auto const& report: oscPayloads(afterRemoval, "\x1b]7;"sv))
        CHECK(report.text != std::format("file://{}", host));
}

TEST_CASE("PowerShell integration reports prompts, the command line and its end",
          "[shell-integration][shells][pwsh]")
{
    // On Windows this runs through ConPTY, whose passing OSC 133 on is what vtpty_test's
    // ConPty.passesOsc133Through establishes. The command line is PSReadLine's, as it accepted it: C goes
    // out once it returns, after the newline, so the command's output starts below the prompt.
    auto const session = start(launchFor("pwsh"));
    awaitFirstPromptEnd(*session);
    checkCommandCycle(*session, launchFor("pwsh"));
}

TEST_CASE("PowerShell integration dot-sourced twice wraps once", "[shell-integration][shells][pwsh]")
{
    auto launch = launchFor("pwsh");
    launch.startupText += ". '${SCRIPT}'\n";
    auto const session = start(launch);
    awaitFirstPromptEnd(*session);
    checkCommandCycle(*session, launch);
}

TEST_CASE("PowerShell integration turns reflow off for a plain-text prompt too",
          "[shell-integration][shells][pwsh]")
{
    // PowerShell takes every DECSET and DECRST out of a prompt it renders as plain text, as it does
    // under NO_COLOR -- so the integration writes its own rather than return them in the prompt.
    auto launch = launchFor("pwsh");
    launch.startupText = "$PSStyle.OutputRendering = 'PlainText'\n" + launch.startupText;
    auto const session = start(launch);
    awaitFirstPromptEnd(*session);
    checkCommandCycle(*session, launch);

    // Reflow off for each prompt, and on for the command's output.
    constexpr auto Order = std::array {
        "\x1b[?2028l"sv, "\x1b]133;A"sv,  "\x1b[?2028h"sv, "\x1b]133;C"sv,
        "\x1b]133;D"sv,  "\x1b[?2028l"sv, "\x1b]133;A"sv,
    };
    INFO("what the shell wrote: " << core::escape(session->transcript()));
    CHECK(holdsInOrder(session->transcript(), Order));
}

TEST_CASE("PowerShell integration reports how a command ended", "[shell-integration][shells][pwsh]")
{
    auto const session = start(launchFor("pwsh"));
    awaitFirstPromptEnd(*session);
    auto const promptAfter = [&](ShellLaunch const& launch, int exitCode) {
        auto const typedAt = session->transcript().size();
        checkCommandCycle(*session, launch, exitCode);
        return session->transcript().substr(typedAt);
    };

    // A native program's exit status, from $LASTEXITCODE; and the prompt sees the $? it would have seen
    // without the integration.
    auto native = launchFor("pwsh");
    native.command = std::string { ExitThreeCommand };
    CHECK(promptAfter(native, 3).contains("PS!> "));

    // A PowerShell error: 1, whatever the native program before it left in $LASTEXITCODE.
    auto failing = launchFor("pwsh");
    failing.command = "Get-Item nosuchfile";
    CHECK(promptAfter(failing, 1).contains("PS!> "));

    // Success: 0, even with that $LASTEXITCODE still about.
    auto succeeding = launchFor("pwsh");
    succeeding.command = "Write-Output ok";
    CHECK(promptAfter(succeeding, 0).contains("PS> "));
}

TEST_CASE("PowerShell integration percent-encodes the command line byte by byte",
          "[shell-integration][shells][pwsh]")
{
    auto const argumentsFor = [](std::vector<std::string> const& inputs) {
        // Handed over in hex, which no quoting on the way can touch.
        auto hexInputs = std::string {};
        for (auto const& input: inputs)
            hexInputs += std::format(
                "{}'{}'", hexInputs.empty() ? "" : ", ", core::toHexString(std::string_view { input }));
        return std::vector<std::string> {
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            std::format(". '${{SCRIPT}}'; "
                        "foreach ($hex in @({})) {{ "
                        "$text = [Text.Encoding]::UTF8.GetString([Convert]::FromHexString($hex)); "
                        "'[' + (__Contour-PercentEncode $text) + ']' }}",
                        hexInputs),
        };
    };
    // A byte that no UTF-8 holds PSReadLine cannot hand the script: what it hands over is a .NET string.
    checkPercentEncoding(
        "pwsh", argumentsFor, [](std::string_view input) { return !input.contains('\xff'); });
}

TEST_CASE("PowerShell integration percent-encodes what no UTF-8 input holds, and a text it cuts",
          "[shell-integration][shells][pwsh]")
{
    // A lone surrogate, which a .NET string can hold, is U+FFFD's three bytes. A text longer than the
    // limit is cut at a byte, and so may end in part of a character.
    constexpr auto Command = ". '${SCRIPT}'; "
                             "'[' + (__Contour-PercentEncode ('a' + [char]0xD800 + 'b')) + ']'; "
                             "'[' + (__Contour-PercentEncode 'a b/c' 4) + ']'; "
                             "'[' + (__Contour-PercentEncode ('x' + [char]0xFC + 'y') 2) + ']'"sv;
    auto launch = launchFor("pwsh");
    launch.arguments = { "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", std::string { Command } };
    auto const printed = resultsPrintedBy(launch, 3);
    CHECK(printed == std::vector<std::string> { "a%EF%BF%BDb", "a%20b/", "x%C3" });
}

TEST_CASE("PowerShell integration reports the working directory as a file URL",
          "[shell-integration][shells][pwsh]")
{
    checkWorkingDirectoryReport(launchFor("pwsh"));
}

#ifndef _WIN32
TEST_CASE("PowerShell integration reports a directory entered through a symbolic link by the link's path",
          "[shell-integration][shells][pwsh]")
{
    checkSymlinkedDirectoryReport(launchFor("pwsh"));
}
#endif
