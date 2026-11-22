// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/FailureWords.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    /// What the daemon world's error codes tell the user. A run's failure carries the code its host call
    /// answered; the summary and the dashboard say these words, and the run's state line keeps the code.
    constexpr auto FailureWordsTable = std::array {
        FailureWords { .code = "not-granted"sv, .plain = "not granted — allow it in the consent dialog"sv },
        FailureWords { .code = "not-owned"sv, .plain = "it belongs to another extension"sv },
        FailureWords { .code = "bad-argument"sv,
                       .plain = "a program is missing or the request was refused"sv },
        FailureWords { .code = "limit"sv, .plain = "busy — try again"sv },
        FailureWords { .code = "too-many"sv, .plain = "too many agent panes — close one first"sv },
        FailureWords {
            .code = "program-missing"sv,
            .plain = "claude isn't installed or isn't on the daemon's PATH"sv,
            .detail =
                "name its executable under the extension's programs in daemon.extensions, such as claude: ~/.local/bin/claude"sv },
        FailureWords { .code = "not-a-directory"sv, .plain = "the worktree path isn't a directory"sv },
        FailureWords { .code = "gone"sv, .plain = "it's gone"sv },
        FailureWords {
            .code = "not-supported-yet"sv,
            .plain = "agents can't start here yet"sv,
            .detail = "a Flatpak or Windows build, or an older host, opens no agent tab in a worktree"sv },
        FailureWords { .code = "directory-gone"sv, .plain = "its directory is gone"sv },
        FailureWords { .code = "not-regular"sv, .plain = "not a regular file"sv },
        FailureWords { .code = "unreadable"sv, .plain = "it can't be read"sv },
        FailureWords { .code = "too-large"sv, .plain = "it is too large"sv },
        FailureWords { .code = "escapes"sv, .plain = "outside the worktree"sv },
    };
} // namespace

std::span<FailureWords const> failureWords() noexcept
{
    return FailureWordsTable;
}

FailureWords const* failureWordsOf(std::string_view why) noexcept
{
    auto const found = std::ranges::find(FailureWordsTable, why, &FailureWords::code);
    return found != FailureWordsTable.end() ? &*found : nullptr;
}

std::string_view plainFailure(std::string_view why) noexcept
{
    auto const* const row = failureWordsOf(why);
    return row ? row->plain : why;
}

} // namespace orchestrator
