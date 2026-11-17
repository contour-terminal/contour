// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshTerminfoInstall.hpp>

#include <algorithm>
#include <format>
#include <ranges>

namespace vtpty
{

namespace
{
    /// The remote script, as one line of `;`-joined commands (csh and tcsh reject a newline inside single
    /// quotes). It holds no `'` and no `\`, so it travels inside `sh -c '...'` verbatim. @c HEX and @c NAME
    /// are substituted.
    constexpr std::string_view ScriptTemplate =
        R"(command -v tic >/dev/null 2>&1 || exit 3; )"
        R"(t=$(mktemp -d 2>/dev/null) || exit 4; )"
        R"(c() { rm -rf "$t"; }; trap c EXIT; )"
        R"(LC_ALL=C awk -v h=HEX -v x=0123456789abcdef -v f=%c "BEGIN{for(i=1;i<length(h);i+=2)printf(f,(index(x,substr(h,i,1))-1)*16+index(x,substr(h,i+1,1))-1)}" >"$t/src" || exit 5; )"
        R"(tic -x -o "$t" "$t/src" >/dev/null || exit 6; )"
        R"(infocmp -x -A "$t" NAME >/dev/null 2>&1 || exit 7; )"
        R"(f=$(cd "$t" && ls -d */NAME 2>/dev/null | head -n 1); )"
        R"([ -n "$f" ] || exit 7; )"
        R"(d="$HOME/.terminfo/${f%/*}"; )"
        R"(mkdir -p "$d" && cp "$t/$f" "$d/.NAME.$$" && mv -f "$d/.NAME.$$" "$d/NAME" || exit 8; )"
        R"(echo CONTOUR-TERMINFO-INSTALLED)";

    [[nodiscard]] bool isValidEntryName(std::string_view name) noexcept
    {
        return !name.empty() && std::ranges::all_of(name, [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        });
    }

    /// Replaces every occurrence of @p placeholder in @p text by @p value.
    [[nodiscard]] std::string substitute(std::string text,
                                         std::string_view placeholder,
                                         std::string_view value)
    {
        auto pos = size_t { 0 };
        while ((pos = text.find(placeholder, pos)) != std::string::npos)
        {
            text.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
        return text;
    }
} // namespace

TerminfoInstall effectiveTerminfoInstall(TerminfoInstall option,
                                         std::optional<std::string_view> profileTerm,
                                         std::string_view entryName)
{
    if (option == TerminfoInstall::Off)
        return TerminfoInstall::Off;
    if (profileTerm && !profileTerm->empty() && *profileTerm != entryName)
        return TerminfoInstall::Off;
    return TerminfoInstall::Install;
}

std::expected<std::string, TerminfoCommandError> buildTerminfoInstallCommand(std::string_view entryName,
                                                                             std::string_view terminfoSource)
{
    if (!isValidEntryName(entryName))
        return std::unexpected(TerminfoCommandError::InvalidEntryName);
    if (terminfoSource.empty())
        return std::unexpected(TerminfoCommandError::EmptySource);

    auto hex = std::string {};
    hex.reserve(terminfoSource.size() * 2);
    for (auto const ch: terminfoSource)
    {
        auto const byte = static_cast<unsigned char>(ch);
        if (byte >= 0x80)
            return std::unexpected(TerminfoCommandError::NonAsciiSource);
        hex += std::format("{:02x}", byte);
    }

    // NAME first: it is validated to hold no placeholder text, and HEX only holds [0-9a-f].
    auto body = substitute(substitute(std::string { ScriptTemplate }, "NAME", entryName), "HEX", hex);
    auto command = std::format("sh -c '{}'", body);
    if (command.size() > MaxTerminfoCommandSize)
        return std::unexpected(TerminfoCommandError::TooLong);
    return command;
}

TerminfoInstallOutcome judgeTerminfoInstallOutput(std::string_view stdoutText)
{
    for (auto const line: std::views::split(stdoutText, '\n'))
    {
        auto text = std::string_view { line.begin(), line.end() };
        if (text.ends_with('\r'))
            text.remove_suffix(1);
        if (text == TerminfoInstalledMarker)
            return TerminfoInstallOutcome::Installed;
    }
    return TerminfoInstallOutcome::Failed;
}

TerminfoInstallOutcome judgeExpiredTerminfoRead(std::string_view stdoutText)
{
    return judgeTerminfoInstallOutput(stdoutText) == TerminfoInstallOutcome::Installed
               ? TerminfoInstallOutcome::Installed
               : TerminfoInstallOutcome::TimedOut;
}

TerminfoInstallOutcome judgeFailedTerminfoRead(std::string_view stdoutText, SshStepFailure failure)
{
    if (judgeTerminfoInstallOutput(stdoutText) == TerminfoInstallOutcome::Installed)
        return TerminfoInstallOutcome::Installed;
    return terminfoOutcomeOf(failure);
}

std::string chooseSshTerm(TerminfoInstallOutcome outcome,
                          std::string_view entryName,
                          std::string_view fallbackTerm)
{
    return std::string { outcome == TerminfoInstallOutcome::Installed ? entryName : fallbackTerm };
}

void appendTail(std::string& buffer, std::string_view chunk, size_t cap)
{
    if (chunk.size() >= cap)
    {
        buffer.assign(chunk.substr(chunk.size() - cap));
        return;
    }
    buffer.append(chunk);
    if (buffer.size() > cap)
        buffer.erase(0, buffer.size() - cap);
}

bool InMemorySshTerminfoHostMemo::hasFailed(std::string const& hostKey) const
{
    auto const lock = std::scoped_lock { _mutex };
    return _failed.contains(hostKey);
}

void InMemorySshTerminfoHostMemo::recordFailure(std::string const& hostKey)
{
    auto const lock = std::scoped_lock { _mutex };
    _failed.insert(hostKey);
}

std::string terminfoHostKey(std::string_view user, std::string_view host, int port)
{
    return std::format("{}@{}:{}", user, host, port);
}

} // namespace vtpty
