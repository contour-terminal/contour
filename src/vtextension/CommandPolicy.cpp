// SPDX-License-Identifier: Apache-2.0
#include <vtextension/CommandPolicy.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtextension
{

namespace
{
    using Args = std::span<std::pair<std::string, std::string> const>;

    /// The template element after which a leading `-` in a value is data, not an option.
    constexpr auto EndOfOptions = std::string_view("--");

    /// Where in a template an element stands, as far as option parsing goes.
    enum class ArgvPosition : uint8_t
    {
        MayBeOption, ///< Before any literal `--`: a leading `-` would be read as an option.
        Operand,     ///< After a literal `--`: a leading `-` is data.
    };

    /// Whether @p value could be taken for an option, or cannot be passed to a program at all.
    /// @param position Where the placeholder the value fills stands in the template.
    [[nodiscard]] bool isRejected(std::string_view value, ArgvPosition position) noexcept
    {
        return value.contains('\0') || (position == ArgvPosition::MayBeOption && value.starts_with('-'));
    }

    /// Fills one argv template element: a placeholder becomes the argument of that name, any other
    /// element stays as it is.
    [[nodiscard]] std::expected<std::string, ExtensionError> fill(std::string const& element,
                                                                  Args args,
                                                                  ArgvPosition position)
    {
        auto const name = placeholderName(element);
        if (!name)
            return element;
        auto const argument = std::ranges::find(args, *name, &std::pair<std::string, std::string>::first);
        if (argument == args.end())
            return std::unexpected(ExtensionError::CommandNotDeclared);
        if (isRejected(argument->second, position))
            return std::unexpected(ExtensionError::ArgumentRejected);
        return argument->second;
    }

    /// Fills every element of a command's argv template.
    [[nodiscard]] std::expected<std::vector<std::string>, ExtensionError> fillAll(
        std::vector<std::string> const& argvTemplate, Args args)
    {
        auto argv = std::vector<std::string>();
        argv.reserve(argvTemplate.size());
        auto position = ArgvPosition::MayBeOption;
        for (auto const& element: argvTemplate)
        {
            auto filled = fill(element, args, position);
            if (!filled)
                return std::unexpected(filled.error());
            argv.push_back(*std::move(filled));
            if (element == EndOfOptions)
                position = ArgvPosition::Operand;
        }
        return argv;
    }

} // namespace

CommandPolicy::CommandPolicy(Manifest manifest,
                             std::string hostId,
                             IGrantStore& store,
                             CommandPermission configured):
    _manifest { std::move(manifest) },
    _hostId { std::move(hostId) },
    _store { store },
    _configured { configured }
{
}

std::expected<std::vector<std::string>, ExtensionError> CommandPolicy::resolve(
    std::string_view name, std::span<std::pair<std::string, std::string> const> args) const
{
    if (_configured == CommandPermission::Deny)
        return std::unexpected(ExtensionError::PermissionDenied);

    auto const declared = std::ranges::find(_manifest.commands, name, &DeclaredCommand::name);
    if (declared == _manifest.commands.end())
        return std::unexpected(ExtensionError::CommandNotDeclared);

    switch (_store.lookup(_hostId, _manifest.digest))
    {
        case GrantState::Granted: break;
        case GrantState::Denied: return std::unexpected(ExtensionError::PermissionDenied);
        case GrantState::Pending:
            if (_configured != CommandPermission::Allow)
                return std::unexpected(ExtensionError::PermissionPending);
            break;
    }
    return fillAll(declared->argvTemplate, args);
}

} // namespace vtextension
