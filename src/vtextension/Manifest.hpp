// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtcontrol/Catalog.hpp>
#include <vtextension/ArgumentKind.hpp>
#include <vtextension/ExtensionError.hpp>

namespace vtextension
{

/// The name of the WebAssembly custom section that carries an extension's manifest.
inline constexpr std::string_view ManifestSectionName = "terminal-extension-manifest";

/// The manifest format revisions this host reads.
inline constexpr auto SupportedManifestVersions = std::array<uint32_t, 2> { 1, 2 };

/// Where an extension runs: the world it is built against (`placement <status-line|daemon>`).
enum class ManifestPlacement : uint8_t
{
    StatusLine, ///< In a terminal session's status line; what a manifest without a `placement` line means.
    Daemon,     ///< In the daemon, with no window attached.
};

/// How long a declared command may run.
enum class CommandTimeout : uint8_t
{
    Short, ///< 30 seconds; every version-1 command.
    Long,  ///< 10 minutes.
};

/// Whether the host creates a root's directory when it is missing.
enum class RootCreation : uint8_t
{
    Existing,        ///< The directory must exist; the host never creates it.
    CreateIfMissing, ///< `root <label> create = …`: the host creates the one directory before it is used.
};

/// One variable of a fixed environment.
struct EnvironmentEntry
{
    std::string name;  ///< The variable's name.
    std::string value; ///< Its value.

    bool operator==(EnvironmentEntry const&) const = default;
};

/// A root: directories taken from the extension's configuration (`root <label> [create] = config:<pointer>`).
struct RootDecl
{
    std::string label;                              ///< What `@<label>`, `read` and `path:<label>` name.
    std::string pointer;                            ///< Slash-separated configuration keys, `*` for a list.
    RootCreation creation = RootCreation::Existing; ///< Whether the host creates a missing directory.
};

/// A typed argument (`arg <name> = <kind>`): what a placeholder `{name}` accepts.
struct ArgumentDecl
{
    std::string name;  ///< The placeholder's name.
    ArgumentKind kind; ///< What a value must look like.
    std::string root;  ///< For a Path, the root it lies inside; empty for every other kind.
};

/// One command an extension declares it may run.
struct DeclaredCommand
{
    /// The name the guest passes to `run-command`.
    std::string name;

    /// The command line as separate arguments. An element of the form `{placeholder}` is filled in
    /// from the call's arguments; no other element is ever changed, and no shell is involved.
    std::vector<std::string> argvTemplate;

    /// How long it may run; Short for every version-1 command.
    CommandTimeout timeout = CommandTimeout::Short;

    /// The root its working directory lies inside; empty in version 1.
    std::string root {};

    /// Its own environment (`command-env-for`), applied after Manifest::commandEnvironment, in declaration
    /// order.
    std::vector<EnvironmentEntry> environment {};
};

/// A program the extension may put in a tab (`pane <name> @<root> = argv…`).
struct PaneTemplate
{
    std::string name;                          ///< What the guest opens it by.
    std::string root;                          ///< The root its working directory lies inside.
    std::vector<std::string> argvTemplate;     ///< As DeclaredCommand::argvTemplate.
    std::vector<EnvironmentEntry> environment; ///< Its fixed environment (`pane-env`), in declaration order.
};

/// Whether an extension may type into its own agent panes (`pane-input`).
enum class PaneInput : uint8_t
{
    Refused = 0, ///< Nothing may be typed into them: the default.
    Typed,       ///< `pane-send` types text the user wrote into them, and `pane-key` presses a key.
};

/// Whether an extension may ask the frontend the user is at to open a web link (`url-open`).
enum class UrlOpening : uint8_t
{
    Refused = 0, ///< It may not: the default.
    Allowed,     ///< `url-open` asks the frontend to open an http or https link.
};

/// A palette row (`action <id> = title`).
struct ActionDecl
{
    std::string id;    ///< What the guest is told was chosen.
    std::string title; ///< What the palette shows.
};

/// What an extension declares about itself, read from its module before any of its code runs.
struct Manifest
{
    /// The extension's name; the key its grants are recorded under.
    std::string name;

    /// What Contour names the extension where it shows it (`title = <text>`, version 2): its served tab, its
    /// palette rows and its frames. Empty when the manifest gives none; displayTitleOf() then answers the
    /// name.
    std::string title {};

    /// The manifest format revision the extension was written against.
    uint32_t version = 0;

    /// The commands the extension may ask the host to run.
    std::vector<DeclaredCommand> commands;

    /// The control-API permissions the extension asks for, each once, in declaration order: the lines
    /// `control <read|screen|input|write>`. Granted, or not, with its commands.
    std::vector<vtcontrol::Permission> control {};

    /// Where the extension runs.
    ManifestPlacement placement = ManifestPlacement::StatusLine;

    /// The roots, in declaration order.
    std::vector<RootDecl> roots {};

    /// The labels of the roots `file-read` may read from, in declaration order.
    std::vector<std::string> readableRoots {};

    /// The typed arguments, in declaration order.
    std::vector<ArgumentDecl> arguments {};

    /// The environment added to every declared command (`command-env`), in declaration order.
    std::vector<EnvironmentEntry> commandEnvironment {};

    /// The programs the extension may put in a tab.
    std::vector<PaneTemplate> panes {};

    /// Whether `pane-send` may type text the user wrote into those tabs, and `pane-key` press a key
    /// (`pane-input`).
    PaneInput paneInput = PaneInput::Refused;

    /// Whether `url-open` may ask the frontend the user is at to open a web link (`url-open`).
    UrlOpening urlOpening = UrlOpening::Refused;

    /// The scheme of the tabs the extension serves (`serve <scheme>`), if any.
    std::optional<std::string> servedScheme {};

    /// The palette rows, in declaration order.
    std::vector<ActionDecl> actions {};

    /// SHA-256 of the whole module's bytes, in lower-case hex: its code and its manifest. A grant is bound
    /// to it: an extension whose code or manifest changes needs a new grant. Empty from parseManifestText().
    std::string digest;
};

/// Why a manifest's text is not a valid manifest.
enum class ManifestFault : uint8_t
{
    NotUtf8,                  ///< Not UTF-8, or a control character other than a line end or a tab.
    UnknownLine,              ///< An unknown keyword, or a known one in the wrong shape.
    MissingKeyword,           ///< A line with a value and no keyword.
    EmptyName,                ///< A line that takes a value given none.
    MissingName,              ///< No `name` line.
    MissingVersion,           ///< No `version` line.
    Repeated,                 ///< `name`, `version`, `placement` or `serve` given twice.
    UnsupportedVersion,       ///< A version other than those in SupportedManifestVersions.
    Version2LineInVersion1,   ///< A line kind of version 2 in a version-1 manifest.
    EmptyArgv,                ///< A command or pane with no program.
    PlaceholderProgram,       ///< A command or pane whose program is a placeholder.
    BraceInsideWord,          ///< A brace that is not part of a whole-word `{name}`.
    QuotedWord,               ///< A quote in an argv word: an attempt at a word with a space.
    UndeclaredPlaceholder,    ///< A `{name}` without an `arg name` line.
    UndeclaredRoot,           ///< `@root`, `read root` or `path:root` naming no `root` line.
    CommandWithoutRoot,       ///< A command or pane bound to no `@root`.
    RootOutOfPlace,           ///< An `@root` somewhere other than right before the `=` of a command or pane.
    UnknownKind,              ///< An `arg` of no known kind.
    UnknownTimeout,           ///< A command's timeout class is neither `short` nor `long`.
    UnknownRootFlag,          ///< A word other than `create` between a root's label and its `=`.
    CreateOverList,           ///< `create` on a root whose pointer walks a list.
    MalformedRoot,            ///< A root's value is not `config:` and slash-separated keys.
    Duplicate,                ///< The same name declared twice for one kind.
    EnvForUndeclared,         ///< `command-env-for` or `pane-env` naming no such command or pane.
    UnknownPermission,        ///< A `control` line naming an unknown permission, or one given twice.
    VersionNotFirst,          ///< In version 2, a line other than `name` before the `version` line.
    MalformedEnvironmentName, ///< An environment name other than `[A-Za-z_][A-Za-z0-9_]*`.
    TabCharacter,             ///< A tab in a version-2 line: words are separated by single spaces only.
    SurroundingBlank,         ///< A version-2 line that begins or ends with a blank, which is never trimmed.
    UnsupportedLine,          ///< An `inbox` line: the kind is reserved, and no version supports it yet.
    BadTitle, ///< A `title` or an `action`'s title longer than MaxTitleLength characters, or that disguises
              ///< text.
    ForbiddenEnvironment, ///< An environment variable an extension may not set, or a value it may not take.
    ForbiddenGitConfig,   ///< A git configuration key an extension may not set, or a value it may not take.
};

/// The most characters (code points) a manifest's `title`, or an `action`'s title, may hold.
inline constexpr std::size_t MaxTitleLength = 40;

/// One row of the fault-description table.
struct ManifestFaultText
{
    ManifestFault fault;   ///< The fault described.
    std::string_view text; ///< What a diagnostic says for it.
};

/// What a diagnostic says for each fault, indexed by the enumerator's value.
inline constexpr auto ManifestFaultTexts = std::array {
    ManifestFaultText { .fault = ManifestFault::NotUtf8,
                        .text = "the manifest is not UTF-8, or holds a control character" },
    ManifestFaultText { .fault = ManifestFault::UnknownLine,
                        .text = "an unknown line, or a line of the wrong shape" },
    ManifestFaultText { .fault = ManifestFault::MissingKeyword, .text = "a line without a keyword" },
    ManifestFaultText { .fault = ManifestFault::EmptyName, .text = "a line that takes a value has none" },
    ManifestFaultText { .fault = ManifestFault::MissingName, .text = "no name line" },
    ManifestFaultText { .fault = ManifestFault::MissingVersion, .text = "no version line" },
    ManifestFaultText { .fault = ManifestFault::Repeated,
                        .text = "a line that may appear once appears twice" },
    ManifestFaultText { .fault = ManifestFault::UnsupportedVersion,
                        .text = "an unsupported manifest version" },
    ManifestFaultText { .fault = ManifestFault::Version2LineInVersion1,
                        .text = "a version-2 line in a version-1 manifest" },
    ManifestFaultText { .fault = ManifestFault::EmptyArgv, .text = "a command or pane names no program" },
    ManifestFaultText { .fault = ManifestFault::PlaceholderProgram,
                        .text = "a command or pane whose program is a placeholder" },
    ManifestFaultText { .fault = ManifestFault::BraceInsideWord,
                        .text = "a brace that is not a whole-word placeholder" },
    ManifestFaultText { .fault = ManifestFault::QuotedWord, .text = "a quote in a command line" },
    ManifestFaultText { .fault = ManifestFault::UndeclaredPlaceholder,
                        .text = "a placeholder without an arg line" },
    ManifestFaultText { .fault = ManifestFault::UndeclaredRoot, .text = "a root that is not declared" },
    ManifestFaultText { .fault = ManifestFault::CommandWithoutRoot,
                        .text = "a command or pane bound to no root" },
    ManifestFaultText { .fault = ManifestFault::RootOutOfPlace,
                        .text = "a root binding out of its place in a command or pane line" },
    ManifestFaultText { .fault = ManifestFault::UnknownKind, .text = "an argument of an unknown kind" },
    ManifestFaultText { .fault = ManifestFault::UnknownTimeout,
                        .text = "a timeout class other than short or long" },
    ManifestFaultText { .fault = ManifestFault::UnknownRootFlag, .text = "a root word other than create" },
    ManifestFaultText { .fault = ManifestFault::CreateOverList,
                        .text = "create on a root that names a list of directories" },
    ManifestFaultText { .fault = ManifestFault::MalformedRoot,
                        .text = "a root that is not config:<pointer>" },
    ManifestFaultText { .fault = ManifestFault::Duplicate, .text = "a name declared twice" },
    ManifestFaultText { .fault = ManifestFault::EnvForUndeclared,
                        .text = "an environment for a command or pane that is not declared" },
    ManifestFaultText { .fault = ManifestFault::UnknownPermission,
                        .text = "an unknown or repeated control permission" },
    ManifestFaultText { .fault = ManifestFault::VersionNotFirst,
                        .text = "a line other than name before the version line" },
    ManifestFaultText { .fault = ManifestFault::MalformedEnvironmentName,
                        .text = "an environment name other than letters, digits and underscores" },
    ManifestFaultText { .fault = ManifestFault::TabCharacter, .text = "a tab in a version-2 line" },
    ManifestFaultText { .fault = ManifestFault::SurroundingBlank,
                        .text = "a version-2 line that begins or ends with a blank" },
    ManifestFaultText { .fault = ManifestFault::UnsupportedLine,
                        .text = "the inbox line is reserved and not supported yet" },
    ManifestFaultText { .fault = ManifestFault::BadTitle,
                        .text = "a title too long, or holding a character that disguises text" },
    ManifestFaultText { .fault = ManifestFault::ForbiddenEnvironment,
                        .text =
                            "an environment variable an extension may not set, or a value it may not take" },
    ManifestFaultText { .fault = ManifestFault::ForbiddenGitConfig,
                        .text =
                            "a git configuration key an extension may not set, or a value it may not take" },
};

static_assert(ManifestFaultTexts.size() == std::to_underlying(ManifestFault::ForbiddenGitConfig) + 1U,
              "every ManifestFault has a row");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ManifestFaultTexts.size()),
                                  [](std::size_t index) {
                                      return std::to_underlying(ManifestFaultTexts.at(index).fault) == index;
                                  }),
              "ManifestFaultTexts is indexed by the enumerator's value");

/// Describes a manifest fault for diagnostics.
/// @param fault The fault to describe.
/// @return A short, human-readable description.
[[nodiscard]] std::string_view manifestFaultText(ManifestFault fault) noexcept;

/// What Contour names an extension where it shows it.
/// @param manifest The extension's manifest.
/// @return Its `title`, or its `name` when it declares none.
[[nodiscard]] std::string_view displayTitleOf(Manifest const& manifest) noexcept;

/// Reads the name of the placeholder an argv template element stands for.
/// @param element One element of a command's argv template.
/// @return The name between the braces if the element is exactly `{name}` with a non-empty name that
///         contains no braces; `std::nullopt` for every other element.
[[nodiscard]] std::optional<std::string_view> placeholderName(std::string_view element) noexcept;

/// Parses a manifest section's text.
///
/// The `version` line is found first and every line is judged under it: version 1 knows `name`, `version`,
/// `command <name> = argv…` and `control <permission>`, in any order; version 2 adds `placement`, `root`,
/// `read`, `arg`, `command-env`, `command-env-for`, `pane`, `pane-env`, `serve`, `action` and `title` (and
/// reserves `inbox`, which it refuses), binds every command to a timeout class and a root, wants `version`
/// before every line but `name`, and a name declared before any line uses it.
/// @param text The section's bytes.
/// @return The manifest, its digest left empty; the first fault found otherwise.
[[nodiscard]] std::expected<Manifest, ManifestFault> parseManifestText(std::string_view text);

/// Reads the manifest out of a WebAssembly core module.
///
/// The module is scanned section by section for the custom section `terminal-extension-manifest`;
/// no code in it is run. Every length is checked against @p module, so truncated or malformed input
/// is an error and never an out-of-bounds read.
/// @param module The bytes of the core module.
/// @return The manifest; `ManifestMissing` if the module has no manifest section, `ManifestInvalid`
///         if the module cannot be walked, or the section's text is not a valid manifest
///         (parseManifestText() finds a ManifestFault).
[[nodiscard]] std::expected<Manifest, ExtensionError> parseManifest(std::span<std::byte const> module);

/// Whether a WebAssembly core module declares a start function, which the runtime would run while
/// instantiating it, before any deadline can guard it. No code in the module is run.
/// @param module The bytes of the core module.
/// @return Whether the module can be walked and has a start section.
[[nodiscard]] bool hasStartFunction(std::span<std::byte const> module) noexcept;

} // namespace vtextension
