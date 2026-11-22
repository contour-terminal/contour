// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>

namespace contour::testing
{

/// A copy of a text in a heap block of exactly its size. Nothing follows its last byte -- no terminating NUL,
/// no slack -- so under AddressSanitizer a reader that looks one byte past view() is reported, where a
/// `std::string` or a string literal would hand it a NUL instead.
class ExactBuffer
{
  public:
    /// @param text The bytes to copy.
    explicit ExactBuffer(std::string_view text):
        _bytes { std::make_unique<char[]>(text.size()) }, _size { text.size() }
    {
        std::ranges::copy(text, _bytes.get());
    }

    /// @return The copied bytes, ending where the heap block ends.
    [[nodiscard]] std::string_view view() const noexcept { return { _bytes.get(), _size }; }

  private:
    std::unique_ptr<char[]> _bytes;
    std::size_t _size;
};

} // namespace contour::testing
