// SPDX-License-Identifier: Apache-2.0
//
// A resolved argv as the shell command line vtpty::CommandCapture runs: every argument taken literally.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CommandLine.hpp>

using namespace vtextension;

namespace
{

/// Splits a command line as the MSVC runtime and CommandLineToArgvW do (the program name included,
/// which those treat the same way when it is quoted): 2n backslashes and a quote are n backslashes and
/// a quote that toggles quoting; 2n+1 backslashes and a quote are n backslashes and a literal quote;
/// other backslashes are literal; whitespace outside quotes separates arguments.
[[nodiscard]] std::vector<std::string> msvcrtSplit(std::string_view line)
{
    auto argv = std::vector<std::string> {};
    auto current = std::string {};
    auto inArgument = false;
    auto quoted = false;
    auto backslashes = std::size_t { 0 };
    for (auto const c: line)
    {
        if (c == '\\')
        {
            ++backslashes;
            inArgument = true;
            continue;
        }
        if (c == '"')
        {
            current.append(backslashes / 2, '\\');
            if (backslashes % 2 == 1)
                current += '"';
            else
                quoted = !quoted;
            backslashes = 0;
            inArgument = true;
            continue;
        }
        current.append(backslashes, '\\');
        backslashes = 0;
        if ((c == ' ' || c == '\t') && !quoted)
        {
            if (inArgument)
                argv.push_back(std::exchange(current, {}));
            inArgument = false;
            continue;
        }
        current += c;
        inArgument = true;
    }
    current.append(backslashes, '\\');
    if (inArgument)
        argv.push_back(current);
    return argv;
}

/// @return What cmd.exe hands the program: @p line without the outermost pair of quotes it strips.
[[nodiscard]] std::string_view cmdStripped(std::string_view line)
{
    REQUIRE(line.size() >= 2);
    REQUIRE(line.front() == '"');
    REQUIRE(line.back() == '"');
    return line.substr(1, line.size() - 2);
}

} // namespace

TEST_CASE("CommandLine.posix", "[vtextension]")
{
    auto const line = [](std::vector<std::string> const& argv) {
        return shellCommandLine(argv, ShellDialect::Posix).value();
    };
    CHECK(line({ "git", "status" }) == "'git' 'status'");
    CHECK(line({ "echo", "it's" }) == R"('echo' 'it'\''s')");
    // Nothing the shell would expand or split survives quoting.
    CHECK(line({ "echo", "$(rm -rf ~); `x` * a b\n|&;" }) == "'echo' '$(rm -rf ~); `x` * a b\n|&;'");
    CHECK(line({ "echo", "" }) == "'echo' ''");
}

TEST_CASE("CommandLine.windowsCmd", "[vtextension]")
{
    auto const line = [](std::vector<std::string> const& argv) {
        return shellCommandLine(argv, ShellDialect::WindowsCmd);
    };
    // The whole line is quoted once more: cmd strips the outermost pair of a line with several quotes.
    CHECK(line({ "git", "-C", "C:\\some dir", "status" }).value()
          == R"(""git" "-C" "C:\some dir" "status"")");
    // A directory with a trailing backslash, then a placeholder's value with spaces: the backslash
    // before the closing quote is doubled, so that it does not escape the quote and swallow the next
    // argument.
    auto const argv =
        std::vector<std::string> { "git", "-C", R"(C:\dir\)", "my file name.txt", R"(\\server\share\)" };
    auto const written = line(argv).value();
    CHECK(written == R"(""git" "-C" "C:\dir\\" "my file name.txt" "\\server\share\\"")");
    CHECK(msvcrtSplit(cmdStripped(written)) == argv);

    for (auto const* const unsafe:
         { "100%", "a&b", "a|b", "a\"b", "a^b", "!x!", "a<b", "a>b", "(a)", "a\nb", "a\tb" })
    {
        INFO(unsafe);
        CHECK(line({ "echo", unsafe }).error() == ExtensionError::ArgumentRejected);
    }
}
