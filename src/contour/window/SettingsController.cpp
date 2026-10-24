// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Shortcut.hpp>
#include <contour/config/FileOpenerTemplate.hpp>
#include <contour/config/GlobalOverrides.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/window/SettingsController.hpp>

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

namespace contour::window
{

using config::SettingsOrigin;
using config::TerminalProfile;
using namespace std::string_view_literals;

namespace
{
    /// A data-driven descriptor for one editable scalar profile field: adding a field to the settings
    /// page is adding a row here, not editing logic in three places.
    struct ProfileFieldDescriptor
    {
        QString key;   //!< Stable identifier / QML key.
        QString label; //!< Human-readable label.
        QString help;  //!< One-line help text.
        QString type;  //!< "bool" | "int" | "double" | "string" | "enum".
        std::function<QVariant(TerminalProfile const&)> get;        //!< Reads the field as a QVariant.
        std::function<void(TerminalProfile&, QVariant const&)> set; //!< Writes the field from QVariant.
        QStringList options {}; //!< For "enum": the allowed values (the combo model + accepted set).
        /// What is wrong with a value, shown under the editor; empty when nothing is. Absent for a field
        /// whose every value is usable.
        std::function<QString(QVariant const&)> validate {};
    };

    /// One titled group of related profile fields, as the settings page shows them.
    ///
    /// The grouping is data, not presentation: the page renders one collapsible section per group, and a
    /// field's section is decided here, next to the field. These boundaries previously existed only as
    /// `// {{{ … }}}` comments around the descriptor list, which left the page rendering ninety
    /// undifferentiated rows in a single flat column -- and left it hiding the indicator fields by
    /// matching on a key prefix in QML, a rule no C++ reader of the descriptor list could see.
    struct ProfileFieldGroup
    {
        QString title;                              //!< Section heading.
        QString glyph;                              //!< Leading pictograph for the heading.
        std::vector<ProfileFieldDescriptor> fields; //!< The fields shown under it, in order.
    };

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
    /// so the row is "is this source in the list" -- three checkboxes rather than a list editor, because
    /// the list is a set of three known values, not free text.
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

    [[nodiscard]] QString toQString(std::string_view text)
    {
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
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
            { "Window",
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
            { "Font",
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
            { "Scrolling",
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
            { "Behaviour",
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
            { "Terminal",
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
                  boolField("option_key_as_alt",
                            "Option key as Alt",
                            "Treat the macOS Option key as Alt modifier.",
                            [](auto& p) -> auto& { return p.optionKeyAsAlt.value(); }),
                  { "wm_class",
                    "WM class",
                    "The X11 WM_CLASS property string.",
                    "string",
                    [](TerminalProfile const& p) {
                        return QVariant(QString::fromStdString(p.wmClass.value()));
                    },
                    [](TerminalProfile& p, QVariant const& v) { p.wmClass = v.toString().toStdString(); } },
                  { "tab_label",
                    "Tab label",
                    "Template for the tab title (e.g. {WindowTitle}).",
                    "string",
                    [](TerminalProfile const& p) {
                        return QVariant(QString::fromStdString(p.tabLabel.value()));
                    },
                    [](TerminalProfile& p, QVariant const& v) { p.tabLabel = v.toString().toStdString(); } },
              } },
            { "History & scrollbar",
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
                  boolField(
                      "hide_scrollbar_in_alt_screen",
                      "Hide scrollbar in alt screen",
                      "Automatically hide the scrollbar when the application enters alternate screen mode.",
                      [](auto& p) -> auto& { return p.scrollbar.value().hideScrollbarInAltScreen; }),
              } },
            { "Command blocks",
              "❯",
              {
                  scrollbarMarkField(
                      "scrollbar_marks_failures",
                      "Scrollbar marks: failed commands",
                      "Mark every command that failed on the scrollbar. Click a mark to jump to it.",
                      vtbackend::ScrollbarMarkSource::Failures),
                  scrollbarMarkField("scrollbar_marks_commands",
                                     "Scrollbar marks: other commands",
                                     "Mark every command that did not fail on the scrollbar, finished or "
                                     "still running. A failed command is marked by the row above alone.",
                                     vtbackend::ScrollbarMarkSource::Commands),
                  scrollbarMarkField("scrollbar_marks_user_marks",
                                     "Scrollbar marks: user marks",
                                     "Mark the lines you marked with Vi mode's mm on the scrollbar.",
                                     vtbackend::ScrollbarMarkSource::UserMarks),
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
                        return QVariant(
                            static_cast<int>(p.notifyOnCommandFinish.value().minDuration.count()));
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
            { "Mouse & permissions",
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
            { "Visual effects",
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
            { "Cursor",
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
            { "Bell",
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
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<double>(p.bell.value().volume));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto b = p.bell.value();
                        b.volume = static_cast<float>(v.toDouble());
                        p.bell = b;
                    } },
              } },
            { "Background",
              "▨",
              {
                  { "background_opacity",
                    "Background opacity",
                    "Terminal background opacity (0.0 = fully transparent, 1.0 = fully opaque).",
                    "double",
                    [](TerminalProfile const& p) {
                        return QVariant(
                            static_cast<double>(static_cast<uint8_t>(p.background.value().opacity)) / 255.0);
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto bg = p.background.value();
                        bg.opacity = vtbackend::Opacity(
                            static_cast<unsigned>(255 * std::clamp(v.toDouble(), 0.0, 1.0)));
                        p.background = bg;
                    } },
                  boolField(
                      "background_blur",
                      "Background blur",
                      "Whether to blur content behind the transparent terminal window (platform dependent).",
                      [](auto& p) -> auto& { return p.background.value().blur; }),
              } },
            { "Links",
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
                      .help =
                          "A command line for detected file paths, with {path}, {line} and {column}; empty "
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
            { "Status line",
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

    /// A colour slot a scheme may leave unset, in which case the palette derives it. The editor row
    /// shows the colour in effect and writes an explicit one only when edited.
    struct DerivedColorSlot
    {
        std::string_view key;   //!< QML key.
        std::string_view label; //!< Swatch label.
        /// The scheme's own colour for the slot; unset while the palette derives it.
        std::optional<vtbackend::RGBColor> (*own)(vtbackend::ColorPalette const&);
        /// Sets the scheme's own colour, or unsets it so that the palette derives it again.
        void (*assign)(vtbackend::ColorPalette&, std::optional<vtbackend::RGBColor>);
        /// The colour in effect: the scheme's own, or the derived one while the slot is unset.
        vtbackend::RGBColor (*resolve)(vtbackend::ColorPalette const&);
    };

    /// The slot of the block column's colour for @p Outcome in one state: the palette's array @p Colors,
    /// resolved by @p Resolve.
    /// @param key   QML key.
    /// @param label Swatch label.
    /// @return The slot.
    template <auto Colors, auto Resolve, vtbackend::CommandBlockOutcome Outcome>
    constexpr DerivedColorSlot outcomeSlot(std::string_view key, std::string_view label)
    {
        constexpr auto Index = static_cast<size_t>(Outcome);
        return { .key = key,
                 .label = label,
                 .own = [](vtbackend::ColorPalette const& p) { return (p.*Colors).at(Index); },
                 .assign = [](vtbackend::ColorPalette& p,
                              std::optional<vtbackend::RGBColor> color) { (p.*Colors).at(Index) = color; },
                 .resolve = [](vtbackend::ColorPalette const& p) { return (p.*Resolve)(Outcome); } };
    }

    /// The slot of the fold column's glyph colour in one state: the foreground of the palette's pair @p Pair,
    /// resolved by @p Resolve. Set, the pair keeps the background it had, or sits on the scheme's own -- the
    /// gutter draws no band of its own (@see the background row, which carries such a pair along).
    /// @param key   QML key.
    /// @param label Swatch label.
    /// @return The slot.
    template <auto Pair, auto Resolve>
    constexpr DerivedColorSlot foldMarkerSlot(std::string_view key, std::string_view label)
    {
        return { .key = key,
                 .label = label,
                 .own = [](vtbackend::ColorPalette const& p) -> std::optional<vtbackend::RGBColor> {
                     if (auto const& pair = p.*Pair)
                         return pair->foreground;
                     return std::nullopt;
                 },
                 .assign =
                     [](vtbackend::ColorPalette& p, std::optional<vtbackend::RGBColor> color) {
                         auto& pair = p.*Pair;
                         if (!color)
                         {
                             pair.reset();
                             return;
                         }
                         auto const background = pair ? pair->background : p.defaultBackground;
                         pair = vtbackend::RGBColorPair { .foreground = *color, .background = background };
                     },
                 .resolve = [](vtbackend::ColorPalette const& p) { return (p.*Resolve)().foreground; } };
    }

    /// The slot of the block column's resting colour for @p Outcome (ColorPalette::blockStatusColor).
    template <vtbackend::CommandBlockOutcome Outcome>
    constexpr DerivedColorSlot blockStatusSlot(std::string_view key, std::string_view label)
    {
        return outcomeSlot<&vtbackend::ColorPalette::blockStatus,
                           &vtbackend::ColorPalette::blockStatusColor,
                           Outcome>(key, label);
    }

    /// The slot of the block column's colour for @p Outcome under the pointer
    /// (ColorPalette::blockStatusHoverColor).
    template <vtbackend::CommandBlockOutcome Outcome>
    constexpr DerivedColorSlot blockStatusHoverSlot(std::string_view key, std::string_view label)
    {
        return outcomeSlot<&vtbackend::ColorPalette::blockStatusHover,
                           &vtbackend::ColorPalette::blockStatusHoverColor,
                           Outcome>(key, label);
    }

    /// The slot of a colour the palette holds as a member of its own, @p Own, resolved by @p Resolve.
    /// @param key   QML key.
    /// @param label Swatch label.
    /// @return The slot.
    template <auto Own, auto Resolve>
    constexpr DerivedColorSlot memberSlot(std::string_view key, std::string_view label)
    {
        return { .key = key,
                 .label = label,
                 .own = [](vtbackend::ColorPalette const& p) { return p.*Own; },
                 .assign = [](vtbackend::ColorPalette& p,
                              std::optional<vtbackend::RGBColor> color) { p.*Own = color; },
                 .resolve = [](vtbackend::ColorPalette const& p) { return (p.*Resolve)(); } };
    }

    /// The command-block colour slots, in the order the editor lists them after the ANSI banks. Adding
    /// a derived slot to the scheme editor is adding a row here.
    constexpr auto DerivedColorSlots = std::array {
        foldMarkerSlot<&vtbackend::ColorPalette::foldMarker, &vtbackend::ColorPalette::foldMarkerColors>(
            "fold_marker", "Fold column"),
        foldMarkerSlot<&vtbackend::ColorPalette::foldMarkerHover,
                       &vtbackend::ColorPalette::foldMarkerHoverColors>("fold_marker_hover",
                                                                        "Fold column: hovered"),
        blockStatusSlot<vtbackend::CommandBlockOutcome::Success>("block_status_success",
                                                                 "Block status: success"),
        blockStatusSlot<vtbackend::CommandBlockOutcome::Failure>("block_status_failure",
                                                                 "Block status: failure"),
        blockStatusSlot<vtbackend::CommandBlockOutcome::Running>("block_status_running",
                                                                 "Block status: running"),
        blockStatusHoverSlot<vtbackend::CommandBlockOutcome::Success>("block_status_hover_success",
                                                                      "Block status hovered: success"),
        blockStatusHoverSlot<vtbackend::CommandBlockOutcome::Failure>("block_status_hover_failure",
                                                                      "Block status hovered: failure"),
        blockStatusHoverSlot<vtbackend::CommandBlockOutcome::Running>("block_status_hover_running",
                                                                      "Block status hovered: running"),
        memberSlot<&vtbackend::ColorPalette::gutterText, &vtbackend::ColorPalette::gutterTextColor>(
            "gutter_text", "Gutter text"),
        memberSlot<&vtbackend::ColorPalette::stickyHeaderBackground,
                   &vtbackend::ColorPalette::stickyHeaderBackgroundColor>("sticky_header_background",
                                                                          "Sticky header: background"),
        memberSlot<&vtbackend::ColorPalette::stickyHeaderSeparator,
                   &vtbackend::ColorPalette::stickyHeaderSeparatorColor>("sticky_header_separator",
                                                                         "Sticky header: separator"),
    };

    /// Where the colour a scheme editor row shows comes from.
    enum class SchemeColorSource : uint8_t
    {
        Scheme,  ///< A slot every scheme sets, such as the background.
        Derived, ///< A slot the scheme leaves unset, so the palette derives it from other colours.
        Pinned,  ///< A slot the palette could derive, set by the scheme to a colour of its own.
    };

    /// The QML-facing token of each SchemeColorSource, indexed by it.
    constexpr auto SchemeColorSourceTokens = std::array { "scheme"sv, "derived"sv, "pinned"sv };

    /// A data-driven descriptor for one editable color slot of a palette.
    struct SchemeColorDescriptor
    {
        QString key;
        QString label;
        std::function<vtbackend::RGBColor(vtbackend::ColorPalette const&)> get; //!< The colour in effect.
        std::function<void(vtbackend::ColorPalette&, vtbackend::RGBColor)>
            set; //!< Makes it the scheme's own.
        /// How the palette derives the slot while the scheme leaves it unset; none for one every scheme sets.
        DerivedColorSlot const* derived = nullptr;

        /// Where the colour in effect comes from in @p palette.
        [[nodiscard]] SchemeColorSource source(vtbackend::ColorPalette const& palette) const
        {
            if (derived == nullptr)
                return SchemeColorSource::Scheme;
            return derived->own(palette) ? SchemeColorSource::Pinned : SchemeColorSource::Derived;
        }
    };

    /// The color slots the scheme editor exposes: the default fg/bg, the 8 normal and 8 bright ANSI
    /// colors (generated from the ANSI names so the two banks are not hand-listed), then the derived
    /// command-block slots of DerivedColorSlots.
    std::vector<SchemeColorDescriptor> const& schemeColorDescriptors()
    {
        static auto const descriptors = [] {
            auto list = std::vector<SchemeColorDescriptor> {};
            list.push_back({ .key = "background",
                             .label = "Background",
                             .get = [](auto const& p) { return p.defaultBackground; },
                             .set =
                                 [](vtbackend::ColorPalette& p, vtbackend::RGBColor c) {
                                     // A fold column the editor set sits on the page's background: it moves
                                     // with it, rather than leaving a band of the old one down the gutter.
                                     for (auto* const pair: { &p.foldMarker, &p.foldMarkerHover })
                                         if (*pair && (*pair)->background == p.defaultBackground)
                                             (*pair)->background = c;
                                     p.defaultBackground = c;
                                 } });
            list.push_back({ .key = "foreground",
                             .label = "Foreground",
                             .get = [](auto const& p) { return p.defaultForeground; },
                             .set = [](auto& p, auto c) { p.defaultForeground = c; } });
            static constexpr auto AnsiNames =
                std::array<std::string_view, 8> { "black", "red",     "green", "yellow",
                                                  "blue",  "magenta", "cyan",  "white" };
            for (auto const bright: { false, true })
                for (auto const i: std::views::iota(size_t { 0 }, AnsiNames.size()))
                {
                    auto const slot = bright ? i + 8 : i;
                    list.push_back({ .key = QString::fromStdString((bright ? "bright_" : "normal_")
                                                                   + std::string(AnsiNames[i])),
                                     .label = QString::fromStdString((bright ? "Bright " : "Normal ")
                                                                     + std::string(AnsiNames[i])),
                                     .get = [slot](auto const& p) { return p.palette.at(slot); },
                                     .set = [slot](auto& p, auto c) { p.palette.at(slot) = c; } });
                }
            for (auto const& derived: DerivedColorSlots)
                list.push_back({ .key = toQString(derived.key),
                                 .label = toQString(derived.label),
                                 .get = derived.resolve,
                                 .set = [assign = derived.assign](auto& p, auto c) { assign(p, c); },
                                 .derived = &derived });
            return list;
        }();
        return descriptors;
    }

    /// The scheme editor's slot named @p key, or nullptr when there is none.
    [[nodiscard]] SchemeColorDescriptor const* findSchemeColor(QString const& key)
    {
        auto const& descriptors = schemeColorDescriptors();
        auto const it = std::ranges::find(descriptors, key, &SchemeColorDescriptor::key);
        return it != descriptors.end() ? &*it : nullptr;
    }

    /// One combo-box entry: what is stored, and what is shown.
    struct EnumOption
    {
        QString value;
        QString label;
    };

    /// What a runtime options provider may consult.
    struct OptionContext
    {
        config::Config const& config;
        std::span<display::GpuCandidate const> gpus;
    };

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

    /// Checks an edited global value before it is written, and says why when it refuses it.
    using GlobalFieldValidator = std::function<std::expected<void, std::string>(QVariant const&)>;

    /// A data-driven descriptor for one editable global (application-scope) setting. `toYaml` turns the
    /// edited value into the YAML scalar written to settings.yml; the load-time merge re-applies it
    /// through the typed per-key loader, so this side needs no parsing.
    struct GlobalFieldDescriptor
    {
        QString key; //!< contour.yml key path: "reflow_on_resize", or "gutter.line_numbers" for a leaf.
        QString label;
        QString help;
        QString type;
        std::function<QVariant(config::Config const&)> get;
        std::function<std::string(QVariant const&)> toYaml;
        /// Reads an override back from settings.yml as the loader reads it, as the row's value: an invalid
        /// QVariant where the loader would refuse it. A hand-edited override may be spelled otherwise than
        /// toYaml spells it -- `True`, `0x10`, `Dark` -- and still take.
        std::function<QVariant(std::string const&)> fromYaml;
        QStringList options {}; //!< For "enum": the allowed values (the combo model + accepted set).
        /// For "enum" rows whose options are only known at runtime; overrides `options` when set.
        std::function<std::vector<EnumOption>(OptionContext const&)> optionsProvider {};
        GlobalFieldValidator validate {}; //!< Refuses a value the loader would reject; empty accepts all.
    };

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

    /// The editable global settings. A key is a contour.yml key path -- top-level ("reflow_on_resize")
    /// or one level into a section ("renderer.gpu"), which settings.yml stores nested -- and needs a row
    /// of config::globalOverrides() that applies it (for a dotted key, its section's), or setGlobalField
    /// refuses to save it. A leaf of a section that has a row needs nothing more.
    std::vector<GlobalFieldDescriptor> const& globalFieldDescriptors()
    {
        static auto const descriptors = std::vector<GlobalFieldDescriptor> {
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
            // Reuse the std::formatter<GuiTheme> (the "system"/"dark"/"light" source of truth) rather than
            // re-switching the enum here, matching the std::format idiom used for modifiers/keys.
            globalEnumField("theme",
                            "GUI theme",
                            "Light/dark appearance of the GUI chrome (menus, tabs, dialogs). The terminal "
                            "keeps following the OS.",
                            [](config::Config const& c) { return std::format("{}", c.theme.value()); },
                            { "system", "dark", "light" }),
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
        return descriptors;
    }

    /// "#rrggbb" for @p color, the format the QML color pickers exchange.
    QString rgbToHex(vtbackend::RGBColor color)
    {
        return QString::asprintf("#%02x%02x%02x", color.red, color.green, color.blue);
    }

    /// Parses "#rrggbb" (or any Qt-recognized color string) into an RGBColor, black on a parse failure.
    vtbackend::RGBColor hexToRgb(QString const& hex)
    {
        auto const color = QColor(hex);
        return vtbackend::RGBColor { static_cast<uint8_t>(color.red()),
                                     static_cast<uint8_t>(color.green()),
                                     static_cast<uint8_t>(color.blue()) };
    }

    /// Formats a modifier chord as "Ctrl+Shift" (the individual Modifier has a formatter, the set does
    /// not), mirroring the config writer's bit walk.
    QString formatModifiers(vtbackend::Modifiers modifiers)
    {
        auto parts = QStringList {};
        for (auto bit = 0u; bit < sizeof(vtbackend::Modifier) * 8; ++bit)
        {
            auto const flag = static_cast<vtbackend::Modifier>(1u << bit);
            if (!modifiers.test(flag))
                continue;
            auto const name = std::format("{}", flag);
            if (!name.empty())
                parts << QString::fromStdString(name);
        }
        return parts.join('+');
    }

    /// Where a row of the bindings viewer comes from.
    enum class BindingOrigin : uint8_t
    {
        Configured, ///< The configuration's `input_mapping:`.
        BuiltIn,    ///< A built-in fallback the configuration leaves free.
    };

    /// How the bindings viewer names a key.
    QString inputLabel(vtbackend::Key key)
    {
        return QString::fromStdString(std::format("{}", key));
    }

    /// How the bindings viewer names a mouse button.
    QString inputLabel(vtbackend::MouseButton button)
    {
        return QString::fromStdString(std::format("{}", button));
    }

    /// How the bindings viewer names a character: the whole codepoint.
    QString inputLabel(char32_t codepoint)
    {
        return QString::fromUcs4(&codepoint, 1);
    }

    /// Builds a read-only display row { trigger, action, mode, builtIn } for one input binding, or an empty
    /// map when the binding carries no action. `action` names every action the binding runs, in order.
    template <typename Input>
    QVariantMap keybindingRow(vtbackend::InputBinding<Input, config::ActionList> const& binding,
                              BindingOrigin origin)
    {
        if (binding.binding.empty())
            return {};
        auto const mods = formatModifiers(binding.modifiers);
        auto row = QVariantMap {};
        // The count is part of the trigger: Ctrl+Left x3 is not Ctrl+Left.
        auto const input = inputLabel(binding.input)
                           + QString::fromStdString(vtbackend::clickCountSuffix(binding.clickCount));
        row[QStringLiteral("trigger")] = mods.isEmpty() ? input : mods + '+' + input;
        auto actions = QStringList {};
        for (auto const& action: binding.binding)
            actions.push_back(QString::fromStdString(std::format("{}", action)));
        row[QStringLiteral("action")] = actions.join(QStringLiteral(", "));
        row[QStringLiteral("mode")] =
            binding.modes.any() ? QString::fromStdString(std::format("{}", binding.modes)) : QString {};
        row[QStringLiteral("builtIn")] = origin == BindingOrigin::BuiltIn;
        return row;
    }

    /// Every state the match modes can be in: each combination of the MatchModes flags, Trace the highest.
    constexpr auto MatchModeStates =
        std::views::iota(0u, static_cast<unsigned>(vtbackend::MatchModes::Trace) << 1);

    /// Whether a press can still reach @p fallback past @p configured, the configuration's bindings, which
    /// are looked up first: whether in some state of the match modes the fallback answers and no configured
    /// binding of the same chord and click count does. Decided on the states, not row by row, so two rows
    /// that cover the fallback's modes only together -- one on the alternate screen, one off it -- shadow it.
    template <typename Input>
    [[nodiscard]] bool reachable(
        vtbackend::InputBinding<Input, config::ActionList> const& fallback,
        std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& configured)
    {
        return std::ranges::any_of(MatchModeStates, [&](unsigned state) {
            auto const flags = static_cast<uint8_t>(state);
            return config::helper::testMatchMode(flags, fallback.modes)
                   && std::ranges::none_of(configured, [&](auto const& binding) {
                          return binding.modifiers == fallback.modifiers && binding.input == fallback.input
                                 && binding.clickCount == fallback.clickCount
                                 && config::helper::testMatchMode(flags, binding.modes);
                      });
        });
    }

    /// Appends the bindings viewer's row for each of @p bindings, the configuration's own.
    template <typename Input>
    void appendConfiguredBindings(
        QVariantList& rows, std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& bindings)
    {
        for (auto const& binding: bindings)
            if (auto row = keybindingRow(binding, BindingOrigin::Configured); !row.isEmpty())
                rows.push_back(std::move(row));
    }

    /// Appends the bindings viewer's row for each built-in fallback of @p fallbacks that @p config switches
    /// on and a press can reach past its @p configured bindings.
    template <typename Input>
    void appendBuiltInBindings(
        QVariantList& rows,
        std::vector<config::FallbackMapping<Input>> const& fallbacks,
        std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& configured,
        config::Config const& config)
    {
        for (auto const& fallback: fallbacks)
            if (fallback.enabled(config) && reachable(fallback.mapping, configured))
                if (auto row = keybindingRow(fallback.mapping, BindingOrigin::BuiltIn); !row.isEmpty())
                    rows.push_back(std::move(row));
    }

    /// The QML-facing token for a provenance value.
    QString originString(SettingsOrigin origin)
    {
        switch (origin)
        {
            case SettingsOrigin::Builtin: return QStringLiteral("builtin");
            case SettingsOrigin::MainConfig: return QStringLiteral("main");
            case SettingsOrigin::SideFile: return QStringLiteral("side");
        }
        return QStringLiteral("builtin");
    }
} // namespace

SettingsController::SettingsController(ConfigAccessor config,
                                       std::shared_ptr<config::GuiConfigStore> store,
                                       ApplyCallback apply,
                                       std::shared_ptr<display::IGpuInventory const> gpuInventory,
                                       QObject* parent):
    QObject { parent },
    _config { std::move(config) },
    _store { std::move(store) },
    _apply { std::move(apply) },
    _gpuInventory { std::move(gpuInventory) }
{
    refresh();
}

SettingsOrigin SettingsController::profileOrigin(std::string const& name) const
{
    auto const& origins = _config().profileOrigins;
    auto const it = origins.find(name);
    return it != origins.end() ? it->second : SettingsOrigin::Builtin;
}

bool SettingsController::colorSchemeSideFileExists(std::string const& name) const
{
    auto ec = std::error_code {};
    auto const path = _config().configFile.parent_path() / "colorschemes" / config::sideFileName(name);
    return std::filesystem::exists(path, ec) && !ec;
}

bool SettingsController::isInlineColorScheme(std::string const& name) const
{
    // "Inline" means declared in contour.yml (origin MainConfig) with no GUI side file backing it. A side
    // file always wins editability regardless of a stale MainConfig marking (a side-file scheme referenced
    // by a profile is loaded into the config map and recorded MainConfig), so exclude it explicitly.
    auto const& origins = _config().colorSchemeOrigins;
    auto const it = origins.find(name);
    return it != origins.end() && it->second == SettingsOrigin::MainConfig
           && !colorSchemeSideFileExists(name);
}

bool SettingsController::fail(std::string const& error)
{
    emit errorOccurred(QString::fromStdString(error));
    return false;
}

std::optional<std::string> SettingsController::draftProblem() const
{
    // A field whose value has a problem (an invalid file_opener) is saved as written: the reader keeps it and
    // reports it, and the terminal disables it, so a refusal would guard nothing. Its row shows the problem.
    //
    // A pattern the reader would skip, or one of the profile's own the terminal would disable: each row shows
    // its own. A built-in's broken override does not refuse the save: the page cannot change its regex, and
    // the reader keeps the entry and reports it at every load, so a refusal would guard nothing and block
    // every unrelated edit.
    for (auto const& pattern: _draft.hintPatterns.value())
        if (auto const reason = config::skippedHintPatternReason(pattern))
            return std::format("Link patterns: {}.", *reason);
    for (auto const& diagnostic: config::linkPatternsFor(_draft)->diagnostics())
        if (diagnostic.severity() == vtbackend::LinkPatternSeverity::Error
            && !vtbackend::isBuiltinLinkPatternName(diagnostic.pattern))
            return diagnostic.message();
    return std::nullopt;
}

void SettingsController::refresh()
{
    auto const& cfg = _config();
    _gpus = _gpuInventory ? _gpuInventory->list() : std::vector<display::GpuCandidate> {};
    _locked = cfg.guiConfigLocked.value();
    _linkGesture =
        QString::fromStdString(command::describeLinkGesture(cfg, command::platformShortcutStyle()));
    _defaultProfile = QString::fromStdString(cfg.defaultProfileName.value());

    // Profiles, name-sorted for a stable list.
    auto profileNames = std::vector<std::string> {};
    for (auto const& [name, _]: cfg.profiles.value())
        profileNames.push_back(name);
    std::ranges::sort(profileNames);

    _profiles.clear();
    for (auto const& name: profileNames)
    {
        auto const origin = profileOrigin(name);
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(name);
        row[QStringLiteral("origin")] = originString(origin);
        row[QStringLiteral("editable")] = (!_locked && origin == SettingsOrigin::SideFile);
        row[QStringLiteral("isDefault")] = (name == cfg.defaultProfileName.value());
        _profiles.push_back(row);
    }

    // Color schemes: those the config knows about, unioned with any colorschemes/*.yml on disk.
    auto schemeNames = std::set<std::string> {};
    for (auto const& [name, _]: cfg.colorschemes.value())
        schemeNames.insert(name);
    auto const schemesDir = cfg.configFile.parent_path() / "colorschemes";
    auto onDisk = std::set<std::string> {};
    auto ec = std::error_code {};
    if (std::filesystem::is_directory(schemesDir, ec))
        for (auto const& entry: std::filesystem::directory_iterator(schemesDir, ec))
            if (entry.is_regular_file() && entry.path().extension() == ".yml")
            {
                // Its name, read back as sideFileName() spelled it.
                auto name = core::pathToUtf8(entry.path().stem());
                schemeNames.insert(name);
                onDisk.insert(std::move(name));
            }

    _colorSchemes.clear();
    for (auto const& name: schemeNames)
    {
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(name);
        row[QStringLiteral("editable")] = (!_locked && onDisk.contains(name));
        _colorSchemes.push_back(row);
    }

    // Keybindings (read-only): the configured key, character and mouse mappings, then the built-in ones a
    // press can still reach, so a binding Contour adds later shows up here without a configuration edit.
    _keybindings.clear();
    auto const& mappings = cfg.inputMappings.value();
    appendConfiguredBindings(_keybindings, mappings.keyMappings);
    appendConfiguredBindings(_keybindings, mappings.charMappings);
    appendConfiguredBindings(_keybindings, mappings.mouseMappings);
    appendBuiltInBindings(_keybindings, config::builtinFallbackKeyMappings(), mappings.keyMappings, cfg);
    appendBuiltInBindings(_keybindings, config::builtinFallbackCharMappings(), mappings.charMappings, cfg);
    appendBuiltInBindings(_keybindings, config::builtinFallbackMouseMappings(), mappings.mouseMappings, cfg);

    emit changed();
}

// {{{ Profile draft

QVariantList SettingsController::profileFields() const
{
    auto fields = QVariantList {};
    if (!_hasDraft)
        return fields;
    // Flat, with each row carrying its group: the page groups the rows itself, and a flat list keeps a
    // key lookup (and every existing caller) working without a second traversal shape.
    for (auto const& group: profileFieldGroups())
        for (auto const& descriptor: group.fields)
        {
            auto row = QVariantMap {};
            row[QStringLiteral("key")] = descriptor.key;
            row[QStringLiteral("label")] = descriptor.label;
            row[QStringLiteral("help")] = descriptor.help;
            row[QStringLiteral("type")] = descriptor.type;
            row[QStringLiteral("value")] = descriptor.get(_draft);
            row[QStringLiteral("problem")] =
                descriptor.validate ? descriptor.validate(row[QStringLiteral("value")]) : QString {};
            row[QStringLiteral("options")] = descriptor.options;
            row[QStringLiteral("group")] = group.title;
            row[QStringLiteral("groupGlyph")] = group.glyph;
            fields.push_back(row);
        }
    return fields;
}

void SettingsController::editProfile(QString const& name)
{
    auto const& cfg = _config();
    auto const* profile = cfg.findProfile(name.toStdString());
    if (profile == nullptr)
        return;
    _draft = *profile;
    _editingProfile = name;
    _hasDraft = true;
    _editingReadOnly = _locked || profileOrigin(name.toStdString()) != SettingsOrigin::SideFile;
    emit draftChanged();
}

void SettingsController::newProfile(QString const& basedOn)
{
    auto const& cfg = _config();
    auto const* base = cfg.findProfile(basedOn.toStdString());
    if (base == nullptr)
        base = cfg.findProfile(cfg.defaultProfileName.value());
    _draft = base != nullptr ? *base : TerminalProfile {};
    _editingProfile.clear(); // unsaved: needs Save As
    _hasDraft = true;
    _editingReadOnly = _locked;
    emit draftChanged();
}

void SettingsController::setProfileField(QString const& key, QVariant const& value)
{
    if (!_hasDraft)
        return;
    for (auto const& group: profileFieldGroups())
        for (auto const& descriptor: group.fields)
            if (descriptor.key == key)
            {
                descriptor.set(_draft, value);
                emit draftChanged();
                return;
            }
}

QString SettingsController::colorSchemeMode() const
{
    if (_hasDraft && std::holds_alternative<config::DualColorConfig>(_draft.colors.value()))
        return QStringLiteral("dual");
    return QStringLiteral("simple");
}

QString SettingsController::colorScheme() const
{
    if (_hasDraft)
        if (auto const* simple = std::get_if<config::SimpleColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(simple->colorScheme);
    return {};
}

QString SettingsController::colorSchemeLight() const
{
    if (_hasDraft)
        if (auto const* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(dual->colorSchemeLight);
    return {};
}

QString SettingsController::colorSchemeDark() const
{
    if (_hasDraft)
        if (auto const* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(dual->colorSchemeDark);
    return {};
}

void SettingsController::setColorSchemeMode(QString const& mode)
{
    if (!_hasDraft)
        return;
    auto& colors = _draft.colors.value();
    if (mode == QStringLiteral("dual") && std::holds_alternative<config::SimpleColorConfig>(colors))
    {
        auto const name = std::get<config::SimpleColorConfig>(colors).colorScheme;
        colors = config::DualColorConfig { .colorSchemeLight = name, .colorSchemeDark = name };
        emit draftChanged();
    }
    else if (mode == QStringLiteral("simple") && std::holds_alternative<config::DualColorConfig>(colors))
    {
        auto const name = std::get<config::DualColorConfig>(colors).colorSchemeDark;
        colors = config::SimpleColorConfig { .colorScheme = name };
        emit draftChanged();
    }
}

void SettingsController::setColorScheme(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* simple = std::get_if<config::SimpleColorConfig>(&_draft.colors.value()))
    {
        simple->colorScheme = name.toStdString();
        emit draftChanged();
    }
}

void SettingsController::setColorSchemeLight(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
    {
        dual->colorSchemeLight = name.toStdString();
        emit draftChanged();
    }
}

void SettingsController::setColorSchemeDark(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
    {
        dual->colorSchemeDark = name.toStdString();
        emit draftChanged();
    }
}

bool SettingsController::saveProfile()
{
    if (!_hasDraft || _locked)
        return fail("The settings page is read-only.");
    if (_editingProfile.isEmpty())
        return fail("This profile has no name yet — use \"Save As\".");
    if (_editingReadOnly)
        return fail("This profile is defined in contour.yml and cannot be overwritten — use \"Save As\".");
    if (auto const problem = draftProblem())
        return fail(*problem);

    if (auto const result = _store->saveProfile(_editingProfile.toStdString(), _draft); !result)
        return fail(result.error());

    _apply();
    refresh();
    editProfile(_editingProfile); // re-seed from the reloaded config
    return true;
}

bool SettingsController::saveProfileAs(QString const& newName)
{
    if (!_hasDraft || _locked)
        return fail("The settings page is read-only.");
    auto const trimmed = newName.trimmed();
    if (trimmed.isEmpty())
        return fail("Enter a name for the new profile.");
    // Reject ANY existing name, not just contour.yml ones: an existing GUI (side-file) profile of the
    // same name would otherwise be silently overwritten with the current draft. findProfile covers both
    // inline and side-file profiles, matching renameProfile's collision guard.
    if (_config().findProfile(trimmed.toStdString()) != nullptr)
        return fail("A profile with that name already exists; choose another name.");
    if (auto const problem = draftProblem())
        return fail(*problem);

    if (auto const result = _store->saveProfile(trimmed.toStdString(), _draft); !result)
        return fail(result.error());

    _apply();
    refresh();
    editProfile(trimmed);
    return true;
}

bool SettingsController::deleteProfile(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");
    if (profileOrigin(name.toStdString()) != SettingsOrigin::SideFile)
        return fail("Only GUI-created profiles can be deleted here.");

    if (auto const result = _store->deleteProfile(name.toStdString()); !result)
        return fail(result.error());

    if (_editingProfile == name)
    {
        _hasDraft = false;
        _editingProfile.clear();
        emit draftChanged();
    }
    _apply();
    refresh();
    return true;
}

// }}}

bool SettingsController::renameProfile(QString const& oldName, QString const& newName)
{
    if (_locked)
        return fail("The settings page is read-only.");
    if (profileOrigin(oldName.toStdString()) != SettingsOrigin::SideFile)
        return fail("Only GUI-created profiles can be renamed here.");
    auto const trimmed = newName.trimmed();
    if (trimmed.isEmpty())
        return fail("Enter a name for the profile.");
    if (trimmed == oldName)
        return true; // nothing to do
    if (_config().findProfile(trimmed.toStdString()) != nullptr)
        return fail("A profile with that name already exists.");

    // Copy the profile out: findProfile's pointer is invalidated by _apply()'s in-place config reload.
    auto const* profile = _config().findProfile(oldName.toStdString());
    if (profile == nullptr)
        return fail("Profile not found.");
    auto const profileCopy = *profile;
    bool const wasDefault = (_defaultProfile == oldName);

    if (auto const result = _store->saveProfile(trimmed.toStdString(), profileCopy); !result)
        return fail(result.error());
    if (auto const result = _store->deleteProfile(oldName.toStdString()); !result)
        return fail(result.error());
    if (wasDefault)
    {
        auto settings = _config().guiManagedSettings;
        settings.defaultProfile = trimmed.toStdString();
        if (auto const result = _store->saveGuiSettings(settings); !result)
            return fail(result.error());
    }

    _apply();
    refresh();
    if (_editingProfile == oldName)
        editProfile(trimmed);
    return true;
}

bool SettingsController::setDefaultProfile(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");

    auto settings = _config().guiManagedSettings;
    settings.defaultProfile = name.toStdString();
    if (auto const result = _store->saveGuiSettings(settings); !result)
        return fail(result.error());

    _apply();
    refresh();
    return true;
}

QVariantList SettingsController::globalFields() const
{
    auto const& cfg = _config();
    auto const& overrides = cfg.guiManagedSettings.globalOverrides;
    auto fields = QVariantList {};
    for (auto const& descriptor: globalFieldDescriptors())
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("help")] = descriptor.help;
        row[QStringLiteral("type")] = descriptor.type;
        row[QStringLiteral("value")] = descriptor.get(cfg);
        if (descriptor.optionsProvider)
        {
            auto values = QStringList {};
            auto labels = QStringList {};
            for (auto const& option:
                 descriptor.optionsProvider(OptionContext { .config = cfg, .gpus = _gpus }))
            {
                values.push_back(option.value);
                labels.push_back(option.label);
            }
            row[QStringLiteral("options")] = values;
            row[QStringLiteral("optionLabels")] = labels;
        }
        else
        {
            row[QStringLiteral("options")] = descriptor.options;
            row[QStringLiteral("optionLabels")] = descriptor.options;
        }
        // Overridden is what took: an override the loader refused -- a bool spelled "maybe" -- leaves the
        // value contour.yml gives, and a Reset beside it would offer to undo a change that never happened.
        // One that took is the value in effect once read back as the loader reads it.
        auto const stored = overrides.find(descriptor.key.toStdString());
        row[QStringLiteral("overridden")] =
            stored != overrides.end() && descriptor.fromYaml(stored->second) == row[QStringLiteral("value")];
        fields.push_back(row);
    }
    return fields;
}

bool SettingsController::setGlobalField(QString const& key, QVariant const& value)
{
    if (_locked)
        return fail("The settings page is read-only.");
    for (auto const& descriptor: globalFieldDescriptors())
        if (descriptor.key == key)
        {
            // A refused value emits no changed(): the model is as it was, and the editor puts the stored
            // value back itself (SettingRow.qml's commitText).
            if (auto const valid =
                    descriptor.validate ? descriptor.validate(value) : std::expected<void, std::string> {};
                !valid)
                return fail(valid.error());
            // An override the merge of settings.yml does not apply would save and then do nothing.
            if (config::globalOverrideFor(key.toStdString()) == nullptr)
                return fail("This setting cannot be saved: settings.yml does not apply it.");
            auto settings = _config().guiManagedSettings;
            settings.globalOverrides[key.toStdString()] = descriptor.toYaml(value);
            if (auto const result = _store->saveGuiSettings(settings); !result)
                return fail(result.error());
            _apply();
            refresh();
            return true;
        }
    return fail("Unknown global setting.");
}

bool SettingsController::resetGlobalField(QString const& key)
{
    if (_locked)
        return fail("The settings page is read-only.");
    auto settings = _config().guiManagedSettings;
    settings.globalOverrides.erase(key.toStdString());
    if (auto const result = _store->saveGuiSettings(settings); !result)
        return fail(result.error());
    _apply();
    refresh();
    return true;
}

// {{{ Color-scheme draft

QVariantList SettingsController::schemeColors() const
{
    auto colors = QVariantList {};
    if (!_hasSchemeDraft)
        return colors;
    for (auto const& descriptor: schemeColorDescriptors())
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("color")] = rgbToHex(descriptor.get(_schemeDraft));
        row[QStringLiteral("colorSource")] =
            toQString(SchemeColorSourceTokens.at(static_cast<size_t>(descriptor.source(_schemeDraft))));
        colors.push_back(row);
    }
    return colors;
}

void SettingsController::editColorScheme(QString const& name)
{
    auto const& cfg = _config();
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = cfg.colorschemes.value().find(name.toStdString());
        it != cfg.colorschemes.value().end())
        palette = it->second;
    else if (auto const loaded = config::loadColorSchemeFile(cfg.configFile.parent_path() / "colorschemes"
                                                             / config::sideFileName(name.toStdString())))
        palette = *loaded;

    _schemeDraft = palette;
    _editingScheme = name;
    _hasSchemeDraft = true;
    emit schemeDraftChanged();
    emit editingSchemeChanged();
}

void SettingsController::newColorScheme(QString const& basedOn)
{
    auto const& cfg = _config();
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = cfg.colorschemes.value().find(basedOn.toStdString());
        it != cfg.colorschemes.value().end())
        palette = it->second;

    _schemeDraft = palette;
    _editingScheme.clear();
    _hasSchemeDraft = true;
    emit schemeDraftChanged();
    emit editingSchemeChanged();
}

void SettingsController::setSchemeColor(QString const& key, QString const& color)
{
    auto const* descriptor = findSchemeColor(key);
    if (!_hasSchemeDraft || descriptor == nullptr)
        return;
    // A colour equal to the one shown is no edit -- unless the slot is derived: committing the colour it
    // shows then pins it, so that the colour stays when what it derives from changes.
    auto const rgb = hexToRgb(color);
    if (descriptor->source(_schemeDraft) != SchemeColorSource::Derived
        && descriptor->get(_schemeDraft) == rgb)
        return;
    descriptor->set(_schemeDraft, rgb);
    emit schemeDraftChanged();
}

void SettingsController::resetSchemeColor(QString const& key)
{
    auto const* descriptor = findSchemeColor(key);
    if (!_hasSchemeDraft || descriptor == nullptr
        || descriptor->source(_schemeDraft) != SchemeColorSource::Pinned)
        return;
    descriptor->derived->assign(_schemeDraft, std::nullopt);
    emit schemeDraftChanged();
}

bool SettingsController::renameColorScheme(QString const& oldName, QString const& newName)
{
    if (_locked)
        return fail("The settings page is read-only.");
    auto const schemesDir = _config().configFile.parent_path() / "colorschemes";
    if (!colorSchemeSideFileExists(oldName.toStdString()))
        return fail("Only GUI-created color schemes can be renamed here.");
    auto const trimmed = newName.trimmed();
    if (trimmed.isEmpty())
        return fail("Enter a name for the color scheme.");
    if (trimmed == oldName)
        return true; // nothing to do
    // Reject a destination naming ANY existing scheme: a side file, a contour.yml inline scheme (which
    // would shadow the written side file at load, leaving a dead file), or a config-known scheme such as
    // the builtin "default". Mirrors renameProfile's guard, and uses the non-throwing exists() overload
    // (via the helper) so an I/O error cannot throw out of a slot.
    if (colorSchemeSideFileExists(trimmed.toStdString()) || isInlineColorScheme(trimmed.toStdString())
        || _config().colorschemes.value().contains(trimmed.toStdString()))
        return fail("A color scheme with that name already exists.");

    // Load the old palette (config-known or from its side file), then write it under the new name.
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = _config().colorschemes.value().find(oldName.toStdString());
        it != _config().colorschemes.value().end())
        palette = it->second;
    else if (auto const loaded =
                 config::loadColorSchemeFile(schemesDir / config::sideFileName(oldName.toStdString())))
        palette = *loaded;

    if (auto const result = _store->saveColorScheme(trimmed.toStdString(), palette); !result)
        return fail(result.error());
    if (auto const result = _store->deleteColorScheme(oldName.toStdString()); !result)
        return fail(result.error());

    _apply();
    refresh();
    if (_editingScheme == oldName)
        editColorScheme(trimmed);
    return true;
}

bool SettingsController::saveColorScheme(QString const& name)
{
    if (!_hasSchemeDraft || _locked)
        return fail("The settings page is read-only.");
    if (name.trimmed().isEmpty())
        return fail("Enter a name for the color scheme.");
    // A contour.yml inline scheme of the same name shadows a side file at load (the inline node wins), so
    // writing one would silently have no effect. Refuse it, mirroring saveProfileAs's contour.yml guard.
    if (isInlineColorScheme(name.trimmed().toStdString()))
        return fail("A color scheme with that name is defined in contour.yml; choose another name.");

    if (auto const result = _store->saveColorScheme(name.toStdString(), _schemeDraft); !result)
        return fail(result.error());

    _apply();
    refresh();
    _editingScheme = name;
    emit schemeDraftChanged();
    emit editingSchemeChanged();
    return true;
}

bool SettingsController::deleteColorScheme(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");
    // Only GUI-created (side-file) schemes can be deleted; a builtin/inline scheme has no side file, and
    // removeFile treats an absent file as success, so without this guard we would report a false delete of
    // a scheme that is actually still present. Mirrors deleteProfile's origin guard.
    if (!colorSchemeSideFileExists(name.toStdString()))
        return fail("Only GUI-created color schemes can be deleted here.");

    if (auto const result = _store->deleteColorScheme(name.toStdString()); !result)
        return fail(result.error());

    if (_editingScheme == name)
    {
        _hasSchemeDraft = false;
        _editingScheme.clear();
        emit schemeDraftChanged();
        emit editingSchemeChanged();
    }
    _apply();
    refresh();
    return true;
}

// }}}

// {{{ Indicator status line visual editor bridge

// The settings page edits the indicator status line as a list of items rather than as a raw template
// string. These four methods are the whole bridge, and all of them drive off
// vtbackend::StatusLineDefinitions::ItemTraits and ::CellFlagNames, so the placeholders the page offers
// and the flags it can toggle are exactly the ones the template parser understands. Both used to be
// re-listed here -- the parse side as seventeen near-identical visitor overloads that exposed four of
// the thirteen flags, the serialize side as a seventeen-branch name chain that rebuilt Tabs with its
// active colours discarded -- so a profile could lose styling merely by being opened and saved.

namespace
{
    /// The QVariantMap key each cell flag is exposed under, i.e. its template spelling.
    [[nodiscard]] QString flagKey(vtbackend::StatusLineDefinitions::CellFlagName const& flag)
    {
        return toQString(flag.name);
    }

    /// Exposes an optional colour as the pair of keys the editor reads: a bool saying whether it is set
    /// at all, and its "#RRGGBB" text. Split in two because an unset colour is not black -- it means
    /// "inherit" -- and one string cannot say both.
    void writeColorPair(QVariantMap& row,
                        QString const& hasKey,
                        QString const& colorKey,
                        std::optional<vtbackend::RGBColor> const& color)
    {
        row[hasKey] = color.has_value();
        row[colorKey] = color ? QString::fromStdString(vtbackend::formatColor(*color)) : QString {};
    }

    /// Reads the styles shared by every item into @p row.
    void readStyles(QVariantMap& row, vtbackend::StatusLineDefinitions::Styles const& styles)
    {
        writeColorPair(row, QStringLiteral("hasColor"), QStringLiteral("color"), styles.foregroundColor);
        writeColorPair(row, QStringLiteral("hasBgColor"), QStringLiteral("bgColor"), styles.backgroundColor);

        auto flags = QVariantMap {};
        for (auto const& flag: vtbackend::StatusLineDefinitions::CellFlagNames)
            flags[flagKey(flag)] = styles.flags.test(flag.flag);
        row[QStringLiteral("flags")] = flags;

        row[QStringLiteral("textLeft")] = QString::fromStdString(styles.textLeft);
        row[QStringLiteral("textRight")] = QString::fromStdString(styles.textRight);
    }

    /// Writes the styles shared by every item back out of @p row.
    [[nodiscard]] vtbackend::StatusLineDefinitions::Styles writeStyles(QVariantMap const& row)
    {
        auto styles = vtbackend::StatusLineDefinitions::Styles {};

        if (row.value(QStringLiteral("hasColor")).toBool())
            styles.foregroundColor = hexToRgb(row.value(QStringLiteral("color")).toString());
        if (row.value(QStringLiteral("hasBgColor")).toBool())
            styles.backgroundColor = hexToRgb(row.value(QStringLiteral("bgColor")).toString());

        auto const flags = row.value(QStringLiteral("flags")).toMap();
        for (auto const& flag: vtbackend::StatusLineDefinitions::CellFlagNames)
            if (flags.value(flagKey(flag)).toBool())
                styles.flags.enable(flag.flag);

        styles.textLeft = row.value(QStringLiteral("textLeft")).toString().toStdString();
        styles.textRight = row.value(QStringLiteral("textRight")).toString().toStdString();
        return styles;
    }

    /// An optional colour from the "has"/value key pair @p hasKey / @p colorKey.
    [[nodiscard]] std::optional<vtbackend::RGBColor> optionalColor(QVariantMap const& row,
                                                                   QString const& hasKey,
                                                                   QString const& colorKey)
    {
        if (!row.value(hasKey).toBool())
            return std::nullopt;
        return hexToRgb(row.value(colorKey).toString());
    }
} // namespace

QVariantList SettingsController::parseIndicatorSegment(QString const& templateStr) const
{
    using namespace vtbackend::StatusLineDefinitions;

    auto items = QVariantList {};
    for (auto const& item: vtbackend::parseStatusLineSegment(templateStr.toStdString()))
    {
        std::visit(
            [&items](auto const& v) {
                using T = std::decay_t<decltype(v)>;

                auto row = QVariantMap {};
                readStyles(row, v);
                row[QStringLiteral("type")] = toQString(ItemTraits<T>::Name);
                row[QStringLiteral("label")] = toQString(ItemTraits<T>::Label);
                row[QStringLiteral("sample")] = toQString(ItemTraits<T>::Sample);

                // The alternatives carrying payload beyond Styles. Everything the editor can change has
                // to appear here, or serializeIndicatorSegment cannot put it back.
                if constexpr (std::same_as<T, Text>)
                {
                    row[QStringLiteral("text")] = QString::fromStdString(v.text);
                    row[QStringLiteral("displayName")] =
                        v.text.empty() ? toQString(ItemTraits<T>::Label) : QString::fromStdString(v.text);
                }
                else if constexpr (std::same_as<T, Command>)
                {
                    row[QStringLiteral("command")] = QString::fromStdString(v.command);
                    // Seconds. No control for it yet, but carried so an edit of the segment keeps it.
                    row[QStringLiteral("interval")] = static_cast<int>(v.interval.count());
                    row[QStringLiteral("displayName")] = v.command.empty()
                                                             ? toQString(ItemTraits<T>::Label)
                                                             : QString::fromStdString(v.command);
                }
                else if constexpr (std::same_as<T, Extension>)
                {
                    row[QStringLiteral("name")] = QString::fromStdString(v.name);
                    row[QStringLiteral("displayName")] =
                        v.name.empty() ? toQString(ItemTraits<T>::Label) : QString::fromStdString(v.name);
                }
                else if constexpr (std::same_as<T, Tabs>)
                {
                    writeColorPair(
                        row, QStringLiteral("hasActiveColor"), QStringLiteral("activeColor"), v.activeColor);
                    writeColorPair(row,
                                   QStringLiteral("hasActiveBackground"),
                                   QStringLiteral("activeBackground"),
                                   v.activeBackground);
                    row[QStringLiteral("separator")] =
                        QString::fromStdString(v.separator.value_or(std::string {}));
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);
                }
                else if constexpr (std::same_as<T, Context>)
                {
                    // Carried through even though the page offers no control for them yet: an item is
                    // round-tripped through here on EVERY edit of the segment, so a payload the reader
                    // drops is a payload the writer silently deletes from the user's profile.
                    row[QStringLiteral("verbosity")] = toQString(contextVerbosityName(v.verbosity));
                    row[QStringLiteral("separator")] =
                        QString::fromStdString(v.separator.value_or(std::string {}));
                    row[QStringLiteral("maxWidth")] = unbox<int>(v.maxWidth);
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);
                }
                else
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);

                items.push_back(row);
            },
            item);
    }
    return items;
}

QString SettingsController::serializeIndicatorSegment(QVariantList const& items) const
{
    using namespace vtbackend::StatusLineDefinitions;

    auto segment = vtbackend::StatusLineSegment {};
    for (auto const& raw: items)
    {
        auto const row = raw.toMap();
        auto const typeName = row.value(QStringLiteral("type")).toString();

        // Matched against ItemTraits rather than a chain of string literals, so the names accepted here
        // are the names parseIndicatorSegment produces. An unknown name is skipped: the page cannot
        // author one, and inventing an item for it would put text into the user's profile.
        forEachItemType([&](auto tag) {
            using T = decltype(tag)::type;
            if (typeName != toQString(ItemTraits<T>::Name))
                return;

            auto const styles = writeStyles(row);
            if constexpr (std::same_as<T, Text>)
                segment.emplace_back(
                    Text { styles, row.value(QStringLiteral("text")).toString().toStdString() });
            else if constexpr (std::same_as<T, Command>)
            {
                auto item = Command { styles, row.value(QStringLiteral("command")).toString().toStdString() };
                // A missing or non-positive interval (e.g. a row QML built without one) keeps the default.
                if (auto const seconds = row.value(QStringLiteral("interval")).toInt(); seconds > 0)
                    item.interval = std::chrono::seconds { seconds };
                segment.emplace_back(std::move(item));
            }
            else if constexpr (std::same_as<T, Extension>)
                segment.emplace_back(
                    Extension { styles, row.value(QStringLiteral("name")).toString().toStdString() });
            else if constexpr (std::same_as<T, Tabs>)
            {
                auto const separator = row.value(QStringLiteral("separator")).toString();
                segment.emplace_back(Tabs {
                    styles,
                    optionalColor(row, QStringLiteral("hasActiveColor"), QStringLiteral("activeColor")),
                    optionalColor(
                        row, QStringLiteral("hasActiveBackground"), QStringLiteral("activeBackground")),
                    // An empty separator is "unset", so Tabs falls back to its built-in "|" rather than
                    // rendering the tabs run together.
                    separator.isEmpty() ? std::nullopt : std::optional { separator.toStdString() },
                });
            }
            else if constexpr (std::same_as<T, Context>)
            {
                auto item = Context { styles };
                item.verbosity =
                    contextVerbosityFrom(row.value(QStringLiteral("verbosity")).toString().toStdString());
                auto const separator = row.value(QStringLiteral("separator")).toString();
                if (!separator.isEmpty())
                    item.separator = separator.toStdString();
                // A missing or nonsensical width keeps the default rather than collapsing the
                // breadcrumb to nothing.
                if (auto const width = row.value(QStringLiteral("maxWidth")).toInt(); width > 0)
                    item.maxWidth = vtbackend::ColumnCount(width);
                segment.emplace_back(std::move(item));
            }
            else
                segment.emplace_back(T { styles });
        });
    }

    return QString::fromStdString(vtbackend::serializeStatusLineSegment(segment));
}

QVariantList SettingsController::indicatorPlaceholders() const
{
    using namespace vtbackend::StatusLineDefinitions;

    auto catalog = QVariantList {};
    forEachItemType([&catalog](auto tag) {
        using T = decltype(tag)::type;
        auto row = QVariantMap {};
        row[QStringLiteral("type")] = toQString(ItemTraits<T>::Name);
        row[QStringLiteral("label")] = toQString(ItemTraits<T>::Label);
        row[QStringLiteral("sample")] = toQString(ItemTraits<T>::Sample);
        catalog.push_back(row);
    });
    return catalog;
}

QVariantList SettingsController::indicatorFlags() const
{
    auto catalog = QVariantList {};
    for (auto const& flag: vtbackend::StatusLineDefinitions::CellFlagNames)
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = toQString(flag.name);
        row[QStringLiteral("label")] = toQString(flag.label);
        catalog.push_back(row);
    }
    return catalog;
}

QString SettingsController::indicatorSegment(int segmentIndex) const
{
    if (!_hasDraft)
        return {};
    auto const& indicator = _draft.statusLine.value().indicator;
    switch (segmentIndex)
    {
        case 0: return QString::fromStdString(indicator.left);
        case 1: return QString::fromStdString(indicator.middle);
        case 2: return QString::fromStdString(indicator.right);
        default: return {};
    }
}

void SettingsController::setIndicatorSegment(int segmentIndex, QString const& value)
{
    if (!_hasDraft || _editingReadOnly)
        return;

    auto statusLine = _draft.statusLine.value();
    switch (segmentIndex)
    {
        case 0: statusLine.indicator.left = value.toStdString(); break;
        case 1: statusLine.indicator.middle = value.toStdString(); break;
        case 2: statusLine.indicator.right = value.toStdString(); break;
        default: return;
    }
    _draft.statusLine = statusLine;
    emit draftChanged();
}

// }}}

// {{{ Link patterns

namespace
{
    using config::HintPatternConfig;

    /// How many rows of SettingsController::linkPatterns the built-ins take, ahead of the profile's own.
    [[nodiscard]] int builtinLinkPatternRows()
    {
        return static_cast<int>(vtbackend::builtinLinkPatternNames().size());
    }

    /// The profile's own patterns among @p entries: those that name no built-in, in their order.
    [[nodiscard]] auto userLinkPatterns(std::vector<HintPatternConfig>& entries)
    {
        return entries | std::views::filter([](HintPatternConfig const& entry) {
                   return !vtbackend::isBuiltinLinkPatternName(entry.name);
               });
    }

    /// The entry of @p entries that user row @p index (counted from the first row after the built-ins'
    /// rows) shows.
    /// @return It, or the end of @p entries for an index out of range.
    [[nodiscard]] std::vector<HintPatternConfig>::iterator userLinkPatternAt(
        std::vector<HintPatternConfig>& entries, int index)
    {
        if (index < 0)
            return entries.end();
        auto users = userLinkPatterns(entries);
        auto const found = std::ranges::next(users.begin(), index, users.end());
        return found == users.end() ? entries.end() : found.base();
    }

    /// A row's `kind`: what the page calls the kind of link a pattern finds.
    [[nodiscard]] QString linkKindName(vtbackend::LinkKind kind)
    {
        switch (kind)
        {
            case vtbackend::LinkKind::Url: return QStringLiteral("url");
            case vtbackend::LinkKind::Path: return QStringLiteral("path");
            case vtbackend::LinkKind::Custom: return QStringLiteral("custom");
        }
        return QStringLiteral("custom");
    }

    /// The regex the last of @p entries named @p name gives that built-in, as the terminal applies them in
    /// order.
    /// @return The regex; nullopt when no entry by that name gives one.
    [[nodiscard]] std::optional<std::string> regexOverrideOf(std::span<HintPatternConfig const> entries,
                                                             std::string_view name)
    {
        auto overrides =
            entries | std::views::reverse | std::views::filter([name](HintPatternConfig const& entry) {
                return entry.name == name && !entry.regex.empty();
            });
        return overrides.empty() ? std::nullopt : std::optional { overrides.front().regex };
    }

    /// The last of @p entries named @p name: the one whose keys win.
    /// @return It, or the end of @p entries when none has that name.
    [[nodiscard]] std::vector<HintPatternConfig>::iterator lastEntryNamed(
        std::vector<HintPatternConfig>& entries, std::string_view name)
    {
        auto const found = std::ranges::find(std::views::reverse(entries), name, &HintPatternConfig::name);
        return found.base() == entries.begin() ? entries.end() : std::prev(found.base());
    }

    /// The messages @p table has for the pattern @p name, one per line; empty when it has none.
    [[nodiscard]] QString diagnosticsOf(vtbackend::LinkPatterns const& table, std::string_view name)
    {
        auto text = QStringList {};
        for (auto const& diagnostic: table.diagnostics())
            if (diagnostic.pattern == name)
                text.push_back(QString::fromStdString(diagnostic.message()));
        return text.join(QLatin1Char('\n'));
    }

    /// What a user row shows as its problem: why the reader would skip the entry, else what compiling it
    /// reports.
    [[nodiscard]] QString problemOf(HintPatternConfig const& entry, vtbackend::LinkPatterns const& table)
    {
        if (auto const reason = config::skippedHintPatternReason(entry))
            return QString::fromStdString(*reason);
        return diagnosticsOf(table, entry.name);
    }

    /// The length, in UTF-16 code units, of the first @p bytes bytes of @p text: the offset QML counts in.
    [[nodiscard]] int utf16Offset(std::string_view text, std::size_t bytes)
    {
        return static_cast<int>(toQString(text.substr(0, bytes)).size());
    }
} // namespace

QVariantList SettingsController::linkPatterns() const
{
    auto rows = QVariantList {};
    if (!_hasDraft)
        return rows;
    auto const table = config::linkPatternsFor(_draft);
    auto const& entries = _draft.hintPatterns.value();
    for (auto const name: vtbackend::builtinLinkPatternNames())
    {
        // Every built-in compiles on its own (LinkPatterns' tests hold them to it), and the rows the
        // mutators count are one per name.
        auto const* builtin = vtbackend::LinkPatterns::builtins()->find(name);
        Require(builtin != nullptr);
        // As the terminal has it: the compiled table, which applies the entries by its name in order. A
        // broken override leaves the built-in out of it: the row shows that override's regex, finds
        // nothing, and says why.
        auto const* effective = table->find(name);
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = toQString(name);
        row[QStringLiteral("regex")] = QString::fromStdString(
            effective ? effective->source : regexOverrideOf(entries, name).value_or(builtin->source));
        row[QStringLiteral("clickable")] =
            effective != nullptr && effective->activation == vtbackend::LinkActivation::Clickable;
        row[QStringLiteral("url")] = QString {};
        row[QStringLiteral("builtin")] = true;
        row[QStringLiteral("kind")] = linkKindName(builtin->kind);
        row[QStringLiteral("problem")] = diagnosticsOf(*table, name);
        rows.push_back(row);
    }
    for (auto const& entry: entries)
    {
        if (vtbackend::isBuiltinLinkPatternName(entry.name))
            continue; // shown on its built-in's row
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(entry.name);
        row[QStringLiteral("regex")] = QString::fromStdString(entry.regex);
        row[QStringLiteral("clickable")] = entry.clickable.value_or(false);
        row[QStringLiteral("url")] = QString::fromStdString(entry.url.value_or(std::string {}));
        row[QStringLiteral("builtin")] = false;
        row[QStringLiteral("kind")] = linkKindName(vtbackend::LinkKind::Custom);
        row[QStringLiteral("problem")] = problemOf(entry, *table);
        rows.push_back(row);
    }
    return rows;
}

void SettingsController::addLinkPattern()
{
    if (!_hasDraft || _editingReadOnly)
        return;
    auto patterns = _draft.hintPatterns.value();
    auto const isTaken = [&patterns](std::string const& name) {
        return std::ranges::find(patterns, name, &HintPatternConfig::name) != patterns.end();
    };
    auto freeNames = std::views::iota(1)
                     | std::views::transform([](int number) { return std::format("pattern{}", number); })
                     | std::views::filter(std::not_fn(isTaken));
    patterns.push_back(
        HintPatternConfig { .name = *freeNames.begin(), .regex = {}, .clickable = true, .url = {} });
    _draft.hintPatterns = std::move(patterns);
    emit draftChanged();
}

void SettingsController::setLinkPattern(int index, QVariantMap const& row)
{
    if (!_hasDraft || _editingReadOnly || index < 0)
        return;
    auto patterns = _draft.hintPatterns.value();
    auto const clickable = row.value(QStringLiteral("clickable")).toBool();
    if (index < builtinLinkPatternRows())
    {
        // A built-in keeps its regex: its row writes `clickable` alone, as a name-only override.
        auto const name = vtbackend::builtinLinkPatternNames()[static_cast<std::size_t>(index)];
        auto const* builtin = vtbackend::LinkPatterns::builtins()->find(name);
        auto const byDefault =
            builtin != nullptr && builtin->activation == vtbackend::LinkActivation::Clickable;
        // The terminal merges the entries by its name, in order, each changing the keys it gives: cleared on
        // every one, then given to the last, the one whose keys win, only where it differs from the default.
        auto const named = [name](HintPatternConfig const& entry) {
            return entry.name == name;
        };
        for (auto& entry: patterns | std::views::filter(named))
            entry.clickable = std::nullopt;
        if (clickable != byDefault)
        {
            auto entry = lastEntryNamed(patterns, name);
            if (entry == patterns.end())
                entry = patterns.insert(
                    patterns.end(),
                    HintPatternConfig {
                        .name = std::string { name }, .regex = {}, .clickable = {}, .url = {} });
            entry->clickable = clickable;
        }
        // An entry that now changes nothing goes.
        std::erase_if(patterns, [&named](HintPatternConfig const& entry) {
            return named(entry) && entry.regex.empty() && !entry.clickable && !entry.url;
        });
    }
    else
    {
        auto const entry = userLinkPatternAt(patterns, index - builtinLinkPatternRows());
        if (entry == patterns.end())
            return;
        auto const name = row.value(QStringLiteral("name")).toString().trimmed().toStdString();
        // Renamed after a built-in or another pattern, it would merge into that one's row.
        if (name != entry->name
            && (vtbackend::isBuiltinLinkPatternName(name)
                || std::ranges::find(patterns, name, &HintPatternConfig::name) != patterns.end()))
        {
            fail(std::format("A link pattern named '{}' exists already; choose another name.", name));
            emit draftChanged(); // the row shows its name again, not the refused one
            return;
        }
        auto const url = row.value(QStringLiteral("url")).toString().toStdString();
        entry->name = name;
        entry->regex = row.value(QStringLiteral("regex")).toString().toStdString();
        entry->clickable = clickable;
        entry->url = url.empty() ? std::nullopt : std::optional { url };
    }
    _draft.hintPatterns = std::move(patterns);
    emit draftChanged();
}

void SettingsController::removeLinkPattern(int index)
{
    if (!_hasDraft || _editingReadOnly || index < builtinLinkPatternRows())
        return;
    auto patterns = _draft.hintPatterns.value();
    auto const entry = userLinkPatternAt(patterns, index - builtinLinkPatternRows());
    if (entry == patterns.end())
        return;
    patterns.erase(entry);
    _draft.hintPatterns = std::move(patterns);
    emit draftChanged();
}

QVariantMap SettingsController::previewLinkPattern(QVariantMap const& row, QString const& sample) const
{
    auto const name = row.value(QStringLiteral("name")).toString().toStdString();
    auto const regex = row.value(QStringLiteral("regex")).toString().toStdString();
    auto const url = row.value(QStringLiteral("url")).toString().toStdString();
    // The row on its own, as the terminal would compile it: a built-in's row carries the regex in effect.
    auto const entries = std::array { vtbackend::LinkPatternSpec {
        .name = name,
        .regex = regex.empty() ? std::nullopt : std::optional { regex },
        .activation = row.value(QStringLiteral("clickable")).toBool() ? vtbackend::LinkActivation::Clickable
                                                                      : vtbackend::LinkActivation::HintOnly,
        .urlTemplate = url.empty() ? std::nullopt : std::optional { url },
    } };
    auto const table = vtbackend::LinkPatterns::compile(entries);

    auto result = QVariantMap {};
    result[QStringLiteral("problem")] = diagnosticsOf(table, name);
    result[QStringLiteral("note")] = QString {};
    auto spans = QVariantList {};
    auto const* pattern = table.find(name);
    if (pattern != nullptr)
    {
        auto const text = sample.toStdString();
        auto const rows = std::array { vtbackend::makePlainScanRow(text) };
        auto const scan = vtbackend::ScanText { rows, vtbackend::ScanClip {} };
        auto const patternIndex = static_cast<std::size_t>(pattern - table.patterns().data());
        auto const clickable = pattern->activation == vtbackend::LinkActivation::Clickable;
        auto anyRefused = false;
        for (auto const& candidate: vtbackend::findLinks(
                 scan, table, vtbackend::PatternSelection::All, vtbackend::LinkOverlap::Keep))
            if (candidate.patternIndex == patternIndex)
            {
                // A match the terminal would not make a link (§7.6), shown as such, as it underlines nothing.
                auto const refused = clickable && candidate.opening == vtbackend::LinkOpening::Refused;
                anyRefused = anyRefused || refused;
                auto span = QVariantMap {};
                span[QStringLiteral("start")] = utf16Offset(text, candidate.byteBegin);
                span[QStringLiteral("end")] = utf16Offset(text, candidate.byteEnd);
                span[QStringLiteral("target")] = QString::fromStdString(candidate.target);
                span[QStringLiteral("refused")] = refused;
                spans.push_back(span);
            }
        if (pattern->kind == vtbackend::LinkKind::Path)
            result[QStringLiteral("note")] =
                tr("Paths are underlined in the terminal once they are found to exist.");
        else if (anyRefused)
            result[QStringLiteral("note")] =
                tr("A struck-out match is no link: the printed text names no scheme Contour opens.");
    }
    result[QStringLiteral("spans")] = spans;
    return result;
}

// }}}

} // namespace contour::window
