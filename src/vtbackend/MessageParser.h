/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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

    Message(HeaderMap _headers, Data _body): headers_ { std::move(_headers) }, body_ { std::move(_body) } {}

    HeaderMap const& headers() const noexcept { return headers_; }
    HeaderMap& headers() noexcept { return headers_; }

    std::string const* header(std::string const& _key) const noexcept
    {
        if (auto const i = headers_.find(_key); i != headers_.end())
            return &i->second;
        else
            return nullptr;
    }

    Data const& body() const noexcept { return body_; }
    Data takeBody() noexcept { return std::move(body_); }

  private:
    HeaderMap headers_;
    Data body_;
};

/**
 * MessageParser provides an API for parsing simple parametrized messages.
 *
 * The format is more simple than HTTP messages.
 * You have a set of headers (key/value pairs)) and an optional body.
 *
 * Duplicate header names will override the previously declared ones.
 *
 * - Headers and body are seperated by ';'
 * - Header entries are seperated by ','
 * - Header name and value is seperated by '='
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

    explicit MessageParser(OnFinalize _finalizer = {}): finalizer_ { std::move(_finalizer) } {}

    void parseFragment(std::string_view chars)
    {
        for (char const ch: chars)
            pass(ch);
    }

    static Message parse(std::string_view _range);

    // ParserExtension overrides
    //
    void pass(char _char) override;
    void finalize() override;

  private:
    void flushHeader();

    enum class State
    {
        ParamKey,
        ParamValue,
        BodyStart,
        Body,
    };

    State state_ = State::ParamKey;
    std::string parsedKey_;
    std::string parsedValue_;

    OnFinalize finalizer_;

    Message::HeaderMap headers_;
    Message::Data body_;
};

} // namespace vtbackend
