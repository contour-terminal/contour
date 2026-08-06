// SPDX-License-Identifier: Apache-2.0
#include <crispy/logsink.hpp>

#include <crispy/utils.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <format>
#include <functional>
#include <iostream>
#include <iterator>
#include <system_error>
#include <utility>

#ifndef _WIN32
    #include <unistd.h>
#else
    #include <process.h>
#endif

namespace logstore
{

namespace
{
    /// @return The current process id, as a plain number for the `[PID]` field.
    [[nodiscard]] int processId() noexcept
    {
#ifndef _WIN32
        return static_cast<int>(::getpid());
#else
        return ::_getpid();
#endif
    }

    /// A curated palette, so two categories are unlikely to collide in one terminal.
    constexpr auto CategoryColors = std::array<int, 23> {
        2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 150, 155, 159, 165, 170, 175, 180, 185, 190, 195, 200,
    };

    /// Appends `YYYY-MM-DD HH:MM:SS.uuuuuu` for the current LOCAL time.
    ///
    /// Formats the broken-down fields directly rather than going through put_time and a
    /// stringstream: this runs once per emitted line — once per PDU with the trace tier on —
    /// and constructing a stringstream is by far the most expensive thing on that path.
    /// (std::format's chrono support would be tidier still, but it renders UTC, and these
    /// timestamps have always been local.)
    /// @param out Receives the formatted stamp.
    void appendNowStamp(std::string& out)
    {
        auto const now = std::chrono::system_clock::now();
        auto const nowTimeT = std::chrono::system_clock::to_time_t(now);
        auto brokenDown = std::tm {};
#ifdef _WIN32
        localtime_s(&brokenDown, &nowTimeT);
#else
        localtime_r(&nowTimeT, &brokenDown);
#endif
        auto const micros =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1'000'000;
        std::format_to(std::back_inserter(out),
                       "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:06}",
                       brokenDown.tm_year + 1900,
                       brokenDown.tm_mon + 1,
                       brokenDown.tm_mday,
                       brokenDown.tm_hour,
                       brokenDown.tm_min,
                       brokenDown.tm_sec,
                       micros);
    }

    /// Lays one message out: the tag block, then the text, with continuation lines indented
    /// under the text column so a multi-line message reads as one entry.
    /// @param message The message being formatted.
    /// @param options The line layout.
    /// @param tagSgr SGR prefix for the tag block ("" when not colourising).
    /// @param messageSgr SGR prefix for the text.
    /// @param tag What goes in the trailing `[...]` of the tag block.
    /// @return The finished line (or lines), newline-terminated.
    [[nodiscard]] std::string layOut(MessageBuilder const& message,
                                     FormatterOptions const& options,
                                     std::string_view tagSgr,
                                     std::string_view messageSgr,
                                     std::string_view tag)
    {
        auto const sgrReset = std::string_view { options.colorize ? "\033[m" : "" };
        auto result = std::string {};
        result.reserve(message.text().size() + 64);

        auto const appendPrefix = [&] {
            result += tagSgr;
            if (options.showTimestamp)
            {
                result += '[';
                appendNowStamp(result);
                result += "] ";
            }
            if (options.showProcessId)
                std::format_to(std::back_inserter(result), "[{}] ", processId());
            result += '[';
            result += tag;
            result += ']';
            result += sgrReset;
            result += ' ';
        };

        auto const appendBody = [&](std::string_view line) {
            result += messageSgr;
            result += line;
            result += sgrReset;
            result += '\n';
        };

        // The overwhelmingly common case is one line; splitting it would allocate a vector to
        // produce a single element.
        if (!message.text().contains('\n'))
        {
            appendPrefix();
            appendBody(message.text());
            return result;
        }

        for (auto const [index, line]: crispy::views::enumerate(crispy::split(message.text(), '\n')))
        {
            if (index == 0)
                appendPrefix();
            else
                result += "        ";
            appendBody(line);
        }

        return result;
    }
} // namespace

std::optional<std::filesystem::path> parseLogFileSpec(std::string_view spec)
{
    // "-" is the tree's established spelling for a standard stream (see `contour capture --to`).
    if (spec.empty() || spec == "-")
        return std::nullopt;
    return std::filesystem::path { spec };
}

Category::Formatter makeStandardFormatter(FormatterOptions options)
{
    return [options](MessageBuilder const& message) -> std::string {
        auto const tagSgr = std::string_view { options.colorize ? "\033[1m" : "" };
        auto messageSgr = std::string {};
        if (options.colorize)
        {
            auto const colorIndex = CategoryColors.at(
                std::hash<std::string_view> {}(message.get_category().name()) % CategoryColors.size());
            messageSgr = std::format("\033[38;5;{}m", colorIndex);
        }
        return layOut(message, options, tagSgr, messageSgr, message.get_category().name());
    };
}

Category::Formatter makeErrorFormatter(FormatterOptions options)
{
    return [options](MessageBuilder const& message) -> std::string {
        auto const tagSgr = std::string_view { options.colorize ? "\033[1;31m" : "" };
        auto const messageSgr = std::string_view { options.colorize ? "\033[31m" : "" };
        return layOut(message, options, tagSgr, messageSgr, "error");
    };
}

std::vector<std::string> unmatchedFilters(std::string_view filterString)
{
    if (filterString.empty() || filterString == "all")
        return {};

    auto unmatched = std::vector<std::string> {};
    for (auto const pattern: crispy::split(filterString, ','))
    {
        if (pattern.empty())
            continue;
        // Through the same predicate configure() uses, so what this reports as unmatched is
        // exactly what configure() will fail to enable.
        auto const matches = std::ranges::any_of(
            get(), [&](auto const& each) { return matchesFilterPattern(pattern, each.get().name()); });
        if (!matches)
            unmatched.emplace_back(pattern);
    }
    return unmatched;
}

std::expected<std::unique_ptr<ScopedOutput>, std::string> ScopedOutput::create(OutputConfig config)
{
    auto file = std::ofstream {};
    if (config.file)
    {
        auto errorCode = std::error_code {};
        if (auto const parent = config.file->parent_path(); !parent.empty())
            // Best effort: if this fails, the open below reports the real reason.
            std::filesystem::create_directories(parent, errorCode);

        // Cleared so the reason below is the OPEN's own. create_directories() leaves errno set
        // from whatever syscall it last tried — even when it succeeded — and the standard does
        // not require ofstream::open to set errno at all, so a stale value would otherwise be
        // reported as the cause.
        errno = 0;
        file.open(*config.file, std::ios::out | std::ios::app);
        if (!file.is_open())
        {
            auto const reason = errno != 0
                                    // system_category().message() rather than strerror(), which
                                    // is not thread-safe.
                                    ? std::system_category().message(errno)
                                    : std::string { "cannot be opened for appending" };
            return std::unexpected(
                std::format("cannot open log file '{}': {}", config.file->string(), reason));
        }
    }
    return std::make_unique<ScopedOutput>(std::move(config), std::move(file));
}

ScopedOutput::ScopedOutput(OutputConfig const& config, std::ofstream file):
    _file { std::move(file) },
    _sink { true, [this](std::string_view const& text) {
               // Sink::write hands us the FULLY formatted message, so locking here makes each
               // line atomic — precisely the granularity that matters. The write and the flush
               // are one critical section, so two threads cannot interleave a line's bytes.
               auto const guard = std::lock_guard { _mutex };
               auto& out = _file.is_open() ? static_cast<std::ostream&>(_file) : std::cerr;
               out.write(text.data(), static_cast<std::streamsize>(text.size()));
               out.flush();
           } }
{
    // NEVER configure on an empty filter: it would match no pattern and thereby disable every
    // category. An empty filter means "leave what $LOG set alone".
    // (configure() itself keeps `error` on, for every caller.)
    if (!config.filter.empty())
        configure(config.filter);

    auto const options = FormatterOptions {
        // A file must never receive SGR escapes. For stderr the gate is stderr's own
        // tty-ness — note that logstore's console formatter asks about STDOUT while writing
        // to a stream that can be redirected independently.
        .colorize = !_file.is_open() && isStdErrTty(),
        .showTimestamp = true,
        .showProcessId = config.showProcessId,
    };

    // Snapshot before installing, so the destructor restores exactly what was here — including
    // whatever crispy::App already set up. Categories constructed AFTER this point are not
    // covered; they default to the console sink, which is why this is installed early.
    for (auto const& each: get())
    {
        auto& target = each.get();
        _restorePoints.push_back(RestorePoint {
            .target = &target, .previousSink = &target.sink(), .previousFormatter = target.get_formatter() });
    }

    set_formatter(makeStandardFormatter(options));
    errorLog.set_formatter(makeErrorFormatter(options));
    set_sink(_sink);
}

ScopedOutput::~ScopedOutput()
{
    // MUST run before _sink dies: every category holds a reference_wrapper into it.
    for (auto const& point: _restorePoints)
    {
        point.target->set_sink(*point.previousSink);
        point.target->set_formatter(point.previousFormatter);
    }
}

bool ScopedOutput::isStdErrTty() noexcept
{
#ifndef _WIN32
    return ::isatty(STDERR_FILENO) != 0;
#else
    return true;
#endif
}

ScopedCapture::ScopedCapture(std::string_view categoryName):
    _sink { true, [this](std::string_view const& text) { _text += text; } }
{
    auto capture = [this](Category& target) {
        _restorePoints.push_back(RestorePoint {
            .target = &target, .previousSink = &target.sink(), .wasEnabled = target.is_enabled() });
        target.enable();
        target.set_sink(_sink);
    };

    if (categoryName.empty())
        for (auto const& each: get())
            capture(each.get());
    else if (auto* const target = get(categoryName))
        capture(*target);
}

ScopedCapture::~ScopedCapture()
{
    for (auto const& point: _restorePoints)
    {
        point.target->set_sink(*point.previousSink);
        point.target->enable(point.wasEnabled);
    }
}

bool ScopedCapture::contains(std::string_view needle) const
{
    return _text.contains(needle);
}

std::size_t ScopedCapture::count(std::string_view needle) const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(lines(), [&](auto const& line) { return line.contains(needle); }));
}

std::vector<std::string> ScopedCapture::lines() const
{
    auto result = std::vector<std::string> {};
    for (auto const line: crispy::split(std::string_view { _text }, '\n'))
        if (!line.empty())
            result.emplace_back(line);
    return result;
}

} // namespace logstore
