// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Shortcut.hpp>
#include <contour/config/FileOpenerTemplate.hpp>
#include <contour/config/GlobalOverrides.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/window/SettingsFields.hpp>

#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/links/LinkMatcher.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/links/ScanText.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <text_shaper/Font.hpp>

#include <core/Assert.hpp>
#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <yaml-cpp/yaml.h>

#include <QtCore/QStringList>
#include <QtGui/QColor>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::window::fields
{

using config::TerminalProfile;
using namespace std::string_view_literals;

QString toQString(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

namespace
{
    /// Builds a bool profile-field descriptor.
    /// @param accessor Returns the bool the row edits, of a const and a non-const profile alike: a bool
    ///                 ConfigEntry's value, or a bool member of a struct-valued one.
    /// @return The descriptor.
    template <typename Accessor>
    ProfileFieldDescriptor boolField(QString key, QString label, QString help, Accessor accessor)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "bool",
                 [accessor](TerminalProfile const& p) { return QVariant(accessor(p)); },
                 [accessor](TerminalProfile& p, QVariant const& v) { accessor(p) = v.toBool(); } };
    }

    /// Builds one `scrollbar.marks` source as a checkbox. The YAML list holds the sources that are on,
    /// so the row is "is this source in the list" -- one checkbox per source rather than a list editor,
    /// because the list is a set of known values, not free text.
    /// @param source The source this row switches.
    /// @return The descriptor.
    ProfileFieldDescriptor scrollbarMarkField(QString key,
                                              QString label,
                                              QString help,
                                              vtbackend::ScrollbarMarkSource source)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "bool",
                 [source](TerminalProfile const& p) {
                     return QVariant(p.scrollbar.value().marks.test(source));
                 },
                 [source](TerminalProfile& p, QVariant const& v) {
                     auto& marks = p.scrollbar.value().marks;
                     if (v.toBool())
                         marks.enable(source);
                     else
                         marks.disable(source);
                 } };
    }

    /// The configuration tokens of every value of configuration enum @p Enum, in table order.
    template <typename Enum>
    [[nodiscard]] QStringList configEnumTokens()
    {
        auto tokens = QStringList {};
        for (auto const& info: config::configEnumValues<Enum>())
            tokens.push_back(toQString(info.token));
        return tokens;
    }

    /// Every cursor shape paired with the token a configuration file names it by.
    ///
    /// Mirrors what vtbackend::makeCursorShape() accepts. It is spelled out here rather than derived
    /// from that function because the function only parses -- it maps a token to a shape and throws on
    /// anything else, so it can neither name a shape nor list the shapes a combo box should offer.
    constexpr auto CursorShapeNames = std::array {
        std::pair { "block"sv, vtbackend::CursorShape::Block },
        std::pair { "rectangle"sv, vtbackend::CursorShape::Rectangle },
        std::pair { "underscore"sv, vtbackend::CursorShape::Underscore },
        std::pair { "bar"sv, vtbackend::CursorShape::Bar },
    };

    /// One of the profile's `history:` depths, in the configuration's own `-1 == unlimited` spelling.
    ///
    /// A pointer-to-member rather than a getter/setter pair, because the two rows this builds differ
    /// in nothing else: written out by hand they were character-for-character identical but for the
    /// field name, and a third depth tomorrow would have been a third copy of the same encoding.
    ///
    /// -1 in both directions, matching what the HistoryConfig writer emits, so what the page shows is
    /// what the file holds. Mapping Infinite to 0 on the way out while reading 0 back as LineCount(0)
    /// -- as this once did -- displayed unlimited scrollback as "0" and then turned it into literally
    /// no scrollback the first time the field was touched.
    ProfileFieldDescriptor historyLimitField(QString key,
                                             QString label,
                                             QString help,
                                             vtbackend::MaxHistoryLineCount config::HistoryConfig::* field)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "int",
                 [field](TerminalProfile const& p) {
                     auto const& limit = p.history.value().*field;
                     if (auto const* lineCount = std::get_if<vtbackend::LineCount>(&limit))
                         return QVariant(static_cast<int>(unbox(*lineCount)));
                     return QVariant(-1);
                 },
                 [field](TerminalProfile& p, QVariant const& v) {
                     auto history = p.history.value();
                     auto const lines = v.toInt();
                     if (lines < 0)
                         history.*field = vtbackend::Infinite {};
                     else
                         history.*field = vtbackend::LineCount(lines);
                     p.history = history;
                 } };
    }

    /// enumField() for an enum that already has a ConfigEnum table.
    ///
    /// The overload below takes the token/value pairs by hand, which for a table-backed enum means
    /// spelling every value a second time, in a file that no test compares against the table. This
    /// one reads the table, so adding a value stays what ConfigEnum.hpp promises it is: adding a row.
    template <typename Enum, typename Getter, typename Setter>
    ProfileFieldDescriptor enumFieldFromTable(
        QString key, QString label, QString help, Getter getter, Setter setter)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "enum",
                 [getter](TerminalProfile const& p) -> QVariant {
                     return QVariant(toQString(config::configEnumToken(getter(p))));
                 },
                 [setter](TerminalProfile& p, QVariant const& v) {
                     if (auto const value = config::configEnumFromToken<Enum>(v.toString().toStdString()))
                         setter(p, *value);
                 },
                 configEnumTokens<Enum>() };
    }

    /// Builds an enum profile-field descriptor from a token-to-value table.
    ///
    /// The combo-box options, the getter's display token and the setter's parse are all derived from
    /// @p mappings, so the set of values a user can pick and the set the setter understands cannot drift
    /// apart. Both halves used to be written out by hand -- an option list beside a chain of string
    /// comparisons -- while the getter derived its token from `std::format`, a third spelling again:
    /// the formatters render values for people ("host writable", "Logical", "Bold"), not as the tokens
    /// this option list and the config grammar use. A `Decorator` field would have shown nothing
    /// selected, because "dotted-underline" lowercases to itself but "DottedUnderline" does not.
    template <typename Enum, size_t N, typename Getter, typename Setter>
    ProfileFieldDescriptor enumField(QString key,
                                     QString label,
                                     QString help,
                                     std::array<std::pair<std::string_view, Enum>, N> mappings,
                                     Getter getter,
                                     Setter setter)
    {
        auto options = QStringList {};
        for (auto const& [token, _]: mappings)
            options.push_back(toQString(token));

        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "enum",
                 [mappings, getter](TerminalProfile const& p) -> QVariant {
                     auto const current = getter(p);
                     for (auto const& [token, value]: mappings)
                         if (value == current)
                             return QVariant(toQString(token));
                     // A value this table has no token for cannot be shown in the combo box; an empty
                     // string leaves it unselected rather than silently displaying a neighbour's token.
                     return QVariant { QString {} };
                 },
                 [mappings, setter](TerminalProfile& p, QVariant const& v) {
                     auto const token = v.toString().toStdString();
                     for (auto const& [candidate, value]: mappings)
                         if (candidate == token)
                         {
                             setter(p, value);
                             return;
                         }
                 },
                 std::move(options) };
    }

} // namespace

/// The editable scalar profile fields, in the groups the settings page shows them under.
///
/// Data-driven twice over: it grows one row at a time toward full parity without touching any other
/// code (the page renders each row by its `type`), and a row's section is the group it sits in (the
/// page renders one collapsible card per group). Nothing here knows what a section looks like, and
/// nothing in QML knows which fields belong together.
std::vector<ProfileFieldGroup> const& profileFieldGroups()
{
    using vtbackend::CursorShape;
    using vtbackend::LineCount;
    using vtbackend::LineOffset;
    using Ms = std::chrono::milliseconds;
    static auto const groups = std::vector<ProfileFieldGroup> {
        { SettingsGroup::ProfileWindow,
          "▭",
          {
              boolField("show_title_bar",
                        "Show title bar",
                        "Whether the window shows its title bar.",
                        [](auto& p) -> auto& { return p.showTitleBar.value(); }),
              boolField("maximized",
                        "Start maximized",
                        "Open the window maximized.",
                        [](auto& p) -> auto& { return p.maximized.value(); }),
              boolField("fullscreen",
                        "Start fullscreen",
                        "Open the window in fullscreen.",
                        [](auto& p) -> auto& { return p.fullscreen.value(); }),
              boolField("size_indicator_on_resize",
                        "Show size on resize",
                        "Briefly show the terminal dimensions when the window is resized.",
                        [](auto& p) -> auto& { return p.sizeIndicatorOnResize.value(); }),
              { "dim_unfocused",
                "Dim when unfocused",
                "How much to dim a pane while it is not focused (0.0 = off, 1.0 = fully dimmed).",
                "double",
                [](TerminalProfile const& p) { return QVariant(p.dimUnfocused.value()); },
                [](TerminalProfile& p, QVariant const& v) { p.dimUnfocused = v.toDouble(); } },
          } },
        { SettingsGroup::ProfileFont,
          "A",
          {
              { "font_family",
                "Font family",
                "The regular font family name.",
                "string",
                [](TerminalProfile const& p) {
                    return QVariant(QString::fromStdString(p.fonts.value().regular.familyName));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    p.fonts.value().regular.familyName = v.toString().toStdString();
                } },
              { "font_size",
                "Font size (pt)",
                "The font size in points.",
                "double",
                [](TerminalProfile const& p) { return QVariant(p.fonts.value().size.pt); },
                [](TerminalProfile& p, QVariant const& v) {
                    p.fonts.value().size = text::FontSize { v.toDouble() };
                } },
              boolField("draw_bold_text_with_bright_colors",
                        "Bold text uses bright colors",
                        "Render bold text using the bright ANSI colors.",
                        [](auto& p) -> auto& { return p.drawBoldTextWithBrightColors.value(); }),
          } },
        { SettingsGroup::ProfileScrolling,
          "↕",
          {
              boolField("smooth_scrolling",
                        "Smooth scrolling",
                        "Animate scrolling between lines.",
                        [](auto& p) -> auto& { return p.smoothScrolling.value(); }),
              boolField("momentum_scrolling",
                        "Momentum scrolling",
                        "Continue scrolling with momentum.",
                        [](auto& p) -> auto& { return p.momentumScrolling.value(); }),
              { "slow_scrolling_time",
                "Slow scrolling time (ms)",
                "Duration of one smooth line-scroll step, in milliseconds.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(p.smoothLineScrolling.value().count()));
                },
                [](TerminalProfile& p, QVariant const& v) { p.smoothLineScrolling = Ms { v.toInt() }; } },
              { "vi_mode_scrolloff",
                "Vi mode scroll-off",
                "Minimum lines kept above/below the cursor while scrolling in Vi mode.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(unbox(p.modalCursorScrollOff.value())));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    p.modalCursorScrollOff = LineCount(v.toInt());
                } },
              { "copy_last_mark_range_offset",
                "Copy last-mark range offset",
                "Line offset applied to the CopyPreviousMarkRange action.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(unbox(p.copyLastMarkRangeOffset.value())));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    p.copyLastMarkRangeOffset = LineOffset(v.toInt());
                } },
          } },
        { SettingsGroup::ProfileBehaviour,
          "⚙",
          {
              boolField("escape_sandbox",
                        "Escape sandbox",
                        "Escape a Flatpak/Snap sandbox when spawning.",
                        [](auto& p) -> auto& { return p.escapeSandbox.value(); }),
              boolField("insert_after_yank",
                        "Insert after yank",
                        "Enter insert mode after yanking in Vi mode.",
                        [](auto& p) -> auto& { return p.insertAfterYank.value(); }),
              boolField("input_method_editor",
                        "Input method editor",
                        "Enable IME (input method) support.",
                        [](auto& p) -> auto& { return p.inputMethodEditor.value(); }),
              boolField("highlight_word_and_matches_on_double_click",
                        "Highlight word on double-click",
                        "Highlight the double-clicked word and its other occurrences.",
                        [](auto& p) -> auto& { return p.highlightDoubleClickedWord.value(); }),
              { "vi_mode_highlight_timeout",
                "Vi highlight timeout (ms)",
                "How long a Vi-mode yank highlight stays visible, in milliseconds.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(p.highlightTimeout.value().count()));
                },
                [](TerminalProfile& p, QVariant const& v) { p.highlightTimeout = Ms { v.toInt() }; } },
              { "screen_transition_duration",
                "Screen transition (ms)",
                "Duration of the alt-screen transition animation, in milliseconds.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(p.screenTransitionDuration.value().count()));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    p.screenTransitionDuration = Ms { v.toInt() };
                } },
          } },
        { SettingsGroup::ProfileTerminal,
          "▣",
          {
              { "terminal_size_lines",
                "Terminal rows",
                "Default number of rows in the terminal grid.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(unbox(p.terminalSize.value().lines)));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto size = p.terminalSize.value();
                    size.lines = vtbackend::LineCount(v.toInt());
                    p.terminalSize = size;
                } },
              { "terminal_size_columns",
                "Terminal columns",
                "Default number of columns in the terminal grid.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(unbox(p.terminalSize.value().columns)));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto size = p.terminalSize.value();
                    size.columns = vtbackend::ColumnCount(v.toInt());
                    p.terminalSize = size;
                } },
              { "margins_horizontal",
                "Horizontal margin (px)",
                "Horizontal window margin in pixels.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(*p.margins.value().horizontal));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto m = p.margins.value();
                    m.horizontal = config::HorizontalMargin(static_cast<unsigned>(v.toInt()));
                    p.margins = m;
                } },
              { "margins_vertical",
                "Vertical margin (px)",
                "Vertical window margin in pixels.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(*p.margins.value().vertical));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto m = p.margins.value();
                    m.vertical = config::VerticalMargin(static_cast<unsigned>(v.toInt()));
                    p.margins = m;
                } },
              enumFieldFromTable<vtbackend::SearchCaseSensitivity>(
                  "search_case_sensitivity",
                  "Search case sensitivity",
                  "How the find bar compares letters when it opens. Its Aa button cycles the same "
                  "three at any time.",
                  [](TerminalProfile const& p) { return p.searchCaseSensitivity.value(); },
                  [](TerminalProfile& p, auto v) { p.searchCaseSensitivity = v; }),
              enumField(
                  "pixel_reporting",
                  "Pixel reporting",
                  "How image dimensions are reported: logical pixels or device pixels.",
                  std::array {
                      std::pair { "logical"sv, config::PixelReporting::Logical },
                      std::pair { "device"sv, config::PixelReporting::Device },
                  },
                  [](TerminalProfile const& p) { return p.pixelReporting.value(); },
                  [](TerminalProfile& p, auto v) { p.pixelReporting = v; }),
              enumField(
                  "terminal_id",
                  "Terminal ID",
                  "The VT terminal type to emulate.",
                  std::array {
                      std::pair { "vt100"sv, vtbackend::VTType::VT100 },
                      std::pair { "vt220"sv, vtbackend::VTType::VT220 },
                      std::pair { "vt240"sv, vtbackend::VTType::VT240 },
                      std::pair { "vt320"sv, vtbackend::VTType::VT320 },
                      std::pair { "vt330"sv, vtbackend::VTType::VT330 },
                      std::pair { "vt340"sv, vtbackend::VTType::VT340 },
                      std::pair { "vt420"sv, vtbackend::VTType::VT420 },
                      std::pair { "vt510"sv, vtbackend::VTType::VT510 },
                      std::pair { "vt520"sv, vtbackend::VTType::VT520 },
                      std::pair { "vt525"sv, vtbackend::VTType::VT525 },
                  },
                  [](TerminalProfile const& p) { return p.terminalId.value(); },
                  [](TerminalProfile& p, auto v) { p.terminalId = v; }),
              { "wm_class",
                "WM class",
                "The X11 WM_CLASS property string.",
                "string",
                [](TerminalProfile const& p) { return QVariant(QString::fromStdString(p.wmClass.value())); },
                [](TerminalProfile& p, QVariant const& v) { p.wmClass = v.toString().toStdString(); } },
              { "tab_label",
                "Tab label",
                "Template for the tab title (e.g. {WindowTitle}).",
                "string",
                [](TerminalProfile const& p) { return QVariant(QString::fromStdString(p.tabLabel.value())); },
                [](TerminalProfile& p, QVariant const& v) { p.tabLabel = v.toString().toStdString(); } },
          } },
        { SettingsGroup::ProfileKeyboard,
          "⌥",
          {
              boolField("option_key_as_alt",
                        "Option key as Alt",
                        "Treat the macOS Option key as Alt modifier.",
                        [](auto& p) -> auto& { return p.optionKeyAsAlt.value(); }),
          } },
        { SettingsGroup::ProfileHistory,
          "≡",
          {
              historyLimitField("history_max_lines",
                                "History max lines",
                                "Maximum number of scrollback lines to keep (-1 = unlimited).",
                                &config::HistoryConfig::maxHistoryLineCount),
              historyLimitField(
                  "history_hard_limit",
                  "History hard limit",
                  "Upper bound on the scrollback (-1 = unlimited). Above the max, the extra "
                  "lines are headroom in which whole (prompt, output) blocks are evicted instead "
                  "of cutting mid-command. Values at or below the max disable that.",
                  &config::HistoryConfig::hardLimit),
              { "history_scroll_multiplier",
                "History scroll multiplier",
                "Number of lines scrolled per scroll wheel step.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(unbox(p.history.value().historyScrollMultiplier)));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto h = p.history.value();
                    h.historyScrollMultiplier = vtbackend::LineCount(v.toInt());
                    p.history = h;
                } },
              boolField("auto_scroll_on_update",
                        "Auto-scroll on update",
                        "Scroll to the bottom automatically when new output arrives.",
                        [](auto& p) -> auto& { return p.history.value().autoScrollOnUpdate; }),
              enumField(
                  "scrollbar_position",
                  "Scrollbar position",
                  "Where the scrollbar is shown. Right is the default for new configurations; a "
                  "contour.yml written by an earlier version keeps hidden. Hidden also hides the "
                  "command marks.",
                  std::array {
                      std::pair { "hidden"sv, config::ScrollBarPosition::Hidden },
                      std::pair { "left"sv, config::ScrollBarPosition::Left },
                      std::pair { "right"sv, config::ScrollBarPosition::Right },
                  },
                  [](TerminalProfile const& p) { return p.scrollbar.value().position; },
                  [](TerminalProfile& p, auto v) {
                      auto sb = p.scrollbar.value();
                      sb.position = v;
                      p.scrollbar = sb;
                  }),
              boolField("hide_scrollbar_in_alt_screen",
                        "Hide scrollbar in alt screen",
                        "Automatically hide the scrollbar when the application enters alternate screen mode.",
                        [](auto& p) -> auto& { return p.scrollbar.value().hideScrollbarInAltScreen; }),
          } },
        { SettingsGroup::ProfileCommandBlocks,
          "❯",
          {
              scrollbarMarkField(
                  "scrollbar_marks_failures",
                  "Scrollbar marks: failed commands",
                  "Mark every command that failed on the scrollbar. Click a mark to jump to it.",
                  vtbackend::ScrollbarMarkSource::Failures),
              scrollbarMarkField("scrollbar_marks_commands",
                                 "Scrollbar marks: other commands",
                                 "Mark every finished command that did not fail on the scrollbar. A "
                                 "failed command is marked by the row above alone.",
                                 vtbackend::ScrollbarMarkSource::Commands),
              scrollbarMarkField("scrollbar_marks_running",
                                 "Scrollbar marks: running commands",
                                 "Mark every command that is still running on the scrollbar, as a "
                                 "hollow tick.",
                                 vtbackend::ScrollbarMarkSource::Running),
              scrollbarMarkField("scrollbar_marks_user_marks",
                                 "Scrollbar marks: user marks",
                                 "Mark the lines you marked with Vi mode's mm on the scrollbar.",
                                 vtbackend::ScrollbarMarkSource::UserMarks),
              scrollbarMarkField("scrollbar_marks_search_matches",
                                 "Scrollbar marks: search matches",
                                 "Mark the rows the active search matches on the scrollbar, in yellow.",
                                 vtbackend::ScrollbarMarkSource::SearchMatches),
              enumFieldFromTable<vtbackend::StickyHeaderMode>(
                  "sticky_header_mode",
                  "Sticky command header",
                  "Pin the command whose output fills the top of the page above it: never, only while "
                  "scrolled back into the history (scrolled), or also while following live output "
                  "(always).",
                  [](TerminalProfile const& p) { return p.stickyHeader.value().mode; },
                  [](TerminalProfile& p, auto v) { p.stickyHeader.value().mode = v; }),
              boolField("sticky_header_show_evicted",
                        "Name evicted commands",
                        "When a command's own line was evicted from the history, name it in the header "
                        "instead: wherever the header shows, and at the very top of the history even "
                        "when the sticky header is set to never.",
                        [](auto& p) -> auto& { return p.stickyHeader.value().showEvicted; }),
              enumFieldFromTable<config::NotifyWhen>(
                  "notify_on_command_finish_when",
                  "Notify on finish",
                  "When a finished command raises a notification: never; when its pane is not focused "
                  "(unfocused); only while its pane is off screen, on another tab, behind a zoomed "
                  "sibling or in a minimised window (hidden); or always.",
                  [](TerminalProfile const& p) { return p.notifyOnCommandFinish.value().when; },
                  [](TerminalProfile& p, auto v) { p.notifyOnCommandFinish.value().when = v; }),
              { "notify_on_command_finish_min_duration",
                "Notify: minimum duration (s)",
                "Commands that finish faster than this never notify.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(p.notifyOnCommandFinish.value().minDuration.count()));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    p.notifyOnCommandFinish.value().minDuration =
                        std::chrono::seconds { std::max(0, v.toInt()) };
                } },
              enumFieldFromTable<config::NotifyOutcome>(
                  "notify_on_command_finish_outcome",
                  "Notify: outcome",
                  "Notify for every command (any), or only for one that failed (failure).",
                  [](TerminalProfile const& p) { return p.notifyOnCommandFinish.value().outcome; },
                  [](TerminalProfile& p, auto v) { p.notifyOnCommandFinish.value().outcome = v; }),
              enumFieldFromTable<config::NotifyAction>(
                  "notify_on_command_finish_action",
                  "Notify: how",
                  "A desktop notification (notify), the profile's bell (bell), or both (notify_bell).",
                  [](TerminalProfile const& p) { return p.notifyOnCommandFinish.value().action; },
                  [](TerminalProfile& p, auto v) { p.notifyOnCommandFinish.value().action = v; }),
              enumFieldFromTable<config::NotifyClearOn>(
                  "notify_on_command_finish_clear_on",
                  "Notify: withdraw on",
                  "Withdraw the notification when the pane is focused (focus), when the next command "
                  "there starts (next), or never.",
                  [](TerminalProfile const& p) { return p.notifyOnCommandFinish.value().clearOn; },
                  [](TerminalProfile& p, auto v) { p.notifyOnCommandFinish.value().clearOn = v; }),
          } },
        { SettingsGroup::ProfileMouse,
          "⊙",
          {
              boolField("hide_mouse_while_typing",
                        "Hide mouse while typing",
                        "Automatically hide the mouse cursor while typing.",
                        [](auto& p) -> auto& { return p.mouse.value().hideWhileTyping; }),
              enumField(
                  "permission_capture_buffer",
                  "Capture buffer permission",
                  "Permission required for the application to capture the screen buffer.",
                  std::array {
                      std::pair { "allow"sv, config::Permission::Allow },
                      std::pair { "deny"sv, config::Permission::Deny },
                      std::pair { "ask"sv, config::Permission::Ask },
                  },
                  [](TerminalProfile const& p) { return p.permissions.value().captureBuffer; },
                  [](TerminalProfile& p, auto v) {
                      auto perm = p.permissions.value();
                      perm.captureBuffer = v;
                      p.permissions = perm;
                  }),
              enumField(
                  "permission_change_font",
                  "Change font permission",
                  "Permission required for the application to change the font.",
                  std::array {
                      std::pair { "allow"sv, config::Permission::Allow },
                      std::pair { "deny"sv, config::Permission::Deny },
                      std::pair { "ask"sv, config::Permission::Ask },
                  },
                  [](TerminalProfile const& p) { return p.permissions.value().changeFont; },
                  [](TerminalProfile& p, auto v) {
                      auto perm = p.permissions.value();
                      perm.changeFont = v;
                      p.permissions = perm;
                  }),
              enumField(
                  "permission_display_statusline",
                  "Display statusline permission",
                  "Permission required for the application to show the host-writable status line.",
                  std::array {
                      std::pair { "allow"sv, config::Permission::Allow },
                      std::pair { "deny"sv, config::Permission::Deny },
                      std::pair { "ask"sv, config::Permission::Ask },
                  },
                  [](TerminalProfile const& p) {
                      return p.permissions.value().displayHostWritableStatusLine;
                  },
                  [](TerminalProfile& p, auto v) {
                      auto perm = p.permissions.value();
                      perm.displayHostWritableStatusLine = v;
                      p.permissions = perm;
                  }),
          } },
        { SettingsGroup::ProfileVisualEffects,
          "✦",
          {
              enumField(
                  "blink_style",
                  "Blink style",
                  "How blinking text (SGR 5/6) is animated: classic toggle, smooth pulse, "
                  "or linger.",
                  std::array {
                      std::pair { "classic"sv, vtbackend::BlinkStyle::Classic },
                      std::pair { "smooth"sv, vtbackend::BlinkStyle::Smooth },
                      std::pair { "linger"sv, vtbackend::BlinkStyle::Linger },
                  },
                  [](TerminalProfile const& p) { return p.blinkStyle.value(); },
                  [](TerminalProfile& p, auto v) { p.blinkStyle = v; }),
              enumField(
                  "screen_transition_style",
                  "Screen transition style",
                  "The visual animation when switching between primary and alternate screens.",
                  std::array {
                      std::pair { "classic"sv, vtbackend::ScreenTransitionStyle::Classic },
                      std::pair { "fade"sv, vtbackend::ScreenTransitionStyle::Fade },
                  },
                  [](TerminalProfile const& p) { return p.screenTransitionStyle.value(); },
                  [](TerminalProfile& p, auto v) { p.screenTransitionStyle = v; }),
              { "cursor_motion_animation_duration",
                "Cursor motion animation (ms)",
                "Duration of the cursor motion animation, in milliseconds (0 = instant).",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<int>(p.cursorMotionAnimationDuration.value().count()));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    p.cursorMotionAnimationDuration = Ms { v.toInt() };
                } },
          } },
        { SettingsGroup::ProfileCursor,
          "▮",
          {
              { "insert_mode_cursor_blinking",
                "Insert mode: blinking",
                "Whether the cursor blinks in Insert mode.",
                "bool",
                [](TerminalProfile const& p) {
                    return QVariant(p.modeInsert.value().cursor.cursorDisplay
                                    == vtbackend::CursorDisplay::Blink);
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto mode = p.modeInsert.value();
                    mode.cursor.cursorDisplay =
                        v.toBool() ? vtbackend::CursorDisplay::Blink : vtbackend::CursorDisplay::Steady;
                    p.modeInsert = mode;
                } },
              enumField(
                  "insert_mode_cursor_shape",
                  "Insert mode: cursor shape",
                  "Cursor shape in Insert mode.",
                  CursorShapeNames,
                  [](TerminalProfile const& p) { return p.modeInsert.value().cursor.cursorShape; },
                  [](TerminalProfile& p, auto v) {
                      auto mode = p.modeInsert.value();
                      mode.cursor.cursorShape = v;
                      p.modeInsert = mode;
                  }),
              { "insert_mode_cursor_blink_interval",
                "Insert mode: blink interval (ms)",
                "Cursor blink interval in Insert mode, in milliseconds.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(
                        static_cast<int>(p.modeInsert.value().cursor.cursorBlinkInterval.count()));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto mode = p.modeInsert.value();
                    mode.cursor.cursorBlinkInterval = Ms { v.toInt() };
                    p.modeInsert = mode;
                } },
              { "normal_mode_cursor_blinking",
                "Normal mode: blinking",
                "Whether the cursor blinks in Normal (Vi) mode.",
                "bool",
                [](TerminalProfile const& p) {
                    return QVariant(p.modeNormal.value().cursor.cursorDisplay
                                    == vtbackend::CursorDisplay::Blink);
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto mode = p.modeNormal.value();
                    mode.cursor.cursorDisplay =
                        v.toBool() ? vtbackend::CursorDisplay::Blink : vtbackend::CursorDisplay::Steady;
                    p.modeNormal = mode;
                } },
              enumField(
                  "normal_mode_cursor_shape",
                  "Normal mode: cursor shape",
                  "Cursor shape in Normal (Vi) mode.",
                  CursorShapeNames,
                  [](TerminalProfile const& p) { return p.modeNormal.value().cursor.cursorShape; },
                  [](TerminalProfile& p, auto v) {
                      auto mode = p.modeNormal.value();
                      mode.cursor.cursorShape = v;
                      p.modeNormal = mode;
                  }),
              { "normal_mode_cursor_blink_interval",
                "Normal mode: blink interval (ms)",
                "Cursor blink interval in Normal (Vi) mode, in milliseconds.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(
                        static_cast<int>(p.modeNormal.value().cursor.cursorBlinkInterval.count()));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto mode = p.modeNormal.value();
                    mode.cursor.cursorBlinkInterval = Ms { v.toInt() };
                    p.modeNormal = mode;
                } },
              { "visual_mode_cursor_blinking",
                "Visual mode: blinking",
                "Whether the cursor blinks in Visual (Vi) mode.",
                "bool",
                [](TerminalProfile const& p) {
                    return QVariant(p.modeVisual.value().cursor.cursorDisplay
                                    == vtbackend::CursorDisplay::Blink);
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto mode = p.modeVisual.value();
                    mode.cursor.cursorDisplay =
                        v.toBool() ? vtbackend::CursorDisplay::Blink : vtbackend::CursorDisplay::Steady;
                    p.modeVisual = mode;
                } },
              enumField(
                  "visual_mode_cursor_shape",
                  "Visual mode: cursor shape",
                  "Cursor shape in Visual (Vi) mode.",
                  CursorShapeNames,
                  [](TerminalProfile const& p) { return p.modeVisual.value().cursor.cursorShape; },
                  [](TerminalProfile& p, auto v) {
                      auto mode = p.modeVisual.value();
                      mode.cursor.cursorShape = v;
                      p.modeVisual = mode;
                  }),
              { "visual_mode_cursor_blink_interval",
                "Visual mode: blink interval (ms)",
                "Cursor blink interval in Visual (Vi) mode, in milliseconds.",
                "int",
                [](TerminalProfile const& p) {
                    return QVariant(
                        static_cast<int>(p.modeVisual.value().cursor.cursorBlinkInterval.count()));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto mode = p.modeVisual.value();
                    mode.cursor.cursorBlinkInterval = Ms { v.toInt() };
                    p.modeVisual = mode;
                } },
          } },
        { SettingsGroup::ProfileBell,
          "♪",
          {
              { "bell_sound",
                "Bell sound",
                "Path to a sound file for the terminal bell, or 'default', or 'off'.",
                "string",
                [](TerminalProfile const& p) {
                    return QVariant(QString::fromStdString(p.bell.value().sound));
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto b = p.bell.value();
                    b.sound = v.toString().toStdString();
                    p.bell = b;
                } },
              boolField("bell_alert",
                        "Bell flash",
                        "Flash the terminal window when the bell rings.",
                        [](auto& p) -> auto& { return p.bell.value().alert; }),
              { "bell_volume",
                "Bell volume",
                "Volume of the bell sound (0.0 = silent, 1.0 = full).",
                "double",
                [](TerminalProfile const& p) { return QVariant(static_cast<double>(p.bell.value().volume)); },
                [](TerminalProfile& p, QVariant const& v) {
                    auto b = p.bell.value();
                    b.volume = static_cast<float>(v.toDouble());
                    p.bell = b;
                } },
          } },
        { SettingsGroup::ProfileBackground,
          "▨",
          {
              { "background_opacity",
                "Background opacity",
                "Terminal background opacity (0.0 = fully transparent, 1.0 = fully opaque).",
                "double",
                [](TerminalProfile const& p) {
                    return QVariant(static_cast<double>(static_cast<uint8_t>(p.background.value().opacity))
                                    / 255.0);
                },
                [](TerminalProfile& p, QVariant const& v) {
                    auto bg = p.background.value();
                    bg.opacity =
                        vtbackend::Opacity(static_cast<unsigned>(255 * std::clamp(v.toDouble(), 0.0, 1.0)));
                    p.background = bg;
                } },
              boolField(
                  "background_blur",
                  "Background blur",
                  "Whether to blur content behind the transparent terminal window (platform dependent).",
                  [](auto& p) -> auto& { return p.background.value().blur; }),
          } },
        { SettingsGroup::ProfileLinks,
          "⧉",
          {
              enumFieldFromTable<vtbackend::LinkDecorationMode>(
                  "hyperlink_decoration_mode",
                  "Show links",
                  "When links are decorated: always, only the one under the pointer, all of them while "
                  "the "
                  "follow modifier is held, or never. Applies to OSC 8 hyperlinks and detected links "
                  "alike.",
                  [](TerminalProfile const& p) { return p.hyperlinkDecoration.value().mode; },
                  [](TerminalProfile& p, auto v) {
                      auto hd = p.hyperlinkDecoration.value();
                      hd.mode = v;
                      p.hyperlinkDecoration = hd;
                  }),
              enumField(
                  "hyperlink_decoration_normal",
                  "Normal style",
                  "The underline style for hyperlinks in their normal state.",
                  vtrasterizer::DecoratorNames,
                  [](TerminalProfile const& p) { return p.hyperlinkDecoration.value().normal; },
                  [](TerminalProfile& p, auto v) {
                      auto hd = p.hyperlinkDecoration.value();
                      hd.normal = v;
                      p.hyperlinkDecoration = hd;
                  }),
              enumField(
                  "hyperlink_decoration_hover",
                  "Hover style",
                  "The underline style for hyperlinks when hovered.",
                  vtrasterizer::DecoratorNames,
                  [](TerminalProfile const& p) { return p.hyperlinkDecoration.value().hover; },
                  [](TerminalProfile& p, auto v) {
                      auto hd = p.hyperlinkDecoration.value();
                      hd.hover = v;
                      p.hyperlinkDecoration = hd;
                  }),
              boolField("link_detection_primary_urls",
                        "Detect URLs on the primary screen",
                        "URLs are detected on the primary screen, where the shell prompt and its output "
                        "live. OSC 8 hyperlinks are always links.",
                        [](auto& p) -> auto& { return p.linkDetection.value().primaryScreen.urls; }),
              boolField("link_detection_primary_file_paths",
                        "Detect file paths on the primary screen",
                        "File paths are detected on the primary screen, where the shell prompt and its "
                        "output live. OSC 8 hyperlinks are always links.",
                        [](auto& p) -> auto& { return p.linkDetection.value().primaryScreen.filePaths; }),
              boolField("link_detection_other_urls",
                        "Detect URLs on other screens",
                        "URLs are detected on other screens. Other screens are the alternate screen used "
                        "by full-screen programs (vim, less, htop) and any further page. OSC 8 "
                        "hyperlinks are always links.",
                        [](auto& p) -> auto& { return p.linkDetection.value().otherScreens.urls; }),
              boolField("link_detection_other_file_paths",
                        "Detect file paths on other screens",
                        "File paths are detected on other screens. Other screens are the alternate "
                        "screen used by full-screen programs (vim, less, htop) and any further page. OSC "
                        "8 hyperlinks are always links.",
                        [](auto& p) -> auto& { return p.linkDetection.value().otherScreens.filePaths; }),
              ProfileFieldDescriptor {
                  .key = "file_opener_command",
                  .label = "Open files with",
                  .help = "A command line for detected file paths, with {path}, {line} and {column}; empty "
                          "opens them with the system's default application. No shell is involved.",
                  .type = "string",
                  .get =
                      [](TerminalProfile const& p) {
                          return QVariant(toQString(p.fileOpener.value().command));
                      },
                  .set =
                      [](TerminalProfile& p, QVariant const& v) {
                          auto opener = p.fileOpener.value();
                          opener.command = v.toString().toStdString();
                          p.fileOpener = opener;
                      },
                  .options = {},
                  .validate = [](QVariant const& v) -> QString {
                      auto const text = v.toString().toStdString();
                      if (text.empty())
                          return {};
                      auto const parsed = config::FileOpenerTemplate::parse(text);
                      return parsed ? QString {} : toQString(config::describe(parsed.error()));
                  } },
              enumFieldFromTable<actions::OutputPlacement>(
                  "file_opener_placement",
                  "Open in",
                  "Where a file opened with the command above appears: a program of its own "
                  "(detached), or a Contour pane running it (split, tab).",
                  [](TerminalProfile const& p) { return p.fileOpener.value().placement; },
                  [](TerminalProfile& p, auto v) {
                      auto opener = p.fileOpener.value();
                      opener.placement = v;
                      p.fileOpener = opener;
                  }),
          } },
        { SettingsGroup::ProfileStatusLine,
          "▤",
          {
              enumField(
                  "status_line_display",
                  "Status line display",
                  "Initial state of the status line: none (hidden) or indicator (shown).",
                  // HostWritable is deliberately absent: it is a state the host puts the
                  // terminal into at runtime, not an initial state a profile may request.
                  std::array {
                      std::pair { "none"sv, vtbackend::StatusDisplayType::None },
                      std::pair { "indicator"sv, vtbackend::StatusDisplayType::Indicator },
                  },
                  [](TerminalProfile const& p) { return p.statusLine.value().initialType; },
                  [](TerminalProfile& p, auto v) {
                      auto sl = p.statusLine.value();
                      sl.initialType = v;
                      p.statusLine = sl;
                  }),
              enumField(
                  "status_line_position",
                  "Status line position",
                  "Where the status line appears: top or bottom of the terminal.",
                  std::array {
                      std::pair { "top"sv, vtbackend::StatusDisplayPosition::Top },
                      std::pair { "bottom"sv, vtbackend::StatusDisplayPosition::Bottom },
                  },
                  [](TerminalProfile const& p) { return p.statusLine.value().position; },
                  [](TerminalProfile& p, auto v) {
                      auto sl = p.statusLine.value();
                      sl.position = v;
                      p.statusLine = sl;
                  }),
              boolField(
                  "status_line_sync_window_title",
                  "Sync window title with status line",
                  "Synchronize the window title with the host-writable status line content when denied.",
                  [](auto& p) -> auto& {
                      return p.statusLine.value().syncWindowTitleWithHostWritableStatusDisplay;
                  }),
          } },
    };
    return groups;
}

namespace
{
    /// The "Rendering GPU" entries: the three keywords, every present GPU by title, and the configured
    /// GPU when it is gone, so the current value stays representable.
    [[nodiscard]] std::vector<EnumOption> gpuOptions(OptionContext const& context)
    {
        auto options = std::vector<EnumOption> {
            { .value = "auto", .label = "Automatic (system default)" },
            { .value = "integrated", .label = "Integrated GPU" },
            { .value = "discrete", .label = "Discrete GPU" },
        };
        for (auto const& gpu: context.gpus)
            options.push_back({ .value = QString::fromStdString(std::format("{}", gpu.id)),
                                .label = QString::fromStdString(gpu.title) });
        auto const& selector = context.config.renderer.value().gpu;
        if (selector.preference == config::GpuPreference::Specific && selector.id
            && std::ranges::none_of(context.gpus, [&](auto const& gpu) { return gpu.id == *selector.id; }))
            options.push_back(
                { .value = QString::fromStdString(std::format("{}", *selector.id)),
                  .label = QString::fromStdString(std::format("Unavailable GPU ({})", *selector.id)) });
        return options;
    }

    /// Reads @p text as the loader reads a scalar of type @p T: through yaml-cpp, which takes `True` for a
    /// bool and `0x10` for a number.
    /// @return The value, or an invalid QVariant where the loader would refuse it.
    template <typename T>
    [[nodiscard]] QVariant scalarFromYaml(std::string const& text)
    {
        auto value = T {};
        if (YAML::convert<T>::decode(YAML::Node(text), value))
            return QVariant(value);
        return {};
    }

    /// Reads @p text as one of @p options, in any case, as the enum loaders do.
    /// @return The option, or an invalid QVariant when @p text is none of them.
    [[nodiscard]] QVariant optionFromYaml(QStringList const& options, std::string const& text)
    {
        auto const wanted = QString::fromStdString(text);
        auto const option = std::ranges::find_if(options, [&wanted](QString const& candidate) {
            return candidate.compare(wanted, Qt::CaseInsensitive) == 0;
        });
        return option != options.end() ? QVariant(*option) : QVariant();
    }

    /// The refusal of @p token, a word none of the offered values is spelled as.
    [[nodiscard]] std::unexpected<std::string> notOffered(std::string const& token)
    {
        return std::unexpected(std::format("'{}' is not one of the offered values.", token));
    }

    /// Builds a bool global-field descriptor.
    /// @param key   The contour.yml key path (see GlobalFieldDescriptor::key).
    /// @param label Human-readable label.
    /// @param help  One-line help text.
    /// @param get   Reads the effective value from the configuration.
    /// @return The descriptor.
    GlobalFieldDescriptor globalBoolField(QString key,
                                          QString label,
                                          QString help,
                                          std::function<bool(config::Config const&)> get)
    {
        return { .key = std::move(key),
                 .label = std::move(label),
                 .help = std::move(help),
                 .type = "bool",
                 .get = [get = std::move(get)](config::Config const& c) { return QVariant(get(c)); },
                 .toYaml = [](QVariant const& v) { return std::string { v.toBool() ? "true" : "false" }; },
                 .fromYaml = scalarFromYaml<bool> };
    }

    /// Builds an integer global-field descriptor that refuses values outside [@p lowest, @p highest], so
    /// the page cannot write a value the loader would clamp or reject.
    /// @param key     The contour.yml key path (see GlobalFieldDescriptor::key).
    /// @param label   Human-readable label.
    /// @param help    One-line help text.
    /// @param get     Reads the effective value from the configuration.
    /// @param lowest  The smallest accepted value.
    /// @param highest The largest accepted value.
    /// @return The descriptor.
    GlobalFieldDescriptor globalIntField(QString key,
                                         QString label,
                                         QString help,
                                         std::function<int(config::Config const&)> get,
                                         int lowest,
                                         int highest)
    {
        return { .key = std::move(key),
                 .label = std::move(label),
                 .help = std::move(help),
                 .type = "int",
                 .get = [get = std::move(get)](config::Config const& c) { return QVariant(get(c)); },
                 .toYaml = [](QVariant const& v) { return std::to_string(v.toInt()); },
                 .fromYaml = scalarFromYaml<int>,
                 .validate = [lowest, highest](QVariant const& v) -> std::expected<void, std::string> {
                     auto isNumber = false; // Qt's out-parameter
                     auto const number = v.toInt(&isNumber);
                     if (isNumber && number >= lowest && number <= highest)
                         return {};
                     if (highest == std::numeric_limits<int>::max())
                         return std::unexpected(std::format("Enter a whole number of at least {}.", lowest));
                     return std::unexpected(
                         std::format("Enter a whole number from {} to {}.", lowest, highest));
                 } };
    }

    /// Builds a decimal global-field descriptor that refuses values below @p lowest.
    /// @param key    The contour.yml key path (see GlobalFieldDescriptor::key).
    /// @param label  Human-readable label.
    /// @param help   One-line help text.
    /// @param get    Reads the effective value from the configuration.
    /// @param lowest The smallest accepted value.
    /// @return The descriptor.
    GlobalFieldDescriptor globalDoubleField(QString key,
                                            QString label,
                                            QString help,
                                            std::function<double(config::Config const&)> get,
                                            double lowest)
    {
        return { .key = std::move(key),
                 .label = std::move(label),
                 .help = std::move(help),
                 .type = "double",
                 .get = [get = std::move(get)](config::Config const& c) { return QVariant(get(c)); },
                 .toYaml = [](QVariant const& v) { return std::format("{}", v.toDouble()); },
                 .fromYaml = scalarFromYaml<double>,
                 .validate = [lowest](QVariant const& v) -> std::expected<void, std::string> {
                     auto isNumber = false; // Qt's out-parameter
                     if (auto const number = v.toDouble(&isNumber); isNumber && number >= lowest)
                         return {};
                     return std::unexpected(std::format("Enter a number of at least {}.", lowest));
                 } };
    }

    /// Builds a string global-field descriptor, optionally checked by @p validate before it is written.
    /// @param key      The contour.yml key path (see GlobalFieldDescriptor::key).
    /// @param label    Human-readable label.
    /// @param help     One-line help text.
    /// @param get      Reads the effective value from the configuration.
    /// @param validate Refuses a value the loader would reject; empty accepts every string.
    /// @return The descriptor.
    GlobalFieldDescriptor globalStringField(QString key,
                                            QString label,
                                            QString help,
                                            std::function<std::string(config::Config const&)> get,
                                            GlobalFieldValidator validate = {})
    {
        return { .key = std::move(key),
                 .label = std::move(label),
                 .help = std::move(help),
                 .type = "string",
                 .get = [get = std::move(get)](
                            config::Config const& c) { return QVariant(QString::fromStdString(get(c))); },
                 .toYaml = [](QVariant const& v) { return v.toString().toStdString(); },
                 .fromYaml = [](std::string const& text) { return QVariant(QString::fromStdString(text)); },
                 .validate = std::move(validate) };
    }

    /// Builds an enum global-field descriptor offering @p options.
    /// @param key      The contour.yml key path (see GlobalFieldDescriptor::key).
    /// @param label    Human-readable label.
    /// @param help     One-line help text.
    /// @param get      Reads the effective value from the configuration, spelled as one of @p options.
    /// @param options  The values offered, spelled as the loader reads them.
    /// @param validate Refuses a value the loader would reject; empty accepts @p options alone.
    /// @return The descriptor.
    GlobalFieldDescriptor globalEnumField(QString key,
                                          QString label,
                                          QString help,
                                          std::function<std::string(config::Config const&)> get,
                                          QStringList const& options,
                                          GlobalFieldValidator validate = {})
    {
        if (!validate)
            validate = [options](QVariant const& v) -> std::expected<void, std::string> {
                if (options.contains(v.toString()))
                    return {};
                return notOffered(v.toString().toStdString());
            };
        return { .key = std::move(key),
                 .label = std::move(label),
                 .help = std::move(help),
                 .type = "enum",
                 .get = [get = std::move(get)](
                            config::Config const& c) { return QVariant(QString::fromStdString(get(c))); },
                 .toYaml = [](QVariant const& v) { return v.toString().toStdString(); },
                 .fromYaml = [options](std::string const& text) { return optionFromYaml(options, text); },
                 .options = options,
                 .validate = std::move(validate) };
    }

    /// Builds an enum global-field descriptor whose options, displayed value and accepted set all come
    /// from the enum's ConfigEnum table -- the one the loader parses with -- so the page can neither
    /// offer nor write a token the loader rejects.
    /// @param key   The contour.yml key path (see GlobalFieldDescriptor::key).
    /// @param label Human-readable label.
    /// @param help  One-line help text.
    /// @param get   Reads the effective value from the configuration.
    /// @return The descriptor.
    template <typename Enum>
    GlobalFieldDescriptor globalEnumFieldFromTable(QString key,
                                                   QString label,
                                                   QString help,
                                                   std::function<Enum(config::Config const&)> get)
    {
        return globalEnumField(
            std::move(key),
            std::move(label),
            std::move(help),
            [get = std::move(get)](config::Config const& c) {
                return std::string { config::configEnumToken(get(c)) };
            },
            configEnumTokens<Enum>(),
            [](QVariant const& v) -> std::expected<void, std::string> {
                auto const token = v.toString().toStdString();
                if (config::configEnumFromToken<Enum>(token))
                    return {};
                return notOffered(token);
            });
    }

    /// The gutter's own timestamp-format check, vtbackend::measureTimestampFormat() -- the one the
    /// GutterConfig loader uses -- phrased for the page with vtbackend::timestampFormatErrorText()'s reason.
    /// @param value The format the user typed.
    /// @return Nothing when valid, or the message the page shows.
    [[nodiscard]] std::expected<void, std::string> checkTimestampFormat(QVariant const& value)
    {
        return vtbackend::measureTimestampFormat(value.toString().toStdString())
            .transform([](int /*columns*/) {})
            .transform_error([](vtbackend::TimestampFormatError error) {
                return std::format("The gutter's timestamp format cannot be used because {}.",
                                   vtbackend::timestampFormatErrorText(error));
            });
    }

    /// Refuses a GPU the loader cannot parse: one of the three keywords, or a vendor:device id.
    /// @param value The GPU picked.
    /// @return Nothing when it parses, or the message the page shows.
    [[nodiscard]] std::expected<void, std::string> checkGpu(QVariant const& value)
    {
        auto const token = value.toString().toStdString();
        if (config::parseGpuSelector(token))
            return {};
        return notOffered(token);
    }

    /// Refuses a blank pager: every OpenCommandOutput binding without a program of its own would then fail
    /// for want of one.
    /// @param value The pager the user typed.
    /// @return Nothing when it names something, or the message the page shows.
    [[nodiscard]] std::expected<void, std::string> checkPager(QVariant const& value)
    {
        if (!value.toString().trimmed().isEmpty())
            return {};
        return std::unexpected(std::string { "Enter the program to open a command's output with. To go "
                                             "back to the one contour.yml names, use Reset." });
    }

} // namespace

namespace
{
    /// The group of each global field; a dotted key's section decides it for every leaf.
    struct GlobalFieldGroupRow
    {
        std::string_view key {};                             ///< A key, or a section followed by a dot.
        SettingsGroup group = SettingsGroup::GeneralWindows; ///< The group its rows are shown in.
    };

    /// Every global key's group (R-h). A key that has no row here fails globalFieldDescriptors().
    constexpr auto GlobalFieldGroups = std::array {
        GlobalFieldGroupRow { .key = "reflow_on_resize", .group = SettingsGroup::GeneralWindows },
        GlobalFieldGroupRow { .key = "snap_to_rows", .group = SettingsGroup::GeneralWindows },
        GlobalFieldGroupRow { .key = "remember_window_position", .group = SettingsGroup::GeneralWindows },
        GlobalFieldGroupRow { .key = "spawn_new_process", .group = SettingsGroup::GeneralWindows },
        GlobalFieldGroupRow { .key = "tab_switch_on_horizontal_wheel",
                              .group = SettingsGroup::GeneralWindows },
        GlobalFieldGroupRow { .key = "command_palette_recent_count", .group = SettingsGroup::GeneralPalette },
        GlobalFieldGroupRow { .key = "word_delimiters", .group = SettingsGroup::GeneralPalette },
        GlobalFieldGroupRow { .key = "read_buffer_size", .group = SettingsGroup::GeneralAdvanced },
        GlobalFieldGroupRow { .key = "grapheme_clustering", .group = SettingsGroup::GeneralAdvanced },
        GlobalFieldGroupRow { .key = "text_scaling_method", .group = SettingsGroup::GeneralAdvanced },
        GlobalFieldGroupRow { .key = "renderer.gpu", .group = SettingsGroup::GeneralAdvanced },
        GlobalFieldGroupRow { .key = "theme", .group = SettingsGroup::AppearanceChrome },
        GlobalFieldGroupRow { .key = "ui_style", .group = SettingsGroup::AppearanceChrome },
        GlobalFieldGroupRow { .key = "chrome_density", .group = SettingsGroup::AppearanceChrome },
        GlobalFieldGroupRow { .key = "window_control_style", .group = SettingsGroup::AppearanceChrome },
        GlobalFieldGroupRow { .key = "ui_font_family", .group = SettingsGroup::AppearanceChrome },
        GlobalFieldGroupRow { .key = "ui_font_size", .group = SettingsGroup::AppearanceChrome },
        GlobalFieldGroupRow { .key = "tab_bar_position", .group = SettingsGroup::AppearanceTabBar },
        GlobalFieldGroupRow { .key = "tab_bar_visibility", .group = SettingsGroup::AppearanceTabBar },
        GlobalFieldGroupRow { .key = "gutter.", .group = SettingsGroup::ShellGutter },
        GlobalFieldGroupRow { .key = "command_blocks.", .group = SettingsGroup::ShellCommandBlocks },
        GlobalFieldGroupRow { .key = "folding.", .group = SettingsGroup::ShellFolding },
    };

    /// The group @p key is shown in: its own row, else its section's.
    [[nodiscard]] SettingsGroup globalFieldGroupOf(std::string_view key)
    {
        auto const exact = std::ranges::find(GlobalFieldGroups, key, &GlobalFieldGroupRow::key);
        if (exact != GlobalFieldGroups.end())
            return exact->group;
        auto const section = std::ranges::find_if(GlobalFieldGroups, [key](GlobalFieldGroupRow const& row) {
            return row.key.ends_with('.') && key.starts_with(row.key);
        });
        Require(section != GlobalFieldGroups.end()); // every global field has a group
        return section->group;
    }
} // namespace

/// The editable global settings. A key is a contour.yml key path -- top-level ("reflow_on_resize")
/// or one level into a section ("renderer.gpu"), which settings.yml stores nested -- and needs a row
/// of config::globalOverrides() that applies it (for a dotted key, its section's), or setGlobalField
/// refuses to save it. A leaf of a section that has a row needs nothing more.
std::vector<GlobalFieldDescriptor> const& globalFieldDescriptors()
{
    static auto const descriptors = [] {
        auto list = std::vector<GlobalFieldDescriptor> {
            globalBoolField("reflow_on_resize",
                            "Reflow on resize",
                            "Reflow lines when the terminal is resized.",
                            [](config::Config const& c) { return c.reflowOnResize.value(); }),
            globalBoolField("snap_to_rows",
                            "Snap window height to rows",
                            "Align startup and manual resizing with one visible pane. Wayland: startup only.",
                            [](config::Config const& c) { return c.snapToRows.value(); }),
            globalBoolField("remember_window_position",
                            "Remember window position",
                            "Remember position and monitor, without saving size. Defaults on for Windows; "
                            "opt in on macOS/X11. Wayland controls placement. Restores on next startup.",
                            [](config::Config const& c) { return c.rememberWindowPosition.value(); }),
            globalBoolField("tab_switch_on_horizontal_wheel",
                            "Switch tabs on horizontal wheel",
                            "A horizontal wheel tilt or trackpad swipe switches to the previous/next tab.",
                            [](config::Config const& c) { return c.tabSwitchOnHorizontalWheel.value(); }),
            globalBoolField("spawn_new_process",
                            "Spawn new process",
                            "Spawn a separate process for each new terminal window.",
                            [](config::Config const& c) { return c.spawnNewProcess.value(); }),
            // At least 1 byte: the size reaches the terminal as an unsigned one, where a negative one would
            // wrap around.
            globalIntField(
                "read_buffer_size",
                "PTY read buffer size",
                "Most bytes read from the pseudo-terminal and parsed in one go.",
                [](config::Config const& c) { return c.ptyReadBufferSize.value(); },
                1,
                std::numeric_limits<int>::max()),
            // 0 is a choice: it turns the recently used section off.
            globalIntField(
                "command_palette_recent_count",
                "Recent commands",
                "How many recently-used commands the command palette lists first.",
                [](config::Config const& c) { return c.commandPaletteRecentCount.value(); },
                0,
                std::numeric_limits<int>::max()),
            globalStringField("word_delimiters",
                              "Word delimiters",
                              "Characters that separate words when selecting by double-click.",
                              [](config::Config const& c) { return c.wordDelimiters.value(); }),
            // Reuse the std::formatter<GuiTheme> (the "terminal"/"system"/"dark"/"light" source of truth)
            // rather than re-switching the enum here, matching the std::format idiom used for modifiers/keys.
            globalEnumField("theme",
                            "GUI theme",
                            "Light/dark appearance of the GUI chrome (menus, tabs, dialogs). 'terminal' "
                            "matches the terminal's background; 'system' follows the OS.",
                            [](config::Config const& c) { return std::format("{}", c.theme.value()); },
                            { "terminal", "system", "dark", "light" }),
            // Both tab bar rows read their options, their rendered value and their accepted set from
            // contour/TabBarMode.h, so the page cannot offer a token the configuration reader would
            // then reject -- and a new mode shows up here with this file untouched.
            globalEnumFieldFromTable<config::TabBarPosition>(
                "tab_bar_position",
                "Tab bar position",
                "Where the tab strip sits relative to the terminal content.",
                [](config::Config const& c) { return c.tabBarPosition.value(); }),
            globalEnumFieldFromTable<config::TabBarVisibility>(
                "tab_bar_visibility",
                "Tab bar visibility",
                "When the tab strip is shown: always, never, or only once a window has more than one tab.",
                [](config::Config const& c) { return c.tabBarVisibility.value(); }),
            // Reads its options and its accepted set from contour/UiStyle.h, like the tab bar rows
            // above. The restart note is part of the help text rather than a separate affordance
            // because the limitation is Qt's: the Quick Controls style is chosen once, before the
            // first control exists (see ContourGuiApp), so there is nothing to apply live.
            globalEnumFieldFromTable<config::UiStyle>(
                "ui_style",
                "Interface style",
                "Native GUI chrome, or cell-quantized terminal (TUI) chrome drawn in a monospace font. "
                "Takes effect after restart.",
                [](config::Config const& c) { return c.uiStyle.value(); }),
            // Takes effect after restart for the same reason as ui_style: the GPU is fixed before the
            // first window exists (see ContourGuiApp::applyGraphicsDevice / applyOpenGlGpuSelection).
            // Shown on macOS too, where it does nothing, and the help says so: one row table for every
            // platform beats a platform-gated row whose tests would need the same gate.
            { .key = "renderer.gpu",
              .label = "Rendering GPU",
              .help =
                  "Which GPU draws the terminal. Takes effect after restart. OpenGL honours this on Linux "
                  "only. Has no effect on macOS.",
              .type = "enum",
              .get =
                  [](config::Config const& c) {
                      return QVariant(QString::fromStdString(std::format("{}", c.renderer.value().gpu)));
                  },
              .toYaml = [](QVariant const& v) { return v.toString().toStdString(); },
              .fromYaml =
                  [](std::string const& text) {
                      auto const selector = config::parseGpuSelector(text);
                      return selector ? QVariant(QString::fromStdString(std::format("{}", *selector)))
                                      : QVariant();
                  },
              .optionsProvider = gpuOptions,
              .validate = checkGpu },
            // No restart note, unlike the three ui_* rows around it: this one IS applied live (see
            // ContourGuiApp::applyWindowControlStyle), because nothing about it is pinned before the
            // first control exists -- it only decides what the title bar draws and where.
            globalEnumFieldFromTable<config::WindowControlStyle>(
                "window_control_style",
                "Window controls",
                "How the minimize/maximize/close buttons are drawn, and which side they sit on. Only "
                "applies while the native title bar is hidden.",
                [](config::Config const& c) { return c.windowControlStyle.value(); }),
            // No restart note either: the chrome style provider is rebuilt on reload.
            globalEnumFieldFromTable<config::ChromeDensity>(
                "chrome_density",
                "Density",
                "How much room the window chrome takes: pane cards with headers, or bare split lines.",
                [](config::Config const& c) { return c.chromeDensity.value(); }),
            // Same restart note as ui_style above, and for the same reason: the chrome's design
            // tokens are resolved from this font once, when the provider is built (ContourGuiApp),
            // and every property of it is CONSTANT. Saying so here is the whole difference between
            // "this setting needs a restart" and "this setting is broken".
            globalStringField("ui_font_family",
                              "Interface font",
                              "Font the terminal-style chrome is drawn with. Empty inherits the running "
                              "profile's font, which is what makes the chrome match the terminal grid. Takes "
                              "effect after restart.",
                              [](config::Config const& c) { return c.uiFontFamily.value(); }),
            globalDoubleField(
                "ui_font_size",
                "Interface font size",
                "Size, in points, the terminal-style chrome is drawn with. 0 inherits the "
                "running profile's font size. Takes effect after restart.",
                [](config::Config const& c) { return c.uiFontSize.value(); },
                0.0),
            globalBoolField("grapheme_clustering",
                            "Grapheme clustering",
                            "Whether DEC mode 2027 starts out set, letting a codepoint arriving after the "
                            "first revise its grapheme cluster's width. Applications may still toggle it at "
                            "runtime.",
                            [](config::Config const& c) { return c.graphemeClustering.value(); }),
            // Reuse the std::formatter<GlyphScalingMethod>, which formats via nameOf() -- the same source of
            // truth the config reader parses, so the two cannot drift apart.
            globalEnumField(
                "text_scaling_method",
                "Scaled text quality",
                "How a glyph is enlarged when an application asks for scaled text. Stretch is "
                "free but softens at large sizes; rerasterize stays crisp at the cost of memory "
                "and rasterization.",
                [](config::Config const& c) { return std::format("{}", c.textScalingMethod.value()); },
                { "stretch", "rerasterize" }),
            // {{{ Command blocks. A dotted key is a path to one leaf of a contour.yml section; settings.yml
            // stores it nested and the merge applies that leaf alone (see mergeGuiManagedSideFiles), so a
            // row here never resets its section's other keys.
            globalBoolField("gutter.exit_status",
                            "Gutter: exit status",
                            "Color each command's fold controls by how it ended, draw a triangle beside "
                            "a running command, and a dot beside a finished one that shows no fold "
                            "control: one with nothing to fold, or any one while fold markers are off.",
                            [](config::Config const& c) { return c.gutter.value().exitStatus; }),
            globalBoolField("gutter.user_marks",
                            "Gutter: user marks",
                            "Show the lines you marked with Vi mode's mm in the gutter.",
                            [](config::Config const& c) { return c.gutter.value().userMarks; }),
            globalBoolField("gutter.bold_on_hover",
                            "Gutter: bold on hover",
                            "Draw the fold under the pointer -- its line, its box and its end -- one stroke "
                            "heavier, as well as at full strength.",
                            [](config::Config const& c) { return c.gutter.value().boldOnHover; }),
            globalIntField(
                "gutter.block_column_width",
                "Gutter: block column width",
                QString::fromStdString(
                    std::format("Cells the fold controls and command marks are drawn across, "
                                "{} to {}. Changing it resizes the page.",
                                vtbackend::MinBlockColumnWidth,
                                vtbackend::MaxBlockColumnWidth)),
                [](config::Config const& c) { return c.gutter.value().blockColumnWidth; },
                vtbackend::MinBlockColumnWidth,
                vtbackend::MaxBlockColumnWidth),
            globalEnumFieldFromTable<vtbackend::LineNumberMode>(
                "gutter.line_numbers",
                "Gutter: line numbers",
                "off, absolute (counted from the session's first line), relative (distance from the "
                "cursor line) or hybrid (the cursor line absolute, the others relative).",
                [](config::Config const& c) { return c.gutter.value().lineNumbers; }),
            globalIntField(
                "gutter.line_number_width",
                "Gutter: line number width",
                QString::fromStdString(std::format("Columns reserved for line numbers, {} to {}. Changing "
                                                   "it resizes the page.",
                                                   vtbackend::MinLineNumberWidth,
                                                   vtbackend::MaxLineNumberWidth)),
                [](config::Config const& c) { return c.gutter.value().lineNumberWidth; },
                vtbackend::MinLineNumberWidth,
                vtbackend::MaxLineNumberWidth),
            globalBoolField("gutter.timestamps",
                            "Gutter: timestamps",
                            "Show beside each line the time it was first written.",
                            [](config::Config const& c) { return c.gutter.value().timestamps; }),
            globalStringField(
                "gutter.timestamp_format",
                "Gutter: timestamp format",
                "A std::format chrono format in local time, such as %H:%M:%S. It begins with "
                "a % conversion, and every character it spells takes one column.",
                [](config::Config const& c) { return c.gutter.value().timestampFormat; },
                checkTimestampFormat),
            globalIntField(
                "command_blocks.max_records",
                "Command blocks: records kept",
                "How many commands are remembered (command line, exit status, times, "
                "directory), including ones whose output has left the history. A change applies to "
                "sessions opened afterwards.",
                [](config::Config const& c) { return static_cast<int>(c.commandBlocks.value().maxRecords); },
                1,
                static_cast<int>(config::MaxCommandBlockRecords)),
            globalStringField(
                "command_blocks.pager",
                "Command blocks: pager",
                "The program a command's output is opened in when an OpenCommandOutput "
                "binding names none.",
                [](config::Config const& c) { return c.commandBlocks.value().pager; },
                checkPager),
            globalEnumFieldFromTable<actions::OutputFormat>(
                "command_blocks.pager_format",
                "Command blocks: pager format",
                "Whether the pager renders colors: sgr keeps them in the file it is given, plain writes "
                "text only. A binding's own format wins, and a binding's own program gets sgr.",
                [](config::Config const& c) { return c.commandBlocks.value().pagerFormat; }),
            globalBoolField("folding.enabled",
                            "Folding: enabled",
                            "Whether a finished command's output can be collapsed to its prompt line.",
                            [](config::Config const& c) { return c.folding.value().enabled; }),
            globalBoolField("folding.show_markers",
                            "Folding: fold markers",
                            "Draw the fold controls in the gutter's block column.",
                            [](config::Config const& c) { return c.folding.value().showMarkers; }),
            globalBoolField(
                "folding.auto_collapse_on_new_command",
                "Folding: collapse on next prompt",
                "Collapse a command's output as soon as the next prompt appears.",
                [](config::Config const& c) { return c.folding.value().autoCollapseOnNewCommand; }),
            globalEnumFieldFromTable<vtbackend::FoldJumpBehavior>(
                "folding.on_jump_into_fold",
                "Folding: jump into a fold",
                "What a Vi-mode jump to a line inside a collapsed block does: expand it, or skip to "
                "its prompt line.",
                [](config::Config const& c) { return c.folding.value().onJumpIntoFold; }),
            // }}}
        };
        for (auto& descriptor: list)
            descriptor.group = globalFieldGroupOf(descriptor.key.toStdString());
        return list;
    }();
    return descriptors;
}

} // namespace contour::window::fields
