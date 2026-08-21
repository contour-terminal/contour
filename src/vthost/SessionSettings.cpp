// SPDX-License-Identifier: Apache-2.0
#include <vthost/SessionSettings.hpp>

#include <libunicode/convert.h>
#include <libunicode/utf8.h>

#include <algorithm>
#include <utility>
#include <variant>

namespace vthost
{

vtbackend::Settings hostedSessionSettings(vtbackend::Settings settings)
{
    // Only the finite alternative is adjusted: Infinite is a choice the configuration offers
    // outright, and there is nothing to raise or cap about it.
    auto const bounded = [](vtbackend::MaxHistoryLineCount limit) -> vtbackend::MaxHistoryLineCount {
        auto const* const finite = std::get_if<vtbackend::LineCount>(&limit);
        if (finite == nullptr)
            return limit;
        // ZERO is replaced, a small value is not. DefaultSessionHistoryLineCount is a default, not a
        // floor: zero is the configuration that breaks delta addressing, whereas 200 lines is a
        // deliberate choice that works, and raising it would overrule the user to fix a defect they
        // do not have.
        auto const asked = unbox<int>(*finite);
        return vtbackend::LineCount(asked <= 0 ? DefaultSessionHistoryLineCount
                                               : std::min(asked, MaxSessionHistoryLineCount));
    };

    settings.historyLimits.capacity = bounded(settings.historyLimits.capacity);

    // The guarantee can never outrun the ceiling it is measured against, and equalling it is the
    // right answer rather than an error: with no headroom the scrollback simply evicts line-wise, as
    // it always has. That is also what a daemon whose capacity was just raised off zero must get --
    // inheriting the raise as HEADROOM would hand a session block eviction nobody configured.
    //
    // A plain min because MaxHistoryLineCount is ORDERED: the variant compares by alternative index
    // first, so Infinite sits above every line count and this reads as "the shallower of the two"
    // across all four combinations, infinities included. It LOWERS the guarantee rather than raising
    // the capacity, because unlike the configuration's own reconciliation the ceiling here is
    // externally imposed -- MaxSessionHistoryLineCount is not ours to exceed.
    settings.historyLimits.guaranteed =
        std::min(bounded(settings.historyLimits.guaranteed), settings.historyLimits.capacity);

    settings.maxImageRegisterCount =
        std::clamp(settings.maxImageRegisterCount, 1U, MaxSessionImageRegisterCount);

    if (settings.wordDelimiters.size() > MaxSessionWordDelimiters)
        settings.wordDelimiters.resize(MaxSessionWordDelimiters);

    // Not a preference the daemon honours -- @see the declaration.
    settings.goodImageProtocol = true;

    return settings;
}

vtbackend::Settings defaultSessionSettings()
{
    return hostedSessionSettings(vtbackend::Settings {});
}

proto::WireSessionSettings toWireSessionSettings(vtbackend::Settings const& settings)
{
    auto const* const finite = std::get_if<vtbackend::LineCount>(&settings.historyLimits.capacity);
    auto wire = proto::WireSessionSettings {
        // -1 is unlimited, matching the configuration's own spelling of `history.limit`.
        .historyLineCount = finite != nullptr ? unbox<int64_t>(*finite) : int64_t { -1 },
        .terminalId = std::to_underlying(settings.terminalId),
        .graphemeClustering = static_cast<uint8_t>(settings.graphemeClustering ? 1 : 0),
        .allowReflowOnResize = static_cast<uint8_t>(settings.primaryScreen.allowReflowOnResize ? 1 : 0),
        .maxImageRegisterCount = settings.maxImageRegisterCount,
        .wordDelimiters = unicode::convert_to<char>(std::u32string_view { settings.wordDelimiters }),
    };

    wire.frozenModes.reserve(settings.frozenModes.size());
    for (auto const& [mode, frozenAs]: settings.frozenModes)
        wire.frozenModes.push_back(proto::WireFrozenMode {
            .mode = vtbackend::toDECModeNum(mode),
            .frozenAs = static_cast<uint8_t>(frozenAs ? 1 : 0),
        });

    return wire;
}

vtbackend::Settings fromWireSessionSettings(proto::WireSessionSettings const& wire,
                                            vtbackend::Settings const& base)
{
    auto settings = base;

    // Bounded BEFORE narrowing, not by hostedSessionSettings afterwards: LineCount holds an `int`,
    // so a peer naming 10^12 lines would arrive as some arbitrary — possibly negative — value, and
    // the clamp downstream would then dutifully "raise" it to the default.
    // Both bounds take the one number the wire carries. A mirror evicts on its own budget and never
    // runs the block-atomic rule -- that is a server-side policy, and the client learns its outcome
    // from the stable floor the delta stream already moves.
    settings.historyLimits = vtbackend::HistoryLimits::plain(
        wire.historyLineCount < 0
            ? vtbackend::MaxHistoryLineCount { vtbackend::Infinite {} }
            : vtbackend::MaxHistoryLineCount { vtbackend::LineCount(
                  static_cast<int>(std::min<int64_t>(wire.historyLineCount, MaxSessionHistoryLineCount))) });

    // The numbering is sparse and not ordered by capability, so an unknown number cannot be clamped
    // into something sensible -- keeping the daemon's own is the only honest answer.
    if (auto const terminalId = vtbackend::fromVTTypeNum(wire.terminalId))
        settings.terminalId = *terminalId;

    settings.graphemeClustering = wire.graphemeClustering != 0;
    settings.primaryScreen.allowReflowOnResize = wire.allowReflowOnResize != 0;
    settings.maxImageRegisterCount = wire.maxImageRegisterCount;
    settings.wordDelimiters = unicode::from_utf8(wire.wordDelimiters);

    // REPLACES rather than merges: the client stated its complete set, so a client that freezes
    // nothing gets sessions that freeze nothing, even against a daemon whose own profile froze modes.
    settings.frozenModes.clear();
    for (auto const& frozen: wire.frozenModes)
        if (auto const mode = vtbackend::fromDECModeNum(frozen.mode))
            settings.frozenModes[*mode] = frozen.frozenAs != 0;

    return hostedSessionSettings(std::move(settings));
}

} // namespace vthost
