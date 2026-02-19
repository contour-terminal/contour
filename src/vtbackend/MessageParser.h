// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtparser/ParserExtension.h>

#include <crispy/algorithm.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vtbackend
{

/**
 * HTTP-like simple parametrized message object.
 *
 * A Message provides a zero or more unique header/value pairs and an optional message body.
 */
class Message
{
  public:
    using HeaderMap = std::unordered_map<std::string, std::string>;
    using Data = std::vector<uint8_t>;

    Message() = default;
    Message(Message const&) = default;
    Message(Message&&) = default;
    Message& operator=(Message const&) = default;
    Message& operator=(Message&&) = default;

    Message(HeaderMap headers, Data body): _headers { std::move(headers) }, _body { std::move(body) } {}

    HeaderMap const& headers() const noexcept { return _headers; }
    HeaderMap& headers() noexcept { return _headers; }

    std::string const* header(std::string const& key) const noexcept
    {
        if (auto const i = _headers.find(key); i != _headers.end())
            return &i->second;
        else
            return nullptr;
    }

    Data const& body() const noexcept { return _body; }
    Data takeBody() noexcept { return std::move(_body); }

  private:
    HeaderMap _headers;
    Data _body;
};

/**
 * MessageParser provides an API for parsing simple parametrized messages.
 *
 * The format is more simple than HTTP messages.
 * You have a set of headers (key/value pairs)) and an optional body.
 *
 * Duplicate header names will override the previously declared ones.
 *
 * - Headers and body are separated by ';'
 * - Header entries are separated by ','
 * - Header name and value is separated by '='
 *
 * Therefore the header name must not contain any ';', ',', '=',
 * and the parameter value must not contain any ';', ',', '!'.
 *
 * In order to allow arbitrary header values or body contents,
 * it may be encoded using Base64.
 * Base64-encoding is introduced with a leading exclamation mark (!).
 *
 * Examples:
 *
 * - "first=Foo,second=Bar;some body here"
 * - ",first=Foo,second,,,another=value,also=;some body here"
 * - "message=!SGVsbG8gV29ybGQ=" (no body, only one Base64 encoded header)
 * - ";!SGVsbG8gV29ybGQ=" (no headers, only one Base64 encoded body)
 */
class MessageParser: public ParserExtension
{
  public:
    constexpr static inline size_t MaxKeyLength = 64;
    constexpr static inline size_t MaxValueLength = 512;
    constexpr static inline size_t MaxParamCount = 32;
    constexpr static inline size_t MaxBodyLength = 16 * 1024 * 1024; // 16 MB

    using OnFinalize = std::function<void(Message&&)>;

    explicit MessageParser(OnFinalize finalizer = {}): _finalizer { std::move(finalizer) } {}

    void parseFragment(std::string_view chars)
    {
        for (char const ch: chars)
            pass(ch);
    }

    static Message parse(std::string_view range);

    // ParserExtension overrides
    //
    void pass(char ch) override;
    void finalize() override;

  private:
    void flushHeader();

    enum class State : std::uint8_t
    {
        ParamKey,
        ParamValue,
        BodyStart,
        Body,
    };

    State _state = State::ParamKey;
    std::string _parsedKey;
    std::string _parsedValue;

    OnFinalize _finalizer;

    Message::HeaderMap _headers;
    Message::Data _body;
};

} // namespace vtbackend
