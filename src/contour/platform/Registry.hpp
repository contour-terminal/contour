// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Windows registry, behind an interface so that what is decided from it can be tested with any
/// registry content, on any platform. Names and values are UTF-8 here; the Windows implementation
/// converts them to and from the UTF-16 the wide registry API speaks. Qt-free.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contour::platform
{

/// The registry hive a key is under.
enum class RegistryRoot : uint8_t
{
    LocalMachine = 0, ///< `HKEY_LOCAL_MACHINE`: the machine's settings.
    CurrentUser,      ///< `HKEY_CURRENT_USER`: the signed-in user's settings.
};

/// Reading and writing registry values.
class IRegistry
{
  public:
    virtual ~IRegistry() = default;

    /// Reads a string value.
    /// @param root The hive @p key is under.
    /// @param key The key's path below @p root, its parts separated by backslashes.
    /// @param value The value's name.
    /// @return The value's text, or nullopt when the key or value is absent or the value is not a string.
    [[nodiscard]] virtual std::optional<std::string> readString(RegistryRoot root,
                                                                std::string_view key,
                                                                std::string_view value) const = 0;

    /// Reads a DWORD value.
    /// @param root The hive @p key is under.
    /// @param key The key's path below @p root, its parts separated by backslashes.
    /// @param value The value's name.
    /// @return The value, or nullopt when the key or value is absent or the value is not a DWORD.
    [[nodiscard]] virtual std::optional<std::uint32_t> readDword(RegistryRoot root,
                                                                 std::string_view key,
                                                                 std::string_view value) const = 0;

    /// Lists a key's subkeys.
    /// @param root The hive @p key is under.
    /// @param key The key's path below @p root, its parts separated by backslashes.
    /// @return The names of the key's direct subkeys, in the registry's order; empty when the key is absent
    ///         or cannot be read.
    [[nodiscard]] virtual std::vector<std::string> subkeys(RegistryRoot root, std::string_view key) const = 0;

    /// Writes a string value, creating the key when it is absent.
    /// @param root The hive @p key is under.
    /// @param key The key's path below @p root, its parts separated by backslashes.
    /// @param value The value's name.
    /// @param data The text to store.
    /// @return Nothing on success, else why the value could not be written.
    [[nodiscard]] virtual std::expected<void, std::string> writeString(RegistryRoot root,
                                                                       std::string_view key,
                                                                       std::string_view value,
                                                                       std::string_view data) = 0;
};

/// The registry of a platform that has none: every value is absent, and nothing can be written.
class NullRegistry final: public IRegistry
{
  public:
    /// @return Always nullopt.
    [[nodiscard]] std::optional<std::string> readString(RegistryRoot /*root*/,
                                                        std::string_view /*key*/,
                                                        std::string_view /*value*/) const override
    {
        return std::nullopt;
    }

    /// @return Always nullopt.
    [[nodiscard]] std::optional<std::uint32_t> readDword(RegistryRoot /*root*/,
                                                         std::string_view /*key*/,
                                                         std::string_view /*value*/) const override
    {
        return std::nullopt;
    }

    /// @return Always empty.
    [[nodiscard]] std::vector<std::string> subkeys(RegistryRoot /*root*/,
                                                   std::string_view /*key*/) const override
    {
        return {};
    }

    /// @return Always the error `no registry`.
    [[nodiscard]] std::expected<void, std::string> writeString(RegistryRoot /*root*/,
                                                               std::string_view /*key*/,
                                                               std::string_view /*value*/,
                                                               std::string_view /*data*/) override
    {
        return std::unexpected(std::string { "no registry" });
    }
};

/// The platform's registry.
/// @return On Windows, the system registry (`RegGetValueW`, `RegEnumKeyExW`, `RegSetKeyValueW`); elsewhere
/// NullRegistry.
[[nodiscard]] std::unique_ptr<IRegistry> makePlatformRegistry();

} // namespace contour::platform
