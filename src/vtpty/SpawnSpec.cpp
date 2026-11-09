// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SpawnSpec.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace vtpty
{

namespace
{
    /// The number of enumerators of each wire enum, from its last one; the decoder rejects anything at or
    /// past it.
    template <typename E>
    inline constexpr std::uint8_t EnumeratorCount = 0;

    template <>
    inline constexpr std::uint8_t EnumeratorCount<ExecLookup> =
        std::to_underlying(ExecLookup::SearchPath) + 1;
    template <>
    inline constexpr std::uint8_t EnumeratorCount<ControllingTerminal> =
        std::to_underlying(ControllingTerminal::Acquire) + 1;
    template <>
    inline constexpr std::uint8_t EnumeratorCount<TerminalSettings> =
        std::to_underlying(TerminalSettings::Configure) + 1;
    template <>
    inline constexpr std::uint8_t EnumeratorCount<FailureReport> =
        std::to_underlying(FailureReport::OnTerminal) + 1;
    template <>
    inline constexpr std::uint8_t EnumeratorCount<FailureExit> =
        std::to_underlying(FailureExit::NotFound) + 1;

    template <typename Launch, typename Variant>
    struct IndexIn;

    template <typename Launch, typename... Alternatives>
    struct IndexIn<Launch, std::variant<Alternatives...>>
    {
        static constexpr auto Matches = std::array { std::is_same_v<Launch, Alternatives>... };
        static constexpr auto Value =
            static_cast<std::uint8_t>(std::ranges::find(Matches, true) - Matches.begin());
    };

    /// The index of @p Launch in SpawnLaunch: the mode byte that names it.
    template <typename Launch>
    inline constexpr std::uint8_t LaunchIndex = IndexIn<Launch, SpawnLaunch>::Value;

    constexpr std::uint8_t AllSlotBits = static_cast<std::uint8_t>((1U << TrampolineSlotCount) - 1U);

    /// Counts what an encoding appends, so that the string it is then written into is allocated once.
    struct SizeCounter
    {
        std::size_t size = 0;

        void byte(char /*value*/) noexcept { ++size; }
        void bytes(std::string_view value) noexcept { size += value.size(); }
    };

    /// Appends what an encoding writes to a string.
    struct StringSink
    {
        std::string& out;

        void byte(char value) { out.push_back(value); }
        void bytes(std::string_view value) { out.append(value); }
    };

    template <typename Sink>
    void putU32(Sink& out, std::uint32_t value)
    {
        for (auto const shift: std::views::iota(0U, 4U))
            out.byte(static_cast<char>((value >> (8U * shift)) & 0xFFU));
    }

    template <typename Sink>
    void putByte(Sink& out, std::uint8_t value)
    {
        out.byte(static_cast<char>(value));
    }

    template <typename Sink, typename E>
    void putEnum(Sink& out, E value)
    {
        putByte(out, std::to_underlying(value));
    }

    template <typename Sink>
    void putString(Sink& out, std::string_view value)
    {
        putU32(out, static_cast<std::uint32_t>(value.size()));
        out.bytes(value);
    }

    template <typename Sink>
    void putStrings(Sink& out, std::span<std::string const> values)
    {
        putU32(out, static_cast<std::uint32_t>(values.size()));
        for (auto const& value: values)
            putString(out, value);
    }

    template <typename Sink>
    void putLaunch(Sink& out, TerminalLaunch const& launch)
    {
        putStrings(out, launch.fallbackArgv);
        putString(out, launch.workingDirectory);
        putEnum(out, launch.controllingTerminal);
        putEnum(out, launch.terminalSettings);
        putEnum(out, launch.failureReport);
        putEnum(out, launch.failureExit);
    }

    template <typename Sink>
    void putLaunch(Sink& /*out*/, DetachedLaunch const& /*launch*/)
    {
    }

    /// Appends @p spec's encoding to @p out.
    template <typename Sink>
    void putSpec(Sink& out, SpawnSpec const& spec)
    {
        putByte(out, static_cast<std::uint8_t>(spec.launch.index()));
        putString(out, spec.image);
        putStrings(out, spec.argv);
        putEnum(out, spec.lookup);
        putByte(out, spec.environment.has_value() ? 1 : 0);
        if (spec.environment)
            putStrings(out, *spec.environment);
        putByte(out, spec.oomScoreAdjust.has_value() ? 1 : 0);
        if (spec.oomScoreAdjust)
            putU32(out, std::bit_cast<std::uint32_t>(*spec.oomScoreAdjust));
        putByte(out, spec.inheritedSlots.raw());
        std::visit([&](auto const& launch) { putLaunch(out, launch); }, spec.launch);
    }

    /// Consumes a byte string front to back; every read reports Truncated rather than overrunning.
    class Reader
    {
      public:
        explicit Reader(std::string_view bytes) noexcept: _rest { bytes } {}

        [[nodiscard]] bool atEnd() const noexcept { return _rest.empty(); }

        [[nodiscard]] std::expected<std::uint8_t, SpawnSpecError> byte()
        {
            if (_rest.empty())
                return std::unexpected { SpawnSpecError::Truncated };
            auto const value = static_cast<std::uint8_t>(_rest.front());
            _rest.remove_prefix(1);
            return value;
        }

        [[nodiscard]] std::expected<std::uint32_t, SpawnSpecError> u32()
        {
            if (_rest.size() < 4)
                return std::unexpected { SpawnSpecError::Truncated };
            std::uint32_t value = 0;
            for (auto const index: std::views::iota(0U, 4U))
                value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(_rest[index])) << (8U * index);
            _rest.remove_prefix(4);
            return value;
        }

        [[nodiscard]] std::expected<std::string, SpawnSpecError> string()
        {
            return u32().and_then([this](std::uint32_t size) -> std::expected<std::string, SpawnSpecError> {
                if (_rest.size() < size)
                    return std::unexpected { SpawnSpecError::Truncated };
                auto result = std::string { _rest.substr(0, size) };
                _rest.remove_prefix(size);
                return result;
            });
        }

        [[nodiscard]] std::expected<std::vector<std::string>, SpawnSpecError> strings()
        {
            auto const count = u32();
            if (!count)
                return std::unexpected { count.error() };
            std::vector<std::string> result;
            // Each element needs at least its 4-byte length, which bounds a hostile count.
            for ([[maybe_unused]] auto const _: std::views::iota(0U, *count))
            {
                auto item = string();
                if (!item)
                    return std::unexpected { item.error() };
                result.push_back(std::move(*item));
            }
            return result;
        }

        template <typename E>
        [[nodiscard]] std::expected<E, SpawnSpecError> enumerator()
        {
            return byte().and_then([](std::uint8_t value) -> std::expected<E, SpawnSpecError> {
                if (value >= EnumeratorCount<E>)
                    return std::unexpected { SpawnSpecError::UnknownEnumerator };
                return static_cast<E>(value);
            });
        }

      private:
        std::string_view _rest;
    };

    /// Reads a presence byte; anything but 0 or 1 is not a presence byte.
    [[nodiscard]] std::expected<bool, SpawnSpecError> presence(Reader& reader)
    {
        return reader.byte().and_then([](std::uint8_t value) -> std::expected<bool, SpawnSpecError> {
            if (value > 1)
                return std::unexpected { SpawnSpecError::UnknownEnumerator };
            return value == 1;
        });
    }
} // namespace

std::string encodeSpawnSpec(SpawnSpec const& spec)
{
    auto counter = SizeCounter {};
    putSpec(counter, spec);
    auto out = std::string {};
    out.reserve(counter.size);
    auto sink = StringSink { out };
    putSpec(sink, spec);
    return out;
}

std::string encodeFramedSpawnSpec(SpawnSpec const& spec)
{
    auto counter = SizeCounter {};
    putSpec(counter, spec);
    auto out = std::string {};
    out.reserve(SpawnSpecLengthSize + counter.size);
    auto sink = StringSink { out };
    putU32(sink, static_cast<std::uint32_t>(counter.size));
    putSpec(sink, spec);
    return out;
}

std::expected<std::uint32_t, SpawnSpecError> decodeSpawnSpecLength(std::string_view prefix)
{
    return Reader { prefix.substr(0, std::min(prefix.size(), SpawnSpecLengthSize)) }.u32();
}

std::expected<SpawnSpec, SpawnSpecError> decodeSpawnSpec(std::string_view bytes)
{
    using Step = std::expected<void, SpawnSpecError>;

    auto reader = Reader { bytes };
    auto spec = SpawnSpec {};

    // Stores a read value into its field; the chain below runs each read only if the previous succeeded.
    auto const into = [](auto& target, auto&& result) -> Step {
        if (!result)
            return std::unexpected { result.error() };
        target = std::move(*result);
        return {};
    };
    auto const optionalInto = [&](auto& target, auto read) -> Step {
        return presence(reader).and_then([&](bool present) -> Step {
            if (!present)
                return {};
            auto value = read();
            if (!value)
                return std::unexpected { value.error() };
            target.emplace(std::move(*value));
            return {};
        });
    };

    // The launch's fields are read into the alternative its mode byte names, once the shared ones are.
    auto const readLaunch = [&](std::uint8_t mode) -> Step {
        switch (mode)
        {
            case LaunchIndex<TerminalLaunch>: {
                auto& launch = spec.launch.emplace<TerminalLaunch>();
                return into(launch.fallbackArgv, reader.strings())
                    .and_then([&] { return into(launch.workingDirectory, reader.string()); })
                    .and_then([&] {
                        return into(launch.controllingTerminal, reader.enumerator<ControllingTerminal>());
                    })
                    .and_then(
                        [&] { return into(launch.terminalSettings, reader.enumerator<TerminalSettings>()); })
                    .and_then([&] { return into(launch.failureReport, reader.enumerator<FailureReport>()); })
                    .and_then([&] { return into(launch.failureExit, reader.enumerator<FailureExit>()); });
            }
            case LaunchIndex<DetachedLaunch>: spec.launch.emplace<DetachedLaunch>(); return {};
            default: return std::unexpected { SpawnSpecError::UnknownEnumerator };
        }
    };

    auto mode = std::uint8_t { 0 };
    return into(mode, reader.byte())
        .and_then([&]() -> Step {
            if (mode >= std::variant_size_v<SpawnLaunch>)
                return std::unexpected { SpawnSpecError::UnknownEnumerator };
            return {};
        })
        .and_then([&] { return into(spec.image, reader.string()); })
        .and_then([&] { return into(spec.argv, reader.strings()); })
        .and_then([&] { return into(spec.lookup, reader.enumerator<ExecLookup>()); })
        .and_then([&] { return optionalInto(spec.environment, [&] { return reader.strings(); }); })
        .and_then([&] {
            return optionalInto(spec.oomScoreAdjust, [&]() -> std::expected<int, SpawnSpecError> {
                return reader.u32().transform([](std::uint32_t raw) { return std::bit_cast<int>(raw); });
            });
        })
        .and_then([&] {
            return reader.byte().and_then([&](std::uint8_t bits) -> Step {
                if ((bits & ~AllSlotBits) != 0)
                    return std::unexpected { SpawnSpecError::UnknownEnumerator };
                spec.inheritedSlots = TrampolineSlots::fromRaw(bits);
                return {};
            });
        })
        .and_then([&] { return readLaunch(mode); })
        .and_then([&]() -> Step {
            if (!reader.atEnd())
                return std::unexpected { SpawnSpecError::TrailingBytes };
            if (spec.argv.empty())
                return std::unexpected { SpawnSpecError::EmptyArgv };
            return {};
        })
        .transform([&] { return std::move(spec); });
}

TrampolineInvocation classifyInvocation(std::span<char const* const> argv) noexcept
{
    if (argv.size() < 2 || argv[1] == nullptr)
        return TrampolineInvocation::NotRequested;
    auto const arg = std::string_view { argv[1] };
    if (!arg.starts_with(SpawnTrampolineMarker))
        return TrampolineInvocation::NotRequested;
    auto const version = arg.substr(SpawnTrampolineMarker.size());
    auto parsed = std::uint32_t { 0 };
    auto const [end, ec] = std::from_chars(version.data(), version.data() + version.size(), parsed);
    auto const wholeNumber = ec == std::errc {} && end == version.data() + version.size();
    return wholeNumber && parsed == SpawnTrampolineProtocolVersion ? TrampolineInvocation::Supported
                                                                   : TrampolineInvocation::UnsupportedVersion;
}

} // namespace vtpty
