// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/ScreenTestFixtures.hpp>

namespace vtbackend::test
{

Image::Data const& black10x10()
{
    static Image::Data const data = [] {
        Image::Data ret(static_cast<std::size_t>(100 * 4), 0);
        for (size_t i = 3; i < ret.size(); i += 4)
        {
            ret[i] = 255;
        }
        return ret;
    }();
    return data;
}

Image::Data const& white10x10()
{
    static Image::Data const data(static_cast<std::size_t>(100 * 4), 255);
    return data;
}

void TextRenderBuilder::startLine(LineOffset lineOffset, LineFlags /*flags*/, ContextId /*contextId*/)
{
    if (!*lineOffset)
        text.clear();
}

void TextRenderBuilder::renderCell(ConstCellProxy cell, LineOffset, ColumnOffset)
{
    text += cell.toUtf8();
}

void TextRenderBuilder::endLine()
{
    text += '\n';
}

void TextRenderBuilder::renderTrivialLine(TrivialLineBuffer const& lineBuffer,
                                          LineOffset lineOffset,
                                          LineFlags /*flags*/,
                                          ContextId /*contextId*/,
                                          std::u32string_view textOverride)
{
    if (!*lineOffset)
        text.clear();

    if (!textOverride.empty())
        text.append(unicode::convert_to<char>(textOverride));
    else
        text.append(lineBuffer.text.data(), lineBuffer.text.size());
    text += '\n';
}

void TextRenderBuilder::finish()
{
}

MockTerm<vtpty::MockPty> screenForDECRA()
{
    return MockTerm<vtpty::MockPty> { PageSize { LineCount(5), ColumnCount(6) }, {}, 1024, [](auto& mock) {
                                         mock.writeToScreen("ABCDEF\r\n"
                                                            "abcdef\r\n"
                                                            "123456\r\n");
                                         mock.writeToScreen("\033[43m");
                                         mock.writeToScreen("GHIJKL\r\n"
                                                            "ghijkl");
                                         mock.writeToScreen("\033[0m");

                                         auto const* const initialText = "ABCDEF\n"
                                                                         "abcdef\n"
                                                                         "123456\n"
                                                                         "GHIJKL\n"
                                                                         "ghijkl\n";

                                         CHECK(mock.terminal.primaryScreen().renderMainPageText()
                                               == initialText);
                                     } };
}

std::set<int> parseDA1Extensions(std::string_view reply)
{
    std::set<int> extensions;

    // Find the CSI ? prefix and 'c' terminator
    auto const prefix = reply.find("\033[?");
    if (prefix == std::string_view::npos)
        return extensions;

    auto const start = prefix + 3; // skip "\033[?"
    auto const end = reply.find('c', start);
    if (end == std::string_view::npos)
        return extensions;

    auto const params = reply.substr(start, end - start);

    // Split by ';' and parse each number
    auto isFirst = true;
    size_t pos = 0;
    while (pos < params.size())
    {
        auto const delim = params.find(';', pos);
        auto const token =
            params.substr(pos, delim == std::string_view::npos ? std::string_view::npos : delim - pos);
        if (!token.empty())
        {
            auto value = 0;
            if (auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
                ec == std::errc {})
            {
                if (isFirst)
                    isFirst = false; // first number is the conformance level, not an extension
                else
                    extensions.insert(value);
            }
        }
        if (delim == std::string_view::npos)
            break;
        pos = delim + 1;
    }

    return extensions;
}

int parseDA1Level(std::string_view reply)
{
    auto const prefix = reply.find("\033[?");
    if (prefix == std::string_view::npos)
        return 0;
    auto const start = prefix + 3;
    auto const delim = reply.find(';', start);
    auto const end = (delim != std::string_view::npos) ? delim : reply.find('c', start);
    if (end == std::string_view::npos)
        return 0;
    auto value = 0;
    std::from_chars(reply.data() + start, reply.data() + end, value);
    return value;
}

GridDeltaCursor drainedDeltaCursor(Grid& grid)
{
    auto cursor = GridDeltaCursor {};
    std::ignore = grid.forEachLineChangedSince(cursor, [](LineOffset, Line const&) {});
    return cursor;
}

std::vector<int> changedLineOffsets(Grid& grid, GridDeltaCursor& cursor)
{
    auto out = std::vector<int> {};
    std::ignore = grid.forEachLineChangedSince(
        cursor, [&](LineOffset offset, Line const&) { out.push_back(unbox<int>(offset)); });
    return out;
}

} // namespace vtbackend::test
