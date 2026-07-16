// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace vtbackend
{

/// VT parser protocol extension.
///
/// Used to implemented sub-parsers.
///
/// @see SixelParser
class ParserExtension
{
  public:
    virtual ~ParserExtension() = default;

    virtual void pass(char ch) = 0;

    /// Passes a whole run of payload bytes at once.
    ///
    /// The default loops over pass(char); an extension overrides it only if it can do better than
    /// a byte at a time. The run never contains anything the VT state machine must act on.
    /// @param bytes The payload bytes.
    virtual void pass(std::string_view bytes)
    {
        for (auto const ch: bytes)
            pass(ch);
    }

    virtual void finalize() = 0;
};

class SimpleStringCollector: public ParserExtension
{
  public:
    explicit SimpleStringCollector(std::function<void(std::string_view)> done): _done { std::move(done) } {}

    void pass(char ch) override { _data.push_back(ch); }
    void pass(std::string_view bytes) override { _data.append(bytes); }

    void finalize() override
    {
        if (_done)
            _done(_data);
        _data.clear();
    }

  private:
    std::string _data;
    std::function<void(std::string_view)> _done;
};

} // namespace vtbackend
