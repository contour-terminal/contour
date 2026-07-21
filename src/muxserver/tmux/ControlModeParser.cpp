// SPDX-License-Identifier: Apache-2.0
#include <muxserver/tmux/ControlModeParser.h>

#include <array>
#include <charconv>

namespace muxserver::tmux
{

namespace
{
    /// Consumes tokens off the front of a notification's argument tail.
    struct FieldReader
    {
        std::string_view rest;

        /// The next space-delimited token (empty once exhausted).
        std::string_view token()
        {
            while (rest.starts_with(' '))
                rest.remove_prefix(1);
            auto const end = rest.find(' ');
            auto const word = rest.substr(0, end);
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            return word;
        }

        /// A decimal number, optionally behind a sigil (`%5`, `@2`, `$0`).
        template <typename T>
        std::optional<T> number(char sigil = '\0')
        {
            auto word = token();
            if (sigil != '\0')
            {
                if (!word.starts_with(sigil))
                    return std::nullopt;
                word.remove_prefix(1);
            }
            auto value = T {};
            auto const [ptr, ec] = std::from_chars(word.data(), word.data() + word.size(), value);
            if (ec != std::errc {} || ptr != word.data() + word.size() || word.empty())
                return std::nullopt;
            return value;
        }
    };

    [[nodiscard]] ControlEvent parseGuard(std::string_view tail, bool isBegin, bool isError)
    {
        auto fields = FieldReader { tail };
        auto const time = fields.number<int64_t>().value_or(0);
        auto const number = fields.number<uint32_t>().value_or(0);
        auto const flags = fields.number<uint32_t>().value_or(0);
        if (isBegin)
            return GuardBegin { .time = time, .number = number, .flags = flags };
        return GuardEnd { .time = time, .number = number, .flags = flags, .isError = isError };
    }

    [[nodiscard]] ControlEvent parseOutput(std::string_view tail)
    {
        auto fields = FieldReader { tail };
        auto const pane = fields.number<uint64_t>('%');
        if (!pane)
            return UnknownNotification { std::string { tail } };
        return OutputEvent { .pane = *pane, .bytes = unescapeOutput(fields.rest), .ageMs = std::nullopt };
    }

    [[nodiscard]] ControlEvent parseExtendedOutput(std::string_view tail)
    {
        // ONE age field before " : "; the payload follows the separator
        // (control.c:653-658 — future fields would land before the colon).
        auto fields = FieldReader { tail };
        auto const pane = fields.number<uint64_t>('%');
        auto const age = fields.number<uint64_t>();
        auto const separator = fields.rest.find(": ");
        auto const payload =
            separator == std::string_view::npos ? fields.rest : fields.rest.substr(separator + 2);
        if (!pane)
            return UnknownNotification { std::string { tail } };
        return OutputEvent { .pane = *pane, .bytes = unescapeOutput(payload), .ageMs = age };
    }

    [[nodiscard]] ControlEvent parseLayoutChange(std::string_view tail)
    {
        auto fields = FieldReader { tail };
        auto const window = fields.number<uint64_t>('@');
        auto const layout = fields.token();
        if (!window)
            return UnknownNotification { std::string { tail } };
        return LayoutChangeEvent { .window = *window, .layout = std::string { layout } };
    }

    [[nodiscard]] ControlEvent parseWindowAdd(std::string_view tail)
    {
        auto fields = FieldReader { tail };
        auto const window = fields.number<uint64_t>('@');
        if (!window)
            return UnknownNotification { std::string { tail } };
        return WindowAddEvent { .window = *window };
    }

    [[nodiscard]] ControlEvent parseWindowClose(std::string_view tail)
    {
        auto fields = FieldReader { tail };
        auto const window = fields.number<uint64_t>('@');
        if (!window)
            return UnknownNotification { std::string { tail } };
        return WindowCloseEvent { .window = *window };
    }

    [[nodiscard]] ControlEvent parseWindowRenamed(std::string_view tail)
    {
        auto fields = FieldReader { tail };
        auto const window = fields.number<uint64_t>('@');
        if (!window)
            return UnknownNotification { std::string { tail } };
        return WindowRenamedEvent { .window = *window, .name = std::string { fields.rest } };
    }

    [[nodiscard]] ControlEvent parseSessionChanged(std::string_view tail)
    {
        auto fields = FieldReader { tail };
        auto const session = fields.number<uint64_t>('$');
        if (!session)
            return UnknownNotification { std::string { tail } };
        return SessionChangedEvent { .session = *session, .name = std::string { fields.rest } };
    }

    [[nodiscard]] ControlEvent parsePause(std::string_view tail)
    {
        auto fields = FieldReader { tail };
        auto const pane = fields.number<uint64_t>('%');
        if (!pane)
            return UnknownNotification { std::string { tail } };
        return PauseEvent { .pane = *pane, .paused = true };
    }

    [[nodiscard]] ControlEvent parseContinue(std::string_view tail)
    {
        auto fields = FieldReader { tail };
        auto const pane = fields.number<uint64_t>('%');
        if (!pane)
            return UnknownNotification { std::string { tail } };
        return PauseEvent { .pane = *pane, .paused = false };
    }

    [[nodiscard]] ControlEvent parseExit(std::string_view tail)
    {
        return ExitEvent { .reason = std::string { tail } };
    }

    /// One row of the notification table: the verb (with `%` and trailing
    /// space handling done by the dispatcher) and its field parser.
    struct NotificationRow
    {
        std::string_view name;
        ControlEvent (*parse)(std::string_view tail);
    };

    constexpr auto NotificationTable = std::array {
        NotificationRow { "begin", [](std::string_view t) { return parseGuard(t, true, false); } },
        NotificationRow { "end", [](std::string_view t) { return parseGuard(t, false, false); } },
        NotificationRow { "error", [](std::string_view t) { return parseGuard(t, false, true); } },
        NotificationRow { "output", parseOutput },
        NotificationRow { "extended-output", parseExtendedOutput },
        NotificationRow { "layout-change", parseLayoutChange },
        NotificationRow { "window-add", parseWindowAdd },
        NotificationRow { "window-close", parseWindowClose },
        NotificationRow { "unlinked-window-close", parseWindowClose },
        NotificationRow { "window-renamed", parseWindowRenamed },
        NotificationRow { "session-changed", parseSessionChanged },
        NotificationRow { "pause", parsePause },
        NotificationRow { "continue", parseContinue },
        NotificationRow { "exit", parseExit },
    };
} // namespace

std::string unescapeOutput(std::string_view escaped)
{
    auto out = std::string {};
    out.reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size(); ++i)
    {
        auto const isOctal = [&](std::size_t at) {
            return at < escaped.size() && escaped[at] >= '0' && escaped[at] <= '7';
        };
        if (escaped[i] == '\\' && isOctal(i + 1) && isOctal(i + 2) && isOctal(i + 3))
        {
            auto const value =
                ((escaped[i + 1] - '0') * 64) + ((escaped[i + 2] - '0') * 8) + (escaped[i + 3] - '0');
            out += static_cast<char>(value);
            i += 3;
            continue;
        }
        out += escaped[i];
    }
    return out;
}

ControlEvent classifyLine(std::string_view line)
{
    if (!line.starts_with('%'))
        return BodyLine { std::string { line } };

    auto const body = line.substr(1);
    for (auto const& row: NotificationTable)
    {
        if (!body.starts_with(row.name))
            continue;
        auto const tail = body.substr(row.name.size());
        // The verb must end here: "%end" must not swallow "%endless-things".
        if (!tail.empty() && !tail.starts_with(' '))
            continue;
        return row.parse(tail.starts_with(' ') ? tail.substr(1) : tail);
    }
    return UnknownNotification { std::string { line } };
}

} // namespace muxserver::tmux
