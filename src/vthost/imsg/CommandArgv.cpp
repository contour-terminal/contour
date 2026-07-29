// SPDX-License-Identifier: Apache-2.0
#include <vthost/imsg/CommandArgv.h>

#include <cstring>
#include <ranges>

namespace vthost::imsg
{

std::vector<std::byte> packArgv(std::span<std::string const> arguments)
{
    auto const argc = static_cast<int>(arguments.size());
    auto out = std::vector<std::byte>(sizeof(int));
    std::memcpy(out.data(), &argc, sizeof(int));
    for (auto const& argument: arguments)
    {
        auto const* begin = reinterpret_cast<std::byte const*>(argument.data());
        out.insert(out.end(), begin, begin + argument.size());
        out.push_back(std::byte { 0 });
    }
    return out;
}

std::expected<std::vector<std::string>, ImsgError> unpackArgv(std::span<std::byte const> payload)
{
    if (payload.size() < sizeof(int))
        return std::unexpected(ImsgError::BadArgv);
    auto argc = 0;
    std::memcpy(&argc, payload.data(), sizeof(int));
    if (argc < 0 || argc > MaxArgc)
        return std::unexpected(ImsgError::BadArgv);

    auto arguments = std::vector<std::string> {};
    arguments.reserve(static_cast<std::size_t>(argc));
    auto rest = payload.subspan(sizeof(int));
    for ([[maybe_unused]] auto const _: std::views::iota(0, argc))
    {
        auto terminator = std::size_t { 0 };
        while (terminator < rest.size() && rest[terminator] != std::byte { 0 })
            ++terminator;
        if (terminator == rest.size())
            return std::unexpected(ImsgError::BadArgv); // string without its NUL
        arguments.emplace_back(reinterpret_cast<char const*>(rest.data()), terminator);
        rest = rest.subspan(terminator + 1);
    }
    return arguments;
}

} // namespace vthost::imsg
