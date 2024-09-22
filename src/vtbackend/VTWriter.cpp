// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/VTWriter.h>

#include <numeric>

using std::string;
using std::string_view;
using std::vector;

namespace vtbackend
{

// TODO: compare with old sgr value set instead to be more generic in reusing stuff

VTWriter::VTWriter(Writer writer): _writer { std::move(writer) }
{
}

VTWriter::VTWriter(std::ostream& output):
    VTWriter { [&](char const* d, size_t n) {
        output.write(d, static_cast<std::streamsize>(n));
    } }
{
}

VTWriter::VTWriter(std::vector<char>& output):
    VTWriter { [&](char const* d, size_t n) {
        output.insert(output.end(), d, d + n);
    } }
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
        _currentForegroundColor = DefaultColor();
        _currentBackgroundColor = DefaultColor();
        _currentUnderlineColor = DefaultColor();
    }

    _sgr.push_back(n);
}

void VTWriter::sgrAdd(unsigned n)
{
    if (n == 0)
    {
        _sgr.clear();
        _sgr.push_back(n);
        _currentForegroundColor = DefaultColor();
        _currentBackgroundColor = DefaultColor();
        _currentUnderlineColor = DefaultColor();
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

template <CellConcept Cell>
void VTWriter::write(Line<Cell> const& line)
{
    if (line.isTrivialBuffer())
    {
        TrivialLineBuffer const& lineBuffer = line.trivialBuffer();
        setForegroundColor(lineBuffer.textAttributes.foregroundColor);
        setBackgroundColor(lineBuffer.textAttributes.backgroundColor);
        // TODO: hyperlinks, underlineColor and other flags (curly underline etc.)
        write(line.toUtf8());
        // TODO: Write fill columns.
    }
    else
    {
        for (Cell const& cell: line.inflatedBuffer())
        {
            if (cell.flags() & CellFlag::Bold)
                sgrAdd(GraphicsRendition::Bold);
            else
                sgrAdd(GraphicsRendition::Normal);

            setForegroundColor(cell.foregroundColor());
            setBackgroundColor(cell.backgroundColor());
            // TODO: other flags (such as underline), hyperlinks, image fragments.

            if (!cell.codepointCount())
                write(' ');
            else
                write(cell.toUtf8());
        }
    }

    sgrAdd(GraphicsRendition::Reset);
}

} // namespace vtbackend

#include <vtbackend/cell/CompactCell.h>
template void vtbackend::VTWriter::write<vtbackend::CompactCell>(Line<CompactCell> const&);

#include <vtbackend/cell/SimpleCell.h>
template void vtbackend::VTWriter::write<vtbackend::SimpleCell>(Line<SimpleCell> const&);
