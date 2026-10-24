// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/Uuid.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <random>
#include <ranges>

namespace vtcontrol
{

namespace
{
    constexpr auto HexDigits = std::string_view { "0123456789abcdef" };
    constexpr auto DashAfterByte = std::array<std::size_t, 4> { 4, 6, 8, 10 };
    constexpr uint32_t CounterBits = 12;
    constexpr uint32_t CounterMax = (1U << CounterBits) - 1;
    /// A fresh millisecond starts the counter in the lower part of its range, leaving room to count.
    constexpr uint32_t CounterStartMask = (1U << (CounterBits - 3)) - 1;
} // namespace

std::string toString(Uuid const& uuid)
{
    auto text = std::string {};
    text.reserve(36);
    for (auto const [index, byte]: uuid.bytes | std::views::enumerate)
    {
        if (std::ranges::contains(DashAfterByte, static_cast<std::size_t>(index)))
            text.push_back('-');
        text.push_back(HexDigits[byte >> 4]);
        text.push_back(HexDigits[byte & 0x0F]);
    }
    return text;
}

std::optional<Uuid> parseUuid(std::string_view text)
{
    if (text.size() != 36)
        return std::nullopt;

    auto uuid = Uuid {};
    auto nibbles = std::array<unsigned, 32> {};
    auto count = std::size_t { 0 };
    for (auto const [position, ch]: text | std::views::enumerate)
    {
        auto const isDashPosition = position == 8 || position == 13 || position == 18 || position == 23;
        if (isDashPosition)
        {
            if (ch != '-')
                return std::nullopt;
            continue;
        }
        auto const digit = core::digitValue<16>(ch);
        if (!digit)
            return std::nullopt;
        nibbles[count++] = *digit;
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, uuid.bytes.size()))
        uuid.bytes[index] = static_cast<uint8_t>((nibbles[(index * 2)] << 4) | nibbles[(index * 2) + 1]);
    return uuid;
}

Uuid makeUuidV7(std::chrono::sys_time<std::chrono::milliseconds> now,
                std::span<uint8_t const, 10> random) noexcept
{
    auto uuid = Uuid {};
    auto const millis = static_cast<uint64_t>(now.time_since_epoch().count());
    for (auto const index: std::views::iota(0, 6))
        uuid.bytes[static_cast<std::size_t>(index)] = static_cast<uint8_t>(millis >> (8 * (5 - index)));
    uuid.bytes[6] = static_cast<uint8_t>(0x70 | (random[0] & 0x0F));
    uuid.bytes[7] = random[1];
    uuid.bytes[8] = static_cast<uint8_t>(0x80 | (random[2] & 0x3F));
    for (auto const index: std::views::iota(std::size_t { 3 }, random.size()))
        uuid.bytes[index + 6] = random[index];
    return uuid;
}

UuidV7Source::UuidV7Source(core::platform::WallClockRef clock, RandomBits bits):
    _clock { clock }, _bits { std::move(bits) }
{
}

Uuid UuidV7Source::next()
{
    auto const lock = std::scoped_lock { _mutex };

    auto const now = std::chrono::time_point_cast<std::chrono::milliseconds>(_clock.now());
    if (now > _lastMillis)
    {
        _lastMillis = now;
        _counter = static_cast<uint32_t>(_bits()) & CounterStartMask;
    }
    else if (_counter >= CounterMax)
    {
        // Out of counter within this millisecond (or the clock went back): borrow the next one.
        _lastMillis += std::chrono::milliseconds { 1 };
        _counter = static_cast<uint32_t>(_bits()) & CounterStartMask;
    }
    else
        ++_counter;

    auto random = std::array<uint8_t, 10> {};
    random[0] = static_cast<uint8_t>(_counter >> 8);
    random[1] = static_cast<uint8_t>(_counter & 0xFF);
    auto const tail = _bits();
    for (auto const index: std::views::iota(std::size_t { 2 }, random.size()))
        random[index] = static_cast<uint8_t>(tail >> (8 * (index - 2)));
    return makeUuidV7(_lastMillis, random);
}

std::shared_ptr<UuidSource> systemUuidSource()
{
    auto device = std::make_shared<std::random_device>();
    return std::make_shared<UuidV7Source>(core::platform::defaultSystemWallClock(), [device]() -> uint64_t {
        auto const high = static_cast<uint64_t>((*device)());
        auto const low = static_cast<uint64_t>((*device)());
        return (high << 32) | low;
    });
}

} // namespace vtcontrol

std::size_t std::hash<vtcontrol::Uuid>::operator()(
    vtcontrol::Uuid const& uuid) const noexcept // NOLINT(readability-identifier-naming)
{
    // FNV-1a over the bytes.
    auto hash = uint64_t { 0xcbf29ce484222325ULL };
    for (auto const byte: uuid.bytes)
        hash = (hash ^ byte) * 0x100000001b3ULL;
    return static_cast<std::size_t>(hash);
}
