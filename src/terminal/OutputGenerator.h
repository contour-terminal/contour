/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

#include <terminal/Commands.h>
#include <terminal/InputGenerator.h> // KeyMode
#include <terminal/util/UTF8.h>

#include <fmt/format.h>

#include <functional>
#include <iostream>
#include <string_view>
#include <vector>

namespace terminal {

/// Encodes Command stream into ANSI codes and text.
class OutputGenerator {
  public:
    using Writer = std::function<void(char const*, size_t)>;

    explicit OutputGenerator(Writer writer) : writer_{std::move(writer)} {}
    explicit OutputGenerator(std::ostream& output) : OutputGenerator{[&](auto d, auto n) { output.write(d, n); }} {}
    explicit OutputGenerator(std::vector<char>& output) : OutputGenerator{[&](auto d, auto n) { output.insert(output.end(), d, d + n); }} {}
    ~OutputGenerator();

    void setCursorKeysMode(KeyMode _mode) noexcept { cursorKeysMode_ = _mode; }
    bool normalCursorKeys() const noexcept { return cursorKeysMode_ == KeyMode::Normal; }
    bool applicationCursorKeys() const noexcept { return !normalCursorKeys(); }

    void operator()(std::vector<Command> const& commands);
    void operator()(Command const& command);

    template <typename T, typename... Args>
    void emitCommand(Args&&... args)
    {
        (*this)(T{std::forward<Args>(args)...});
    }

    void flush();

    static std::string generate(std::vector<Command> const& commands)
    {
        auto output = std::string{};
        OutputGenerator{[&](auto d, auto n) { output += std::string{d, n}; }}(commands);
        return output;
    }

    static Color parseColor(std::string const& _value);

  private:
    static std::string flush(std::vector<unsigned> const& _sgr);
    void sgr_add(unsigned _param);

    void write(char32_t v)
    {
        write(utf8::encode(v));
    }

    void write(utf8::Bytes const& v)
    {
        flush();
        writer_((char const*) &v[0], v.size());
    }

    void write(std::string_view const& _s)
    {
        flush();
        writer_(_s.data(), _s.size());
    }

    template <typename... Args>
    void write(std::string_view const& _s, Args&&... _args)
    {
        write(fmt::format(_s, std::forward<Args>(_args)...));
    }

  private:
    Writer writer_;
    std::vector<unsigned> sgr_;
    Color currentForegroundColor_ = DefaultColor{};
    Color currentBackgroundColor_ = DefaultColor{};
    KeyMode cursorKeysMode_ = KeyMode::Normal;
};

}  // namespace terminal
