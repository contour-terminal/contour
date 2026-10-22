// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/Manifest.hpp>

namespace vtextension
{

/// What the user's configuration says about letting extensions run commands.
enum class CommandPermission : uint8_t
{
    Deny,  ///< Extensions may not run commands.
    Allow, ///< Pre-approved: declared commands run without asking.
    Ask,   ///< Declared commands run once the user has approved the extension.
};

/// The user's answer to "may this extension run its declared commands?".
enum class GrantState : uint8_t
{
    Pending, ///< Not answered yet.
    Granted, ///< Approved.
    Denied,  ///< Refused.
};

/// Where answers to the permission question are kept. The host supplies the storage.
class IGrantStore
{
  public:
    IGrantStore() = default;
    IGrantStore(IGrantStore const&) = delete;
    IGrantStore(IGrantStore&&) = delete;
    IGrantStore& operator=(IGrantStore const&) = delete;
    IGrantStore& operator=(IGrantStore&&) = delete;
    virtual ~IGrantStore() = default;

    /// Looks up the answer recorded for an installed extension's manifest.
    /// @param hostId The host-assigned identity of the installed extension.
    /// @param digest The digest of the manifest the answer is for.
    /// @return The recorded answer; `Pending` if there is none for this exact digest.
    [[nodiscard]] virtual GrantState lookup(std::string_view hostId, std::string_view digest) const = 0;

    /// Records the answer for an installed extension's manifest.
    /// @param hostId The host-assigned identity of the installed extension.
    /// @param digest The digest of the manifest the answer is for.
    /// @param state The answer.
    virtual void record(std::string_view hostId, std::string_view digest, GrantState state) = 0;
};

/// Decides whether an extension may run one of its declared commands, and builds its argv.
///
/// The policy never blocks and never asks: an unanswered question is an error the caller reports,
/// and the user is asked elsewhere.
class CommandPolicy
{
  public:
    /// @param manifest The extension's manifest; the policy keeps it.
    /// @param hostId What the host calls this installation: for example its configuration key plus
    ///        the canonical path of the module. Grants are keyed on it and on the manifest's digest.
    ///        It is never the manifest's own `name`, which the guest chooses.
    /// @param store Where grants are looked up; it must outlive the policy.
    /// @param configured What the user's configuration says about running commands.
    CommandPolicy(Manifest manifest, std::string hostId, IGrantStore& store, CommandPermission configured);

    /// Resolves a `run-command` call to the concrete argv to execute.
    ///
    /// The checks run in this order: configured `Deny` is `PermissionDenied`; a name the manifest
    /// does not declare is `CommandNotDeclared`; then the store is asked about this installation's
    /// host id and the manifest digest: `Denied` is `PermissionDenied`, `Granted` is allowed, and `Pending`
    /// is allowed when the configuration says `Allow` and is `PermissionPending` otherwise.
    ///
    /// Each `{placeholder}` element is replaced, whole, by the argument of that name (the first
    /// one, if a name repeats). Arguments no placeholder names are ignored. A call that lacks an
    /// argument for a placeholder does not match the declared command and is `CommandNotDeclared`.
    ///
    /// A value is refused with `ArgumentRejected` if it contains a NUL, or if it starts with `-`
    /// and no literal `--` element precedes its placeholder in the template: such a value could be
    /// taken for an option. After a literal `--`, a leading `-` is data.
    /// @param name The declared command's name.
    /// @param args The call's arguments, by name.
    /// @return The argv, or the reason the command may not run.
    [[nodiscard]] std::expected<std::vector<std::string>, ExtensionError> resolve(
        std::string_view name, std::span<std::pair<std::string, std::string> const> args) const;

  private:
    Manifest _manifest;
    std::string _hostId;
    IGrantStore& _store;
    CommandPermission _configured;
};

} // namespace vtextension
