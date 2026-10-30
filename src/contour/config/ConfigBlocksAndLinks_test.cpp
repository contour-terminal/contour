// SPDX-License-Identifier: Apache-2.0
//
// Config loader tests, continued: semantic blocks, command-finish notifications, the sticky header,
// link detection, hyperlink decoration, file_opener and hint patterns.

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ConfigDocumentation.hpp>
#include <contour/config/ConfigEnum.hpp>
#include <contour/config/ConfigReader.hpp>
#include <contour/config/ConfigTestSupport.hpp>
#include <contour/test/CoreFixtures.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <vtrasterizer/Decorator.hpp>

#include <core/log/LogSink.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace contour::test::configSupport;

namespace
{

/// The action of type @p BoundAction bound to @p key.
/// @param config The loaded configuration.
/// @param key The key the binding is for.
/// @return The action, or nullopt when the key has no binding (the reader dropped it).
template <typename BoundAction>
[[nodiscard]] std::optional<BoundAction> boundAction(contour::config::Config const& config,
                                                     vtbackend::Key key)
{
    auto const* mapping = keyMappingFor(config, key);
    if (mapping == nullptr || mapping->binding.empty())
        return std::nullopt;
    return std::get<BoundAction>(mapping->binding.at(0));
}

/// A configuration entry given a value of the wrong shape: a scalar where a map belongs, or a list where
/// one word belongs.
struct MalformedEntryCase
{
    std::string_view topLevel;  ///< Lines at the document's top level; empty for none.
    std::string_view inProfile; ///< Lines inside the main profile, indented by eight spaces; empty for none.
    std::string_view key;       ///< The key the diagnostic must name, spelled out from the document's root.
    /// Whether the entry still holds its default (or, for a nested value, what its siblings say).
    bool (*keptDefault)(contour::config::Config const& config);
};

/// The main profile of @p config, which every malformed-entry document defines.
[[nodiscard]] contour::config::TerminalProfile const& mainProfile(contour::config::Config const& config)
{
    auto const* profile = config.findProfile("main");
    REQUIRE(profile != nullptr);
    return *profile;
}

/// The palette of the `broken` scheme the main profile of @p config uses, once its sibling map after the
/// malformed one was read; nullptr when the profile lost the scheme, or the sibling was not read.
[[nodiscard]] vtbackend::ColorPalette const* brokenScheme(contour::config::Config const& config)
{
    auto const* colors = std::get_if<contour::config::SimpleColorConfig>(&mainProfile(config).colors.value());
    if (colors == nullptr || colors->colorScheme != "broken"
        || colors->colors.gutterText != vtbackend::RGBColor { 0x123456 })
        return nullptr;
    return &colors->colors;
}

constexpr auto MalformedEntryCases = std::array {
    // A map-valued key given a scalar.
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        notify_on_command_finish: never\n",
                         .key = "profiles.main.notify_on_command_finish",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).notifyOnCommandFinish.value()
                                        == contour::config::FinishNotificationConfig {};
                             } },
    MalformedEntryCase { .topLevel = "command_blocks: true\n",
                         .inProfile = {},
                         .key = "command_blocks",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return config.commandBlocks.value().maxRecords
                                        == contour::config::CommandBlocksConfig {}.maxRecords;
                             } },
    MalformedEntryCase { .topLevel = "osc_context: off\n",
                         .inProfile = {},
                         .key = "osc_context",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return config.oscContext.value().enabled
                                        == contour::config::OscContextConfig {}.enabled;
                             } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        scrollbar: left\n",
                         .key = "profiles.main.scrollbar",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).scrollbar.value().position
                                        == contour::config::ScrollBarConfig {}.position;
                             } },
    // A list-valued key given one word: it costs the list, not its siblings.
    MalformedEntryCase {
        .topLevel = {},
        .inProfile = "        scrollbar:\n"
                     "            position: left\n"
                     "            marks: failures\n",
        .key = "profiles.main.scrollbar.marks",
        .keptDefault =
            [](contour::config::Config const& config) {
                auto const& scrollbar = mainProfile(config).scrollbar.value();
                return scrollbar.marks
                           == contour::config::editionDefaults(config.defaultsEdition).scrollBarMarks
                       && scrollbar.position == contour::config::ScrollBarPosition::Left;
            } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        sticky_header: always\n",
                         .key = "profiles.main.sticky_header",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).stickyHeader.value()
                                        == contour::config::StickyHeaderConfig {};
                             } },
    // A colour map of a scheme given a scalar: it costs that map, whose palette slot keeps its default, and
    // the profile keeps the scheme -- the map after it is still read.
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        sticky_header: '#202830'\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.sticky_header",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr && !scheme->stickyHeaderBackground
                                        && !scheme->stickyHeaderSeparator;
                             } },
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        fold_marker: '#202830'\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.fold_marker",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr && !scheme->foldMarker && !scheme->foldMarkerHover;
                             } },
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        block_status: '#202830'\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.block_status",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr
                                        && std::ranges::none_of(scheme->blockStatus,
                                                                [](auto const& c) { return c.has_value(); });
                             } },
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        tint: '#202830'\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.tint",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr && !scheme->hasContextTints();
                             } },
    // A map whose malformed key comes after a good one: the map is put back whole, not left half read.
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        sticky_header:\n"
                                     "            background: '#202830'\n"
                                     "            separator: [1, 2]\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.sticky_header",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr && !scheme->stickyHeaderBackground
                                        && !scheme->stickyHeaderSeparator;
                             } },
    // So are the palette's own colour maps.
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        gutter_text: '#123456'\n"
                                     "        normal:\n"
                                     "            red: [1, 2]\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.normal",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr
                                        && scheme->palette[1] == vtbackend::ColorPalette {}.palette[1];
                             } },
    // The scheme's older maps are guarded the same way.
    MalformedEntryCase { .topLevel = "color_schemes:\n"
                                     "    broken:\n"
                                     "        cursor: block\n"
                                     "        gutter_text: '#123456'\n",
                         .inProfile = "        colors: broken\n",
                         .key = "color_schemes.broken.cursor",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const* scheme = brokenScheme(config);
                                 return scheme != nullptr
                                        && scheme->cursor.color == vtbackend::ColorPalette {}.cursor.color;
                             } },
    // A map whose malformed key comes after a good one: the entry is put back whole, not left half read.
    MalformedEntryCase {
        .topLevel = {},
        .inProfile = "        history:\n"
                     "            limit: 4242\n"
                     "            auto_scroll_on_update: [yes]\n",
        .key = "profiles.main.history",
        .keptDefault =
            [](contour::config::Config const& config) {
                return mainProfile(config).history.value().maxHistoryLineCount
                       == contour::config::TerminalProfile {}.history.value().maxHistoryLineCount;
            } },
    // An enum key given a list.
    MalformedEntryCase { .topLevel = "tab_bar_position: [bottom]\n",
                         .inProfile = {},
                         .key = "tab_bar_position",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return config.tabBarPosition.value() == contour::config::TabBarPosition::Top;
                             } },
    MalformedEntryCase { .topLevel = "ui_style: [terminal]\n",
                         .inProfile = {},
                         .key = "ui_style",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return config.uiStyle.value() == contour::config::UiStyle::Native;
                             } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        search_case_sensitivity: [sensitive]\n",
                         .key = "profiles.main.search_case_sensitivity",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).searchCaseSensitivity.value()
                                        == vtbackend::SearchCaseSensitivity::Smart;
                             } },
    // Nested: the bad value costs only itself, so its sibling after it is still read.
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        notify_on_command_finish:\n"
                                      "            when: [always]\n"
                                      "            outcome: failure\n",
                         .key = "profiles.main.notify_on_command_finish.when",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 auto const& policy = mainProfile(config).notifyOnCommandFinish.value();
                                 return policy.when == contour::config::NotifyWhen::Unfocused
                                        && policy.outcome == contour::config::NotifyOutcome::Failure;
                             } },
    // Nested map-valued keys that are no config entries of their own: each costs itself, not its profile.
    MalformedEntryCase {
        .topLevel = {},
        // Not `block`: that is the insert cursor's default now, and a value equal to the default could
        // not tell a skipped entry from a misread one.
        .inProfile = "        cursor: underscore\n",
        .key = "profiles.main.cursor",
        .keptDefault =
            [](contour::config::Config const& config) {
                return mainProfile(config).modeInsert.value().cursor.cursorShape
                       == contour::config::TerminalProfile {}.modeInsert.value().cursor.cursorShape;
            } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        normal_mode: block\n",
                         .key = "profiles.main.normal_mode",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).modeNormal.value().cursor.cursorShape
                                        == vtbackend::CursorShape::Block;
                             } },
    MalformedEntryCase { .topLevel = {},
                         .inProfile = "        visual_mode:\n"
                                      "            cursor: underscore\n",
                         .key = "profiles.main.visual_mode.cursor",
                         .keptDefault =
                             [](contour::config::Config const& config) {
                                 return mainProfile(config).modeVisual.value().cursor.cursorShape
                                        == vtbackend::CursorShape::Block;
                             } },
};

/// A configuration whose one profile reads the colour scheme `coverage`: a background, a foreground, and then
/// @p stickyHeader.
/// @param stickyHeader The scheme's further keys, indented as its own -- its sticky_header map, if any.
/// @return The YAML.
[[nodiscard]] std::string schemeWithStickyHeader(std::string_view stickyHeader)
{
    return std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: coverage
color_schemes:
    coverage:
        default:
            background: '#102030'
            foreground: '#D0D0D0'
{})",
                       stickyHeader);
}

/// Whether @p node has a defined child at @p path, walked one segment at a time: yaml-cpp throws on
/// indexing below a missing node, so a chained lookup could not report a missing section.
[[nodiscard]] bool hasPath(YAML::Node const& node, std::span<std::string const> path)
{
    if (path.empty())
        return node.IsDefined();
    if (!node.IsDefined() || !node.IsMap())
        return false;
    return hasPath(node[path.front()], path.subspan(1));
}

/// The entry of @p section in reference page @p doc, from its heading to the next one; empty when it has
/// none. A leaf is looked for there, as its `==term==`: a bare word such as `marks` or `pager` turns up in
/// other entries' prose, and in its own entry's YAML example besides.
[[nodiscard]] std::string_view referenceEntry(std::string_view doc, std::string_view section)
{
    auto const heading = std::format("### `{}`\n", section);
    auto const begin = doc.find(heading);
    if (begin == std::string_view::npos)
        return {};
    // Without a next heading, the length is npos less a little, and substr() stops at the page's end.
    return doc.substr(begin, doc.find("### `", begin + heading.size()) - begin);
}

/// Loads a main profile whose `link_detection` map is @p body (empty: the key is absent).
[[nodiscard]] contour::config::LinkDetectionConfig linkDetectionOf(QTemporaryDir& dir, std::string_view body)
{
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
{}
)",
                                                 body));
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    return profile->linkDetection.value();
}

/// Loads a main profile whose `hyperlink_decoration` map is @p body and returns its decorators.
[[nodiscard]] contour::config::HyperlinkDecorationConfig hyperlinkDecorationOf(QTemporaryDir& dir,
                                                                               std::string_view body)
{
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        hyperlink_decoration:
{}
)",
                                                 body));
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    return profile->hyperlinkDecoration.value();
}

/// Loads a main profile whose `file_opener` entry is @p entry: its value, as it follows the key.
[[nodiscard]] contour::config::FileOpenerConfig fileOpenerOf(QTemporaryDir& dir, std::string_view entry)
{
    auto const config = loadFromYaml(dir,
                                     std::format("default_profile: main\n"
                                                 "profiles:\n"
                                                 "    main:\n"
                                                 "        shell: /bin/sh\n"
                                                 "        file_opener:{}\n",
                                                 entry));
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    return profile->fileOpener.value();
}

} // namespace

TEST_CASE("Config: SelectCommandBlock reads its target, part and pinned block", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'F1', action: SelectCommandBlock }
    - { mods: [Control], key: 'F2', action: SelectCommandBlock, target: cursor, part: input }
    - { mods: [Control], key: 'F3', action: SelectCommandBlock, target: POINTER, part: all, block: 7 }
    - { mods: [Control], key: 'F4', action: SelectCommandBlock, part: bogus }
)"sv);

    auto const bound = [&](vtbackend::Key key) {
        return boundAction<contour::actions::SelectCommandBlock>(config, key);
    };

    auto const plain = bound(vtbackend::Key::F1);
    REQUIRE(plain.has_value());
    CHECK(plain->target == vtbackend::CommandBlockTarget::Last);
    CHECK(plain->part == vtbackend::CommandBlockPart::Output);
    CHECK(plain->block.value == 0);

    auto const atCursor = bound(vtbackend::Key::F2);
    REQUIRE(atCursor.has_value());
    CHECK(atCursor->target == vtbackend::CommandBlockTarget::Cursor);
    CHECK(atCursor->part == vtbackend::CommandBlockPart::Prompt);

    auto const pinned = bound(vtbackend::Key::F3);
    REQUIRE(pinned.has_value());
    CHECK(pinned->target == vtbackend::CommandBlockTarget::Pointer); // tokens ignore case
    CHECK(pinned->part == vtbackend::CommandBlockPart::PromptAndOutput);
    CHECK(pinned->block.value == 7);

    // A misspelt part drops the binding rather than silently selecting something else.
    CHECK_FALSE(bound(vtbackend::Key::F4).has_value());
}

TEST_CASE("Config: a block that names no block id drops the binding", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'F1', action: SelectCommandBlock, block: abc }
    - { mods: [Control], key: 'F2', action: SelectCommandBlock, block: -1 }
    - { mods: [Control], key: 'F3', action: SelectCommandBlock, block: 4294967296 }
    - { mods: [Control], key: 'F4', action: SelectCommandBlock, block: [7] }
    - { mods: [Control], key: 'F5', action: SelectCommandBlock, block: 0 }
    - { mods: [Control], key: 'F6', action: SelectCommandBlock, block: 4294967295 }
    - { mods: [Control], key: 'F7', action: SelectCommandBlock, block: 7 }
    - { mods: [Control], key: 'F8', action: SelectCommandBlock, part: bogus }
    - { mods: [Control], key: 'F9', action: SelectCommandBlock, block: 0x7 }
)"sv);

    auto const bound = [&](vtbackend::Key key) {
        return boundAction<contour::actions::SelectCommandBlock>(config, key);
    };

    // None of these is a block id written as Contour writes one. Read as some id anyway -- "not pinned", or
    // the digits before the junk -- each would quietly act on another block, so each drops the binding, as
    // a misspelt part does.
    CHECK_FALSE(bound(vtbackend::Key::F1).has_value()); // not a number
    CHECK_FALSE(bound(vtbackend::Key::F2).has_value()); // negative
    CHECK_FALSE(bound(vtbackend::Key::F3).has_value()); // past the id's range
    CHECK_FALSE(bound(vtbackend::Key::F4).has_value()); // not a scalar
    CHECK_FALSE(bound(vtbackend::Key::F9).has_value()); // not decimal: digits with more after them

    // Zero is "not pinned"; every other id the type holds pins.
    auto const unpinned = bound(vtbackend::Key::F5);
    REQUIRE(unpinned.has_value());
    CHECK(unpinned->block.value == 0);

    auto const largest = bound(vtbackend::Key::F6);
    REQUIRE(largest.has_value());
    CHECK(largest->block.value == 4294967295U);

    auto const pinned = bound(vtbackend::Key::F7);
    REQUIRE(pinned.has_value());
    CHECK(pinned->block.value == 7);

    // A dropped block says so in the words a dropped part uses.
    CHECK_FALSE(bound(vtbackend::Key::F8).has_value());
    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("Invalid part 'bogus' in action 'SelectCommandBlock'; ignoring the binding."));
    CHECK(log.contains("Invalid block 'abc' in action 'SelectCommandBlock'; ignoring the binding."));
    CHECK(log.contains("Invalid block '' in action 'SelectCommandBlock'; ignoring the binding.")); // [7]
}

TEST_CASE("Config: the block copy actions read back what the config writer writes",
          "[config][semanticblocks]")
{
    using namespace contour::actions;
    using vtbackend::CommandBlockId;
    using vtbackend::CommandBlockPart;
    using vtbackend::CommandBlockTarget;

    // The writer's own output, fed to the reader: what Contour writes is by construction what it reads.
    auto const copyPrompt = CopyCommandBlock { .target = CommandBlockTarget::Pointer,
                                               .part = CommandBlockPart::Prompt,
                                               .block = CommandBlockId(4) };
    auto const copyLine =
        CopyCommandLine { .target = CommandBlockTarget::Cursor, .block = CommandBlockId(2) };

    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [Control], key: 'F1', action: CopyCommandBlock }}
    - {{ mods: [Control], key: 'F2', action: {} }}
    - {{ mods: [Control], key: 'F3', action: CopyCommandLine }}
    - {{ mods: [Control], key: 'F4', action: {} }}
    - {{ mods: [Control], key: 'F5', action: CopyCommandBlock, part: bogus }}
    - {{ mods: [Control], key: 'F6', action: CopyCommandLine, target: nowhere }}
    - {{ mods: [Control], key: 'F7', action: CopyCommandLine, block: abc }}
)",
                                                 Action { copyPrompt },
                                                 Action { copyLine }));

    auto const plainBlock = boundAction<CopyCommandBlock>(config, vtbackend::Key::F1);
    REQUIRE(plainBlock.has_value());
    CHECK(plainBlock->target == CommandBlockTarget::Last);
    CHECK(plainBlock->part == CommandBlockPart::Output);
    CHECK(plainBlock->block.value == 0);

    auto const prompt = boundAction<CopyCommandBlock>(config, vtbackend::Key::F2);
    REQUIRE(prompt.has_value());
    CHECK(prompt->target == copyPrompt.target);
    CHECK(prompt->part == copyPrompt.part);
    CHECK(prompt->block == copyPrompt.block);

    auto const plainLine = boundAction<CopyCommandLine>(config, vtbackend::Key::F3);
    REQUIRE(plainLine.has_value());
    CHECK(plainLine->target == CommandBlockTarget::Last);
    CHECK(plainLine->block.value == 0);

    auto const line = boundAction<CopyCommandLine>(config, vtbackend::Key::F4);
    REQUIRE(line.has_value());
    CHECK(line->target == copyLine.target);
    CHECK(line->block == copyLine.block);

    // A misspelt argument drops the binding, in the words SelectCommandBlock's use.
    CHECK_FALSE(boundAction<CopyCommandBlock>(config, vtbackend::Key::F5).has_value());
    CHECK_FALSE(boundAction<CopyCommandLine>(config, vtbackend::Key::F6).has_value());
    CHECK_FALSE(boundAction<CopyCommandLine>(config, vtbackend::Key::F7).has_value());
    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("Invalid part 'bogus' in action 'CopyCommandBlock'; ignoring the binding."));
    CHECK(log.contains("Invalid target 'nowhere' in action 'CopyCommandLine'; ignoring the binding."));
    CHECK(log.contains("Invalid block 'abc' in action 'CopyCommandLine'; ignoring the binding."));
}

TEST_CASE("Config: CopySelection reads its fallback", "[config][semanticblocks]")
{
    using contour::actions::CopyFallback;
    using contour::actions::CopyFormat;
    using contour::actions::CopySelection;

    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(
        dir,
        std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [Control], key: 'F1', action: CopySelection, fallback: last_command_output }}
    - {{ mods: [Control], key: 'F2', action: CopySelection, format: HTML }}
    - {{ mods: [Control], key: 'F3', action: CopySelection, fallback: everything }}
    - {{ mods: [Control], key: 'F4', action: {} }}
)",
                    contour::actions::Action { CopySelection {
                        .format = CopyFormat::HTML, .fallback = CopyFallback::LastCommandOutput } }));

    auto const fallback = boundAction<CopySelection>(config, vtbackend::Key::F1);
    REQUIRE(fallback.has_value());
    CHECK(fallback->fallback == CopyFallback::LastCommandOutput);
    CHECK(fallback->format == CopyFormat::Text);

    auto const html = boundAction<CopySelection>(config, vtbackend::Key::F2);
    REQUIRE(html.has_value());
    CHECK(html->format == CopyFormat::HTML);
    CHECK(html->fallback == CopyFallback::None);

    // A misspelt fallback drops the binding: guessing would either empty the clipboard or fill it with a
    // build log the user never asked for.
    CHECK_FALSE(boundAction<CopySelection>(config, vtbackend::Key::F3).has_value());
    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("Invalid fallback 'everything' in action 'CopySelection'; ignoring the binding."));

    // What the config writer writes reads back.
    auto const both = boundAction<CopySelection>(config, vtbackend::Key::F4);
    REQUIRE(both.has_value());
    CHECK(both->format == CopyFormat::HTML);
    CHECK(both->fallback == CopyFallback::LastCommandOutput);
}

TEST_CASE("Config: a mouse binding may name a click count", "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], mouse: Left, action: FollowHyperlink }
    - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer }
    - { mods: [Control], mouse: Left, clicks: 9, action: SelectAll }
    - { mods: [Control], key: 'F5', clicks: 2, action: SelectAll }
)"sv);

    auto const& mappings = config.inputMappings.value();

    // The click-count row is a binding of its own, not a second action on the plain Ctrl+Left one; the
    // out-of-range count and the count on a key are dropped.
    REQUIRE(mappings.mouseMappings.size() == 2);
    CHECK(mappings.keyMappings.empty());

    auto const ctrl = Modifiers { Modifier::Control };
    auto const* plain =
        contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 });
    REQUIRE(plain != nullptr);
    CHECK(std::holds_alternative<contour::actions::FollowHyperlink>(plain->at(0)));

    auto const* third =
        contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 3 });
    REQUIRE(third != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::SelectCommandBlock>(third->at(0)));
    CHECK(std::get<contour::actions::SelectCommandBlock>(third->at(0)).target
          == vtbackend::CommandBlockTarget::Pointer);

    CHECK(
        contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 2 })
        == nullptr);
}

TEST_CASE("Config: a malformed click count drops the binding", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], mouse: Left, clicks: 0, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: 5, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: -1, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: three, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: 0x3, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: 256, action: SelectAll }
    - { mods: [Control], mouse: Left, clicks: [3], action: SelectAll }
    - { mods: [Control], key: 'F5', clicks: 2, action: SelectAll }
    - { mods: [Control], key: 'F6', mouse: Left, clicks: 2, action: SelectAll }
    - { mods: [Control], mouse: WheelUp, clicks: 1, action: SelectAll }
    - { mods: [Control], mouse: Middle, clicks: 2, action: SelectCommandBlock, part: bogus }
    - { mods: [Control], mouse: Right, clicks: 4, action: SelectCommandBlock, target: cursor }
)"sv);

    // Every count above but the last's is no count, or one no press carries. Read as some count anyway --
    // the digits before the junk, the count wrapped into range, "any count" -- each would quietly bind
    // another click, so each drops the binding, as a misspelt modifier does. A count on a key or a wheel
    // names a click that never comes. The action's own arguments are read as on any other row.
    auto const& mappings = config.inputMappings.value();
    CHECK(mappings.keyMappings.empty());
    REQUIRE(mappings.mouseMappings.size() == 1);
    auto const& kept = mappings.mouseMappings.front();
    CHECK(kept.input == vtbackend::MouseButton::Right);
    CHECK(kept.clickCount == uint8_t { 4 });
    REQUIRE(std::holds_alternative<contour::actions::SelectCommandBlock>(kept.binding.at(0)));
    CHECK(std::get<contour::actions::SelectCommandBlock>(kept.binding.at(0)).target
          == vtbackend::CommandBlockTarget::Cursor);

    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    for (auto const* count: { "0", "5", "-1", "three", "0x3", "256" })
    {
        INFO("clicks: " << count);
        CHECK(log.contains(
            std::format("Dropping input_mapping entry [action: SelectAll, mouse: Left, clicks: {}]: "
                        "could not parse its clicks (expected 1 to 4).",
                        count)));
    }
    CHECK(log.contains("Dropping input_mapping entry [action: SelectAll, mouse: Left]: could not parse its "
                       "clicks (expected 1 to 4).")); // [3]
    CHECK(
        log.contains("Dropping input_mapping entry [action: SelectAll, key: F5, clicks: 2]: 'clicks' applies "
                     "to the Left, Middle and Right mouse buttons only."));
    CHECK(log.contains("Dropping input_mapping entry [action: SelectAll, key: F6, mouse: Left, clicks: 2]: "
                       "'clicks' applies to the Left, Middle and Right mouse buttons only."));
    CHECK(
        log.contains("Dropping input_mapping entry [action: SelectAll, mouse: WheelUp, clicks: 1]: 'clicks' "
                     "applies to the Left, Middle and Right mouse buttons only."));
    CHECK(log.contains("Invalid part 'bogus' in action 'SelectCommandBlock'; ignoring the binding."));
}

TEST_CASE("Config: Ctrl+triple-click selects the output under the pointer by default",
          "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;

    auto const config = contour::config::Config {};
    auto const ctrl = Modifiers { Modifier::Control };

    auto const* bound =
        contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 3 });
    REQUIRE(bound != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::SelectCommandBlock>(bound->at(0)));
    auto const& select = std::get<contour::actions::SelectCommandBlock>(bound->at(0));
    CHECK(select.target == vtbackend::CommandBlockTarget::Pointer);
    CHECK(select.part == vtbackend::CommandBlockPart::Output);

    // Only the third click: a plain Ctrl+click and a Ctrl+double-click are not claimed by it, nor is a
    // triple-click without Ctrl or of another button.
    CHECK(contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }) == nullptr);
    CHECK(contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 2 })
          == nullptr);
    CHECK(contour::config::applyBuiltinFallback(
              config, MouseButton::Left, Modifiers { Modifier::None }, uint8_t { 0 }, uint8_t { 3 })
          == nullptr);
    CHECK(
        contour::config::applyBuiltinFallback(config, MouseButton::Middle, ctrl, uint8_t { 0 }, uint8_t { 3 })
        == nullptr);

    // A fallback, not a default (see builtinFallbackMouseMappings): no default row names a click count.
    CHECK(std::ranges::none_of(config.inputMappings.value().mouseMappings,
                               [](auto const& mapping) { return mapping.clickCount.has_value(); }));
}

TEST_CASE("Config: Ctrl+triple-click leaves the alternate screen's extended word alone",
          "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;

    // There are no blocks on the alternate screen, so the row would claim the click only to refuse it,
    // and the terminal's own extended-word selection would be lost.
    auto const config = contour::config::Config {};
    auto const ctrl = Modifiers { Modifier::Control };
    auto const alternateScreen = uint8_t { vtbackend::MatchModes::AlternateScreen };
    CHECK(
        contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, alternateScreen, uint8_t { 3 })
        == nullptr);
    CHECK(contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 3 })
          != nullptr);
}

TEST_CASE("Config: a mouse binding's mode survives the writer", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Alt], mouse: Middle, action: SelectAll, mode: '~Select' }
)");
    auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
    REQUIRE(written.contains("mode: '~Select'"));

    QTemporaryDir reloadDir;
    auto const reloaded = loadFromYaml(reloadDir, written);
    auto const& mappings = reloaded.inputMappings.value().mouseMappings;
    REQUIRE(mappings.size() == 1);
    CHECK(mappings.front().modes == vtbackend::MatchModes { 0, vtbackend::MatchModes::Select });
}

TEST_CASE("Config: a click count survives the writer", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto config = contour::config::Config {};
    // Pushed in place: copying the mappings out and assigning them back instantiates the copy of every
    // action alternative twice more, which GCC at -O2 pays for with some 7 GB.
    config.inputMappings.value().mouseMappings.push_back(
        contour::config::MouseInputMapping { .modes { vtbackend::MatchModes {} },
                                             .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                                             .input = vtbackend::MouseButton::Middle,
                                             .binding = { { contour::actions::SelectAll {} } },
                                             .clickCount = uint8_t { 2 } });

    auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
    REQUIRE(written.contains("clicks: 2"));

    auto const reloaded = loadFromYaml(dir, written);
    CHECK(std::ranges::any_of(reloaded.inputMappings.value().mouseMappings, [](auto const& mapping) {
        return mapping.input == vtbackend::MouseButton::Middle && mapping.clickCount == uint8_t { 2 };
    }));
}

TEST_CASE("Config: command_blocks.pager", "[config][semanticblocks]")
{
    QTemporaryDir dir;

    SECTION("defaults to the platform's pager")
    {
        auto const config = contour::config::Config {};
#ifdef _WIN32
        CHECK(config.commandBlocks.value().pager == "more");
#else
        CHECK(config.commandBlocks.value().pager == "less -R");
#endif
        CHECK(config.commandBlocks.value().pager == contour::config::defaultCommandOutputPager());
    }

    SECTION("pager_format defaults to sgr, which less -R and Windows more both render")
    {
        CHECK(contour::config::Config {}.commandBlocks.value().pagerFormat
              == contour::actions::OutputFormat::Sgr);
    }

    SECTION("pager_format is read, and survives the writer")
    {
        auto const config = loadFromYaml(dir,
                                         "command_blocks:\n"
                                         "    pager: 'pg'\n"
                                         "    pager_format: plain\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.commandBlocks.value().pagerFormat == contour::actions::OutputFormat::Plain);

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("pager_format: plain"));
        QTemporaryDir reloadDir;
        CHECK(loadFromYaml(reloadDir, written).commandBlocks.value().pagerFormat
              == contour::actions::OutputFormat::Plain);
    }

    SECTION("the format follows the program unless the binding names one")
    {
        using contour::actions::OpenCommandOutput;
        using contour::actions::OutputFormat;
        using contour::config::outputFormatFor;
        auto blocks = contour::config::CommandBlocksConfig {};
        blocks.pagerFormat = OutputFormat::Plain;

        // The configured pager's format; a named program is assumed to render SGR, as less -R does.
        CHECK(outputFormatFor(OpenCommandOutput {}, blocks) == OutputFormat::Plain);
        CHECK(outputFormatFor(OpenCommandOutput { .program = "less -R" }, blocks) == OutputFormat::Sgr);
        // The binding's own format wins either way.
        CHECK(outputFormatFor(OpenCommandOutput { .format = OutputFormat::Sgr }, blocks)
              == OutputFormat::Sgr);
        CHECK(outputFormatFor(OpenCommandOutput { .program = "pg", .format = OutputFormat::Plain }, blocks)
              == OutputFormat::Plain);
    }

    SECTION("is read from command_blocks")
    {
        auto const config = loadFromYaml(dir,
                                         "command_blocks:\n"
                                         "    pager: 'bat --paging=always'\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.commandBlocks.value().pager == "bat --paging=always");
    }

    SECTION("round-trips through the generated config")
    {
        auto config = contour::config::Config {};
        auto blocks = config.commandBlocks.value();
        blocks.pager = "most -s";
        config.commandBlocks = blocks;

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("pager: 'most -s'"));
        CHECK(loadFromYaml(dir, written).commandBlocks.value().pager == "most -s");
    }

    SECTION("round-trips a quote, a colon and a hash")
    {
        auto config = contour::config::Config {};
        auto blocks = config.commandBlocks.value();
        blocks.pager = "less --prompt='page: #%d'";
        config.commandBlocks = blocks;

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        CHECK(loadFromYaml(dir, written).commandBlocks.value().pager == "less --prompt='page: #%d'");
    }
}

TEST_CASE("Config: OpenCommandOutput reads its arguments", "[config][semanticblocks]")
{
    using contour::actions::OpenCommandOutput;
    using contour::actions::OutputFormat;
    using contour::actions::OutputPlacement;

    // The writer's own output, fed to the reader: what Contour writes is by construction what it reads.
    auto const written = OpenCommandOutput { .target = vtbackend::CommandBlockTarget::Pointer,
                                             .program = "less --prompt='x'",
                                             .placement = OutputPlacement::Tab,
                                             .format = OutputFormat::Plain,
                                             .block = vtbackend::CommandBlockId(4) };

    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - {{ mods: [Control], key: 'F1', action: OpenCommandOutput }}
    - {{ mods: [Control], key: 'F2', action: OpenCommandOutput, program: 'wl-copy', placement: detached, format: plain }}
    - {{ mods: [Control], key: 'F3', action: {} }}
    - {{ mods: [Control], key: 'F4', action: OpenCommandOutput, placement: window }}
    - {{ mods: [Control], key: 'F5', action: OpenCommandOutput, format: html }}
    - {{ mods: [Control], key: 'F6', action: OpenCommandOutput, target: nowhere }}
    - {{ mods: [Control], key: 'F7', action: OpenCommandOutput, block: abc }}
    - {{ mods: [Control], key: 'F8', action: OpenCommandOutput, program: [less, -R] }}
)",
                                                 contour::actions::Action { written }));

    auto const plain = boundAction<OpenCommandOutput>(config, vtbackend::Key::F1);
    REQUIRE(plain.has_value());
    CHECK(plain->target == vtbackend::CommandBlockTarget::Last);
    CHECK(plain->program.empty());
    CHECK(plain->placement == OutputPlacement::Split);
    CHECK_FALSE(plain->format.has_value()); // the program's own format applies
    CHECK(plain->block.value == 0);

    auto const piped = boundAction<OpenCommandOutput>(config, vtbackend::Key::F2);
    REQUIRE(piped.has_value());
    CHECK(piped->program == "wl-copy");
    CHECK(piped->placement == OutputPlacement::Detached);
    CHECK(piped->format == OutputFormat::Plain);

    auto const readBack = boundAction<OpenCommandOutput>(config, vtbackend::Key::F3);
    REQUIRE(readBack.has_value());
    CHECK(readBack->target == written.target);
    CHECK(readBack->program == written.program);
    CHECK(readBack->placement == written.placement);
    CHECK(readBack->format == written.format);
    CHECK(readBack->block == written.block);

    // A misspelt argument drops the binding, in the words the other block actions use: guessing would turn
    // the binding into one that does something else. A program that is no scalar is no program either.
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F4).has_value());
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F5).has_value());
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F6).has_value());
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F7).has_value());
    CHECK_FALSE(boundAction<OpenCommandOutput>(config, vtbackend::Key::F8).has_value());
    auto const& log = capture.text();
    INFO("captured error log:\n" << log);
    CHECK(log.contains("Invalid placement 'window' in action 'OpenCommandOutput'; ignoring the binding."));
    CHECK(log.contains("Invalid format 'html' in action 'OpenCommandOutput'; ignoring the binding."));
    CHECK(log.contains("Invalid target 'nowhere' in action 'OpenCommandOutput'; ignoring the binding."));
    CHECK(log.contains("Invalid block 'abc' in action 'OpenCommandOutput'; ignoring the binding."));
    CHECK(log.contains("Invalid program '' in action 'OpenCommandOutput'; ignoring the binding."));
}

TEST_CASE("Config: Ctrl+Shift+G opens the last command's output by default", "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    auto const config = contour::config::Config {};
    auto const ctrlShift = Modifiers { Modifier::Control, Modifier::Shift };

    auto const* bound = contour::config::applyBuiltinFallback(config, U'G', ctrlShift, uint8_t { 0 });
    REQUIRE(bound != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::OpenCommandOutput>(bound->at(0)));
    auto const& open = std::get<contour::actions::OpenCommandOutput>(bound->at(0));
    CHECK(open.target == vtbackend::CommandBlockTarget::Last);
    CHECK(open.program.empty()); // command_blocks.pager
    CHECK(open.placement == contour::actions::OutputPlacement::Split);

    // Exactly the chord: not Ctrl+G, which the shell owns.
    CHECK(contour::config::applyBuiltinFallback(config, U'G', Modifiers { Modifier::Control }, uint8_t { 0 })
          == nullptr);

    // A fallback, not a default -- it must reach users whose contour.yml lists the defaults.
    CHECK(std::ranges::none_of(config.inputMappings.value().charMappings,
                               [](auto const& mapping) { return mapping.input == U'G'; }));
}

// {{{ notify_on_command_finish

TEST_CASE("Config: notify_on_command_finish is on by default for unfocused commands of ten seconds",
          "[config][notification]")
{
    auto const policy = contour::config::TerminalProfile {}.notifyOnCommandFinish.value();
    CHECK(policy.when == contour::config::NotifyWhen::Unfocused);
    CHECK(policy.minDuration == std::chrono::seconds { 10 });
    CHECK(policy.outcome == contour::config::NotifyOutcome::Any);
    CHECK(policy.action == contour::config::NotifyAction::Notify);
    CHECK(policy.clearOn == contour::config::NotifyClearOn::Focus);
}

TEST_CASE("Config: notify_on_command_finish loads every key from a profile", "[config][notification]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        notify_on_command_finish:
            when: hidden
            min_duration: 42
            outcome: failure
            action: notify_bell
            clear_on: next
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const expected = contour::config::FinishNotificationConfig {
        .when = contour::config::NotifyWhen::Hidden,
        .minDuration = std::chrono::seconds { 42 },
        .outcome = contour::config::NotifyOutcome::Failure,
        .action = contour::config::NotifyAction::NotifyBell,
        .clearOn = contour::config::NotifyClearOn::Next,
    };
    CHECK(profile->notifyOnCommandFinish.value() == expected);
}

TEST_CASE("Config: an unknown notify_on_command_finish value keeps the default and is reported",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        notify_on_command_finish:
            when: sometimes
            min_duration: -5
            action: shout
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& policy = profile->notifyOnCommandFinish.value();
    CHECK(policy.when == contour::config::NotifyWhen::Unfocused);
    CHECK(policy.action == contour::config::NotifyAction::Notify);
    // A negative floor reads as "every command", which is what zero already says.
    CHECK(policy.minDuration == std::chrono::seconds { 0 });
    CHECK(capture.text().contains("sometimes"));
    CHECK(capture.text().contains("shout"));
}

TEST_CASE("Config: a min_duration that is no number keeps the default and is reported",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        notify_on_command_finish:
            min_duration: soon
            clear_on: next
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& policy = profile->notifyOnCommandFinish.value();
    CHECK(policy.minDuration == std::chrono::seconds { 10 });
    // One bad value costs only itself: the keys after it are still read.
    CHECK(policy.clearOn == contour::config::NotifyClearOn::Next);
    CHECK(capture.text().contains("soon"));
}

TEST_CASE("Config: a value of the wrong shape costs only its own entry", "[config][notification]")
{
    // yaml-cpp throws when a value has another shape than its reader expects. Uncaught, that throw ended
    // the whole load, and every entry read after it silently kept its default.
    for (auto const& testCase: MalformedEntryCases)
    {
        INFO("key: " << testCase.key);
        QTemporaryDir dir;
        auto capture = core::log::ScopedCapture { "error" };
        auto const config = loadFromYaml(dir,
                                         std::format(R"(default_profile: main
{}profiles:
    main:
        shell: /bin/sh
{}        terminal_id: VT420
    second:
        shell: /bin/sh
layouts:
    work:
        tabs:
            - title: "editor"
input_mapping:
    - {{ mods: [Control], key: 'J', action: ScreenshotVT }}
)",
                                                     testCase.topLevel,
                                                     testCase.inProfile));

        CHECK(testCase.keptDefault(config));
        // Read after the malformed entry: in the same profile, in another profile, and at the top level.
        CHECK(mainProfile(config).terminalId.value() == vtbackend::VTType::VT420);
        CHECK(config.findProfile("second") != nullptr);
        CHECK(config.layouts.value().contains("work"));
        CHECK(config.inputMappings.value().charMappings.size() == 1);

        INFO("captured error log:\n" << capture.text());
        CHECK(capture.text().contains(testCase.key));
    }
}

TEST_CASE("Config: a malformed entry is reported by its key path and its place in the file", "[config]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(default_profile: main
profiles:
    main:
        cursor: block
        notify_on_command_finish:
            when: [always]
        search_case_sensitivity: [sensitive]
)");

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("profiles.main.cursor"));
    CHECK(capture.text().contains("line 4, column 17"));
    CHECK(capture.text().contains("profiles.main.notify_on_command_finish.when (line 6, column 19)"));
    CHECK(capture.text().contains("profiles.main.search_case_sensitivity (line 7, column 34)"));
}

TEST_CASE("Config: a malformed map of a colour scheme is reported where the scheme is written", "[config]")
{
    // The profile only names the scheme: the map is somewhere else, and that is where to look for it.
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    SECTION("in a scheme file: by the file and the key path in it")
    {
        writeSideFile(dir, "colorschemes/broken.yml", "gutter_text: '#123456'\nsticky_header: '#202830'\n");
        std::ignore = loadFromYaml(dir, R"(default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: broken
)");
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.text().contains("broken.yml: sticky_header"));
        CHECK(capture.text().contains("line 2, column 16"));
        CHECK_FALSE(capture.text().contains("profiles.main.colors"));
    }

    SECTION("in the configuration: by the key path from its root")
    {
        std::ignore = loadFromYaml(dir, R"(default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: broken
color_schemes:
    broken:
        sticky_header: '#202830'
)");
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.text().contains("color_schemes.broken.sticky_header"));
        CHECK(capture.text().contains("line 8, column 24"));
    }
}

TEST_CASE("Config: after a colour scheme's file, the profile's entries are reported by its key path again",
          "[config]")
{
    // The scheme's file is read with the key path rooted at that file, and the profile's own path is put
    // back afterwards: an entry the profile sets after `colors` is found in contour.yml, not in the file.
    QTemporaryDir dir;
    writeSideFile(dir, "colorschemes/broken.yml", "sticky_header: '#202830'\n");
    auto capture = core::log::ScopedCapture { "error" };
    std::ignore = loadFromYaml(dir, R"(default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: broken
        hint_scrollback_lines: [x]
)");

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().contains("broken.yml: sticky_header"));
    CHECK(capture.text().contains("profiles.main.hint_scrollback_lines: yaml-cpp: error at line 6"));
}

TEST_CASE("Config: notify_on_command_finish round-trips through a profile side file",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto const source = loadFromYaml(dir, "default_profile: main\n");
    auto const* base = source.findProfile("main");
    REQUIRE(base != nullptr);
    auto profile = *base;
    auto const policy = contour::config::FinishNotificationConfig {
        .when = contour::config::NotifyWhen::Always,
        .minDuration = std::chrono::seconds { 3 },
        .outcome = contour::config::NotifyOutcome::Failure,
        .action = contour::config::NotifyAction::Bell,
        .clearOn = contour::config::NotifyClearOn::Never,
    };
    profile.notifyOnCommandFinish = policy;

    writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

    auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
    auto const* roundTripped = reloaded.findProfile("roundtrip");
    REQUIRE(roundTripped != nullptr);
    CHECK(roundTripped->notifyOnCommandFinish.value() == policy);
}

TEST_CASE("Config: the generated config spells out notify_on_command_finish and reads it back",
          "[config][notification]")
{
    QTemporaryDir dir;
    auto const rendered = contour::config::defaultConfigString();
    CHECK(rendered.contains("notify_on_command_finish:"));
    CHECK(rendered.contains("when: unfocused"));
    CHECK(rendered.contains("min_duration: 10"));
    CHECK(rendered.contains("clear_on: focus"));

    auto const defaults = contour::config::Config {};
    auto const reloaded = loadFromYaml(dir, rendered);
    auto const* profile = reloaded.profile(defaults.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    CHECK(profile->notifyOnCommandFinish.value() == contour::config::FinishNotificationConfig {});
}

TEST_CASE("Config: every notify_on_command_finish value has one token that reads back",
          "[config][notification]")
{
    using namespace contour::config;
    checkEnumerators(
        std::array { NotifyWhen::Never, NotifyWhen::Unfocused, NotifyWhen::Hidden, NotifyWhen::Always });
    checkEnumerators(std::array { NotifyOutcome::Any, NotifyOutcome::Failure });
    checkEnumerators(std::array { NotifyAction::Notify, NotifyAction::Bell, NotifyAction::NotifyBell });
    checkEnumerators(std::array { NotifyClearOn::Focus, NotifyClearOn::Next, NotifyClearOn::Never });
}

// }}}

// {{{ sticky_header

TEST_CASE("Config: every sticky header mode is described by its table", "[config][stickyheader]")
{
    using namespace contour::config;
    using vtbackend::StickyHeaderMode;

    checkEnumerators(
        std::array { StickyHeaderMode::Never, StickyHeaderMode::Scrolled, StickyHeaderMode::Always });
    // The spellings the documentation promises, and the reader's case-insensitivity.
    CHECK(configEnumToken(StickyHeaderMode::Never) == "never");
    CHECK(configEnumToken(StickyHeaderMode::Scrolled) == "scrolled");
    CHECK(configEnumToken(StickyHeaderMode::Always) == "always");
    CHECK(configEnumFromToken<StickyHeaderMode>("ALWAYS") == StickyHeaderMode::Always);
}

TEST_CASE("Config: sticky_header loads from YAML and reaches the session settings", "[config][stickyheader]")
{
    QTemporaryDir dir;

    SECTION("defaults: scrolled, with the evicted placeholder shown")
    {
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Scrolled);
        CHECK(profile->stickyHeader.value().showEvicted);
        CHECK(contour::config::sessionSettings(
                  config, *profile, vtbackend::ColorPreference::Dark, vtbackend::BlockColumnForm::Glyphs)
                  .stickyHeader
              == vtbackend::StickyHeaderSettings {});
    }

    SECTION("explicit values reach vtbackend, the YAML bool as the engine's enum")
    {
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        sticky_header:
            mode: always
            show_evicted: false
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(contour::config::sessionSettings(
                  config, *profile, vtbackend::ColorPreference::Dark, vtbackend::BlockColumnForm::Glyphs)
                  .stickyHeader
              == vtbackend::StickyHeaderSettings {
                  .mode = vtbackend::StickyHeaderMode::Always,
                  .evictedPlaceholder = vtbackend::EvictedPlaceholder::Hidden,
                  .presentation = vtbackend::StickyHeaderPresentation::InGrid });
    }

    SECTION("an unknown mode keeps the default, and its sibling still loads")
    {
        auto capture = core::log::ScopedCapture { "error" };
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        sticky_header:
            mode: sometimes
            show_evicted: false
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Scrolled);
        CHECK_FALSE(profile->stickyHeader.value().showEvicted);
        CHECK(capture.text().contains("sometimes"));
    }

    SECTION("a profile round-trips through the writer")
    {
        auto const source = loadFromYaml(dir, "default_profile: main\n");
        auto const* base = source.findProfile("main");
        REQUIRE(base != nullptr);
        auto profile = *base;
        auto const written = contour::config::StickyHeaderConfig { .mode = vtbackend::StickyHeaderMode::Never,
                                                                   .showEvicted = false };
        profile.stickyHeader = written;
        writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

        auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
        auto const* roundTripped = reloaded.findProfile("roundtrip");
        REQUIRE(roundTripped != nullptr);
        CHECK(roundTripped->stickyHeader.value() == written);
    }
}

TEST_CASE("Config: the generated config spells out sticky_header and reads it back", "[config][stickyheader]")
{
    QTemporaryDir dir;
    auto const rendered = contour::config::defaultConfigString();

    // The profile key's own lines -- not the colour scheme's commented example, which names the key too.
    auto const key = rendered.find("\n        sticky_header:\n");
    REQUIRE(key != std::string::npos);
    auto const section = std::string_view { rendered }.substr(key, rendered.find("\n\n", key + 1) + 1 - key);
    CHECK(section.contains("\n            mode: scrolled\n"));
    CHECK(section.contains("\n            show_evicted: true\n"));
    // What a header needs: the prompt's own mark. An integration that marks only the command gets none.
    CHECK(section.contains("Needs prompt marks"));
    CHECK(section.contains("(OSC 133;A)"));

    auto const defaults = contour::config::Config {};
    auto const reloaded = loadFromYaml(dir, rendered);
    auto const* profile = reloaded.profile(defaults.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    CHECK(profile->stickyHeader.value() == contour::config::StickyHeaderConfig {});
}

TEST_CASE("Config: the sticky header's colors load and are written back", "[config][stickyheader]")
{
    QTemporaryDir dir;

    SECTION("unset by default, so both derive from the scheme")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, schemeWithStickyHeader("")));
        CHECK_FALSE(scheme.stickyHeaderBackground.has_value());
        CHECK_FALSE(scheme.stickyHeaderSeparator.has_value());
    }

    SECTION("both slots load")
    {
        auto const scheme =
            schemeOf(loadFromYaml(dir,
                                  schemeWithStickyHeader("        sticky_header:\n"
                                                         "            background: '#202830'\n"
                                                         "            separator: '#506070'\n")));
        CHECK(scheme.stickyHeaderBackground == vtbackend::RGBColor(0x20, 0x28, 0x30));
        CHECK(scheme.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
    }

    SECTION("one slot alone loads, and the other stays unset")
    {
        auto const scheme =
            schemeOf(loadFromYaml(dir,
                                  schemeWithStickyHeader("        sticky_header:\n"
                                                         "            separator: '#506070'\n")));
        CHECK_FALSE(scheme.stickyHeaderBackground.has_value());
        CHECK(scheme.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
    }

    SECTION("the writer emits what was set, and only that")
    {
        auto config = classicConfig();
        config.colorschemes.value()["default"].stickyHeaderSeparator = vtbackend::RGBColor(0x50, 0x60, 0x70);

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        // The commented example stands in for a map the scheme does not set, and only for that.
        CHECK_FALSE(written.contains("# sticky_header:"));

        auto const reloaded = schemeOf(loadFromYaml(dir, written));
        CHECK(reloaded.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
        // Writing back a derived colour would pin what the scheme deliberately left to the palette.
        CHECK_FALSE(reloaded.stickyHeaderBackground.has_value());
    }

    SECTION("a colour scheme file carries both through the side-file loader")
    {
        auto palette = vtbackend::ColorPalette {};
        palette.stickyHeaderBackground = vtbackend::RGBColor(0x20, 0x28, 0x30);
        palette.stickyHeaderSeparator = vtbackend::RGBColor(0x50, 0x60, 0x70);
        auto const written = contour::config::emitColorSchemeYaml(palette);
        REQUIRE(written.contains("\nsticky_header:\n"));
        writeSideFile(dir, "colorschemes/banded.yml", written);

        auto const reloaded = schemeOf(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: banded
)"sv));
        CHECK(reloaded.stickyHeaderBackground == vtbackend::RGBColor(0x20, 0x28, 0x30));
        CHECK(reloaded.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
    }
}

TEST_CASE("Config: the sticky header colors the generated config quotes are what the default scheme derives",
          "[config][stickyheader]")
{
    // The commented example says so, so it is held to it here: retuning a lift in ColorPalette fails this
    // rather than leaving the generated config quoting the old colour.
    auto const generated = contour::config::defaultConfigString();
    auto const palette = vtbackend::ColorPalette {};
    CHECK(generated.contains("# sticky_header:\n"));
    CHECK(generated.contains(std::format("#     background: {}\n", palette.stickyHeaderBackgroundColor())));
    CHECK(generated.contains(std::format("#     separator: {}\n", palette.stickyHeaderSeparatorColor())));
}

TEST_CASE("Config: the generated config and the reference docs carry every command-block key", "[config]")
{
    // A key that `contour generate config` does not write, or the website does not document, is a key
    // nobody discovers. Each section's leaves are listed so a dropped one names itself.
    auto const text = contour::config::defaultConfigString();
    auto const generated = YAML::Load(text);
    auto const defaults = contour::config::Config {};
    auto const profile = generated["profiles"][defaults.defaultProfileName.value()];
    auto const globalDoc = contour::config::documentationGlobalConfig();
    auto const profileDoc = contour::config::documentationProfileConfig();

    using Sections = std::vector<std::pair<std::string, std::vector<std::string>>>;
    auto const globalSections = Sections {
        { "gutter",
          { "exit_status",
            "user_marks",
            "block_column_width",
            "bold_on_hover",
            "line_numbers",
            "line_number_width",
            "timestamps",
            "timestamp_format" } },
        { "command_blocks", { "max_records", "pager", "pager_format" } },
        { "folding", { "enabled", "show_markers", "auto_collapse_on_new_command", "on_jump_into_fold" } },
    };
    auto const profileSections = Sections {
        { "scrollbar", { "marks" } },
        { "sticky_header", { "mode", "show_evicted" } },
        { "notify_on_command_finish", { "when", "min_duration", "outcome", "action", "clear_on" } },
    };
    auto const checkSections = [](std::string_view scope,
                                  YAML::Node const& written,
                                  std::string_view reference,
                                  Sections const& sections) {
        for (auto const& [section, leaves]: sections)
            for (auto const& leaf: leaves)
            {
                INFO(scope << ": " << section << "." << leaf);
                CHECK(hasPath(written, std::array { section, leaf }));
                CHECK(referenceEntry(reference, section).contains(std::format("=={}==", leaf)));
            }
    };
    checkSections("global", generated, globalDoc, globalSections);
    checkSections("profile", profile, profileDoc, profileSections);

    // The colour-scheme slots are optional, so the generated scheme carries them as commented examples, as
    // it does fold_marker: present in the text, absent from the parsed tree.
    auto const schemes = text.substr(text.find("\ncolor_schemes:"));
    for (auto const* key: { "block_status", "block_status_hover", "gutter_text", "sticky_header" })
    {
        INFO("generated colour scheme: " << key);
        CHECK(schemes.contains(key));
    }

    // The scrollbar is shown on the right by default: the generated config spells the default out, and the
    // profile reference -- docs/configuration/profiles.md -- shows and states it.
    CHECK(profile["scrollbar"]["position"].as<std::string>() == "Right");
    CHECK(profileDoc.contains("position: Right"));
    CHECK(profileDoc.contains("The default is Right"));
}

TEST_CASE("Config: link_detection defaults to paths on the primary screen only", "[config][links]")
{
    QTemporaryDir dir;
    auto const value = linkDetectionOf(dir, "");
    CHECK(value == contour::config::LinkDetectionConfig {});
    CHECK(contour::config::linkDetectionPolicyOf(value) == vtbackend::LinkDetectionPolicy {});
}

TEST_CASE("Config: link_detection reads every switch", "[config][links]")
{
    QTemporaryDir dir;
    auto const value = linkDetectionOf(dir,
                                       "        link_detection:\n"
                                       "            primary_screen:\n"
                                       "                urls: false\n"
                                       "                file_paths: false\n"
                                       "            other_screens:\n"
                                       "                urls: false\n"
                                       "                file_paths: true\n");
    CHECK_FALSE(value.primaryScreen.urls);
    CHECK_FALSE(value.primaryScreen.filePaths);
    CHECK_FALSE(value.otherScreens.urls);
    CHECK(value.otherScreens.filePaths);
}

TEST_CASE("Config: a partial link_detection keeps the other defaults", "[config][links]")
{
    QTemporaryDir dir;
    auto const value = linkDetectionOf(dir,
                                       "        link_detection:\n"
                                       "            other_screens:\n"
                                       "                file_paths: true\n");
    CHECK(value.primaryScreen
          == contour::config::ScreenLinkDetectionConfig { .urls = true, .filePaths = true });
    CHECK(value.otherScreens.urls);
    CHECK(value.otherScreens.filePaths);
}

TEST_CASE("Config: link_detection round-trips through the writer", "[config][links]")
{
    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("link_detection:"));
    CHECK(generated.contains("other_screens:"));
    CHECK(generated.contains("file_paths: false"));

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, generated);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->linkDetection.value() == contour::config::LinkDetectionConfig {});
}

TEST_CASE("Config: a non-default link_detection round-trips through the writer", "[config][links]")
{
    // Order-sensitive on purpose: the screens differ, and so do a screen's two switches, so a writer or a
    // reader that exchanges urls with file_paths, or primary_screen with other_screens, is caught.
    constexpr auto Chosen =
        contour::config::LinkDetectionConfig { .primaryScreen = { .urls = false, .filePaths = true },
                                               .otherScreens = { .urls = true, .filePaths = false } };
    QTemporaryDir dir;
    auto configured = loadFromYaml(dir, "default_profile: main\n");
    auto* const profile = configured.profile("main");
    REQUIRE(profile != nullptr);
    profile->linkDetection.value() = Chosen;
    REQUIRE(profile->linkDetection.value() != contour::config::LinkDetectionConfig {});

    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(configured));
    auto const* const reloadedProfile = reloaded.profile("main");
    REQUIRE(reloadedProfile != nullptr);
    CHECK(reloadedProfile->linkDetection.value() == Chosen);
}

TEST_CASE("Config: a profile inherits link_detection keys it leaves out", "[config][links]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        link_detection:
            primary_screen:
                urls: false
    alt:
        link_detection:
            other_screens:
                file_paths: true
)");
    auto const* alt = config.profile("alt");
    REQUIRE(alt != nullptr);
    auto const value = alt->linkDetection.value();
    CHECK_FALSE(value.primaryScreen.urls); // inherited from the default profile
    CHECK(value.primaryScreen.filePaths);  // the default
    CHECK(value.otherScreens.urls);        // the default
    CHECK(value.otherScreens.filePaths);   // its own
}

TEST_CASE("Config: sessionSettings carries the link detection policy", "[config][links]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        link_detection:
            primary_screen:
                urls: false
)");
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings = contour::config::sessionSettings(
        config, *profile, vtbackend::ColorPreference::Dark, vtbackend::BlockColumnForm::Glyphs);
    CHECK(settings.linkDetection == contour::config::linkDetectionPolicyOf(profile->linkDetection.value()));
    CHECK_FALSE(settings.linkDetection.primaryScreen.urls == vtbackend::DetectLinks::Yes);
}

TEST_CASE("Config: hyperlink_decoration accepts the spelling the documentation used",
          "[config][hyperlink-decoration]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    // `dotted` is what the documentation showed for years. The hover default is Underline, so the
    // value read back can only have come from the file.
    auto const decorators = hyperlinkDecorationOf(dir,
                                                  "            normal: curly-underline\n"
                                                  "            hover: dotted\n");
    CHECK(decorators.normal == vtrasterizer::Decorator::CurlyUnderline);
    CHECK(decorators.hover == vtrasterizer::Decorator::DottedUnderline);
    CHECK_FALSE(capture.contains("hyperlink_decoration"));
}

TEST_CASE("Config: every decorator name parses, whatever its case", "[config][hyperlink-decoration]")
{
    for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
    {
        QTemporaryDir dir;
        auto capture = core::log::ScopedCapture { "error" };

        auto upper = std::string { token };
        std::ranges::transform(
            upper, upper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

        INFO("token: " << token);
        CHECK(hyperlinkDecorationOf(dir, std::format("            hover: {}\n", token)).hover == decorator);
        CHECK(hyperlinkDecorationOf(dir, std::format("            hover: {}\n", upper)).hover == decorator);
        CHECK_FALSE(capture.contains("hyperlink_decoration"));
    }
}

TEST_CASE("Config: an unknown hyperlink decorator is reported and keeps its default",
          "[config][hyperlink-decoration]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    auto const decorators = hyperlinkDecorationOf(dir,
                                                  "            normal: wavy\n"
                                                  "            hover: framed\n");

    // The bad value costs only its own key: normal keeps the default, hover still loads.
    CHECK(decorators.normal == contour::config::HyperlinkDecorationConfig {}.normal);
    CHECK(decorators.hover == vtrasterizer::Decorator::Framed);

    INFO("captured error log:\n" << capture.text());
    CHECK(capture.contains("wavy"));
    CHECK(capture.contains("hyperlink_decoration.normal"));
    for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
        CHECK(capture.contains(token));
}

TEST_CASE("Config: the decorator names the documentation lists are the ones the parser accepts",
          "[config][hyperlink-decoration]")
{
    namespace doc = contour::config::documentation;
    auto const text = std::string_view { doc::HyperlinkDecorationWeb.value };

    constexpr auto Marker = "Possible values: "sv;
    auto const begin = text.find(Marker);
    REQUIRE(begin != std::string_view::npos);
    auto const start = begin + Marker.size();
    auto const line = text.substr(start, text.find('\n', start) - start);

    auto listed = std::vector<std::string_view> {};
    for (auto const word: std::views::split(line, ", "sv))
    {
        auto const name = std::string_view { word.begin(), word.end() };
        // The sentence ends in a remark ("encircle (if implemented)"): the name is the first word.
        listed.push_back(name.substr(0, name.find(' ')));
    }

    // Every listed name parses ...
    for (auto const name: listed)
    {
        INFO("documented name: " << name);
        CHECK(vtrasterizer::toDecorator(name).has_value());
    }
    // ... and every name the parser prints is listed.
    for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
    {
        INFO("table name: " << token);
        CHECK(std::ranges::find(listed, token) != listed.end());
    }

    // The example's own values must be among them.
    CHECK(text.contains("normal: dotted-underline"));
    CHECK(text.contains("hover: underline"));
}

TEST_CASE("Config: input_method_editor_style follows the edition, reads and writes back", "[config][ime]")
{
    using vtbackend::PreeditStyle;

    SECTION("a Classic profile keeps the highlighted preedit, a generated one is inline")
    {
        CHECK(
            contour::config::TerminalProfile { .defaultsEdition = contour::config::DefaultsEdition::Classic }
                .inputMethodEditorStyle.value()
            == PreeditStyle::Highlighted);
        CHECK(contour::config::defaultConfigString().contains("input_method_editor_style: inline\n"));
        QTemporaryDir dir;
        auto const generated = loadFromYaml(dir, contour::config::defaultConfigString());
        auto const* profile = generated.profile(generated.defaultProfileName.value());
        REQUIRE(profile != nullptr);
        CHECK(profile->inputMethodEditorStyle.value() == PreeditStyle::Inline);
    }

    SECTION("highlighted in a Native2026 file is read, and written back as such")
    {
        QTemporaryDir dir;
        auto const config = loadFromYaml(
            dir,
            "defaults_edition: native2026\nprofiles:\n  main:\n    input_method_editor_style: highlighted\n"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->inputMethodEditorStyle.value() == PreeditStyle::Highlighted);
        CHECK(contour::config::sessionSettings(
                  config, *profile, vtbackend::ColorPreference::Dark, vtbackend::BlockColumnForm::Glyphs)
                  .preeditStyle
              == PreeditStyle::Highlighted);

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        CHECK(written.contains("input_method_editor_style: highlighted\n"));
        auto const reread = loadFromYaml(dir, written);
        REQUIRE(reread.profile("main") != nullptr);
        CHECK(reread.profile("main")->inputMethodEditorStyle.value() == PreeditStyle::Highlighted);
    }

    SECTION("an unknown spelling is reported and keeps the edition's value")
    {
        auto const capture = core::log::ScopedCapture { "error" };
        QTemporaryDir dir;
        auto const config = loadFromYaml(
            dir, "defaults_edition: native2026\nprofiles:\n  main:\n    input_method_editor_style: loud\n"sv);
        INFO("captured error log:\n" << capture.text());
        CHECK(capture.contains("'loud'"));
        CHECK(capture.contains("input_method_editor_style"));
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->inputMethodEditorStyle.value() == PreeditStyle::Inline);
    }
}

TEST_CASE("Config: hyperlink_decoration.mode reads, writes and defaults", "[config][links]")
{
    QTemporaryDir dir;
    auto const config =
        loadFromYaml(dir, "profiles:\n  main:\n    hyperlink_decoration:\n      mode: modifier\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->hyperlinkDecoration.value().mode == vtbackend::LinkDecorationMode::Modifier);
    CHECK(contour::config::TerminalProfile {}.hyperlinkDecoration.value().mode
          == vtbackend::LinkDecorationMode::Modifier);
    CHECK(contour::config::TerminalProfile { .defaultsEdition = contour::config::DefaultsEdition::Classic }
              .hyperlinkDecoration.value()
              .mode
          == vtbackend::LinkDecorationMode::Always);

    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("mode: modifier"));
    CHECK(generated.contains("normal: dotted-underline"));
}

TEST_CASE("Config: an unknown hyperlink_decoration.mode is reported and keeps the default", "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config =
        loadFromYaml(dir, "profiles:\n  main:\n    hyperlink_decoration:\n      mode: sometimes\n"sv);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.contains("'sometimes'"));
    CHECK(capture.contains("hyperlink_decoration.mode"));
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->hyperlinkDecoration.value().mode == vtbackend::LinkDecorationMode::Modifier); // Native2026
}

TEST_CASE("Config: the generated hyperlink_decoration comment lists the names the parser accepts",
          "[config][links]")
{
    namespace doc = contour::config::documentation;
    auto const text = std::string_view { doc::HyperlinkDecorationConfig.value };

    // The decorator names are the sentence after "under the pointer: ", up to its full stop, spread over
    // comment lines.
    constexpr auto Marker = "under the pointer: "sv;
    auto const begin = text.find(Marker);
    REQUIRE(begin != std::string_view::npos);
    auto const start = begin + Marker.size();
    auto const sentence = text.substr(start, text.find('.', start) - start);

    auto listed = std::vector<std::string> {};
    for (auto const line: std::views::split(sentence, '\n'))
    {
        auto words = std::string_view { line.begin(), line.end() };
        words.remove_prefix(words.starts_with("{comment} ") ? std::string_view { "{comment} " }.size() : 0);
        for (auto const word: std::views::split(words, ","sv))
        {
            auto name = std::string { word.begin(), word.end() };
            std::erase(name, ' ');
            if (!name.empty())
                listed.push_back(std::move(name));
        }
    }

    for (auto const& [token, decorator]: vtrasterizer::DecoratorNames)
    {
        INFO("table name: " << token);
        CHECK(std::ranges::find(listed, std::string { token }) != listed.end());
    }
    for (auto const& name: listed)
    {
        INFO("documented name: " << name);
        CHECK(vtrasterizer::toDecorator(name).has_value());
    }

    // Every row of the mode table has its line: "{comment}   <token> - ...".
    for (auto const& row: contour::config::configEnumValues<vtbackend::LinkDecorationMode>())
    {
        INFO("mode: " << row.token);
        CHECK(text.contains(std::format("{{comment}}   {} ", row.token)));
    }
}

TEST_CASE("Config: a profile without hint patterns shares the built-in link table", "[config][links]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, "profiles:\n  main:\n    shell: /bin/sh\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(contour::config::linkPatternsFor(*profile) == vtbackend::LinkPatterns::builtins());
}

TEST_CASE("Config: a broken hint pattern is reported once at load, naming it", "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(
        dir,
        "profiles:\n  main:\n    shell: /bin/sh\n    hint_patterns:\n      - name: bad\n        regex: '([a-'\n"sv);
    INFO("captured error log:\n" << capture.text());
    auto const& text = capture.text();
    auto const first = text.find("'bad'");
    REQUIRE(first != std::string::npos);
    CHECK(text.find("'bad'", first + 1) == std::string::npos);
    CHECK(text.contains("'main'"));

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(contour::config::linkPatternsFor(*profile)->find("bad") == nullptr);
}

TEST_CASE("Config: a profile inheriting a broken hint pattern does not report it again", "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\nprofiles:\n  main:\n    shell: /bin/sh\n"
                                     "    hint_patterns:\n      - name: bad\n        regex: '([a-'\n"
                                     "  other:\n    shell: /bin/sh\n"sv);
    auto const& text = capture.text();
    auto const first = text.find("'bad'");
    REQUIRE(first != std::string::npos);
    CHECK(text.find("'bad'", first + 1) == std::string::npos);
}

TEST_CASE("Config: sessionSettings hands the terminal the profile's compiled link table", "[config][links]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(
        dir,
        "profiles:\n  main:\n    shell: /bin/sh\n    hint_patterns:\n      - name: jira\n        regex: '\\b[A-Z]+-\\d+\\b'\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings = contour::config::sessionSettings(
        config, *profile, vtbackend::ColorPreference::Dark, vtbackend::BlockColumnForm::Glyphs);
    REQUIRE(settings.linkPatterns != nullptr);
    CHECK(settings.linkPatterns->find("jira") != nullptr);
    CHECK(settings.linkPatterns != vtbackend::LinkPatterns::builtins());
}

// }}}

TEST_CASE("Config: the follow gesture's modifiers come from the mouse bindings", "[config][links]")
{
    auto const defaults = contour::config::Config {};
#ifdef __APPLE__
    CHECK(contour::config::followHyperlinkModifiers(defaults)
          == vtbackend::Modifiers { vtbackend::Modifier::Super }); // Command, under the Native2026 defaults
#else
    CHECK(contour::config::followHyperlinkModifiers(defaults)
          == vtbackend::Modifiers { vtbackend::Modifier::Control });
#endif
    auto const classic =
        contour::config::Config { .defaultsEdition = contour::config::DefaultsEdition::Classic };
    CHECK(contour::config::followHyperlinkModifiers(classic)
          == vtbackend::Modifiers { vtbackend::Modifier::Control });

    auto const alt = contour::test::loadConfigFromYaml(
        "input_mapping:\n  - { mods: [Alt], mouse: Left, action: FollowHyperlink }\n");
    CHECK(contour::config::followHyperlinkModifiers(alt)
          == vtbackend::Modifiers { vtbackend::Modifier::Alt });
}

TEST_CASE("Config: no mouse binding that follows links leaves no follow modifiers", "[config][links]")
{
    auto const none = contour::test::loadConfigFromYaml(
        "input_mapping:\n  - { mods: [Alt], mouse: Left, action: PasteClipboard }\n");
    CHECK(contour::config::followHyperlinkModifiers(none) == std::nullopt);
}

TEST_CASE("Config: sessionSettings carries the follow modifiers", "[config][links]")
{
    QTemporaryDir dir;
    auto const config =
        loadFromYaml(dir,
                     "profiles:\n  main:\n    shell: /bin/sh\n"
                     "input_mapping:\n  - { mods: [Alt], mouse: Left, action: FollowHyperlink }\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings = contour::config::sessionSettings(
        config, *profile, vtbackend::ColorPreference::Dark, vtbackend::BlockColumnForm::Glyphs);
    CHECK(settings.linkFollowModifiers
          == std::optional { vtbackend::Modifiers { vtbackend::Modifier::Alt } });
}

TEST_CASE("Config: mode modifier without a follow binding is one diagnostic naming every such profile",
          "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    // "other" sets no mode and inherits modifier from the default profile; "plain" says always.
    auto const config =
        loadFromYaml(dir,
                     "default_profile: main\n"
                     "profiles:\n"
                     "  main:\n    shell: /bin/sh\n    hyperlink_decoration:\n      mode: modifier\n"
                     "  other:\n    shell: /bin/sh\n"
                     "  plain:\n    shell: /bin/sh\n    hyperlink_decoration:\n      mode: always\n"
                     "input_mapping:\n  - { mods: [Alt], mouse: Left, action: PasteClipboard }\n"sv);
    INFO("captured error log:\n" << capture.text());
    auto const& text = capture.text();
    auto const first = text.find("hyperlink_decoration.mode is modifier");
    REQUIRE(first != std::string::npos);
    CHECK(text.find("hyperlink_decoration.mode is modifier", first + 1) == std::string::npos);
    CHECK(text.contains("in profiles main, other but no FollowHyperlink mouse binding exists"));
    CHECK_FALSE(text.contains("plain"));
}

TEST_CASE("Config: mode modifier with a follow binding is not reported", "[config][links]")
{
    auto const capture = core::log::ScopedCapture { "error" };
    QTemporaryDir dir;
    auto const config = loadFromYaml(
        dir,
        "profiles:\n  main:\n    shell: /bin/sh\n    hyperlink_decoration:\n      mode: modifier\n"
        "input_mapping:\n  - { mods: [Alt], mouse: Left, action: FollowHyperlink }\n"sv);
    INFO("captured error log:\n" << capture.text());
    CHECK_FALSE(capture.contains("hyperlink_decoration.mode is modifier"));
}

TEST_CASE("Config: file_opener reads a string or a map, and rejects an unknown placeholder",
          "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    auto const asString = fileOpenerOf(dir, " 'code --goto {path}:{line}'");
    CHECK(asString.command == "code --goto {path}:{line}");
    CHECK(asString.placement == contour::actions::OutputPlacement::Detached);

    auto const asMap = fileOpenerOf(dir,
                                    "\n"
                                    "            command: 'nvim +{line} {path}'\n"
                                    "            placement: split\n");
    CHECK(asMap.command == "nvim +{line} {path}");
    CHECK(asMap.placement == contour::actions::OutputPlacement::Split);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().empty());

    // Reported, and kept as written: it is disabled where a file is opened, so that a save of the profile
    // writes back what the user wrote rather than nothing.
    auto const bad = fileOpenerOf(dir, " 'code {file}'");
    CHECK(bad.command == "code {file}");
    CHECK(capture.contains("file_opener 'code {file}': an unknown placeholder"));
}

TEST_CASE("Config: an invalid file_opener survives the writer and the reader as written", "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "    main:\n"
                                     "        shell: /bin/sh\n"
                                     "        file_opener: 'code {file}'\n"sv);
    REQUIRE(config.profile("main") != nullptr);
    REQUIRE(config.profile("main")->fileOpener.value().command == "code {file}");

    QTemporaryDir again;
    auto const reloaded =
        loadFromYaml(again, contour::config::createString<contour::config::YAMLConfigWriter>(config));
    REQUIRE(reloaded.profile("main") != nullptr);
    CHECK(reloaded.profile("main")->fileOpener.value().command == "code {file}");
}

TEST_CASE("Config: file_opener's string form runs detached, whatever the profile inherited",
          "[config][links]")
{
    // A profile starts as a copy of the default one: the string form names its command alone, which runs
    // detached.
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "    main:\n"
                                     "        shell: /bin/sh\n"
                                     "        file_opener:\n"
                                     "            command: 'nvim +{line} {path}'\n"
                                     "            placement: split\n"
                                     "    other:\n"
                                     "        shell: /bin/sh\n"
                                     "        file_opener: 'code --goto {path}'\n"sv);
    REQUIRE(config.profile("main") != nullptr);
    REQUIRE(config.profile("other") != nullptr);
    CHECK(config.profile("main")->fileOpener.value().placement == contour::actions::OutputPlacement::Split);
    CHECK(config.profile("other")->fileOpener.value().command == "code --goto {path}");
    CHECK(config.profile("other")->fileOpener.value().placement
          == contour::actions::OutputPlacement::Detached);
}

TEST_CASE("Config: file_opener defaults to the system's default application", "[config][links]")
{
    auto const defaults = contour::config::Config {};
    auto const* profile = defaults.profile(defaults.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    CHECK(profile->fileOpener.value().command.empty());
    CHECK(profile->fileOpener.value().placement == contour::actions::OutputPlacement::Detached);
}

TEST_CASE("Config: the generated config spells out file_opener and reads it back", "[config][links]")
{
    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("file_opener:"));
    CHECK(generated.contains("placement: detached"));
    CHECK(generated.contains("{path}"));

    // A configured opener survives the writer and the reader.
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto config = contour::config::Config {};
    auto* profile = config.profile(config.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    profile->fileOpener =
        contour::config::FileOpenerConfig { .command = "code --goto '{path}:{line}'",
                                            .placement = contour::actions::OutputPlacement::Tab };
    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().empty());
    auto const* back = reloaded.profile(config.defaultProfileName.value());
    REQUIRE(back != nullptr);
    CHECK(back->fileOpener.value().command == "code --goto '{path}:{line}'");
    CHECK(back->fileOpener.value().placement == contour::actions::OutputPlacement::Tab);
}

// }}}

// {{{ hint_patterns: clickable, url, name-only overrides

TEST_CASE("Config: hint_patterns take clickable and url, and a built-in's name alone", "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "  main:\n"
                                     "    shell: /bin/sh\n"
                                     "    hint_patterns:\n"
                                     "      - name: url\n"
                                     "        clickable: false\n"
                                     "      - name: jira\n"
                                     "        regex: '\\b[A-Z]+-\\d+\\b'\n"
                                     "        clickable: true\n"
                                     "        url: 'https://jira.example.com/browse/{0}'\n"sv);
    INFO("captured error log:\n" << capture.text());
    CHECK(capture.text().empty());
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& patterns = profile->hintPatterns.value();
    REQUIRE(patterns.size() == 2);
    CHECK(patterns[0].regex.empty());
    CHECK(patterns[0].clickable == false);
    CHECK_FALSE(patterns[0].url.has_value());
    CHECK(patterns[1].clickable == true);
    CHECK(patterns[1].url == "https://jira.example.com/browse/{0}");

    auto const specs = contour::config::linkPatternSpecs(*profile);
    REQUIRE(specs.size() == 2);
    CHECK(specs[0].regex == std::nullopt);
    CHECK(specs[0].activation == vtbackend::LinkActivation::HintOnly);
    CHECK(specs[1].activation == vtbackend::LinkActivation::Clickable);
    CHECK(specs[1].urlTemplate == "https://jira.example.com/browse/{0}");

    auto const table = contour::config::linkPatternsFor(*profile);
    REQUIRE(table->find("url") != nullptr);
    CHECK(table->find("url")->activation == vtbackend::LinkActivation::HintOnly);
    REQUIRE(table->find("jira") != nullptr);
    CHECK(table->find("jira")->activation == vtbackend::LinkActivation::Clickable);
    CHECK(table->find("jira")->urlTemplate.has_value());
}

TEST_CASE("Config: a name-only hint pattern that names no built-in is skipped and reported",
          "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "  main:\n"
                                     "    shell: /bin/sh\n"
                                     "    hint_patterns:\n"
                                     "      - name: nobody\n"
                                     "        clickable: true\n"sv);
    REQUIRE(config.profile("main") != nullptr);
    CHECK(config.profile("main")->hintPatterns.value().empty());
    CHECK(capture.contains("nobody"));
    CHECK(capture.contains("filepath")); // it names the built-ins a name-only entry may override
}

TEST_CASE("Config: a hint pattern's url naming a group its regex lacks is reported at load",
          "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "  main:\n"
                                     "    shell: /bin/sh\n"
                                     "    hint_patterns:\n"
                                     "      - name: jira\n"
                                     "        regex: '\\b[A-Z]+-\\d+\\b'\n"
                                     "        clickable: true\n"
                                     "        url: 'https://j/{1}'\n"sv);
    REQUIRE(config.profile("main") != nullptr);
    CHECK(capture.contains("jira"));
    CHECK(capture.contains("capture group"));
}

TEST_CASE("Config: hint_patterns' new keys survive a write and a read", "[config][links]")
{
    QTemporaryDir dir;
    auto config = contour::config::Config {};
    auto* const profile = config.profile(config.defaultProfileName.value());
    REQUIRE(profile != nullptr);
    profile->hintPatterns.value() = {
        contour::config::HintPatternConfig { .name = "url", .regex = {}, .clickable = false, .url = {} },
        contour::config::HintPatternConfig {
            .name = "jira", .regex = "[A-Z]+-[0-9]+", .clickable = true, .url = "https://j/{0}" },
    };
    auto const reloaded =
        loadFromYaml(dir, contour::config::createString<contour::config::YAMLConfigWriter>(config));
    auto const* back = reloaded.profile(config.defaultProfileName.value());
    REQUIRE(back != nullptr);
    CHECK(back->hintPatterns.value() == profile->hintPatterns.value());
}

TEST_CASE("Config: the generated config documents clickable patterns", "[config][links]")
{
    auto const generated = contour::config::defaultConfigString();
    CHECK(generated.contains("clickable:"));
    CHECK(generated.contains("url:"));
    CHECK(generated.contains("group 1"));
}

TEST_CASE("Config: a malformed key drops its hint pattern alone, and says which", "[config][links]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir,
                                     "default_profile: main\n"
                                     "profiles:\n"
                                     "  main:\n"
                                     "    shell: /bin/sh\n"
                                     "    hint_patterns:\n"
                                     "      - name: jira\n"
                                     "        regex: '[A-Z]+-[0-9]+'\n"
                                     "        clickable: maybe\n"
                                     "      - name: ticket\n"
                                     "        regex: 'T-[0-9]+'\n"
                                     "        url: [not, a, string]\n"
                                     "      - name: uuid\n"
                                     "        regex: '[0-9a-f]{8}'\n"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const& patterns = profile->hintPatterns.value();
    REQUIRE(patterns.size() == 1);
    CHECK(patterns[0].name == "uuid");
    CHECK(capture.contains("jira"));
    CHECK(capture.contains("ticket"));
}

// }}}

TEST_CASE("Config: motionAdjusted zeroes the motion durations under Reduce Motion and nothing else",
          "[config][motion]")
{
    using namespace std::chrono_literals;
    using contour::config::MotionPreference;

    auto settings = vtbackend::Settings {};
    settings.screenTransitionDuration = 150ms;
    settings.cursorMotionAnimationDuration = 80ms;
    settings.smoothScrolling = true;

    auto const reduced = contour::config::motionAdjusted(settings, MotionPreference::Reduced);
    CHECK(reduced.screenTransitionDuration == 0ms);
    CHECK(reduced.cursorMotionAnimationDuration == 0ms);
    CHECK(reduced.smoothScrolling);

    auto const full = contour::config::motionAdjusted(settings, MotionPreference::Full);
    for (auto const setting: contour::config::motionSettings())
        CHECK(full.*setting == settings.*setting);
    CHECK(full.smoothScrolling);
}

TEST_CASE("Config: the find chords follow the platform", "[config][input-mapping]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    namespace actions = contour::actions;

    // Fallbacks rather than defaults (F12): every edition, an existing config included, gets them, and an
    // explicit binding of the same chord still wins. In every mode, so ⌘G with no search reaches the session.
    auto const config = contour::config::Config {};
    auto const super = Modifiers { Modifier::Super };
    auto const shiftSuper = Modifiers { Modifier::Super, Modifier::Shift };
    auto const rows = std::array {
        std::tuple { super, U'F', actions::Action { actions::SearchReverse {} } },
        std::tuple { super, U'G', actions::Action { actions::FocusNextSearchMatch {} } },
        std::tuple { shiftSuper, U'G', actions::Action { actions::FocusPreviousSearchMatch {} } },
        std::tuple { super, U'E', actions::Action { actions::SearchSelection {} } },
    };
    REQUIRE(contour::config::macFindChords().size() == rows.size());
    for (auto const& [modifiers, input, action]: rows)
    {
        INFO(std::format("{}", static_cast<uint32_t>(input)));
        for (auto const modeFlags: { uint8_t { 0 }, static_cast<uint8_t>(0xFF) })
        {
            auto const* bound = contour::config::applyBuiltinFallback(config, input, modifiers, modeFlags);
#ifdef __APPLE__
            REQUIRE(bound != nullptr);
            REQUIRE(bound->size() == 1);
            CHECK(bound->front().index() == action.index());
#else
            CHECK(bound == nullptr);
#endif
        }
    }

    // The existing chords stand: Ctrl+Shift+F and the two F3 rows.
    for (auto const edition:
         { contour::config::DefaultsEdition::Classic, contour::config::DefaultsEdition::Native2026 })
    {
        auto const mappings = contour::config::defaultInputMappings(edition);
        CHECK(std::ranges::any_of(mappings.charMappings, [](auto const& m) {
            return m.input == U'F' && m.modifiers == Modifiers { Modifier::Control, Modifier::Shift }
                   && std::holds_alternative<actions::SearchReverse>(m.binding.front());
        }));
        CHECK(std::ranges::count_if(mappings.keyMappings,
                                    [](auto const& m) { return m.input == vtbackend::Key::F3; })
              == 2);
    }
}

TEST_CASE("Config: the tab chords follow the platform", "[config][input-mapping]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    namespace actions = contour::actions;

    auto const config = contour::config::Config {};
    auto const super = Modifiers { Modifier::Super };
    auto const shiftSuper = Modifiers { Modifier::Super, Modifier::Shift };

    auto rows = std::vector<std::tuple<Modifiers, char32_t, actions::Action>> {
        { super, U'T', actions::CreateNewTab {} },
        { super, U'W', actions::CloseTab {} },
        { shiftSuper, U'[', actions::SwitchToTabLeft {} },
        { shiftSuper, U']', actions::SwitchToTabRight {} },
    };
    for (auto const position: std::views::iota(1, contour::config::MacTabPositionCount + 1))
        rows.emplace_back(super, static_cast<char32_t>(U'0' + position), actions::SwitchToTab { position });
    REQUIRE(contour::config::macTabChords().size() == rows.size());

    for (auto const& [modifiers, input, action]: rows)
    {
        INFO(std::format("{}", static_cast<uint32_t>(input)));
        // The table itself, on every host.
        auto const& table = contour::config::macTabChords();
        auto const found = std::ranges::find_if(table, [&](auto const& row) {
            return row.mapping.input == input && row.mapping.modifiers == modifiers;
        });
        REQUIRE(found != table.end());
        REQUIRE(found->mapping.binding.size() == 1);
        CHECK(found->mapping.binding.front().index() == action.index());
        if (auto const* wanted = std::get_if<actions::SwitchToTab>(&action))
            CHECK(std::get<actions::SwitchToTab>(found->mapping.binding.front()).position
                  == wanted->position);

        // The built-in lookup, which holds the rows on macOS only.
        auto const* bound = contour::config::applyBuiltinFallback(config, input, modifiers, uint8_t { 0 });
#ifdef __APPLE__
        REQUIRE(bound != nullptr);
        REQUIRE(bound->size() == 1);
        CHECK(bound->front().index() == action.index());
#else
        CHECK(bound == nullptr);
#endif
    }
}

TEST_CASE("Config: the block chords need block focus", "[config][input-mapping][blockfocus]")
{
    using vtbackend::Key;
    using vtbackend::MatchModes;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    namespace actions = contour::actions;

    auto const focused = static_cast<uint8_t>(MatchModes::BlockFocus);
    auto const unfocused = uint8_t { 0 };
    auto const ctrl = Modifiers { Modifier::Control };
    auto const shift = Modifiers { Modifier::Shift };
    auto const none = Modifiers {};

    struct Chord
    {
        Modifiers modifiers;
        Key key;
        actions::Action action;
    };
    auto const chords = std::array {
        Chord { ctrl, Key::Enter, actions::Action { actions::OpenBlockMenu {} } },
        Chord { shift, Key::F10, actions::Action { actions::OpenBlockMenu {} } },
        Chord { none, Key::Enter, actions::Action { actions::ToggleFold {} } },
        Chord { none, Key::Escape, actions::Action { actions::ClearCommandBlockFocus {} } },
    };

    // The same four rows serve a freshly written configuration (the defaults) and one that predates them
    // (the fallbacks), in both editions.
    auto const config = contour::config::Config {};
    for (auto const& chord: chords)
    {
        INFO(std::format("key {}", static_cast<int>(chord.key)));
        auto const* const bound =
            contour::config::applyBuiltinFallback(config, chord.key, chord.modifiers, focused);
        REQUIRE(bound != nullptr);
        REQUIRE(bound->size() == 1);
        CHECK(bound->front().index() == chord.action.index());

        // Without a focused block the key is the application's: nothing here claims it.
        auto const* const unclaimed =
            contour::config::applyBuiltinFallback(config, chord.key, chord.modifiers, unfocused);
        CHECK(unclaimed == nullptr);
    }

    // In vi's select mode Escape is CancelSelection's, so the block's own Escape stands aside.
    CHECK(contour::config::applyBuiltinFallback(
              config, Key::Escape, none, static_cast<uint8_t>(focused | MatchModes::Select))
          == nullptr);

    for (auto const edition:
         { contour::config::DefaultsEdition::Classic, contour::config::DefaultsEdition::Native2026 })
    {
        auto const mappings = contour::config::defaultInputMappings(edition);
        for (auto const& chord: chords)
        {
            auto const row = std::ranges::find_if(mappings.keyMappings, [&](auto const& m) {
                return m.input == chord.key && m.modifiers == chord.modifiers
                       && m.binding.front().index() == chord.action.index();
            });
            REQUIRE(row != mappings.keyMappings.end());
            CHECK(row->modes.status(MatchModes::BlockFocus) == MatchModes::Status::Enabled);
        }
    }

    SECTION("the macOS chords step the focus, outside the alternate screen")
    {
        auto const super = Modifiers { Modifier::Super };
        auto const shiftSuper = Modifiers { Modifier::Super, Modifier::Shift };
        auto const rows = std::array {
            Chord { super, Key::UpArrow, actions::Action { actions::FocusPreviousCommand {} } },
            Chord { super, Key::DownArrow, actions::Action { actions::FocusNextCommand {} } },
            Chord { shiftSuper, Key::UpArrow, actions::Action { actions::SelectPreviousCommandOutput {} } },
            Chord { shiftSuper, Key::DownArrow, actions::Action { actions::SelectNextCommandOutput {} } },
        };
        REQUIRE(contour::config::macBlockChords().size() == rows.size());
        for (auto const& chord: rows)
        {
            INFO(std::format("key {}", static_cast<int>(chord.key)));
            auto const alternate = static_cast<uint8_t>(MatchModes::AlternateScreen);
            auto const* const primary =
                contour::config::applyBuiltinFallback(config, chord.key, chord.modifiers, unfocused);
            auto const* const onAlternate =
                contour::config::applyBuiltinFallback(config, chord.key, chord.modifiers, alternate);
            CHECK(onAlternate == nullptr); // vim keeps it
#ifdef __APPLE__
            REQUIRE(primary != nullptr);
            REQUIRE(primary->size() == 1);
            CHECK(primary->front().index() == chord.action.index());
#else
            CHECK(primary == nullptr); // no default mapping carries Super with Up or Down elsewhere
#endif
        }
    }
}

TEST_CASE("Config: a fresh macOS configuration carries the Command pane chords and not Control+Shift+E/O",
          "[config][panes]")
{
    using contour::config::PaneChordHost;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    // What first start writes, with the macOS host injected: the writer and the reader are the production
    // ones, only the host that picks the pane rows is not the build's.
    auto config = contour::config::Config {};
    config.inputMappings = contour::config::makeDefaultInputMappings(
        contour::config::DefaultsEdition::Native2026, PaneChordHost::Mac);
    auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);

    QTemporaryDir dir;
    auto const reloaded = loadFromYaml(dir, written);
    auto const& mappings = reloaded.inputMappings.value();

    auto const charBinding = [&](Modifiers modifiers, char32_t input) -> contour::config::ActionList const* {
        return contour::config::apply(
            std::span { mappings.charMappings }, input, modifiers, uint8_t { 0 }, std::nullopt);
    };
    auto const keyBinding = [&](Modifiers modifiers,
                                vtbackend::Key key) -> contour::config::ActionList const* {
        return contour::config::apply(std::span { mappings.keyMappings }, key, modifiers, uint8_t { 0 });
    };

    auto const* split = charBinding(Modifiers { Modifier::Super }, U'D');
    REQUIRE(split != nullptr);
    CHECK(std::holds_alternative<contour::actions::SplitVertical>(split->front()));
    auto const* stacked = charBinding(Modifiers { Modifier::Super, Modifier::Shift }, U'D');
    REQUIRE(stacked != nullptr);
    CHECK(std::holds_alternative<contour::actions::SplitHorizontal>(stacked->front()));

    auto const* focus = keyBinding(Modifiers { Modifier::Super, Modifier::Alt }, vtbackend::Key::LeftArrow);
    REQUIRE(focus != nullptr);
    CHECK(std::holds_alternative<contour::actions::FocusPaneLeft>(focus->front()));
    auto const* zoom = keyBinding(Modifiers { Modifier::Super, Modifier::Shift }, vtbackend::Key::Enter);
    REQUIRE(zoom != nullptr);
    CHECK(std::holds_alternative<contour::actions::TogglePaneZoom>(zoom->front()));

    CHECK(charBinding(Modifiers { Modifier::Control, Modifier::Shift }, U'E') == nullptr);
    CHECK(charBinding(Modifiers { Modifier::Control, Modifier::Shift }, U'O') == nullptr);
}
