// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ConfigEnumReader.hpp>

#include <yaml-cpp/emitter.h>
#include <yaml-cpp/node/detail/iterator_fwd.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <format>
#include <functional>
#include <regex>
#include <string>
#include <string_view>

#include <reflection-cpp/reflection.hpp>

// The YAML reader and the writers, apart from the data model in Config.hpp: only the files that parse or emit
// contour.yml include this, so everyone else is spared yaml-cpp's, reflection-cpp's and <regex>'s templates.

namespace contour::config
{

struct YAMLConfigReader
{
    /// Callable that resolves a variable name to its value.
    using VariableReplacer = std::function<std::string(std::string_view)>;

    std::filesystem::path configFile;
    YAML::Node doc;
    core::log::Category const& logger;
    VariableReplacer variableReplacer;

    /// @param filename The document to parse.
    /// @param log      Where parse diagnostics go.
    /// @param env      The environment `${VAR}` expands against. It must outlive this reader: the
    ///                 default replacer holds a reference to it.
    /// @param replacer An expansion policy of the caller's own; the default one reads @p env.
    YAMLConfigReader(std::filesystem::path const& filename,
                     core::log::Category const& log,
                     core::Environment const& env,
                     VariableReplacer replacer = {});

    /// Reads a document the caller has parsed already, so that a file it has to parse anyway, to tell
    /// a broken one from an empty one, is not read and parsed a second time.
    /// @param filename Where @p document came from.
    /// @param document The parsed document.
    /// @param log      Where diagnostics go.
    /// @param env      The environment `${VAR}` expands against. It must outlive this reader: the
    ///                 default replacer holds a reference to it.
    /// @param replacer An expansion policy of the caller's own; the default one reads @p env.
    YAMLConfigReader(std::filesystem::path filename,
                     YAML::Node const& document,
                     core::log::Category const& log,
                     core::Environment const& env,
                     VariableReplacer replacer = {});

    /// Expands `${VAR}` tokens in @p input using the configured variable replacer.
    [[nodiscard]] std::string resolveVariables(std::string const& input) const;

    /// Expands `${VAR}` tokens then resolves `~` to the home directory.
    [[nodiscard]] std::filesystem::path resolvedPath(std::string const& input) const;

    /// The directory a relative path in the document being read is relative to: the one holding the file
    /// that declares it -- the side file inside readSideFile(), else the configuration file.
    /// @return That directory, made absolute.
    [[nodiscard]] std::filesystem::path declaringDirectory() const;

    /// Parses a single pane node. A node is a LEAF unless it carries a `split:` mapping.
    void parseLayoutPane(YAML::Node const& node, LayoutPane& where);

    /// Loads @p entry of @p node into @p where, so that a value of the wrong shape costs that entry alone.
    ///
    /// yaml-cpp throws when a value has another shape than its reader expects: a scalar where a map
    /// belongs, a list where one word belongs. Uncaught, that throw would end the whole load, and every
    /// entry read after it would keep its default without a word. Instead @p where is put back as it
    /// was, the entry is reported by its whole key path, and loading goes on. A reader subscripts its
    /// own value under its own guard, so a value of the wrong shape is blamed on its own key.
    /// @param node  The mapping to read from.
    /// @param entry The key within @p node.
    /// @param where Receives the value; unchanged when the entry cannot be read.
    /// @param args  Passed on to the reader of @p where.
    template <typename T, typename... Args>
    void loadGuarded(YAML::Node const& node, std::string const& entry, T& where, Args&&... args)
    {
        readGuarded(
            node, entry, where, [&]() { loadFromEntry(node, entry, where, std::forward<Args>(args)...); });
    }

    /// Runs @p read, which reads @p entry of @p node into @p where, under loadGuarded()'s guard: for an
    /// entry whose reader is no loadFromEntry() overload of its own, such as one map of a colour scheme.
    /// @param node  The mapping @p entry is read from.
    /// @param entry The key within @p node.
    /// @param where What @p read may change; put back as it was when @p read throws.
    /// @param read  Reads the entry.
    template <typename T, typename Reader>
    void readGuarded(YAML::Node const& node, std::string const& entry, T& where, Reader const& read)
    {
        auto fallback = where;
        _keyPath.push_back(KeyStep { .container = node, .entry = entry });
        auto const leave = core::Finally { [this]() { _keyPath.pop_back(); } };
        try
        {
            read();
        }
        catch (std::exception const& e)
        {
            where = std::move(fallback);
            errorLog()("Ignoring the malformed config entry {}: {}", keyPathTo(node, entry), e.what());
        }
    }

    /// Runs @p read with every key path it reports rooted at side file @p file -- `<file>: <key>` --
    /// rather than at the document this reader began with: a side file's entry is found in that file,
    /// not where the configuration names it, and settings.yml spells its keys as contour.yml does.
    /// @param file     The side file @p document was parsed from.
    /// @param document The side file's root.
    /// @param read     Reads from @p document.
    template <typename Reader>
    void readSideFile(std::filesystem::path const& file, YAML::Node const& document, Reader const& read)
    {
        auto outerPath = std::exchange(_keyPath,
                                       std::vector { KeyStep { .container = document,
                                                               .entry = core::pathToUtf8(file),
                                                               .kind = KeyStepKind::File } });
        auto const restore = core::Finally { [&]() { _keyPath = std::move(outerPath); } };
        read();
    }

    /// Reads and parses side file @p file -- settings.yml, a profile's file or a colour scheme's -- once,
    /// for readSideFile() to read from.
    /// @param file The side file.
    /// @return Its document, a map or empty, or why there is none: `<file>: <reason>`, a missing file and
    ///         a document of another shape included.
    [[nodiscard]] static std::expected<YAML::Node, std::string> parseSideFile(
        std::filesystem::path const& file);

    /// Parses @p contents as side file @p file, as parseSideFile() parses what it reads.
    /// @param file     The side file the contents are of, for messages.
    /// @param contents Its bytes; a CR is dropped as parseSideFile() drops it.
    /// @return Its document, a map or empty, or why there is none: `<file>: <reason>`.
    [[nodiscard]] static std::expected<YAML::Node, std::string> parseSideFileText(
        std::filesystem::path const& file, std::string contents);

    /// @p entry of @p node, spelled out from the document's root as loadGuarded() descended to it:
    /// `profiles.main.cursor`.
    /// @param node  The mapping @p entry is read from.
    /// @param entry The key within @p node.
    /// @return The key path.
    [[nodiscard]] std::string keyPathTo(YAML::Node const& node, std::string_view entry) const;

    template <typename T, typename Doc>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ConfigEntry<T, Doc>& where)
    {
        loadGuarded(node, entry, where.value());
    }

    template <typename T, typename Doc, typename... Args>
    void loadFromEntry(std::string const& entry, ConfigEntry<T, Doc>& where, Args&&... args)
    {
        loadGuarded(doc, entry, where.value(), std::forward<Args>(args)...);
    }

    template <typename T>
        requires(std::is_scalar_v<T> && !ConfigEnumConcept<T>)
    void loadFromEntry(YAML::Node const& node, std::string const& entry, T& where)
    {
        auto const child = node[entry];
        if (child)
            where = child.as<T>();
        logger()("Loading entry: {}, value {}", entry, where);
    }

    /// Reads an enum that has a configuration table, through that table.
    ///
    /// The settings page builds its combo box from the very same rows (@see configEnumValues), so a
    /// value this reader accepts is by construction one the settings page can offer and the writer
    /// can spell back. An unknown word, a list or a map is reported and leaves @p where as it was.
    /// @param node  The mapping to read from.
    /// @param entry The key within @p node.
    /// @param where Receives the value.
    template <ConfigEnumConcept Enum>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Enum& where)
    {
        (void) detail::loadConfigEnum(node, entry, keyPathTo(node, entry), where, logger);
    }

    template <typename V, typename T>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, boxed::boxed<V, T>& where)
    {
        auto const child = node[entry];
        if (child)
            where = boxed::boxed<V, T>(child.as<V>());
        logger()("Loading entry: {}, value {}", entry, where.template as<V>());
    }

    /// Reads `profiles:`. The default profile is read first and every other one starts as a copy of it.
    /// @param node The mapping that holds @p entry.
    /// @param entry The key to read (`profiles`).
    /// @param where Receives the profiles.
    /// @param defaultProfileName The profile the others inherit from.
    /// @param edition The defaults a default profile that is not seeded yet starts from.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::unordered_map<std::string, TerminalProfile>& where,
                       std::string const& defaultProfileName,
                       DefaultsEdition edition)
    {
        if (auto const child = node[entry]; child && child.IsMap())
        {
            logger()("Loading default profile: {}", defaultProfileName);
            // Built only when absent: the default profile is nearly always seeded already.
            if (!where.contains(defaultProfileName))
                where.emplace(defaultProfileName, TerminalProfile { .defaultsEdition = edition });
            // Guarded one profile at a time, so a profile that is not a map costs no other profile.
            loadGuarded(child, defaultProfileName, where[defaultProfileName]);
            for (auto entry: child)
            {
                auto const name = entry.first.as<std::string>();
                if (name == defaultProfileName)
                    continue;
                logger()("Loading map with entry: {}", name);
                where[name] = where[defaultProfileName]; // inherit from default
                loadGuarded(child, name, where[name]);
            }
        }
    }

    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::unordered_map<std::string, Layout>& where);

    void loadFromEntry(YAML::Node const& node, std::string const& entry, Layout& where);

    /// Reads `hosts:` -- a map from a host's name to its `url`, `token_file`, `tls_ca`, `remote_command`
    /// and `profile`. A malformed entry costs that entry alone, reported, never the map.
    /// @param node The mapping that holds @p entry.
    /// @param entry The key to read (`hosts`).
    /// @param where Receives the entries; a later duplicate replaces an earlier one.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::map<std::string, HostEntry>& where) const;

    /// Parses one entry of `hosts:`. Never throws: an entry that cannot be used is reported and skipped.
    /// @param hostNode The entry's value.
    /// @param name The entry's key, for the report.
    /// @return The host, or no value when it is skipped.
    [[nodiscard]] std::optional<HostEntry> parseHostNode(YAML::Node const& hostNode,
                                                         std::string const& name) const;

    /// Parses one layout's node (its `tabs:` sequence) into @p where. Never throws: malformed
    /// scalars are logged and skipped, so one broken layout cannot unwind the whole config load
    /// (which would silently drop every entry loaded after it, e.g. the user's input_mapping).
    void parseLayoutNode(YAML::Node const& layoutNode, Layout& where);

    // Used for color scheme loading
    template <typename T>
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::unordered_map<std::string, T>& where)
    {
        if (auto const child = node[entry]; child && child.IsMap())
        {
            for (auto entry: child)
            {
                auto const name = entry.first.as<std::string>();
                logger()("Loading map with entry: {}", name);
                loadGuarded(child, name, where[name]);
            }
        }
    }

    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::chrono::milliseconds& where)
    {
        if (auto const child = node[entry]; child)
            where = std::chrono::milliseconds(child.as<int>());
        logger()("Loading entry: {}, value {}", entry, where.count());
    }

    template <typename Input>
    void appendOrCreateBinding(std::vector<vtbackend::InputBinding<Input, ActionList>>& bindings,
                               vtbackend::MatchModes modes,
                               vtbackend::Modifiers modifier,
                               Input input,
                               actions::Action action,
                               std::optional<uint8_t> clickCount = std::nullopt)
    {
        for (auto& binding: bindings)
        {
            if (match(binding, modes, modifier, input, clickCount))
            {
                binding.binding.push_back(std::move(action));
                return;
            }
        }

        bindings.emplace_back(vtbackend::InputBinding<Input, ActionList> {
            modes, modifier, input, ActionList { std::move(action) }, clickCount });
    }

    // clang-format off
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::filesystem::path& where) const;
    void loadFromEntry(YAML::Node const& node, std::string const& entry, RenderingBackend& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, GpuSelector& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, crispy::StrongHashtableSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::MaxHistoryLineCount& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, crispy::LRUCapacity& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtrasterizer::GlyphScalingMethod& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorDisplay& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::BlinkStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ScreenTransitionStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::Modifiers& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorShape& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, contour::config::SelectionAction& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, InputMappings& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ImageSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::set<std::string>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::string& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::StatusDisplayPosition& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarPosition& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, PixelReporting& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, SecureInputMode& where);
    /// Reads `option_as_alt`: a token, or the boolean it was before it named sides (@see parseOptionAsAlt).
    void loadFromEntry(YAML::Node const& node, std::string const& entry, OptionAsAlt& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::FontDescriptions& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::RenderMode& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::TextOutlineConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::FontLocatorEngine& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::TextShapingEngine& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ColorConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontDescription& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::vector<text::FontFeature>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontWeight& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontSlant& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::FontSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::LineCount& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::VTType& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtpty::PageSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, WindowMargins& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::LineOffset& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtpty::Process::ExecInfo& where) const;
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtpty::SshHostConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Bell& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, FinishNotificationConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::map<vtbackend::DECMode, bool>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::Decorator& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::Opacity& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::StatusDisplayType& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Permission& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::shared_ptr<vtbackend::BackgroundImage>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CellRGBColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::RGBColor& where);
    /// Reads an optional tab color; null clears the profile's color.
    /// @param node Mapping containing the entry.
    /// @param entry Configuration key.
    /// @param where Receives the color, or no value for null.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::optional<vtbackend::RGBColor>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::RGBColorPair& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CellRGBColorAndAlphaPair& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ColorPalette::Palette& colors);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node const& node, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, TerminalProfile& where);

    /// Reads a mode's `cursor:` map -- its shape, blinking and blinking interval.
    /// @param node  The mapping to read from.
    /// @param entry The key within @p node.
    /// @param where Receives what the map states.
    void loadFromEntry(YAML::Node const& node, std::string const& entry, CursorConfig& where);

    /// Reads a Vi mode's map (`normal_mode:`, `visual_mode:`), its `cursor:` guarded on its own.
    /// @param node  The mapping to read from.
    /// @param entry The key within @p node.
    /// @param where Receives what the map states.
    void loadFromEntry(YAML::Node const& node, std::string const& entry, InputModeConfig& where);

    /// Loads @p entry of @p node into @p where if the key is there, and leaves @p where as it is if not:
    /// for a value whose absence means "derive it", such as a colour scheme's fold column.
    ///
    /// The key is a view, so the tables that spell a colour scheme's keys -- block_status, sticky_header,
    /// tint -- hand theirs over as they are.
    /// @param node The mapping to read.
    /// @param entry The key.
    /// @param where Set to the value read; untouched without the key.
    template <typename T>
    void loadOptional(YAML::Node const& node, std::string_view entry, std::optional<T>& where)
    {
        auto const key = std::string { entry };
        if (!node[key])
            return;
        auto value = T {};
        loadFromEntry(node, key, value);
        where = value;
    }

    /// Parses a profile body map (a profile's `shell`, `font`, `colors`, ... keys) directly into
    /// @p where. This is the shared core of profile parsing: the `loadFromEntry(node, entry, profile)`
    /// overload above resolves `node[entry]` to the body and delegates here, and the GUI side-file
    /// loader (whose `profiles/<name>.yml` document root IS the body) calls it directly.
    /// @param profileNode The profile body map; a null node is a no-op (@p where keeps its base values).
    /// @param where The profile to populate; pre-seed it with an inheritance base before calling.
    /// @param profileName The profile's name, as problems found in its body name it.
    void loadProfileBody(YAML::Node const& child, TerminalProfile& where, std::string_view profileName);

    void loadFromEntry(YAML::Node const& node, std::string const& entry, RendererConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ImagesConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, DaemonSettings& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, QuickTerminalConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, DaemonRestoreConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, HistoryConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, FoldingConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, GutterConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, OscContextConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, CommandBlocksConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarConfig& where);

    /// Loads @p entry as a YAML list of scrollbar mark sources, replacing @p where whole: `[]` is a
    /// deliberate "none". Left unchanged when absent or not a list; a list item that names no source is
    /// reported and skipped.
    /// @param node The mapping that holds @p entry.
    /// @param entry The key to read (`marks`).
    /// @param where The sources to fill.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtbackend::ScrollbarMarkSources& where);

    void loadFromEntry(YAML::Node const& node, std::string const& entry, StickyHeaderConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, MouseConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, StatusLineConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, BackgroundConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, HyperlinkDecorationConfig& where);
    /// Reads link_detection: per screen group, which of urls and file_paths are detected; a key left out
    /// keeps the value already in @p where.
    void loadFromEntry(YAML::Node const& node, std::string const& entry, LinkDetectionConfig& where);
    /// Reads file_opener as a command line, or as a map of `command` and `placement`, and validates the command
    /// (FileOpenerTemplate::parse()): one that cannot be used is reported and cleared, so files open with the
    /// system's default application.
    void loadFromEntry(YAML::Node const& node, std::string const& entry, FileOpenerConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, PermissionsConfig& where);
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::vector<HintPatternConfig>& where) const;
    /// Reads `extensions:` -- a map from an extension's name to its `path`, `config` and `run_command`.
    ///
    /// A relative `path` is relative to the directory of the file that declares it. A malformed entry costs
    /// that entry, not the map, and so does a `path` that is empty or names nothing: the entry never falls
    /// back to the bundled extension of its name. An unknown `run_command` is reported and the entry asks.
    /// A name declared twice keeps the first, a `config` key set twice the later.
    /// @param node The mapping that holds @p entry.
    /// @param entry The key to read (`extensions`).
    /// @param where Receives the entries in file order; replaced when the key is present.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::vector<ExtensionConfig>& where) const;

    /// Reads one entry of `extensions:`.
    /// @param name The entry's key.
    /// @param settings The entry's value: a map, or null for a bundled extension with the defaults.
    /// @param keyPath Where `extensions` is declared, for the diagnostics.
    /// @return The extension, or no value when the entry is skipped, with the reason reported.
    [[nodiscard]] std::optional<ExtensionConfig> readExtension(std::string name,
                                                               YAML::Node const& settings,
                                                               std::string_view keyPath) const;
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::BoxDrawingRenderer::ArcStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::BoxDrawingRenderer::BrailleStyle& where);


    void defaultSettings(vtpty::Process::ExecInfo& shell);
    // clang-format on

    void load(Config& c);

    /// Loads the `layouts:` entry of this reader's own document into @p where.
    /// Used to merge a sibling layouts.yml on top of the already-loaded Config.
    void loadLayoutsInto(std::unordered_map<std::string, Layout>& where)
    {
        loadFromEntry(doc, "layouts", where);
    }

    std::optional<actions::Action> parseAction(YAML::Node const& node);
    std::optional<vtbackend::Modifiers> parseModifierKey(std::string const& key);
    std::optional<vtbackend::Modifiers> parseModifier(YAML::Node const& nodeYAML);
    static std::optional<vtbackend::MatchModes> parseMatchModes(YAML::Node const& nodeYAML);
    std::optional<vtbackend::Key> parseKey(std::string const& name);
    std::optional<std::variant<vtbackend::Key, char32_t>> parseKeyOrChar(std::string const& name);

    bool tryAddKey(InputMappings& inputMappings,
                   vtbackend::MatchModes modes,
                   vtbackend::Modifiers modifier,
                   YAML::Node const& node,
                   actions::Action action);
    std::optional<vtbackend::MouseButton> parseMouseButton(YAML::Node const& node);
    bool tryAddMouse(std::vector<MouseInputMapping>& bindings,
                     vtbackend::MatchModes modes,
                     vtbackend::Modifiers modifier,
                     YAML::Node const& node,
                     actions::Action action,
                     std::optional<uint8_t> clickCount);

  private:
    /// What one step of the key path names, which says how the next step is joined to it.
    enum class KeyStepKind : uint8_t
    {
        Key = 0, ///< A key of its mapping; a dot joins the key read within it.
        File,    ///< A side file, whose own key path follows it after a colon: `<file>: <map>`.
    };

    /// One key loadGuarded() is reading, and the mapping it is read from.
    struct KeyStep
    {
        YAML::Node container;                ///< The mapping the key is read from.
        std::string entry;                   ///< The key, or the side file's path.
        KeyStepKind kind = KeyStepKind::Key; ///< What @ref entry names.
    };

    /// The keys from the document's root down to the one loadGuarded() is reading.
    std::vector<KeyStep> _keyPath;
};

struct Writer
{
    virtual ~Writer() = default;

    struct Offset
    {
        static inline int Levels = 0;
        Offset() { Levels++; }
        ~Offset() { --Levels; }
    };

    template <typename F>
    void scoped(F lambda)
    {
        auto const _ = Offset {};
        lambda();
    }

    virtual inline std::string replaceCommentPlaceholder(std::string const& docString) = 0;

    template <typename... T>
    [[nodiscard]] std::string format(std::string_view doc, T... args)
    {
        return std::vformat(replaceCommentPlaceholder(std::string(doc)), std::make_format_args(args...));
    }

    [[nodiscard]] std::string format(KeyInputMapping const& v)
    {
        return formatKeyBinding(v, std::format("{}", v.input));
    }

    [[nodiscard]] std::string format(CharInputMapping const& v)
    {
        // As UTF-8, which is how the reader takes a one-character key back.
        return formatKeyBinding(v, unicode::convert_to<char>(v.input));
    }

    /// One `key:` binding row.
    /// @param v The binding.
    /// @param key How the binding's key is spelled, unquoted.
    /// @return The row, ending in a newline.
    template <typename Binding>
    [[nodiscard]] std::string formatKeyBinding(Binding const& v, std::string_view key)
    {
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      format(" key: {}", SingleQuoted { key }),
                      formatActionAndModes(v));
    }

    /// The tail of a binding row: its action, then its `mode:` when it names one, closing the map.
    template <typename Binding>
    [[nodiscard]] std::string formatActionAndModes(Binding const& v)
    {
        if (!v.modes.any())
            return format(" action: {} }}", v.binding[0]);
        return format(" action: {}, mode: {} }}", v.binding[0], SingleQuoted { std::format("{}", v.modes) });
    }

    [[nodiscard]] std::string format(MouseInputMapping v)
    {
        auto button = v.clickCount ? format(" mouse: {}, clicks: {}", v.input, *v.clickCount)
                                   : format(" mouse: {}", v.input);
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      button,
                      formatActionAndModes(v));
    }

    [[nodiscard]] std::string static format(std::vector<text::FontFeature> const& v)
    {

        auto result = std::string { "[" };
        result.append(v | std::views::transform([](auto f) { return std::format("{}", f); })
                      | core::views::joinWith(std::string_view(", ")));
        result.append("]");
        return result;
    }

    [[nodiscard]] std::string static format(vtbackend::Modifiers const& flags)
    {
        std::string result;
        for (auto i = 0u; i < sizeof(vtbackend::Modifier) * 8; ++i)
        {
            auto const flag = static_cast<vtbackend::Modifier>(1 << i);
            if (!flags.test(flag))
                continue;

            // We assume that only valid enum values resulting into non-empty strings.
            auto const element = std::format("{}", flag);
            if (element.empty())
                continue;

            if (!result.empty())
                result += ',';

            result += element;
        }
        return result;
    }

    [[nodiscard]] std::string format(std::string_view doc, vtrasterizer::FontDescriptions const& v)
    {
        return format(doc,
                      v.size.pt,
                      v.fontLocator,
                      v.textShapingEngine,
                      v.builtinBoxDrawing,
                      v.maxFallbackCount,
                      v.renderMode,
                      // strict_spacing is stored per face, but the reader fans one value out to
                      // all four, so the regular face carries it for the whole block.
                      v.regular.strictSpacing,
                      v.regular.familyName,
                      v.regular.weight,
                      v.regular.slant,
                      format(v.regular.features),
                      v.emoji.familyName);
    }

    template <typename T, typename R>
    [[nodiscard]] std::string format(std::string_view doc, std::chrono::duration<T, R> v)
    {
        return format(doc, v.count());
    }

    [[nodiscard]] std::string format(std::string_view doc, vtpty::Process::ExecInfo const& v)
    {
        auto args = std::string { "[" };
        args.append(v.arguments | core::views::joinWith(std::string_view(", ")));
        args.append("]");
        return format(doc, v.program, args, spelledWorkingDirectory(v.workingDirectory));
    }

    [[nodiscard]] std::string format(std::string_view doc, std::map<vtbackend::DECMode, bool> const& mods)
    {
        if (mods.empty())
            return format(doc, std::string_view { "0" });

        auto result = std::string { "[" };
        for (auto const& [mode, value]: mods)
        {
            if (!result.empty())
                result += ',';
            result += std::format("{}", toString(mode));
        }
        result += "]";
        return format(doc, result);
    }

    [[nodiscard]] std::string format(std::string_view doc, vtpty::SshHostConfig const& v)
    {
        return format(doc, v.hostname);
    }

    /// Serializes hint patterns: outputs the doc template (pure comment) followed by any user entries.
    [[nodiscard]] std::string format(std::string_view doc, std::vector<HintPatternConfig> const& patterns)
    {
        // doc is already processed by process() (comment placeholders replaced, indentation applied).
        auto result = std::string { doc };
        if (!patterns.empty())
        {
            // Derive indentation from the first non-empty line in doc.
            auto indent = std::string {};
            for (auto const ch: doc)
            {
                if (ch == '\n')
                {
                    indent.clear();
                    continue;
                }
                if (ch == ' ' || ch == '\t')
                    indent.push_back(ch);
                else
                    break;
            }
            result += indent + "hint_patterns:\n";
            for (auto const& p: patterns)
            {
                result += std::format("{}    - name: {}\n", indent, SingleQuoted { p.name });
                if (!p.regex.empty())
                    result += std::format("{}      regex: {}\n", indent, SingleQuoted { p.regex });
                if (p.clickable)
                    result += std::format("{}      clickable: {}\n", indent, *p.clickable);
                if (p.url)
                    result += std::format("{}      url: {}\n", indent, SingleQuoted { *p.url });
            }
        }
        return result;
    }

    /// Serializes extensions: the doc template (pure comment) followed by the entries, if any.
    /// @param doc The documentation, already indented.
    /// @param extensions The profile's extensions, written in order.
    /// @return @p doc, then an `extensions:` map when @p extensions holds an entry.
    [[nodiscard]] std::string format(std::string_view doc, std::vector<ExtensionConfig> const& extensions)
    {
        auto result = format(doc);
        if (extensions.empty())
            return result;

        // The doc is indented to where the key goes: derive that from its last line.
        auto const indent = [&] {
            auto const lastLine = doc.substr(doc.rfind('\n', doc.size() > 1 ? doc.size() - 2 : 0) + 1);
            return std::string(std::min(lastLine.find_first_not_of(' '), lastLine.size()), ' ');
        }();
        result += indent + "extensions:\n";
        for (auto const& extension: extensions)
        {
            result += std::format("{}    {}:\n", indent, SingleQuoted { extension.name });
            if (!extension.path.empty())
                result += std::format(
                    "{}        path: {}\n", indent, SingleQuoted { core::pathToUtf8(extension.path) });
            if (!extension.config.empty())
            {
                result += std::format("{}        config:\n", indent);
                for (auto const& [key, value]: extension.config)
                    result += std::format(
                        "{}            {}: {}\n", indent, SingleQuoted { key }, SingleQuoted { value });
            }
            result +=
                std::format("{}        run_command: {}\n", indent, permissionToken(extension.runCommand));
        }
        return result;
    }

    [[nodiscard]] std::string static format(vtbackend::CellRGBColor const& v)
    {
        if (auto const* const color = std::get_if<vtbackend::RGBColor>(&v))
            return format(*color);

        return std::format("{}", v);
    }

    [[nodiscard]] std::string static format(vtbackend::RGBColor const& v)
    {
        return std::format("{}", SingleQuoted { std::format("{}", v) });
    }

    /// Formats an optional color as a YAML scalar.
    /// @param v Color to quote, or no value for null.
    /// @return A quoted '#RRGGBB' value or the unquoted null literal.
    [[nodiscard]] std::string static format(std::optional<vtbackend::RGBColor> const& v)
    {
        return v ? std::format("{}", SingleQuoted { vtbackend::formatColor(*v) }) : "null";
    }

    /// Substitutes an optional color into a documentation template.
    /// @param doc Format string containing the color placeholder.
    /// @param v Optional color to serialize.
    /// @return The template with the serialized color substituted.
    [[nodiscard]] std::string format(std::string_view doc, std::optional<vtbackend::RGBColor> const& v)
    {
        return format(doc, format(v));
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::ImageSize v)
    {
        return format(doc, unbox(v.width), unbox(v.height));
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::PageSize v)
    {
        return format(doc, unbox(v.columns), unbox(v.lines));
    }

    [[nodiscard]] std::string format(std::string_view doc, ColorConfig const& v)
    {
        if (auto const* simple = get_if<SimpleColorConfig>(&v))
            return format(doc, simple->colorScheme);
        else if (auto const* dual = get_if<DualColorConfig>(&v))
        {
            auto const formattedValue = format("\n"
                                               "    light: {}\n"
                                               "    dark: {}\n",
                                               dual->colorSchemeLight,
                                               dual->colorSchemeDark);
            return format(doc, formattedValue);
        }

        return format(doc, "BAD");
    }

    [[nodiscard]] std::string format(std::string_view doc, Bell const& v)
    {
        return format(doc, v.sound, v.volume, v.alert);
    }

    [[nodiscard]] std::string format(std::string_view doc, FinishNotificationConfig const& v)
    {
        return format(doc, v.when, v.minDuration.count(), v.outcome, v.action, v.clearOn);
    }

    [[nodiscard]] std::string format(std::string_view doc, RendererConfig const& v)
    {
        return format(doc,
                      v.renderingBackend,
                      v.gpu,
                      v.textureAtlasDirectMapping,
                      v.textureAtlasHashtableSlots,
                      v.textureAtlasTileCount);
    }

    [[nodiscard]] std::string format(std::string_view doc, ImagesConfig const& v)
    {
        return format(doc, v.sixelScrolling, v.maxImageColorRegisters, v.goodImageProtocol);
    }

    [[nodiscard]] std::string format(std::string_view doc, DaemonSettings const& v)
    {
        auto const historyLimit = [&]() -> std::string {
            if (!v.historyLimit)
                return "~";
            if (std::holds_alternative<vtbackend::Infinite>(*v.historyLimit))
                return "-1";
            return std::format("{}", unbox(std::get<vtbackend::LineCount>(*v.historyLimit)));
        }();
        auto rerunPrograms = std::string { "[" };
        for (auto const& program: v.restore.rerunPrograms)
            rerunPrograms +=
                std::format("{}{}", rerunPrograms.size() > 1 ? ", " : "", SingleQuoted { program });
        rerunPrograms += "]";
        return format(doc,
                      nameOf(v.mode),
                      SingleQuoted { v.socket },
                      vthost::nameOf(v.sizePolicy),
                      historyLimit,
                      v.restore.scrollback,
                      v.restore.scrollbackLines,
                      rerunPrograms,
                      v.restore.interval.count());
    }

    [[nodiscard]] std::string format(std::string_view doc, QuickTerminalConfig const& v)
    {
        auto const hotkey = [&]() -> std::string {
            if (!v.hotkey)
                return "~";
            return hotkeyYaml(*v.hotkey);
        }();
        return format(doc, hotkey, v.focusLoss == QuickTerminalFocusLoss::Hide);
    }

    [[nodiscard]] std::string format(std::string_view doc, WindowMargins const& v)
    {
        return format(doc, v.horizontal, v.vertical);
    }

    [[nodiscard]] std::string format(std::string_view doc, FoldingConfig const& v)
    {
        return format(doc, v.enabled, v.showMarkers, v.autoCollapseOnNewCommand, v.onJumpIntoFold);
    }

    [[nodiscard]] std::string format(std::string_view doc, GutterConfig const& v)
    {
        // Single-quoted: a plain YAML scalar cannot begin with `%`.
        return format(doc,
                      v.exitStatus,
                      v.userMarks,
                      v.blockColumnWidth,
                      v.boldOnHover,
                      v.lineNumbers,
                      v.lineNumberWidth,
                      v.timestamps,
                      SingleQuoted { v.timestampFormat });
    }

    [[nodiscard]] std::string format(std::string_view doc, OscContextConfig const& v)
    {
        return format(doc, v.enabled, v.maxDepth, v.maxRetained, v.deriveMarkers, v.tinting);
    }

    [[nodiscard]] std::string format(std::string_view doc, CommandBlocksConfig const& v)
    {
        return format(doc, v.maxRecords, SingleQuoted { v.pager }, v.pagerFormat);
    }

    [[nodiscard]] std::string format(std::string_view doc, HistoryConfig const& v)
    {
        auto const spell = [](vtbackend::MaxHistoryLineCount limit) {
            if (std::holds_alternative<vtbackend::Infinite>(limit))
                return -1;
            return unbox(std::get<vtbackend::LineCount>(limit));
        };
        return format(doc,
                      spell(v.maxHistoryLineCount),
                      // The resolved ceiling rather than the raw field: an unset hard_limit is zero,
                      // and writing that back would read as a scrollback of none.
                      spell(v.limits().capacity),
                      v.autoScrollOnUpdate,
                      v.historyScrollMultiplier);
    }

    [[nodiscard]] std::string format(std::string_view doc, ScrollBarConfig const& v)
    {
        return format(doc, v.position, v.mode, v.hideScrollbarInAltScreen, format(v.marks));
    }

    /// `scrollbar.marks` as the YAML flow list the reader accepts, spelled from the same token table.
    /// @param sources The configured sources.
    /// @return E.g. `[failures, commands, user_marks]`, or `[]`.
    [[nodiscard]] static std::string format(vtbackend::ScrollbarMarkSources sources)
    {
        using Info = ConfigEnumInfo<vtbackend::ScrollbarMarkSource>;
        auto const tokens = std::ranges::to<std::vector>(
            configEnumValues<vtbackend::ScrollbarMarkSource>()
            | std::views::filter([sources](Info const& info) { return sources.test(info.value); })
            | std::views::transform(&Info::token));
        return std::format("[{}]", tokens | core::views::joinWith(std::string_view(", ")));
    }

    [[nodiscard]] std::string format(std::string_view doc, StickyHeaderConfig const& v)
    {
        return format(doc, v.mode, v.showEvicted);
    }

    [[nodiscard]] std::string format(std::string_view doc, MouseConfig const& v)
    {
        return format(doc, v.hideWhileTyping);
    }

    [[nodiscard]] std::string format(std::string_view doc, StatusLineConfig const& v)
    {
        return format(doc,
                      v.initialType,
                      v.position,
                      v.syncWindowTitleWithHostWritableStatusDisplay,
                      v.indicator.left,
                      v.indicator.middle,
                      v.indicator.right,
                      v.extension ? std::format("{}", SingleQuoted { *v.extension })
                                  : std::string { "null" });
    }

    [[nodiscard]] std::string format(std::string_view doc, BackgroundConfig const& v)
    {
        return format(doc, v.opacity, v.blur);
    }

    [[nodiscard]] std::string format(std::string_view doc, HyperlinkDecorationConfig const& v)
    {
        return format(doc, v.mode, v.normal, v.hover);
    }

    [[nodiscard]] std::string format(std::string_view doc, LinkDetectionConfig const& v)
    {
        return format(doc,
                      v.primaryScreen.urls,
                      v.primaryScreen.filePaths,
                      v.otherScreens.urls,
                      v.otherScreens.filePaths);
    }

    [[nodiscard]] std::string format(std::string_view doc, FileOpenerConfig const& v)
    {
        return format(doc, SingleQuoted { v.command }, v.placement);
    }

    [[nodiscard]] std::string format(std::string_view doc, PermissionsConfig const& v)
    {
        return format(doc, v.captureBuffer, v.changeFont, v.displayHostWritableStatusLine);
    }

    [[nodiscard]] std::string format(std::string_view doc, InputModeConfig v)
    {
        auto const shape = [v]() -> std::string_view {
            switch (v.cursor.cursorShape)
            {
                case vtbackend::CursorShape::Block: return "block";
                case vtbackend::CursorShape::Rectangle: return "rectangle";
                case vtbackend::CursorShape::Underscore: return "underscore";
                case vtbackend::CursorShape::Bar: return "bar";
            };
            return "unknown";
        }();
        auto const blinking = v.cursor.cursorDisplay == vtbackend::CursorDisplay::Blink;
        auto const blinkingInterval = v.cursor.cursorBlinkInterval.count();
        return format(doc, shape, blinking, blinkingInterval);
    }

    [[nodiscard]] std::string format(std::string_view doc,
                                     vtrasterizer::BoxDrawingRenderer::ArcStyle const& v)
    {
        using ArcStyle = vtrasterizer::BoxDrawingRenderer::ArcStyle;
        switch (v)
        {
            case ArcStyle::Elliptic: return format(doc, "elliptic");
            case ArcStyle::Round: return format(doc, "round");
            default: assert(false); return format(doc, "round");
        }
    }

    [[nodiscard]] std::string format(std::string_view doc,
                                     vtrasterizer::BoxDrawingRenderer::BrailleStyle const& v)
    {
        using BrailleStyle = vtrasterizer::BoxDrawingRenderer::BrailleStyle;
        switch (v)
        {
            case BrailleStyle::Font: return format(doc, "font");
            case BrailleStyle::Solid: return format(doc, "solid");
            case BrailleStyle::Circle: return format(doc, "circle");
            case BrailleStyle::CircleEmpty: return format(doc, "circle_empty");
            case BrailleStyle::Square: return format(doc, "square");
            case BrailleStyle::SquareEmpty: return format(doc, "square_empty");
            case BrailleStyle::AASquare: return format(doc, "aa_square");
            case BrailleStyle::AASquareEmpty: return format(doc, "aa_square_empty");
            default: assert(false); return format(doc, "circle");
        }
    }

    [[nodiscard]] std::string format(std::string_view doc,
                                     vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle const& v)
    {
        using ArcStyle = vtrasterizer::BoxDrawingRenderer::ArcStyle;
        using DrawingStyle = vtrasterizer::BoxDrawingRenderer::GitDrawingsStyle;
        auto const arc = [&]() -> std::string_view {
            switch (v.arcStyle)
            {
                case ArcStyle::Elliptic: return "elliptic";
                case ArcStyle::Round: return "round";
                default: assert(false); return "round";
            }
        }();
        auto const branch = [&]() -> std::string_view {
            switch (v.branchStyle)
            {
                case DrawingStyle::BranchStyle::None: return "none";
                case DrawingStyle::BranchStyle::Thin: return "thin";
                case DrawingStyle::BranchStyle::Double: return "double";
                case DrawingStyle::BranchStyle::Thick: return "thick";
                default: assert(false); return "thin";
            }
        }();
        auto const mergeCommit = [&]() -> std::string_view {
            switch (v.mergeCommitStyle)
            {
                case DrawingStyle::MergeCommitStyle::Solid: return "solid";
                case DrawingStyle::MergeCommitStyle::Bullet: return "bullet";
                default: assert(false); return "solid";
            }
        }();
        return format(doc, branch, arc, mergeCommit);
    }
};

template <typename T>
std::string createString(Config const& c);

template <typename T>
std::string documentationGlobalConfig(Config const& c);

template <typename T>
std::string documentationProfileConfig(Config const& c);

struct YAMLConfigWriter: Writer
{

    constexpr static std::string_view FormatTemplate = "{}";
    std::string replaceCommentPlaceholder(std::string const& docString) override
    {
        return std::regex_replace(docString, std::regex { "\\{comment\\}" }, "#");
    }

    static constexpr int OneOffset = 4;
    using Writer::format;
    std::string static addOffset(std::string_view doc, size_t off)
    {
        auto offset = std::string(off, ' ');
        return std::regex_replace(std::string { doc }, std::regex(".+\n"), offset + "$&");
    }

    /// Writes a color configuration. A dark/light pair is a block map, so its keys are indented one level
    /// deeper than the `colors:` key itself: process() indents the documentation template, never the
    /// lines a value brings with it.
    /// @param doc The entry's documentation, with one placeholder for the value.
    /// @param v The color configuration.
    /// @return @p doc filled in.
    [[nodiscard]] std::string formatColorConfig(std::string_view doc, ColorConfig const& v)
    {
        auto const* dual = std::get_if<DualColorConfig>(&v);
        if (dual == nullptr)
            return Writer::format(doc, v);
        auto const indent = std::string((static_cast<size_t>(Offset::Levels) + 1) * OneOffset, ' ');
        return Writer::format(
            doc,
            std::format(
                "\n{0}light: {1}\n{0}dark: {2}", indent, dual->colorSchemeLight, dual->colorSchemeDark));
    }

    /// Fills @p doc with @p val: the Writer's own formatting, except for a colour configuration.
    template <typename... T>
    std::string formatValue(std::string_view doc, T const&... val)
    {
        return format(doc, val...);
    }

    /// A colour configuration's dark/light pair needs this writer's indentation. @see formatColorConfig
    std::string formatValue(std::string_view doc, ColorConfig const& val)
    {
        return formatColorConfig(doc, val);
    }

    template <typename... T>
    std::string process(std::string_view doc, T const&... val)
    {
        return formatValue(addOffset(replaceCommentPlaceholder(std::string { doc }),
                                     static_cast<size_t>(Offset::Levels) * OneOffset),
                           val...);
    }

    /// Writes the configuration entry a reflection walk reached under @p name, which is dropped: @p doc
    /// already spells the entry's key.
    ///
    /// A function of its own, not an overload of process(): one taking the name first also takes a row
    /// whose first value is a std::string_view -- a tint's context type -- and drops that value instead.
    /// @param doc The entry's documentation, with a placeholder per value.
    /// @param name The entry's member name.
    /// @param val The entry's values.
    /// @return @p doc, indented and filled in.
    template <typename... T>
    std::string processEntry(std::string_view doc, [[maybe_unused]] std::string_view name, T const&... val)
    {
        return process(doc, val...);
    }

    template <typename T, typename Doc>
    constexpr std::string_view whichDoc(ConfigEntry<T, Doc> const&)
    {
        return documentation::Text<Doc>::Config;
    }

    /// The documentation of an entry that is not a ConfigEntry, named by its tag alone.
    template <typename Doc>
    constexpr std::string_view whichDoc(Doc const&)
    {
        return documentation::Text<Doc>::Config;
    }
};

struct DocumentationWriter: Writer
{
    constexpr static std::string_view FormatTemplate = "{}";
    std::string replaceCommentPlaceholder(std::string const& docString) override
    {
        return std::regex_replace(docString, std::regex { "\\{comment\\}" }, "");
    }

    static constexpr int OneOffset = 0;
    std::string static addOffset(std::string_view doc, [[maybe_unused]] size_t off)
    {
        return std::string { doc };
    }

    using Writer::format;
    template <typename... T>
    std::string process(std::string_view doc, T const&... val)
    {
        return processEntry(doc, std::string_view { "" }, val...);
    }

    /// Writes the configuration entry a reflection walk reached, headed by @p name in snake_case.
    /// @see YAMLConfigWriter::processEntry for why this is not an overload of process().
    /// @param doc The entry's documentation, with a placeholder per value.
    /// @param name The entry's member name.
    /// @param val The entry's values.
    /// @return The heading, then @p doc filled in.
    template <typename... T>
    std::string processEntry(std::string_view doc, std::string_view name, T const&... val)
    {
        return format(
            "### `{}`\n"
            "{}\n",
            [](std::string_view name) -> std::string {
                // camelCase into snake_case
                auto result = std::string { name };
                for (auto i = 0u; i < result.size(); ++i)
                {
                    if (std::isupper(result[i]))
                    {
                        result.insert(i, 1, '_');
                        result[i + 1] = static_cast<char>(std::tolower(result[i + 1]));
                    }
                }
                return result;
            }(name),
            format(replaceCommentPlaceholder(std::string { doc }), val...));
    }

    template <typename T, typename Doc>
    constexpr std::string_view whichDoc(ConfigEntry<T, Doc> const&)
    {
        return documentation::Text<Doc>::Web;
    }

    /// The documentation of an entry that is not a ConfigEntry, named by its tag alone.
    template <typename Doc>
    constexpr std::string_view whichDoc(Doc const&)
    {
        return documentation::Text<Doc>::Web;
    }
};

// Will ignore documentation
struct PlainWriter: Writer
{
    constexpr static std::string_view FormatTemplate = "{}";
    std::string replaceCommentPlaceholder(std::string const& docString) override
    {
        return std::regex_replace(docString, std::regex { "\\{comment\\}" }, "");
    }

    static constexpr int OneOffset = 0;
    std::string static addOffset(std::string_view doc, [[maybe_unused]] size_t off)
    {
        return std::string { doc };
    }

    using Writer::format;
    template <typename... T>
    std::string process([[maybe_unused]] std::string_view doc, T... val)
    {
        return format("{}", format(val...));
    }
};

} // namespace contour::config
