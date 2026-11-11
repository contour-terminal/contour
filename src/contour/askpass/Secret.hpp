// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A piece of text that must not outlive its use: an answer to a prompt.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace contour::askpass
{

/// Overwrites the whole storage of @p text -- its capacity, not only its length -- with zeros, then empties
/// it. The writes go through a volatile pointer, so the compiler cannot drop them as dead stores.
/// @param text The string to wipe.
void wipeStorage(std::string& text) noexcept;

/// Overwrites @p bytes with zeros, through a volatile pointer: for a scratch buffer an answer passed through.
/// @param bytes The bytes to wipe.
void wipeBytes(std::span<std::byte> bytes) noexcept;

/// Text that is wiped when it goes, and when it is moved from. Move-only, so it is never copied by accident,
/// and built to leave nothing behind: a moved-from std::string keeps the bytes of a short string in its own
/// buffer (the small-string optimisation), which a plain move would leave for the next reader of that memory.
class Secret
{
  public:
    Secret() = default;

    /// Copies @p text.
    /// @param text What to keep; the caller's own copy is the caller's to wipe.
    explicit Secret(std::string_view text): _bytes { text } {}

    /// Copies @p text.
    explicit Secret(char const* text): Secret(std::string_view { text }) {}

    /// Takes @p text over: the caller's string is wiped, whole.
    /// @param text What to keep.
    explicit Secret(std::string&& text): _bytes { take(std::move(text)) } {}

    ~Secret() { wipe(); }

    Secret(Secret const&) = delete;
    Secret& operator=(Secret const&) = delete;

    /// Takes @p other's text; @p other is wiped, its whole storage.
    Secret(Secret&& other) noexcept
    {
        _bytes.swap(other._bytes);
        other.wipe();
    }

    /// Wipes this one, then takes @p other's text; @p other is wiped, its whole storage.
    Secret& operator=(Secret&& other) noexcept
    {
        if (this != &other)
        {
            wipe();
            _bytes.swap(other._bytes);
            other.wipe();
        }
        return *this;
    }

    /// @return The text, valid until the next change of this object.
    [[nodiscard]] std::string_view view() const noexcept { return _bytes; }
    [[nodiscard]] std::size_t size() const noexcept { return _bytes.size(); }
    [[nodiscard]] bool empty() const noexcept { return _bytes.empty(); }

    /// @return Every byte of the storage the text lives in, the length of the text and beyond: what a test
    /// looks
    ///         at to see that nothing was left behind.
    [[nodiscard]] std::span<char const> storage() const noexcept
    {
        return { _bytes.data(), _bytes.capacity() };
    }

    /// Appends @p more. When that needs a larger buffer the old one is wiped, not just released.
    void append(std::string_view more);

    /// Wipes the text, whole storage, and empties it.
    void wipe() noexcept { wipeStorage(_bytes); }

    /// @return Whether the text is @p text. Not constant-time: for tests and for answers that are not
    /// compared
    ///         against anything secret.
    [[nodiscard]] bool operator==(std::string_view text) const noexcept { return _bytes == text; }
    [[nodiscard]] bool operator==(Secret const& other) const noexcept { return _bytes == other._bytes; }

  private:
    /// @param source The caller's string, left empty and wiped whole.
    /// @return What it held. Wiped through a pointer: what a moved-from string leaves behind (a short one's
    ///         bytes, in its own buffer) is exactly what is to be wiped, so the moved-from object is touched
    ///         on purpose.
    [[nodiscard]] static std::string take(std::string&& source)
    {
        auto* const caller = &source;
        auto taken = std::move(source);
        wipeStorage(*caller);
        return taken;
    }

    std::string _bytes;
};

} // namespace contour::askpass
