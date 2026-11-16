// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace vtextension
{

/// Where a daemon-world extension's summary (`summary-set`) is shown: the status line's segment, `contour
/// extension status`. The text it receives is bounded and neutralized already.
class IExtensionPresence
{
  public:
    IExtensionPresence() = default;
    IExtensionPresence(IExtensionPresence const&) = delete;
    IExtensionPresence(IExtensionPresence&&) = delete;
    IExtensionPresence& operator=(IExtensionPresence const&) = delete;
    IExtensionPresence& operator=(IExtensionPresence&&) = delete;
    virtual ~IExtensionPresence() = default;

    /// Replaces what an extension shows of itself. Called from any thread; must be cheap and must not throw.
    /// @param extension What the configuration calls the extension.
    /// @param shortText The one-line summary.
    /// @param detail The longer text; its lines are separated by `\n`.
    virtual void publishSummary(std::string_view extension,
                                std::string_view shortText,
                                std::string_view detail) = 0;
};

} // namespace vtextension
