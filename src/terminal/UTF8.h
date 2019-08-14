#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace utf8 {

// XXX some type traits (TODO use STD specialization instead)
constexpr bool isASCII(wchar_t x) noexcept
{
    return x <= 0x7F;
}

constexpr bool isLatin(wchar_t x) noexcept
{
    return x < 0xFF;
}

constexpr bool isC1(wchar_t x) noexcept
{
    return x >= 0x7F      // std::numeric_limits<terminal::C1_8bit>::min()
           && x <= 0x9F;  // std::numeric_limits<terminal::C1_8bit>::max()
}

constexpr bool isControl(wchar_t x) noexcept
{
    return (0 <= x && x < 0x1F) || (0x80 <= x && x <= 0x9F);
}

//! Decodes an UTF8 byte stream into Unicode characters of type wchar_t.
class Decoder {
  public:
    constexpr Decoder() noexcept {}

    constexpr void reset() noexcept
    {
        expectedLength_ = 0;
        currentLength_ = 0;
        character_ = 0;
    }

    struct Incomplete {};
    struct Invalid { static constexpr wchar_t replacementCharacter {0xFFFD}; };
    struct Success { wchar_t value; };
    using Result = std::variant<Incomplete, Invalid, Success>;

    constexpr Result decode(uint8_t _byte)
    {
        if (!expectedLength_)
        {
            if ((_byte >> 7) == 0)
            {
                expectedLength_ = 1;
                character_ = _byte;
            }
            else if ((_byte >> 5) == 0b110)
            {
                expectedLength_ = 2;
                character_ = _byte & 0b0001'1111;
            }
            else if ((_byte >> 4) == 0b1110)
            {
                expectedLength_ = 3;
                character_ = _byte & 0b0000'1111;
            }
            else if ((_byte >> 3) == 0b1111'0)
            {
                expectedLength_ = 4;
                character_ = _byte & 0b0000'0111;
            }
            else
            {
                reset();
                return {Invalid{}};
            }
        }
        else
        {
            character_ <<= 6;
            character_ |= _byte & 0b0011'1111;
        }
        currentLength_++;

        if (isIncomplete())
            return {Incomplete{}};
        else
        {
            auto const result = character_;
            reset();
            return {Success{result}};
        }
    }

    template <typename... Args>
    constexpr Result decode(uint8_t _b0, uint8_t _b1, Args... args)
    {
        auto x = decode(_b0);
        if (std::holds_alternative<Success>(x))
            throw std::invalid_argument{"decoding finished early"};
        return decode(_b1, std::forward<Args>(args)...);
    }

    constexpr Result operator()(uint8_t _byte) { return decode(_byte); }

    template <typename... Args>
    constexpr Result operator()(uint8_t _b0, uint8_t _b1, Args... args)
    {
        return decode(_b0, _b1, args...);
    }

  private:
    constexpr bool isIncomplete() const noexcept { return currentLength_ < expectedLength_; }

  private:
    size_t expectedLength_{};
    size_t currentLength_{};
    wchar_t character_{};
};

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...)->overloaded<Ts...>;

inline std::wstring decode(char const* begin, char const* end)
{
    std::wstring out;
    for (auto decode = Decoder{}; begin != end; ++begin)
    {
        visit(
            overloaded{
                [&](Decoder::Incomplete) {},
                [&](Decoder::Invalid) { out += L'?'; },
                [&](Decoder::Success ok) { out += ok.value; },
            },
            decode(*begin));
    }
    return out;
}

// UTF8-representation of Unicode character.
struct Bytes : public std::vector<uint8_t> {
    explicit Bytes(uint8_t b0) {
        emplace_back(b0);
    }

    Bytes(uint8_t b0, uint8_t b1) {
        emplace_back(b0);
        emplace_back(b1);
    }

    Bytes(uint8_t b0, uint8_t b1, uint8_t b2) {
        emplace_back(b0);
        emplace_back(b1);
        emplace_back(b2);
    }

    Bytes(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
        emplace_back(b0);
        emplace_back(b1);
        emplace_back(b2);
        emplace_back(b3);
    }
};

std::string to_string(Bytes const& ch);

inline std::string to_string(Bytes const& _utf8)
{
    std::string s;
    s.reserve(_utf8.size());
    for (auto b: _utf8)
        s.push_back(static_cast<char>(b));
    return s;
}

inline Bytes encode(wchar_t character)
{
    if (character <= 0x7F)
        return Bytes{static_cast<uint8_t>(character & 0b0111'1111)};
    else if (character <= 0x07FF)
        return Bytes{static_cast<uint8_t>(((character >> 6) & 0b0001'1111) | 0b1100'0000),
                     static_cast<uint8_t>(((character >> 0) & 0b0011'1111) | 0b1000'0000)};
    else if (character <= 0xFFFF)
        return Bytes{static_cast<uint8_t>(((character >> 12) & 0b0000'1111) | 0b1110'0000),
                     static_cast<uint8_t>(((character >> 6) & 0b0011'1111) | 0b1000'0000),
                     static_cast<uint8_t>(((character >> 0) & 0b0011'1111) | 0b1000'0000)};
    else
        return Bytes{static_cast<uint8_t>(((character >> 18) & 0b0000'0111) | 0b1111'0000),
                     static_cast<uint8_t>(((character >> 12) & 0b0011'1111) | 0b1000'0000),
                     static_cast<uint8_t>(((character >> 6) & 0b0011'1111) | 0b1000'0000),
                     static_cast<uint8_t>(((character >> 0) & 0b0011'1111) | 0b1000'0000)};
}

}  // namespace utf8
