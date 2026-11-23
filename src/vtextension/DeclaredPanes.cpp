// SPDX-License-Identifier: Apache-2.0
#include <vtextension/DeclaredPanes.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ArgumentKind.hpp>
#include <vtextension/ChildEnvironment.hpp>
#include <vtextension/DeclaredArguments.hpp>
#include <vtextension/Utf8.hpp>

using namespace std::string_view_literals;

namespace vtextension
{

namespace
{
    /// Why a working directory is refused, indexed by CwdRefusal: the words declared processes use too.
    constexpr auto CwdErrorOfRefusal = std::array {
        PaneError::CwdOutsideRoot,   // OutsideRoot
        PaneError::CwdMissing,       // Missing
        PaneError::CwdNotADirectory, // NotADirectory
        PaneError::NotSupportedYet,  // Unsupported
    };
    static_assert(CwdErrorOfRefusal.size() == std::to_underlying(CwdRefusal::Unsupported) + 1U);

    /// @return The name and value of a `NAME=value` entry; nullopt for one without `=`.
    [[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>> splitEntry(
        std::string_view entry) noexcept
    {
        auto const equals = entry.find('=');
        if (equals == std::string_view::npos)
            return std::nullopt;
        return std::pair { entry.substr(0, equals), entry.substr(equals + 1) };
    }

    /// @return Whether @p name is a variable of @p family: its count, or a key or value with a numeric index.
    [[nodiscard]] bool belongsTo(IndexedEnvironmentFamily const& family, std::string_view name) noexcept
    {
        return name == family.count || name.starts_with(family.keyPrefix)
               || name.starts_with(family.valuePrefix);
    }

    /// @return @p text as a non-negative number; nullopt for anything else.
    [[nodiscard]] std::optional<std::uint32_t> numberOf(std::string_view text) noexcept
    {
        auto value = std::uint32_t { 0 };
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc {} || end != text.data() + text.size())
            return std::nullopt;
        return value;
    }

    /// @return The inherited value of @p name, if set.
    [[nodiscard]] std::optional<std::string_view> inheritedValue(std::span<std::string const> inherited,
                                                                 std::string_view name) noexcept
    {
        for (std::string_view const entry: inherited)
            if (auto const split = splitEntry(entry); split && split->first == name)
                return split->second;
        return std::nullopt;
    }

    /// @return @p name of @p family renumbered by @p offset: a key or value's index raised; the count and
    ///         any other name as given.
    [[nodiscard]] std::string renumbered(IndexedEnvironmentFamily const& family,
                                         std::string_view name,
                                         std::uint32_t offset)
    {
        for (auto const prefix: { family.keyPrefix, family.valuePrefix })
            if (name.starts_with(prefix))
                if (auto const index = numberOf(name.substr(prefix.size())))
                    return std::format("{}{}", prefix, *index + offset);
        return std::string { name };
    }

    /// @return Whether @p title is fit for a tab's name: at most MaxPaneTitleLength code points of valid
    /// UTF-8,
    ///         none of them a control or disguising one.
    [[nodiscard]] bool isPaneTitle(std::string_view title) noexcept
    {
        auto length = std::size_t { 0 };
        while (!title.empty())
        {
            auto const decoded = decodeUtf8(title);
            if (!decoded || disguisesText(decoded->first) || ++length > MaxPaneTitleLength)
                return false;
            title.remove_prefix(decoded->second);
        }
        return true;
    }
} // namespace

std::vector<std::string> paneWithheldNames(std::span<std::string const> inherited)
{
    auto names = std::vector<std::string> {};
    for (std::string_view const entry: inherited)
    {
        auto const split = splitEntry(entry);
        if (!split || inheritanceOf(split->first, NativeEnvironmentNameCase) == Inheritance::Kept)
            continue;
        auto const appended = std::ranges::any_of(
            AppendedPaneEnvironment, [&](auto const& family) { return belongsTo(family, split->first); });
        if (!appended)
            names.emplace_back(split->first);
    }
    return names;
}

std::vector<std::pair<std::string, std::string>> paneEnvironment(std::span<EnvironmentEntry const> declared,
                                                                 std::span<std::string const> inherited)
{
    auto environment = std::vector<std::pair<std::string, std::string>> {};
    environment.reserve(declared.size());
    for (auto const& [name, value]: declared)
    {
        auto const family = std::ranges::find_if(
            AppendedPaneEnvironment, [&](auto const& candidate) { return belongsTo(candidate, name); });
        auto const offset = family == AppendedPaneEnvironment.end()
                                ? std::nullopt
                                : inheritedValue(inherited, family->count).and_then(numberOf);
        if (!offset)
        {
            environment.emplace_back(name, value);
            continue;
        }
        if (name != family->count)
        {
            environment.emplace_back(renumbered(*family, name, *offset), value);
            continue;
        }
        auto const declaredCount = numberOf(value);
        environment.emplace_back(name, declaredCount ? std::to_string(*declaredCount + *offset) : value);
    }
    return environment;
}

std::expected<std::unique_ptr<DeclaredPanes>, RootError> DeclaredPanes::create(
    DeclaredPanesConfig config, vtpty::IDirectoryOpener& directories, IPaneHost& host)
{
    if (auto const distinct = checkRootsDistinct(config.roots, directories.pathStyle()); !distinct)
        return std::unexpected(distinct.error());
    auto const resolved = [&](std::string_view label) {
        return std::ranges::contains(config.roots, label, &ResolvedRoot::label);
    };
    auto const templateRootsResolved = std::ranges::all_of(
        config.manifest.panes, [&](PaneTemplate const& pane) { return resolved(pane.root); });
    auto const argumentRootsResolved =
        std::ranges::all_of(config.manifest.arguments, [&](ArgumentDecl const& argument) {
            return argument.kind != ArgumentKind::Path || resolved(argument.root);
        });
    if (!templateRootsResolved || !argumentRootsResolved)
        return std::unexpected(RootError::Unresolved);
    return std::make_unique<DeclaredPanes>(Key {}, std::move(config), directories, host);
}

DeclaredPanes::DeclaredPanes(Key /*key*/,
                             DeclaredPanesConfig config,
                             vtpty::IDirectoryOpener& directories,
                             IPaneHost& host):
    _config { std::move(config) },
    _directories { directories },
    _host { host },
    _withheld { paneWithheldNames(_config.inheritedEnvironment) }
{
}

std::expected<std::string, PaneError> DeclaredPanes::open(std::string_view paneTemplate,
                                                          ConfigMap const& args,
                                                          std::string_view cwd,
                                                          std::string_view title,
                                                          std::string_view tag,
                                                          std::optional<std::string_view> nearTab)
{
    auto const declared = std::ranges::find(_config.manifest.panes, paneTemplate, &PaneTemplate::name);
    if (declared == _config.manifest.panes.end())
        return std::unexpected(PaneError::NotDeclared);

    if (!isPaneTitle(title))
        return std::unexpected(PaneError::BadTitle);
    if (!detail::isWord(tag, MaxPaneTagBytes, "._-"))
        return std::unexpected(PaneError::BadTag);

    auto argv =
        substituteDeclared(declared->argvTemplate, _config.manifest, args, _directories, _config.roots);
    if (!argv)
        return std::unexpected(PaneError::ArgumentRejected);

    resolveProgram(*argv, _config.programs);

    auto directory = openInRoot(_directories, _config.roots, declared->root, cwd);
    if (!directory)
        return std::unexpected(CwdErrorOfRefusal.at(std::to_underlying(cwdRefusalOf(directory.error()))));
    auto resolvedCwd = directory->resolvedPath();

    return _host.open(PaneSpawn {
        .argv = std::move(*argv),
        .environment = paneEnvironment(declared->environment, _config.inheritedEnvironment),
        .removedEnvironment = _withheld,
        .directory = std::make_shared<vtpty::DirectoryHandle const>(std::move(*directory)),
        .cwd = std::move(resolvedCwd),
        .title = std::string { title },
        .tag = std::string { tag },
        .nearTab = nearTab.transform([](std::string_view tab) { return std::string { tab }; }),
    });
}

std::expected<void, PaneError> DeclaredPanes::close(std::string_view pane)
{
    return _host.close(pane);
}

std::expected<std::vector<PaneInfo>, PaneError> DeclaredPanes::list()
{
    return _host.list();
}

std::expected<void, PaneError> DeclaredPanes::reveal(std::string_view pane, Origin origin)
{
    return _host.reveal(pane, origin);
}

bool isSendableText(std::string_view text) noexcept
{
    if (text.empty() || text.size() > MaxPaneSendBytes)
        return false;
    while (!text.empty())
    {
        auto const decoded = decodeUtf8(text);
        if (!decoded)
            return false;
        auto const [codePoint, length] = *decoded;
        auto const isControl = codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F);
        if (isControl && codePoint != U'\n' && codePoint != U'\t')
            return false;
        text.remove_prefix(length);
    }
    return true;
}

std::expected<void, PaneError> DeclaredPanes::send(std::string_view pane, std::string_view text)
{
    if (_config.manifest.paneInput != PaneInput::Typed)
        return std::unexpected(PaneError::InputNotDeclared);
    if (!isSendableText(text))
        return std::unexpected(PaneError::TextRefused);
    return _host.write(pane, std::format("{}{}{}{}", PasteStart, text, PasteEnd, SendKey));
}

std::string_view bytesOf(PaneKeyName key) noexcept
{
    // Indexed by PaneKeyName.
    constexpr auto KeyBytes = std::array {
        "1"sv, "2"sv, "3"sv, "4"sv, "5"sv, "6"sv, "7"sv, "8"sv, "9"sv, "\x1b"sv,
    };
    static_assert(KeyBytes.size() == std::to_underlying(PaneKeyName::Escape) + 1U, "every key has its bytes");
    return KeyBytes.at(std::to_underlying(key));
}

std::expected<void, PaneError> DeclaredPanes::press(std::string_view pane, PaneKeyName key)
{
    // The same policy as pane-send: a key is input, so it takes the manifest's `pane-input`.
    if (_config.manifest.paneInput != PaneInput::Typed)
        return std::unexpected(PaneError::InputNotDeclared);
    return _host.write(pane, bytesOf(key));
}

} // namespace vtextension
