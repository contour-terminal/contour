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
    explicit StringStream(std::string&& s): source_ { std::move(s) } {}

    [[nodiscard]] bool isEof() const noexcept override { return pos_ >= source_.size(); }
    char get() override { return source_[pos_++]; }
    void rollback(int count) override { pos_ -= count; }
    void rewind() override { pos_ = 0; }

  private:
    std::string source_;
    size_t pos_ = 0;
};

class StandardStream: public CharStream
{
  public:
    explicit StandardStream(std::istream* source);

    [[nodiscard]] bool isEof() const noexcept override { return !source_->good(); }
    char get() override { return static_cast<char>(source_->get()); }

    void rollback(int count) override
    {
        source_->clear();
        source_->seekg(-count, std::ios::cur);
    }

    void rewind() override
    {
        source_->clear();
        source_->seekg(initialOffset_, std::ios::beg);
    }

  private:
    std::istream* source_;
    std::streamoff initialOffset_;
};

} // namespace regex_dfa
