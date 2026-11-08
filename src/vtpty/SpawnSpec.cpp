// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SpawnSpec.hpp>

#include <array>
#include <bit>
#include <charconv>
#include <ranges>

namespace vtpty
{

namespace
{
    /// The number of enumerators of each wire enum; the decoder rejects anything at or past it.
    template <typename E>
    inline constexpr std::uint8_t EnumeratorCount = 0;

    template <>
    inline constexpr std::uint8_t EnumeratorCount<SpawnMode> = 2;
    template <>
    inline constexpr std::uint8_t EnumeratorCount<ExecLookup> = 2;
    template <>
    inline constexpr std::uint8_t EnumeratorCount<ControllingTerminal> = 2;
    template <>
    inline constexpr std::uint8_t EnumeratorCount<TerminalSettings> = 2;
    template <>
    inline constexpr std::uint8_t EnumeratorCount<FailureReport> = 2;

    static_assert(static_cast<int>(SpawnMode::Detached) + 1 == EnumeratorCount<SpawnMode>);
    static_assert(static_cast<int>(ExecLookup::SearchPath) + 1 == EnumeratorCount<ExecLookup>);
    static_assert(static_cast<int>(ControllingTerminal::Acquire) + 1 == EnumeratorCount<ControllingTerminal>);
    static_assert(static_cast<int>(TerminalSettings::Configure) + 1 == EnumeratorCount<TerminalSettings>);
    static_assert(static_cast<int>(FailureReport::OnTerminal) + 1 == EnumeratorCount<FailureReport>);
    static_assert(static_cast<int>(TrampolineSlot::Output) + 1 == TrampolineSlotCount);

    constexpr std::uint8_t AllSlotBits = static_cast<std::uint8_t>((1U << TrampolineSlotCount) - 1U);

    void putU32(std::string& out, std::uint32_t value)
    {
        for (auto const shift: std::views::iota(0U, 4U))
            out.push_back(static_cast<char>((value >> (8U * shift)) & 0xFFU));
    }

    void putByte(std::string& out, std::uint8_t value)
    {
        out.push_back(static_cast<char>(value));
    }

    template <typename E>
    void putEnum(std::string& out, E value)
    {
        putByte(out, static_cast<std::uint8_t>(value));
    }

    void putString(std::string& out, std::string_view value)
    {
        putU32(out, static_cast<std::uint32_t>(value.size()));
        out.append(value);
    }

    void putStrings(std::string& out, std::span<std::string const> values)
    {
        putU32(out, static_cast<std::uint32_t>(values.size()));
        for (auto const& value: values)
            putString(out, value);
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
            for (auto const _: std::views::iota(0U, *count))
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
    std::string out;
    putEnum(out, spec.mode);
    putString(out, spec.image);
    putStrings(out, spec.argv);
    putEnum(out, spec.lookup);
    putStrings(out, spec.fallbackArgv);
    putString(out, spec.fallbackText);
    putString(out, spec.workingDirectory);
    putByte(out, spec.environment.has_value() ? 1 : 0);
    if (spec.environment)
        putStrings(out, *spec.environment);
    putByte(out, spec.oomScoreAdjust.has_value() ? 1 : 0);
    if (spec.oomScoreAdjust)
        putU32(out, std::bit_cast<std::uint32_t>(*spec.oomScoreAdjust));
    putEnum(out, spec.controllingTerminal);
    putEnum(out, spec.terminalSettings);
    putEnum(out, spec.failureReport);
    putByte(out, spec.inheritedSlots.raw());
    return out;
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

    return into(spec.mode, reader.enumerator<SpawnMode>())
        .and_then([&] { return into(spec.image, reader.string()); })
        .and_then([&] { return into(spec.argv, reader.strings()); })
        .and_then([&] { return into(spec.lookup, reader.enumerator<ExecLookup>()); })
        .and_then([&] { return into(spec.fallbackArgv, reader.strings()); })
        .and_then([&] { return into(spec.fallbackText, reader.string()); })
        .and_then([&] { return into(spec.workingDirectory, reader.string()); })
        .and_then([&] { return optionalInto(spec.environment, [&] { return reader.strings(); }); })
        .and_then([&] {
            return optionalInto(spec.oomScoreAdjust, [&]() -> std::expected<int, SpawnSpecError> {
                return reader.u32().transform([](std::uint32_t raw) { return std::bit_cast<int>(raw); });
            });
        })
        .and_then([&] { return into(spec.controllingTerminal, reader.enumerator<ControllingTerminal>()); })
        .and_then([&] { return into(spec.terminalSettings, reader.enumerator<TerminalSettings>()); })
        .and_then([&] { return into(spec.failureReport, reader.enumerator<FailureReport>()); })
        .and_then([&] {
            return reader.byte().and_then([&](std::uint8_t bits) -> Step {
                if ((bits & ~AllSlotBits) != 0)
                    return std::unexpected { SpawnSpecError::UnknownEnumerator };
                spec.inheritedSlots = TrampolineSlots::fromRaw(bits);
                return {};
            });
        })
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
