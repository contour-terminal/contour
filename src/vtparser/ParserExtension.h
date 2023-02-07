#pragma once

#include <functional>
#include <string>

namespace terminal
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
    virtual void finalize() = 0;
};

class SimpleStringCollector: public ParserExtension
{
  public:
    explicit SimpleStringCollector(std::function<void(std::string_view)> done): _done { std::move(done) } {}

    void pass(char ch) override { _data.push_back(ch); }

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

} // namespace terminal
