/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <string>
#include <string_view>

namespace crispy::base64
{

namespace detail
{
    // clang-format off
    constexpr inline unsigned char indexmap[256] = {
        /* ASCII table */
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, //   0..15
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, //  16..31
    //                                              43  44  45  46  47       +/
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63, //  32..47
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64, //  48..63
        64, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, //  64..95
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64, //  80..95
        64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, //  96..111
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64, // 112..127
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 128..143
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 144..150
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 160..175
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 176..191
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 192..207
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 208..223
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, // 224..239
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64  // 240..255
    };
    // clang-format on
} // namespace detail

struct EncoderState
{
    int modulo = 0;
    uint8_t pending[3];
};

template <typename Alphabet, typename Sink>
constexpr void encode(uint8_t _byte, Alphabet const& alphabet, EncoderState& _state, Sink&& _sink)
{
    _state.pending[_state.modulo] = _byte;
    if (++_state.modulo != 3)
        return;

    _state.modulo = 0;
    uint8_t const* input = _state.pending;
    char const out[4] = { alphabet[(input[0] >> 2) & 0x3F],
                          alphabet[((input[0] & 0x03) << 4) | ((uint8_t) (input[1] & 0xF0) >> 4)],
                          alphabet[((input[1] & 0x0F) << 2) | ((uint8_t) (input[2] & 0xC0) >> 6)],
                          alphabet[input[2] & 0x3F] };
    _sink(std::string_view(out, 4));
}

template <typename Alphabet, typename Sink>
constexpr void finish(Alphabet const& alphabet, EncoderState& _state, Sink&& _sink)
{
    if (_state.modulo == 0)
        return;

    auto const* input = _state.pending;

    switch (_state.modulo)
    {
    case 2: {
        char const out[4] = { alphabet[(input[0] >> 2) & 0x3F],
                              alphabet[((input[0] & 0x03) << 4) | ((uint8_t) (input[1] & 0xF0) >> 4)],
                              alphabet[((input[1] & 0x0F) << 2)],
                              '=' };
        _sink(std::string_view { out });
    }
    break;
    case 1: {
        char const out[4] = {
            alphabet[(input[0] >> 2) & 0x3F], alphabet[((input[0] & 0x03) << 4)], '=', '='
        };
        _sink(std::string_view { out });
    }
    break;
    case 0: break;
    }
}

template <typename Sink>
constexpr void encode(uint8_t _byte, EncoderState& _state, Sink&& _sink)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "abcdefghijklmnopqrstuvwxyz"
                                "0123456789+/";
    return encode(_byte, alphabet, _state, std::forward<Sink>(_sink));
    // return encode(_byte, detail::indexmap, _state, std::forward<Sink>(_sink));
}

template <typename Sink>
constexpr void finish(EncoderState& _state, Sink&& _sink)
{
    finish(detail::indexmap, _state, std::forward<Sink>(_sink));
}

template <typename Iterator, typename Alphabet>
std::string encode(Iterator begin, Iterator end, Alphabet alphabet)
{
    std::string output;
    output.reserve(((std::distance(begin, end) + 2) / 3 * 4) + 1);

    EncoderState state {};
    for (auto i = begin; i != end; ++i)
        encode(*i, alphabet, state, [&](std::string_view _data) { output += _data; });
    finish(alphabet, state, [&](std::string_view _data) { output += _data; });

    return output;
}

template <typename Iterator>
std::string encode(Iterator begin, Iterator end)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                       "abcdefghijklmnopqrstuvwxyz"
                                       "0123456789+/";
    return encode(begin, end, alphabet);
}

inline std::string encode(std::string_view _value)
{
    return encode(_value.begin(), _value.end());
}

template <typename Iterator, typename IndexTable>
size_t decodeLength(Iterator begin, Iterator end, IndexTable const& index)
{
    auto pos = begin;

    auto const indexSize = std::size(index);

    while (pos != end && index[static_cast<uint8_t>(*pos)] < indexSize)
        pos++;

    auto const nprbytes = std::distance(begin, pos) - 1;
    auto const nbytesdecoded = ((nprbytes + 3) / 4) * 3;

    return size_t(nbytesdecoded);
}

template <typename Iterator>
size_t decodeLength(Iterator begin, Iterator end)
{
    return decodeLength(begin, end, detail::indexmap);
}

inline size_t decodeLength(const std::string_view& value)
{
    return decodeLength(value.begin(), value.end());
}

template <typename Iterator, typename IndexTable, typename Output>
size_t decode(Iterator begin, Iterator end, const IndexTable& indexmap, Output output)
{
    auto const index = [indexmap](Iterator i) -> unsigned char {
        return indexmap[static_cast<uint8_t>(*i)];
    };

    if (begin == end)
        return 0;

    // count input bytes (excluding any trailing pad bytes)
    Iterator input = begin;
    while (input != end && index(input) <= 63)
        input++;
    size_t nprbytes = static_cast<unsigned>(std::distance(begin, input));
    size_t decodedCount = ((nprbytes + 3) / 4) * 3;

    auto out = output;
    input = begin;

    while (nprbytes > 4)
    {
        *out++ = (char) (index(input + 0) << 2 | index(input + 1) >> 4);
        *out++ = (char) (index(input + 1) << 4 | index(input + 2) >> 2);
        *out++ = (char) (index(input + 2) << 6 | index(input + 3));

        input += 4;
        nprbytes -= 4;
    }

    if (nprbytes > 1)
    {
        *(out++) = (char) (index(input + 0) << 2 | index(input + 1) >> 4);

        if (nprbytes > 2)
        {
            *(out++) = (char) (index(input + 1) << 4 | index(input + 2) >> 2);

            if (nprbytes > 3)
            {
                *(out++) = (char) (index(input + 2) << 6 | index(input + 3));
            }
        }
    }

    decodedCount -= (4 - nprbytes) & 0x03;

    return decodedCount;
}

template <typename Iterator, typename Output>
size_t decode(Iterator begin, Iterator end, Output output)
{
    return decode(begin, end, detail::indexmap, output);
}

template <typename Output>
size_t decode(std::string_view const& input, Output output)
{
    return decode(input.begin(), input.end(), output);
}

inline std::string decode(const std::string_view& input)
{
    std::string output;
    output.resize(decodeLength(input));
    output.resize(decode(input, &output[0]));
    return output;
}

} // namespace crispy::base64
