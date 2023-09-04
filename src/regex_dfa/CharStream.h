// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <iosfwd>
#include <istream>
#include <string>

namespace regex_dfa
{

class CharStream
{
  public:
    virtual ~CharStream() = default;

    [[nodiscard]] virtual bool isEof() const noexcept = 0;
    virtual char get() = 0;
    virtual void rollback(int count) = 0;
    virtual void rewind() = 0;
};

class StringStream: public CharStream
{
  public:
    explicit StringStream(std::string&& s): _source { std::move(s) } {}

    [[nodiscard]] bool isEof() const noexcept override { return _pos >= _source.size(); }
    char get() override { return _source[_pos++]; }
    void rollback(int count) override { _pos -= count; }
    void rewind() override { _pos = 0; }

  private:
    std::string _source;
    size_t _pos = 0;
};

class StandardStream: public CharStream
{
  public:
    explicit StandardStream(std::istream* source);

    [[nodiscard]] bool isEof() const noexcept override { return !_source->good(); }
    char get() override { return static_cast<char>(_source->get()); }

    void rollback(int count) override
    {
        _source->clear();
        _source->seekg(-count, std::ios::cur);
    }

    void rewind() override
    {
        _source->clear();
        _source->seekg(_initialOffset, std::ios::beg);
    }

  private:
    std::istream* _source;
    std::streamoff _initialOffset;
};

} // namespace regex_dfa
