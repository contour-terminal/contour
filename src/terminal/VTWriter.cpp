/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <terminal/VTWriter.h>

#include <numeric>

using namespace std;

namespace terminal
{

// TODO: compare with old sgr value set instead to be more generic in reusing stuff

VTWriter::VTWriter(Writer writer): writer_ { std::move(writer) }
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
    write(std::string_view(buf, static_cast<size_t>(count)));
}

void VTWriter::write(std::string_view s)
{
    sgrFlush();
    writer_(s.data(), s.size());
}

void VTWriter::sgrFlush()
{
    if (sgr_.empty())
        return;

    auto const f = sgrFlush(sgr_);
    if (sgr_ != lastSGR_)
        writer_(f.data(), f.size());

    sgrRewind();
}

string VTWriter::sgrFlush(vector<unsigned> const& sgr)
{
    if (sgr.empty())
        return "";

    auto const params =
        sgr.size() != 1 || sgr[0] != 0 ? accumulate(
            begin(sgr),
            end(sgr),
            string {},
            [](auto a, auto b) { return a.empty() ? fmt::format("{}", b) : fmt::format("{};{}", a, b); })
                                       : string();

    return fmt::format("\033[{}m", params);
}

void VTWriter::sgrAdd(unsigned n)
{
    if (n == 0)
    {
        sgr_.clear();
        sgr_.push_back(n);
        currentForegroundColor_ = DefaultColor();
        currentBackgroundColor_ = DefaultColor();
        currentUnderlineColor_ = DefaultColor();
    }
    else
    {
        if (sgr_.empty() || sgr_.back() != n)
            sgr_.push_back(n);

        if (sgr_.size() == 16)
        {
            sgrFlush();
        }
    }
}

void VTWriter::sgrRewind()
{
    swap(lastSGR_, sgr_);
    sgr_.clear();
}

void VTWriter::sgrAdd(GraphicsRendition m)
{
    sgrAdd(static_cast<unsigned>(m));
}

void VTWriter::setForegroundColor(Color _color)
{
    // if (_color == currentForegroundColor_)
    //     return;

    currentForegroundColor_ = _color;
    switch (_color.type())
    {
        case ColorType::Default:
            //.
            sgrAdd(39);
            break;
        case ColorType::Indexed:
            if (static_cast<unsigned>(_color.index()) < 8)
                sgrAdd(30 + static_cast<unsigned>(_color.index()));
            else
            {
                sgrAdd(38);
                sgrAdd(5);
                sgrAdd(static_cast<unsigned>(_color.index()));
            }
            break;
        case ColorType::Bright:
            //.
            sgrAdd(90 + static_cast<unsigned>(getBrightColor(_color)));
            break;
        case ColorType::RGB:
            sgrAdd(38);
            sgrAdd(2);
            sgrAdd(static_cast<unsigned>(_color.rgb().red));
            sgrAdd(static_cast<unsigned>(_color.rgb().green));
            sgrAdd(static_cast<unsigned>(_color.rgb().blue));
            break;
        case ColorType::Undefined: break;
    }
}

void VTWriter::setBackgroundColor(Color _color)
{
    // if (_color == currentBackgroundColor_)
    //     return;

    currentBackgroundColor_ = _color;
    switch (_color.type())
    {
        case ColorType::Default: sgrAdd(49); break;
        case ColorType::Indexed:
            if (static_cast<unsigned>(_color.index()) < 8)
                sgrAdd(40 + static_cast<unsigned>(_color.index()));
            else
            {
                sgrAdd(48);
                sgrAdd(5);
                sgrAdd(static_cast<unsigned>(_color.index()));
            }
            break;
        case ColorType::Bright:
            //.
            sgrAdd(100 + static_cast<unsigned>(getBrightColor(_color)));
            break;
        case ColorType::RGB:
            sgrAdd(48);
            sgrAdd(2);
            sgrAdd(static_cast<unsigned>(_color.rgb().red));
            sgrAdd(static_cast<unsigned>(_color.rgb().green));
            sgrAdd(static_cast<unsigned>(_color.rgb().blue));
            break;
        case ColorType::Undefined:
            //.
            break;
    }
}

template <typename Cell>
void VTWriter::write(Line<Cell> const& line)
{
    if (line.isTrivialBuffer())
    {
        TriviallyStyledLineBuffer const& lineBuffer = line.trivialBuffer();
        setForegroundColor(lineBuffer.attributes.foregroundColor);
        setBackgroundColor(lineBuffer.attributes.backgroundColor);
        // TODO: hyperlinks, underlineColor and other styles (curly underline etc.)
        write(line.toUtf8());
    }
    else
    {
        for (Cell const& cell: line.inflatedBuffer())
        {
            if (cell.styles() & CellFlags::Bold)
                sgrAdd(GraphicsRendition::Bold);
            else
                sgrAdd(GraphicsRendition::Normal);

            setForegroundColor(cell.foregroundColor());
            setBackgroundColor(cell.backgroundColor());
            // TODO: other styles (such as underline), hyperlinks, image fragments.

            if (!cell.codepointCount())
                write(' ');
            else
                write(cell.toUtf8());
        }
    }

    sgrAdd(GraphicsRendition::Reset);
}

} // namespace terminal

#include <terminal/Cell.h>
template void terminal::VTWriter::write<terminal::Cell>(Line<Cell> const&);
