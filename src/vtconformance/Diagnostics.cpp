// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/logging.h>

#include <crispy/logstore.h>

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <utility>

#include <vtconformance/Diagnostics.h>

using namespace std::string_view_literals;

namespace vtconformance
{

namespace
{
    /// The message prefixes `vtbackend::Screen` and the parser use for their failure buckets.
    ///
    /// These are the contract between the engine's logging and this oracle. If a prefix here stops
    /// matching the engine, `Diagnostics_test.cpp` fails — deliberately, because a silently
    /// mismatching prefix would turn the conformance report into a false all-clear.
    constexpr auto DiagnosticPrefixes = std::array {
        std::pair { "Unknown VT sequence: "sv, DiagnosticKind::Unknown },
        std::pair { "Unsupported VT sequence: "sv, DiagnosticKind::Unsupported },
        std::pair { "Invalid VT sequence: "sv, DiagnosticKind::Invalid },
        std::pair { "Parser error: "sv, DiagnosticKind::ParserError },
    };

    /// The `vt.parser` category, looked up by name so this module does not have to be a friend of
    /// the engine's logging header layout.
    [[nodiscard]] logstore::category* parserCategory() noexcept
    {
        return logstore::get(vtbackend::vtParserLog.name());
    }

    /// A self-echoed DECRQCRA reply, rendered by the engine as `DCS <Pid> ! ~`.
    constexpr auto DcsReplyPrefix = "DCS "sv;
    constexpr auto DcsReplySuffix = " ! ~"sv;
} // namespace

std::string canonicalizeSequence(std::string_view sequence)
{
    if (sequence.starts_with(DcsReplyPrefix) && sequence.ends_with(DcsReplySuffix)
        && sequence.size() > DcsReplyPrefix.size() + DcsReplySuffix.size())
    {
        auto const digits = sequence.substr(DcsReplyPrefix.size(),
                                            sequence.size() - DcsReplyPrefix.size() - DcsReplySuffix.size());
        if (std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }))
            return std::string(DcsReplyPrefix) + "<id>" + std::string(DcsReplySuffix);
    }
    return std::string(sequence);
}

std::optional<Diagnostic> classifyDiagnostic(std::string_view line)
{
    for (auto const& [prefix, kind]: DiagnosticPrefixes)
    {
        auto const at = line.find(prefix);
        if (at == std::string_view::npos)
            continue;

        auto sequence = line.substr(at + prefix.size());
        while (!sequence.empty() && (sequence.back() == '\n' || sequence.back() == '\r'))
            sequence.remove_suffix(1);

        return Diagnostic { .kind = kind, .sequence = canonicalizeSequence(sequence), .count = 1 };
    }

    return std::nullopt;
}

DiagnosticsCollector::DiagnosticsCollector():
    _sink(std::make_unique<logstore::sink>(true, [this](std::string_view const& line) { record(line); }))
{
    auto* const category = parserCategory();
    if (!category)
        return;

    category->enable();
    // The default formatter decorates messages with source locations; the oracle wants the bare
    // text so that `classifyDiagnostic` can stay a pure string function.
    category->set_formatter([](logstore::message_builder const& message) { return message.text(); });
    category->set_sink(*_sink);
}

DiagnosticsCollector::~DiagnosticsCollector()
{
    if (auto* const category = parserCategory())
    {
        category->set_formatter(&logstore::category::defaultFormatter);
        category->set_sink(logstore::sink::console());
    }
}

void DiagnosticsCollector::record(std::string_view line)
{
    auto diagnostic = classifyDiagnostic(line);
    if (!diagnostic)
        return;

    auto const guard = std::lock_guard { _mutex };

    auto const existing = std::ranges::find_if(_diagnostics, [&](Diagnostic const& candidate) {
        return candidate.kind == diagnostic->kind && candidate.sequence == diagnostic->sequence;
    });

    if (existing != _diagnostics.end())
        ++existing->count;
    else
        _diagnostics.push_back(std::move(*diagnostic));
}

std::vector<Diagnostic> DiagnosticsCollector::collected() const
{
    auto const guard = std::lock_guard { _mutex };
    return _diagnostics;
}

void DiagnosticsCollector::clear()
{
    auto const guard = std::lock_guard { _mutex };
    _diagnostics.clear();
}

} // namespace vtconformance
