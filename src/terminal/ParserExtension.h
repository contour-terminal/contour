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
    virtual void pass(char _char) = 0;
    virtual void finalize() = 0;
};

class SimpleStringCollector : public ParserExtension
{
  public:
    explicit SimpleStringCollector(std::function<void(std::string_view)> _done):
        done_{ std::move(_done) }
    {}

    void start() override
    {
        data_.clear();
    }

    void pass(char _char) override
    {
        data_.push_back(_char);
    }

    void finalize() override
    {
        if (done_)
            done_(data_);
    }

  private:
    std::string data_;
    std::function<void(std::string_view)> done_;
};

} // end namespace
