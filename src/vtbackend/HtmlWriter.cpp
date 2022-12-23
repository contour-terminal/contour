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
#include <vtbackend/HtmlWriter.h>

#include <numeric>

using std::string;
using std::string_view;
using std::vector;

namespace terminal
{

// TODO: compare with old sgr value set instead to be more generic in reusing stuff

HtmlWriter::HtmlWriter(Writer writer): _writer { std::move(writer) }
{
}

HtmlWriter::HtmlWriter(std::ostream& output):
    HtmlWriter { [&](char const* d, size_t n) {
        output.write(d, static_cast<std::streamsize>(n));
    } }
{
}

HtmlWriter::HtmlWriter(std::vector<char>& output):
    HtmlWriter { [&](char const* d, size_t n) {
        output.insert(output.end(), d, d + n);
    } }
{
}

/*void HtmlWriter::write(char32_t v)
{

}

void HtmlWriter::write(string_view s)
{

}*/

template <typename Cell>
void HtmlWriter::write(Line<Cell> const& line)
{

}

string HtmlWriter::setCssTextFormatting(CellFlags cellFlags)
{
    constexpr auto allFlags = 31; //0001 1111
    string result {"#myDIV {}\n"};
    switch(cellFlags)
    {
        case CellFlags::Bold: result.append("font-weight: bold;\n"); break;
        case CellFlags::Italic: result.append("font-style: italic;\n"); break;
        case CellFlags::Underline: result.append("text-decoration: underline;\n"); break;
        case CellFlags::DottedUnderline: result.append("text-decoration: underline dotted;\n"); break;
        default: ;
    }

    return result+ "\n}\n";
}

} // namespace terminal

#include <vtbackend/cell/CompactCell.h>
template void terminal::HtmlWriter::write<terminal::CompactCell>(Line<CompactCell> const&);

#include <vtbackend/cell/SimpleCell.h>
template void terminal::HtmlWriter::write<terminal::SimpleCell>(Line<SimpleCell> const&);
