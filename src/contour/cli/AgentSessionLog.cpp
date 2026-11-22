// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentSessionLog.hpp>
#include <contour/cli/ExitStatus.hpp>

#include <vtpty/PathContainment.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

#include <glaze/glaze.hpp>
#include <vthost/control/StatusText.hpp>

using namespace std::string_view_literals;

namespace contour::cli
{

namespace detail
{
    /// The binding read: the directory an entry was written in. Glaze reflects only types with linkage.
    struct CwdField
    {
        std::optional<std::string> cwd;
    };
} // namespace detail

namespace
{
    using detail::Binding;
    using detail::LineExtent;
    using detail::LogLine;
    using detail::LookupFailure;

    /// How many bytes a read takes from the log at a time.
    constexpr std::size_t ChunkBytes = std::size_t { 64 } * 1024;

    /// Unknown keys skipped, nothing past the line read.
    constexpr auto LineReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// @return Whether @p line names a directory, and where it lies against @p root. A cut line or one that
    ///         does not parse names none.
    [[nodiscard]] std::optional<Binding> bindingOf(LogLine const& line, std::filesystem::path const& root)
    {
        auto fields = detail::CwdField {};
        if (line.extent == LineExtent::Cut || glz::read<LineReadOptions>(fields, line.text) || !fields.cwd)
            return std::nullopt;
        auto const cwd = core::pathFromUtf8(*fields.cwd);
        return cwd && vtpty::isAtOrBeneath(*cwd, root, vtpty::NativePathCase) ? Binding::Beneath
                                                                              : Binding::Outside;
    }

    [[nodiscard]] std::optional<std::filesystem::path> pathOf(std::optional<std::string> const& text)
    {
        if (!text || text->empty())
            return std::nullopt;
        auto path = core::pathFromUtf8(*text);
        return path ? std::optional { *path } : std::nullopt;
    }

    /// @return `$CLAUDE_CONFIG_DIR`, else `.claude` in ClaudeHomeVariable: where Claude Code itself looks.
    [[nodiscard]] std::optional<std::filesystem::path> configDirectory(core::Environment const& env)
    {
        if (auto const config = pathOf(env.get("CLAUDE_CONFIG_DIR")))
            return config;
        if (auto const home = pathOf(env.get(ClaudeHomeVariable)))
            return *home / ".claude";
        return std::nullopt;
    }

    /// How a refusal exits.
    [[nodiscard]] ExitStatus statusOf(TranscriptError error) noexcept
    {
        switch (error)
        {
            case TranscriptError::BadSession:
            case TranscriptError::UnsupportedFormat: return ExitStatus::BadUsage;
            case TranscriptError::NoLog:
            case TranscriptError::PageOutOfRange: return ExitStatus::TargetNotFound;
            case TranscriptError::NoConfigDirectory:
            case TranscriptError::AmbiguousLog:
            case TranscriptError::NotRegular:
            case TranscriptError::Unreadable: return ExitStatus::Failed;
        }
        return ExitStatus::Failed;
    }
} // namespace

bool isCanonicalSessionId(std::string_view session) noexcept
{
    constexpr auto Shape = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"sv;
    if (session.size() != Shape.size())
        return false;
    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, Shape.size()), [&](std::size_t index) {
        auto const c = session[index];
        if (Shape[index] == '-')
            return c == '-';
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

namespace detail
{
    LineReader::LineReader(std::istream& in): _in { in }, _chunk(ChunkBytes)
    {
    }

    std::optional<LogLine> LineReader::next()
    {
        _line.clear();
        auto extent = LineExtent::Whole;
        while (fill())
        {
            auto const available = std::string_view { _chunk.data() + _begin, _end - _begin };
            auto const newline = available.find('\n');
            auto const taken = available.substr(0, newline);
            if (extent == LineExtent::Whole)
            {
                auto const room = MaxLogLineBytes - _line.size();
                _line.append(taken.substr(0, room));
                if (taken.size() > room)
                    extent = LineExtent::Cut;
            }
            _begin += taken.size();
            if (newline != std::string_view::npos)
            {
                ++_begin;
                return LogLine { .text = _line, .extent = extent };
            }
        }
        return std::nullopt;
    }

    bool LineReader::fill()
    {
        if (_begin < _end)
            return true;
        if (!_in.good())
            return false;
        _in.read(_chunk.data(), static_cast<std::streamsize>(_chunk.size()));
        _begin = 0;
        _end = static_cast<std::size_t>(_in.gcount());
        return _end > 0;
    }

    std::expected<Binding, TranscriptError> scanBound(std::istream& log,
                                                      std::filesystem::path const& root,
                                                      LineSink const& sink)
    {
        auto reader = LineReader { log };
        auto binding = Binding::NoDirectory;
        while (auto const line = reader.next())
        {
            if (auto const lineBinding = bindingOf(*line, root))
                binding = *lineBinding;
            if (binding == Binding::Outside)
                break;
            sink(*line);
        }
        if (reader.failed())
            return std::unexpected(TranscriptError::Unreadable);
        return binding;
    }

    std::expected<LocatedLog, LookupFailure> locateSessionLog(SessionLogQuery query,
                                                              core::Environment const& env,
                                                              core::platform::FileSystem const& fs,
                                                              ISessionLogOpener& opener,
                                                              CandidateScan const& scan)
    {
        if (!isCanonicalSessionId(query.session))
            return std::unexpected(
                LookupFailure { .error = TranscriptError::BadSession, .directory = {}, .logs = {} });
        auto const config = configDirectory(env);
        if (!config)
            return std::unexpected(
                LookupFailure { .error = TranscriptError::NoConfigDirectory, .directory = {}, .logs = {} });
        auto const projects = *config / "projects";
        auto const root = core::pathFromUtf8(query.cwd).value_or(std::filesystem::path {});
        auto const fileName = std::filesystem::path { std::string { query.session } + ".jsonl" };
        auto bound = std::vector<LocatedLog> {};
        auto scans = std::size_t { 0 };
        for (auto const& entry:
             fs.listDirectory(projects).value_or(std::vector<core::platform::FileSystem::DirectoryEntry> {}))
        {
            if (!entry.isDirectory || entry.isSymlink || opener.isLinkedDirectory(entry.path))
                continue;
            auto const candidate = entry.path / fileName;
            if (!fs.exists(candidate) && !fs.isSymlink(candidate))
                continue;
            auto const failure = [&](TranscriptError error) {
                return std::unexpected(
                    LookupFailure { .error = error, .directory = projects, .logs = { candidate } });
            };
            auto stream = opener.open(candidate);
            if (!stream)
                return failure(stream.error());
            auto const binding = scan(**stream, root);
            if (!binding)
                return failure(binding.error());
            if (*binding == Binding::Beneath)
                bound.push_back(LocatedLog { .log = candidate, .candidate = scans });
            ++scans;
        }
        if (bound.size() == 1)
            return std::move(bound.front());
        auto logs = std::vector<std::filesystem::path> {};
        for (auto& located: bound)
            logs.push_back(std::move(located.log));
        return std::unexpected(
            LookupFailure { .error = bound.empty() ? TranscriptError::NoLog : TranscriptError::AmbiguousLog,
                            .directory = projects,
                            .logs = std::move(logs) });
    }

    std::string printable(std::filesystem::path path)
    {
        return vthost::control::sanitizeStatusText(core::pathToUtf8(path.make_preferred()),
                                                   MaxLogMessageFieldBytes);
    }

    std::string lookupMessage(LookupFailure const& failure, SessionLogQuery query)
    {
        auto const firstLog = failure.logs.empty() ? std::string {} : printable(failure.logs.front());
        switch (failure.error)
        {
            case TranscriptError::BadSession:
                return std::format("not a session id: {}",
                                   vthost::control::sanitizeStatusText(query.session));
            case TranscriptError::NoConfigDirectory:
                return std::format(
                    "no Claude Code configuration directory: none of CLAUDE_CONFIG_DIR and {} is set",
                    ClaudeHomeVariable);
            case TranscriptError::NoLog:
                return std::format("no session log for {} in {} under {}",
                                   query.session,
                                   vthost::control::sanitizeStatusText(query.cwd, MaxLogMessageFieldBytes),
                                   printable(failure.directory));
            case TranscriptError::AmbiguousLog:
                return std::format("two session logs for {} in {}: {}, {}",
                                   query.session,
                                   vthost::control::sanitizeStatusText(query.cwd, MaxLogMessageFieldBytes),
                                   firstLog,
                                   failure.logs.size() > 1 ? printable(failure.logs[1]) : std::string {});
            case TranscriptError::NotRegular: return std::format("{} is not a regular file", firstLog);
            case TranscriptError::Unreadable: return std::format("{}: cannot be read", firstLog);
            case TranscriptError::PageOutOfRange:
            case TranscriptError::UnsupportedFormat: break;
        }
        return {};
    }

    int refuse(std::ostream& err, TranscriptError error, std::string_view message)
    {
        err << message << '\n';
        return exitCode(statusOf(error));
    }
} // namespace detail

} // namespace contour::cli
