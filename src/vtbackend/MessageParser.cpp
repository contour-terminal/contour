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
#include <vtbackend/MessageParser.h>

#include <crispy/base64.h>

#include <iostream>
#include <vector>

namespace vtbackend
{

// XXX prominent use case:
//
// Good Image Protocol
// ===================
//
// DCS u format=N width=N height=N id=S pixmap=D
// DCS r id=S rows=N cols=N align=N? resize=N? [x=N y=N w=N h=N] reqStatus?
// DCS s rows=N cols=N align=N? resize=N? pixmap=D
// DCS d id=S

void MessageParser::pass(char _char)
{
    switch (state_)
    {
        case State::ParamKey:
            if (_char == ',')
                flushHeader();
            else if (_char == ';')
                state_ = State::BodyStart;
            else if (_char == '=')
                state_ = State::ParamValue;
            else if (parsedKey_.size() < MaxKeyLength)
                parsedKey_.push_back(_char);
            break;
        case State::ParamValue:
            if (_char == ',')
            {
                flushHeader();
                state_ = State::ParamKey;
            }
            else if (_char == ';')
                state_ = State::BodyStart;
            else if (parsedValue_.size() < MaxValueLength)
                parsedValue_.push_back(_char);
            break;
        case State::BodyStart:
            flushHeader();
            // TODO: check if a transport-encoding header was specified and make use of that,
            //       so that the body directly contains decoded raw data.
            state_ = State::Body;
            [[fallthrough]];
        case State::Body:
            if (body_.size() < MaxBodyLength)
                body_.push_back(static_cast<uint8_t>(_char));
            // TODO: In order to avoid needless copies, I could pass the body incrementally back to the
            // caller.
            break;
    }
}

void MessageParser::flushHeader()
{
    bool const hasSpaceAvailable = headers_.size() < MaxParamCount || headers_.count(parsedKey_);
    bool const isValidParameter = !parsedKey_.empty();

    if (!parsedValue_.empty() && parsedValue_[0] == '!')
    {
        auto decoded = std::string {};
        decoded.resize(crispy::base64::decodeLength(next(begin(parsedValue_)), end(parsedValue_)));
        crispy::base64::decode(next(begin(parsedValue_)), end(parsedValue_), &decoded[0]);
        parsedValue_ = std::move(decoded);
    }

    if (hasSpaceAvailable && isValidParameter)
        headers_[std::move(parsedKey_)] = std::move(parsedValue_);

    parsedKey_.clear();
    parsedValue_.clear();
}

void MessageParser::finalize()
{
    switch (state_)
    {
        case State::ParamKey:
        case State::ParamValue: flushHeader(); break;
        case State::BodyStart: break;
        case State::Body:
            if (!body_.empty() && body_[0] == '!')
            {
                auto decoded = std::vector<uint8_t> {};
                decoded.resize(crispy::base64::decodeLength(next(begin(body_)), end(body_)));
                crispy::base64::decode(next(begin(body_)), end(body_), (char*) &decoded[0]);
                body_ = std::move(decoded);
            }
            break;
    }
    finalizer_(Message(std::move(headers_), std::move(body_)));
}

Message MessageParser::parse(std::string_view _range)
{
    Message m;
    auto mp = MessageParser([&](Message&& _message) { m = std::move(_message); });
    mp.parseFragment(_range);
    mp.finalize();
    return m;
}

} // namespace vtbackend
