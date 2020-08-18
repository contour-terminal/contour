#pragma once

#include <functional>
#include <string>

namespace terminal {

/// VT parser protocol extension.
///
/// Used to implemented sub-parsers.
///
/// @see SixelParser
class ParserExtension
{
  public:
    virtual ~ParserExtension() = default;

    virtual void start() = 0;
    virtual void pass(char32_t _char) = 0;
    virtual void finalize() = 0;
};

class SimpleStringCollector : public ParserExtension
{
  public:
    explicit SimpleStringCollector(std::function<void(std::u32string const&)> _done)
        : data_{},
          done_{ std::move(_done) }
    {}

    void start() override
    {
        data_.clear();
    }

    void pass(char32_t _char) override
    {
        data_.push_back(_char);
    }

    void finalize() override
    {
        if (done_)
            done_(data_);
    }

  private:
    std::u32string data_;
    std::function<void(std::u32string const&)> done_;
};

} // end namespace
