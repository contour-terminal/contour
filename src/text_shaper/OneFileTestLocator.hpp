// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <text_shaper/FontLocator.hpp>

#include <string>
#include <utility>

namespace text::test
{

/// Answers every query with one font file, so that a shaper loads exactly the file a test chose -- a
/// system font, say, or a path that is no font at all.
class OneFileLocator final: public FontLocator
{
  public:
    /// @param path The font file every query answers with.
    explicit OneFileLocator(std::string path): _path { std::move(path) } {}

    [[nodiscard]] FontSourceList all() override { return { FontPath { .value = _path } }; }

    [[nodiscard]] FontSourceList locate(FontDescription const& /*description*/) override { return all(); }

    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> /*codepoints*/) override { return all(); }

  private:
    std::string _path;
};

} // namespace text::test
