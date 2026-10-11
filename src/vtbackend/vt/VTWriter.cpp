// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/VTWriter.hpp>

#include <vtbackend/grid/CellProxy.hpp>

#include <libunicode/width.h>

#include <algorithm>
#include <format>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

using std::string;
using std::string_view;
using std::vector;

namespace vtbackend
{

namespace
{
    /// The OSC 66 that writes @p cell's text-sizing block again, or nothing where the cell is ordinary text.
    ///
    /// Written the way kitty dumps a multicell character (write_multicell_ansi_prefix, line.c): the
    /// explicit width `w` only where the text's own width would not give the block its columns, the scale
    /// `s` only above 1, and the fraction and alignment only where set.
    [[nodiscard]] std::optional<std::string> textSizingOf(ConstCellProxy const& cell)
    {
        auto const sizing = cell.textScale();
        auto const scale = std::max(1U, unsigned { sizing.scale });
        auto const columns = unsigned { cell.width() };
        auto const ownWidth = std::max(1U, unicode::grapheme_cluster_width(cell.codepoints()));
        if (sizing.isOrdinary() && sizing.numerator == 0 && sizing.denominator == 0 && columns <= ownWidth)
            return std::nullopt;

        auto keys = std::vector<std::string> {};
        // Without `w` a block holds one grapheme cluster, `scale` times its own width.
        if (cell.codepointCount() != 1 || columns != scale * ownWidth)
            keys.push_back(std::format("w={}", columns / scale));
        if (scale > 1)
            keys.push_back(std::format("s={}", scale));
        if (sizing.numerator != 0)
            keys.push_back(std::format("n={}", sizing.numerator));
        if (sizing.denominator != 0)
            keys.push_back(std::format("d={}", sizing.denominator));
        if (sizing.verticalAlignment != 0)
            keys.push_back(std::format("v={}", sizing.verticalAlignment));
        if (sizing.horizontalAlignment != 0)
            keys.push_back(std::format("h={}", sizing.horizontalAlignment));

        auto metadata = std::string {};
        for (auto const& key: keys)
            metadata += (metadata.empty() ? "" : ":") + key;
        return std::format("\033]66;{};{}\a", metadata, cell.toUtf8());
    }
} // namespace

// TODO: compare with old sgr value set instead to be more generic in reusing stuff

VTWriter::VTWriter(Writer writer): _writer { std::move(writer) }
{
}

VTWriter::VTWriter(std::ostream& output):
    VTWriter { [&](char const* d, size_t n) { output.write(d, static_cast<std::streamsize>(n)); } }
{
}

VTWriter::VTWriter(std::vector<char>& output):
    VTWriter { [&](char const* d, size_t n) { output.insert(output.end(), d, d + n); } }
{
}

void VTWriter::write(char32_t v)
{
    sgrFlush();
    char buf[4];
    auto enc = unicode::encoder<char> {};
    auto count = std::distance(buf, enc(v, buf));
    write(string_view(buf, static_cast<size_t>(count)));
}

void VTWriter::write(string_view s)
{
    sgrFlush();
    _writer(s.data(), s.size());
}

void VTWriter::sgrFlush()
{
    if (_sgr.empty())
        return;

    auto const f = sgrFlush(_sgr);
    if (_sgr != _lastSGR)
        _writer(f.data(), f.size());

    sgrRewind();
}

string VTWriter::sgrFlush(vector<unsigned> const& sgr)
{
    if (sgr.empty())
        return "";

    auto const params =
        sgr.size() != 1 || sgr[0] != 0
            ? accumulate(begin(sgr),
                         end(sgr),
                         string {},
                         [](auto a, auto b) {
                             return a.empty() ? std::format("{}", b) : std::format("{};{}", a, b);
                         })
            : string();

    return std::format("\033[{}m", params);
}

void VTWriter::sgrAddExplicit(unsigned n)
{
    if (n == 0)
    {
        _currentForegroundColor = defaultColor();
        _currentBackgroundColor = defaultColor();
        _currentUnderlineColor = defaultColor();
    }

    _sgr.push_back(n);
}

void VTWriter::sgrAdd(unsigned n)
{
    if (n == 0)
    {
        _sgr.clear();
        _sgr.push_back(n);
        _currentForegroundColor = defaultColor();
        _currentBackgroundColor = defaultColor();
        _currentUnderlineColor = defaultColor();
    }
    else
    {
        if (_sgr.empty() || _sgr.back() != n)
            _sgr.push_back(n);

        if (_sgr.size() == MaxParameterCount)
        {
            sgrFlush();
        }
    }
}

void VTWriter::sgrRewind()
{
    swap(_lastSGR, _sgr);
    _sgr.clear();
}

void VTWriter::sgrAdd(GraphicsRendition m)
{
    sgrAdd(static_cast<unsigned>(m));
}

void VTWriter::setForegroundColor(Color color)
{
    // if (color == _currentForegroundColor)
    //     return;

    _currentForegroundColor = color;
    switch (color.type())
    {
        case ColorType::Default:
            //.
            sgrAdd(39);
            break;
        case ColorType::Indexed:
            if (static_cast<unsigned>(color.index()) < 8)
                sgrAdd(30 + static_cast<unsigned>(color.index()));
            else
                sgrAdd(38, 5, static_cast<unsigned>(color.index()));
            break;
        case ColorType::Bright:
            //.
            sgrAdd(90 + static_cast<unsigned>(getBrightColor(color)));
            break;
        case ColorType::RGB:
            // clang-format off
            sgrAdd(38, 2, static_cast<unsigned>(color.rgb().red),
                          static_cast<unsigned>(color.rgb().green),
                          static_cast<unsigned>(color.rgb().blue));
            // clang-format on
            break;
        case ColorType::Undefined: break;
    }
}

void VTWriter::setBackgroundColor(Color color)
{
    // if (color == _currentBackgroundColor)
    //     return;

    _currentBackgroundColor = color;
    switch (color.type())
    {
        case ColorType::Default: sgrAdd(49); break;
        case ColorType::Indexed:
            if (static_cast<unsigned>(color.index()) < 8)
                sgrAdd(40 + static_cast<unsigned>(color.index()));
            else
            {
                sgrAdd(48);
                sgrAdd(5);
                sgrAdd(static_cast<unsigned>(color.index()));
            }
            break;
        case ColorType::Bright:
            //.
            sgrAdd(100 + static_cast<unsigned>(getBrightColor(color)));
            break;
        case ColorType::RGB:
            sgrAdd(48);
            sgrAdd(2);
            sgrAdd(static_cast<unsigned>(color.rgb().red));
            sgrAdd(static_cast<unsigned>(color.rgb().green));
            sgrAdd(static_cast<unsigned>(color.rgb().blue));
            break;
        case ColorType::Undefined:
            //.
            break;
    }
}

void VTWriter::write(Line const& line)
{
    auto const cols = unbox<size_t>(line.size());

    // Blank lines have no cells to inspect — emit `cols` spaces using the line's
    // cached fill attributes as the SGR pen, then reset. Batch into one writer call
    // so SGR state is flushed once instead of per-column. Mirror the per-cell path's
    // Bold/Normal handling so the blank-line fast path does not silently drop weight.
    if (line.isBlank())
    {
        auto const& attrs = line.storage().fillAttrs;
        if (attrs.flags & CellFlag::Bold)
            sgrAdd(GraphicsRendition::Bold);
        else
            sgrAdd(GraphicsRendition::Normal);
        setForegroundColor(attrs.foregroundColor);
        setBackgroundColor(attrs.backgroundColor);
        write(std::string(cols, ' '));
        sgrAdd(GraphicsRendition::Reset);
        return;
    }

    // How many cells after the character just written its replay covers by itself: a wide character's
    // second half, or the rest of a text-sizing block's columns, which a space would push the rest of the
    // row to the right past.
    auto covered = 0U;
    for (auto const i: std::views::iota(size_t { 0 }, cols))
    {
        auto const cell = ConstCellProxy(line.storage(), i);
        if (covered > 0 && cell.isFlagEnabled(CellFlag::WideCharContinuation))
        {
            --covered;
            continue;
        }
        covered = 0;

        // A tall text-sizing block's lower part belongs to the block the row above made, and writing
        // anything there would destroy that block: the cursor steps over it instead.
        if (cell.isFlagEnabled(CellFlag::MulticellContinuation))
        {
            write("\033[C");
            continue;
        }

        if (cell.flags() & CellFlag::Bold)
            sgrAdd(GraphicsRendition::Bold);
        else
            sgrAdd(GraphicsRendition::Normal);

        setForegroundColor(cell.foregroundColor());
        setBackgroundColor(cell.backgroundColor());
        // TODO: other flags (such as underline), hyperlinks, image fragments.

        if (!cell.codepointCount())
            write(' ');
        else if (auto const sizing = textSizingOf(cell))
        {
            write(*sizing);
            covered = unsigned { cell.width() } - 1;
        }
        else
        {
            write(cell.toUtf8());
            auto const ownWidth = std::max(1U, unicode::grapheme_cluster_width(cell.codepoints()));
            covered = std::min(unsigned { cell.width() }, ownWidth) - 1;
        }
    }

    sgrAdd(GraphicsRendition::Reset);
}

} // namespace vtbackend
