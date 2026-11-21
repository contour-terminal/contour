// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentRecord.hpp>
#include <contour/cli/ExitStatus.hpp>

#include <format>
#include <utility>

#include <glaze/glaze.hpp>

namespace contour::cli
{

namespace
{
    [[nodiscard]] ExitStatus statusOf(RecordError error) noexcept
    {
        switch (error)
        {
            case RecordError::NotARecord:
            case RecordError::UnsupportedFormat: return ExitStatus::BadUsage;
            case RecordError::Unwritable: return ExitStatus::Failed;
        }
        return ExitStatus::Failed;
    }

    [[nodiscard]] int refuse(std::ostream& err, RecordError error, std::string_view message)
    {
        err << message << '\n';
        return exitCode(statusOf(error));
    }

    /// @return @p line as one compact line of JSON, if it is a JSON object of RecordFormat.
    [[nodiscard]] std::expected<std::string, RecordError> compactRecord(std::string_view line,
                                                                        uint32_t format)
    {
        auto document = glz::generic {};
        if (glz::read<glz::opts { .null_terminated = false }>(document, line) || !document.is_object())
            return std::unexpected(RecordError::NotARecord);
        auto const& object = document.get_object();
        auto const written = object.find("format");
        if (written == object.end() || !written->second.is_number()
            || written->second.get_number() != static_cast<double>(format))
            return std::unexpected(RecordError::NotARecord);
        // Written again rather than copied: whatever the line held, the bytes appended are one line.
        auto compact = std::string {};
        if (glz::write_json(document, compact))
            return std::unexpected(RecordError::NotARecord);
        return compact;
    }
} // namespace

int runAgentRecord(RecordRequest const& request,
                   std::filesystem::path const& file,
                   IRecordAppender& appender,
                   std::ostream& err)
{
    if (request.format != RecordFormat)
        return refuse(err,
                      RecordError::UnsupportedFormat,
                      std::format("unsupported record format {}; this contour writes {}",
                                  request.format,
                                  RecordFormat));
    auto const compact = compactRecord(request.line, request.format);
    if (!compact)
        return refuse(err, compact.error(), "not a run record: one JSON object of the format asked for");
    if (auto const appended = appender.append(file, *compact + '\n'); !appended)
        return refuse(err, RecordError::Unwritable, appended.error());
    return exitCode(ExitStatus::Ok);
}

} // namespace contour::cli
