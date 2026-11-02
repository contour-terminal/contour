// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/state/TerminalState.hpp>
#include <vtbackend/state/TerminalStateCodec.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend::test
{

/// The terminals a handover harness builds, all alike.
struct HandoverSetup
{
    PageSize pageSize { LineCount(10), ColumnCount(40) }; ///< Every terminal's page.
    LineCount history = LineCount(20);                    ///< Every terminal's scrollback.
    ImageSize cellPixelSize { Width(10), Height(20) };    ///< What the frontend reports.
    size_t inFlightTailLimit = DefaultInFlightTailLimit;  ///< Settings::inFlightTailLimit.

    /// How far apart the points are where a handed-over terminal is compared with the source: a handover at
    /// an offset is checked at the first of them at or after it, so the harness feeds each offset at most
    /// this many bytes.
    size_t checkpointStride = 64;

    /// @return The settings every terminal of this setup is built from.
    [[nodiscard]] Settings settings() const
    {
        auto settings = MockTerm<>::createSettings(pageSize, HistoryLimits::plain(history), 1024);
        settings.inFlightTailLimit = inFlightTailLimit;
        return settings;
    }

    /// @return A terminal of this setup built from @p settings, nothing parsed yet.
    [[nodiscard]] std::unique_ptr<MockTerm<>> makeTerminal(Settings settings) const
    {
        auto mock = std::make_unique<MockTerm<>>(std::move(settings));
        mock->terminal.setCellPixelSize(cellPixelSize);
        return mock;
    }

    /// @return A terminal of this setup, nothing parsed yet.
    [[nodiscard]] std::unique_ptr<MockTerm<>> makeTerminal() const { return makeTerminal(settings()); }
};

/// @return @p state as the bytes a handover sends: the identity two terminals are the same state by.
[[nodiscard]] inline std::vector<uint8_t> encodedState(TerminalState const& state)
{
    return encodeTerminalState(state);
}

/// A fixed sequence of well-spread numbers (SplitMix64): the same on every run and every machine, which a
/// test sampling offsets or damaging bytes needs, and which a random number generator is not meant to be.
class FixedSequence
{
  public:
    /// @param start Where the sequence starts.
    explicit constexpr FixedSequence(uint64_t start) noexcept: _state { start } {}

    /// @return The next number.
    constexpr uint64_t operator()() noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15;
        auto z = _state;
        z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9;
        z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EB;
        return z ^ (z >> 31U);
    }

  private:
    uint64_t _state;
};

/// Up to this many bytes of a corpus, a harness hands over at every offset; beyond it, at a sample.
inline constexpr auto FullSweepLimit = size_t { 4 } * 1024;

/// How many offsets of a corpus beyond FullSweepLimit a harness samples.
inline constexpr auto SampledOffsetCount = size_t { 512 };

/// The seed the sampled offsets are drawn from.
inline constexpr auto OffsetSampleSeed = 0x5EEDU;

/// @param corpusSize The length of the corpus.
/// @return Every offset of the corpus when it is at most FullSweepLimit long, else SampledOffsetCount
///         distinct ones, ascending, drawn from a fixed seed -- the same on every run and every machine.
[[nodiscard]] inline std::vector<size_t> sampledOffsets(size_t corpusSize)
{
    if (corpusSize <= FullSweepLimit)
        return std::ranges::to<std::vector>(std::views::iota(size_t { 0 }, corpusSize + 1));
    // Both ends are always in: the empty prefix and the whole corpus.
    auto chosen = std::set<size_t> { 0, corpusSize };
    auto rng = FixedSequence { OffsetSampleSeed };
    while (chosen.size() < SampledOffsetCount)
        chosen.insert(static_cast<size_t>(rng() % (corpusSize + 1)));
    return std::ranges::to<std::vector>(chosen);
}

/// Compares two states area by area, as the codec lists them (TerminalState::visitFields).
class AreaComparer
{
  public:
    /// @param other The state the visited one is compared with.
    explicit AreaComparer(TerminalState const& other): _other { other } {}

    template <typename T>
    void operator()(uint32_t id, T const& value, std::string_view name)
    {
        auto const& counterpart = otherArea<T>(id);
        if (bytesOf(value) != bytesOf(counterpart))
            _names += std::format("{}{}", _names.empty() ? "" : ", ", name);
    }

    /// @return The names of the areas that differ, comma separated.
    [[nodiscard]] std::string const& names() const noexcept { return _names; }

  private:
    template <typename T>
    [[nodiscard]] static std::vector<uint8_t> bytesOf(T const& area)
    {
        auto writer = StateWriter::payload();
        auto encoder = detail::FieldEncoder { writer };
        encoder.writeValue(area);
        return std::move(writer).take();
    }

    /// Finds the area of the other state with field id @p id.
    template <typename T>
    [[nodiscard]] T const& otherArea(uint32_t id) const
    {
        auto const* found = static_cast<T const*>(nullptr);
        auto finder = AreaFinder<T> { id, found };
        _other.visitFields(finder);
        return *found;
    }

    template <typename T>
    struct AreaFinder
    {
        uint32_t id;
        T const*& found;

        template <typename U>
        void operator()(uint32_t areaId, U const& area, std::string_view /*name*/)
        {
            if constexpr (std::is_same_v<T, U>)
                if (areaId == id)
                    found = &area;
        }
    };

    TerminalState const& _other;
    std::string _names;
};

/// @return The areas in which @p a and @p b differ, by name; empty when they are the same state.
[[nodiscard]] inline std::string differingAreas(TerminalState const& a, TerminalState const& b)
{
    auto comparer = AreaComparer { b };
    a.visitFields(comparer);
    return comparer.names();
}

// {{{ probes

/// The queries a program can ask a terminal about its state, one row each: what a program sees, as opposed to
/// what a capture holds. A handed-over terminal must answer every one of them as the terminal it came from.
inline constexpr auto StateQueries = std::to_array<std::string_view>({
    "\033[c",                         // DA1
    "\033[>c",                        // DA2
    "\033[=c",                        // DA3
    "\033[6n",                        // CPR
    "\033[?6n",                       // DECXCPR
    "\033P$qr\033\\",                 // DECRQSS DECSTBM
    "\033P$qs\033\\",                 // DECRQSS DECSLRM
    "\033P$qm\033\\",                 // DECRQSS SGR
    "\033P$q q\033\\",                // DECRQSS DECSCUSR
    "\033P$q\"p\033\\",               // DECRQSS DECSCL
    "\033P$q\"q\033\\",               // DECRQSS DECSCA
    "\033P$q*x\033\\",                // DECRQSS DECSACE
    "\033[1$w",                       // DECRQPSR DECCIR
    "\033[2$w",                       // DECRQPSR DECTABSR
    "\033[#R",                        // XTREPORTCOLORS
    "\033]4;1;?\033\\",               // OSC 4, a palette entry
    "\033]4;200;?\033\\",             // OSC 4, another
    "\033]10;?\033\\",                // OSC 10
    "\033]11;?\033\\",                // OSC 11
    "\033]12;?\033\\",                // OSC 12
    "\033[?u",                        // kitty keyboard flags
    "\033[14t",                       // XTWINOPS: text area in pixels
    "\033[16t",                       // XTWINOPS: cell in pixels
    "\033[18t",                       // XTWINOPS: text area in cells
    "\033[21t",                       // XTWINOPS: window title
    "\033]99;i=probe:p=alive;\033\\", // OSC 99: the notifications alive
    "\033_Ga=q,i=7;\033\\",           // kitty graphics: is image 7 there
    "\033[&u",                        // DECRQUPSS
    "\033[\"v",                       // DECRQDE
    "\033[?15n",                      // printer status
    "\033[?26n",                      // keyboard status
    "\033[1;1;1;1;30;100*y",          // DECRQCRA over the page and beyond
});

/// The replies @p mock gives to every query of StateQueries and to a DECRQM of every mode, and the bytes it
/// sends for a few keys, a click and a paste.
/// @param mock A terminal; probed, so changed: it is no longer the terminal it was.
/// @return The transcript, one query per line, the query escaped, then its reply.
[[nodiscard]] inline std::string probeTranscript(MockTerm<>& mock)
{
    auto queries = std::vector<std::string> { StateQueries.begin(), StateQueries.end() };
    for (auto const mode: AnsiModes)
        queries.push_back(std::format("\033[{}$p", toAnsiModeNum(mode)));
    for (auto const mode: PermanentlyResetAnsiModes)
        queries.push_back(std::format("\033[{}$p", mode));
    for (auto const& row: DECModeNumbers)
        queries.push_back(std::format("\033[?{}$p", row.number));

    auto const escaped = [](std::string_view text) {
        auto result = std::string {};
        for (auto const ch: text)
            result += std::isprint(static_cast<unsigned char>(ch))
                          ? std::string(1, ch)
                          : std::format("\\x{:02X}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
        return result;
    };
    auto transcript = std::string {};
    auto const record = [&](std::string_view what, auto&& ask) {
        mock.terminal.flushInput();
        mock.resetReplyData();
        ask();
        mock.terminal.flushInput();
        transcript += std::format("{} -> {}\n", escaped(what), escaped(mock.replyData()));
    };
    for (auto const& query: queries)
        record(query, [&] { mock.writeToScreen(query); });
    for (auto const key: { Key::UpArrow, Key::F1, Key::Home, Key::Numpad_5 })
        record(std::format("key {}", static_cast<int>(key)), [&] { std::ignore = mock.sendKeyEvent(key); });
    record("paste", [&] { mock.terminal.sendPaste("pasted"); });
    record("click", [&] {
        std::ignore = mock.terminal.sendMousePressEvent(Modifiers {},
                                                        MouseButton::Left,
                                                        CellLocation { LineOffset(1), ColumnOffset(2) },
                                                        PixelCoordinate {},
                                                        false);
    });
    return transcript;
}

// }}}

/// Restores what @p source holds, through the byte codec, into a fresh terminal of @p setup.
/// @return The terminal, or why the codec or the restore refused.
[[nodiscard]] inline std::expected<std::unique_ptr<MockTerm<>>, std::string> handedOver(
    MockTerm<> const& source, HandoverSetup const& setup)
{
    auto const bytes = encodedState(source.terminal.captureState());
    auto const decoded = decodeTerminalState(bytes);
    if (!decoded)
        return std::unexpected(
            std::format("the stream does not decode ({})", static_cast<int>(decoded.error())));
    // Built as the restoring daemon builds it: from what the state carries.
    auto const settings = settingsFor(*decoded, setup.settings());
    if (!settings)
        return std::unexpected(std::format("the state carries no settings to build from ({})",
                                           static_cast<int>(settings.error())));
    auto target = setup.makeTerminal(*settings);
    if (auto const restored = target->terminal.restoreState(*decoded); !restored)
        return std::unexpected(std::format("the restore refuses ({})", static_cast<int>(restored.error())));
    return target;
}

/// Hands @p source over and probes both (probeTranscript()).
/// @param source The terminal; probed, so changed.
/// @param setup  The terminals.
/// @return The first query answered differently, or why the handover failed; none when every one matches.
[[nodiscard]] inline std::optional<std::string> findProbeMismatch(MockTerm<>& source,
                                                                  HandoverSetup const& setup)
{
    auto target = handedOver(source, setup);
    if (!target)
        return target.error();
    auto const expected = probeTranscript(source);
    auto const actual = probeTranscript(**target);
    if (expected == actual)
        return std::nullopt;
    auto expectedLines = expected | std::views::split('\n');
    auto actualLines = actual | std::views::split('\n');
    for (auto const [want, got]: std::views::zip(expectedLines, actualLines))
        if (!std::ranges::equal(want, got))
            return std::format("a probe is answered differently: expected {}, got {}",
                               std::string_view { want },
                               std::string_view { got });
    return std::string { "the probe transcripts differ in length" };
}

/// Where a terminal handed over in the middle of a stream stopped being the terminal it was handed over from.
struct HandoverMismatch
{
    size_t offset = 0;     ///< How many bytes the source had parsed when it was handed over.
    size_t checkpoint = 0; ///< How many bytes both had parsed when they were compared.
    std::string what;      ///< What differed.
};

/// Hands a terminal parsing @p corpus over at every byte offset of it, and checks each handed-over terminal
/// against the source a few bytes on: the same state, and the same replies sent to the application.
///
/// The source is fed a byte at a time, so every offset is a fragment boundary and every sequence is split
/// there at least once; the handed-over terminal is fed the rest up to the next checkpoint in one go.
/// @param corpus The bytes the host sends.
/// @param setup  The terminals.
/// @param offsets The offsets to hand over at, ascending; empty for every offset of the corpus.
/// @return The first mismatch; none when every offset hands over exactly.
[[nodiscard]] inline std::optional<HandoverMismatch> findHandoverMismatch(
    std::string_view corpus, HandoverSetup const& setup, std::span<size_t const> offsets = {})
{
    auto const checkpointAtOrAfter = [&](size_t offset) {
        auto const stride = setup.checkpointStride;
        return std::min(corpus.size(), ((offset + stride - 1) / stride) * stride);
    };
    auto const feedByteByByte = [&](MockTerm<>& mock, size_t from, size_t to) {
        for (auto const offset: std::views::iota(from, to))
            mock.writeToScreen(corpus.substr(offset, 1));
    };

    // The source as it is at every checkpoint: its state, and every reply it has sent so far.
    struct Checkpoint
    {
        std::vector<uint8_t> state;
        std::string replies;
    };
    auto checkpoints = std::vector<Checkpoint> {};
    {
        auto source = setup.makeTerminal();
        auto offset = size_t { 0 };
        while (true)
        {
            source->terminal.flushInput();
            checkpoints.push_back(Checkpoint { .state = encodedState(source->terminal.captureState()),
                                               .replies = source->replyData() });
            if (offset == corpus.size())
                break;
            auto const next = checkpointAtOrAfter(offset + 1);
            feedByteByByte(*source, offset, next);
            offset = next;
        }
    }
    auto const checkpointIndexOf = [&](size_t offset) {
        return (offset + setup.checkpointStride - 1) / setup.checkpointStride;
    };

    auto source = setup.makeTerminal();
    for (auto const offset: std::views::iota(size_t { 0 }, corpus.size() + 1))
    {
        if (offset != 0)
            feedByteByByte(*source, offset - 1, offset);
        // Replies are sent at the checkpoints only, as in the first pass: with SRM reset what is sent is
        // echoed, and where the echo lands depends on when.
        if (checkpointAtOrAfter(offset) == offset)
            source->terminal.flushInput();
        if (!offsets.empty() && !std::ranges::binary_search(offsets, offset))
            continue;

        auto target = handedOver(*source, setup);
        auto const checkpoint = checkpointAtOrAfter(offset);
        if (!target)
            return HandoverMismatch { .offset = offset, .checkpoint = offset, .what = target.error() };
        (*target)->writeToScreen(corpus.substr(offset, checkpoint - offset));
        (*target)->terminal.flushInput();

        auto const& expected = checkpoints.at(checkpointIndexOf(offset));
        if (auto const actual = (*target)->terminal.captureState(); encodedState(actual) != expected.state)
        {
            auto const decoded = decodeTerminalState(expected.state);
            return HandoverMismatch {
                .offset = offset,
                .checkpoint = checkpoint,
                .what = std::format("the state differs in: {}", differingAreas(actual, *decoded)),
            };
        }
        if (source->replyData() + (*target)->replyData() != expected.replies)
            return HandoverMismatch { .offset = offset,
                                      .checkpoint = checkpoint,
                                      .what = "the replies differ" };
    }

    // Last, what no capture can be trusted to see: the source, handed over once more at the end, must answer
    // every query a program can ask exactly as the source does.
    if (auto const mismatch = findProbeMismatch(*source, setup))
        return HandoverMismatch { .offset = corpus.size(), .checkpoint = corpus.size(), .what = *mismatch };
    return std::nullopt;
}

} // namespace vtbackend::test
