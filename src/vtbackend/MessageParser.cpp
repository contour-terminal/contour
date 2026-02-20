// SPDX-License-Identifier: Apache-2.0
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

void MessageParser::pass(char ch)
{
    switch (_state)
    {
        case State::ParamKey:
            if (ch == ',')
                flushHeader();
            else if (ch == ';')
                _state = State::BodyStart;
            else if (ch == '=')
                _state = State::ParamValue;
            else if (_parsedKey.size() < MaxKeyLength)
                _parsedKey.push_back(ch);
            break;
        case State::ParamValue:
            if (ch == ',')
            {
                flushHeader();
                _state = State::ParamKey;
            }
            else if (ch == ';')
                _state = State::BodyStart;
            else if (_parsedValue.size() < MaxValueLength)
                _parsedValue.push_back(ch);
            break;
        case State::BodyStart:
            flushHeader();
            // TODO: check if a transport-encoding header was specified and make use of that,
            //       so that the body directly contains decoded raw data.
            _state = State::Body;
            [[fallthrough]];
        case State::Body:
            if (_body.size() < MaxBodyLength)
                _body.push_back(static_cast<uint8_t>(ch));
            // TODO: In order to avoid needless copies, I could pass the body incrementally back to the
            // caller.
            break;
    }
}

void MessageParser::flushHeader()
{
    bool const hasSpaceAvailable = _headers.size() < MaxParamCount || _headers.count(_parsedKey);
    bool const isValidParameter = !_parsedKey.empty();

    if (_parsedValue.size() > 1 && _parsedValue[0] == '!')
    {
        auto decoded = std::string {};
        decoded.resize(crispy::base64::decodeLength(next(begin(_parsedValue)), end(_parsedValue)));
        auto const actualSize =
            crispy::base64::decode(next(begin(_parsedValue)), end(_parsedValue), decoded.data());
        decoded.resize(actualSize);
        _parsedValue = std::move(decoded);
    }

    if (hasSpaceAvailable && isValidParameter)
        _headers[std::move(_parsedKey)] = std::move(_parsedValue);

    _parsedKey.clear();
    _parsedValue.clear();
}

void MessageParser::finalize()
{
    switch (_state)
    {
        case State::ParamKey:
        case State::ParamValue: flushHeader(); break;
        case State::BodyStart: break;
        case State::Body:
            if (_body.size() > 1 && _body[0] == '!')
            {
                auto decoded = std::vector<uint8_t> {};
                decoded.resize(crispy::base64::decodeLength(next(begin(_body)), end(_body)));
                auto const actualSize = crispy::base64::decode(
                    next(begin(_body)), end(_body), reinterpret_cast<char*>(decoded.data()));
                decoded.resize(actualSize);
                _body = std::move(decoded);
            }
            break;
    }
    _finalizer(Message(std::move(_headers), std::move(_body)));
}

Message MessageParser::parse(std::string_view range)
{
    Message m;
    auto mp = MessageParser([&](Message&& message) { m = std::move(message); });
    mp.parseFragment(range);
    mp.finalize();
    return m;
}

} // namespace vtbackend
