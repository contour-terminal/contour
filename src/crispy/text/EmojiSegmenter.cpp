#include <crispy/text/EmojiSegmenter.h>
#include <crispy/text/Unicode.h>

namespace crispy::text {

constexpr bool operator<(EmojiSegmentationCategory a, EmojiSegmentationCategory b) noexcept
{
    return static_cast<int>(a) < static_cast<int>(b);
}

constexpr bool operator>(EmojiSegmentationCategory a, EmojiSegmentationCategory b) noexcept
{
    return static_cast<int>(a) > static_cast<int>(b);
}

class RagelIterator {
    EmojiSegmentationCategory category_;
    char32_t const* buffer_;
    size_t size_;
    size_t cursor_;

  public:
    constexpr RagelIterator(char32_t const* _buffer, size_t _size, size_t _cursor) noexcept
      : category_{ EmojiSegmentationCategory::Invalid },
        buffer_{ _buffer },
        size_{ _size },
        cursor_{ _cursor }
    {
        updateCategory();
    }

    constexpr RagelIterator() noexcept : RagelIterator(nullptr, 0, 0) {}

    constexpr char32_t codepoint() const noexcept { return buffer_[cursor_]; }
    constexpr EmojiSegmentationCategory category() const noexcept { return category_; }
    constexpr size_t cursor() const noexcept { return cursor_; }

    constexpr void updateCategory() { category_ = toCategory(codepoint()); }

    constexpr int operator*() const noexcept { return static_cast<int>(category_); }

    constexpr RagelIterator& operator++() noexcept { cursor_++; updateCategory(); return *this; }
    constexpr RagelIterator& operator--(int) noexcept { cursor_--; updateCategory(); return *this; }

    constexpr RagelIterator operator+(int v) const noexcept { return {buffer_, size_, cursor_ + v}; }
    constexpr RagelIterator operator-(int v) const noexcept { return {buffer_, size_, cursor_ - v}; }

    constexpr RagelIterator& operator=(int v) noexcept { cursor_ = v; updateCategory(); return *this; }

    constexpr bool operator==(RagelIterator const& _rhs) const noexcept
    {
        return buffer_ == _rhs.buffer_ && size_ == _rhs.size_ && cursor_ == _rhs.cursor_;
    }

    constexpr bool operator!=(RagelIterator const& _rhs) const noexcept { return !(*this == _rhs); }
};

namespace {
using emoji_text_iter_t = RagelIterator;
#include "emoji_presentation_scanner.c"
}

void EmojiSegmenter::scan() noexcept
{
    auto const i = RagelIterator(begin_, size_, cursor_);
    auto const e = RagelIterator(begin_, size_, size_);
    auto const o = scan_emoji_presentation(i, e, &isEmoji_);
    cursor_ = o.cursor();
}

} // end namespace
