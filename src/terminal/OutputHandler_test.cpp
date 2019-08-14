#include <terminal/OutputHandler.h>
#include <terminal/Parser.h>
#include <util/testing.h>

using namespace std;
using namespace terminal;

constexpr size_t RowCount = 25;

TEST(OutputHandler, utf8_single)  // TODO: move to Parser_test
{
    auto output = OutputHandler{
            RowCount,
            [&](auto const& msg) { logf("[OutputHandler]: {}", msg); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { logf("[debug] parser: {}", msg); },
            [&](auto const& msg) { logf("[trace] parser: {}", msg); }};

    parser.parseFragment("\xC3\xB6");  // ö

    ASSERT_EQ(1, output.commands().size());

    Command const cmd = output.commands()[0];
    ASSERT_TRUE(holds_alternative<AppendChar>(cmd));
    AppendChar const& ch = get<AppendChar>(cmd);

    ASSERT_EQ(0xF6, static_cast<unsigned>(ch.ch));
}

TEST(OutputHandler, utf8_middle)  // TODO: move to Parser_test
{
    auto output = OutputHandler{
            RowCount,
            [&](auto const& msg) { logf("[OutputHandler]: {}", msg); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { logf("[debug] parser: {}", msg); },
            [&](auto const& msg) { logf("[trace] parser: {}", msg); }};

    parser.parseFragment("A\xC3\xB6Z");  // AöZ

    ASSERT_EQ(3, output.commands().size());

    ASSERT_TRUE(holds_alternative<AppendChar>(output.commands()[0]));
    EXPECT_EQ('A', static_cast<unsigned>(get<AppendChar>(output.commands()[0]).ch));

    ASSERT_TRUE(holds_alternative<AppendChar>(output.commands()[1]));
    EXPECT_EQ(0xF6, static_cast<unsigned>(get<AppendChar>(output.commands()[1]).ch));

    ASSERT_TRUE(holds_alternative<AppendChar>(output.commands()[2]));
    EXPECT_EQ('Z', static_cast<unsigned>(get<AppendChar>(output.commands()[2]).ch));
}

TEST(OutputHandler, set_g1_special)
{
    auto output = OutputHandler{
            RowCount,
            [&](auto const& msg) { logf("[OutputHandler]: {}", msg); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { logf("[debug] parser: {}", msg); },
            [&](auto const& msg) { logf("[trace] parser: {}", msg); }};

    parser.parseFragment("\033)0");
    ASSERT_EQ(1, output.commands().size());
    ASSERT_TRUE(holds_alternative<DesignateCharset>(output.commands()[0]));
    auto ct = get<DesignateCharset>(output.commands()[0]);
    EXPECT_EQ(CharsetTable::G1, ct.table);
    EXPECT_EQ(Charset::Special, ct.charset);
}

TEST(OutputHandler, color_fg_indexed)
{
    auto output = OutputHandler{
            RowCount,
            [&](auto const& msg) { logf("[OutputHandler]: {}", msg); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { logf("[debug] parser: {}", msg); },
            [&](auto const& msg) { logf("[trace] parser: {}", msg); }};

    parser.parseFragment("\033[38;5;235m");
    ASSERT_EQ(1, output.commands().size());
    logf("sgr: {}", to_string(output.commands()[0]));
    ASSERT_TRUE(holds_alternative<SetForegroundColor>(output.commands()[0]));
    auto sgr = get<SetForegroundColor>(output.commands()[0]);
    ASSERT_TRUE(holds_alternative<IndexedColor>(sgr.color));
    auto indexedColor = get<IndexedColor>(sgr.color);
    EXPECT_EQ(235, static_cast<unsigned>(indexedColor));
}

TEST(OutputHandler, color_bg_indexed)
{
    auto output = OutputHandler{
            RowCount,
            [&](auto const& msg) { logf("[OutputHandler]: {}", msg); }};
    auto parser = Parser{
            ref(output),
            [&](auto const& msg) { logf("[debug] parser: {}", msg); },
            [&](auto const& msg) { logf("[trace] parser: {}", msg); }};

    parser.parseFragment("\033[48;5;235m");
    ASSERT_EQ(1, output.commands().size());
    logf("sgr: {}", to_string(output.commands()[0]));
    ASSERT_TRUE(holds_alternative<SetBackgroundColor>(output.commands()[0]));
    auto sgr = get<SetBackgroundColor>(output.commands()[0]);
    ASSERT_TRUE(holds_alternative<IndexedColor>(sgr.color));
    auto indexedColor = get<IndexedColor>(sgr.color);
    EXPECT_EQ(235, static_cast<unsigned>(indexedColor));
}
