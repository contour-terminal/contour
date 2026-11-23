// SPDX-License-Identifier: Apache-2.0
#include <vtextension/DeclaredArguments.hpp>

#include <algorithm>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vtextension/ArgumentKind.hpp>

namespace vtextension
{

std::expected<std::vector<std::string>, ArgumentFault> substituteDeclared(
    std::span<std::string const> argvTemplate,
    Manifest const& manifest,
    ConfigMap const& args,
    vtpty::IDirectoryOpener& directories,
    std::span<ResolvedRoot const> roots)
{
    auto const isPlaceholder = [&](std::string_view name) {
        return std::ranges::any_of(
            argvTemplate, [&](std::string const& element) { return placeholderName(element) == name; });
    };
    for (auto const& [name, value]: args)
        if (!isPlaceholder(name) || std::ranges::count(args, name, &ConfigMap::value_type::first) != 1)
            return std::unexpected(ArgumentFault::NotAPlaceholder);

    auto argv = std::vector<std::string> {};
    argv.reserve(argvTemplate.size());
    for (auto const& element: argvTemplate)
    {
        auto const name = placeholderName(element);
        if (!name)
        {
            argv.push_back(element);
            continue;
        }
        auto const argument = std::ranges::find(args, *name, &ConfigMap::value_type::first);
        if (argument == args.end())
            return std::unexpected(ArgumentFault::Missing);
        auto const declared = std::ranges::find(manifest.arguments, *name, &ArgumentDecl::name);
        if (declared == manifest.arguments.end()
            || !argumentAccepts(declared->kind, argument->second, directories.pathStyle()))
            return std::unexpected(ArgumentFault::NotItsKind);
        if (declared->kind != ArgumentKind::Path)
        {
            argv.push_back(argument->second);
            continue;
        }
        auto resolved = resolvePathArgument(directories, roots, declared->root, argument->second);
        if (!resolved)
            return std::unexpected(ArgumentFault::OutsideItsRoot);
        argv.push_back(std::move(*resolved));
    }
    return argv;
}

} // namespace vtextension
