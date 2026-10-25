// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/UiStyleProvider.hpp>

#include <memory>

class QQmlContext;

namespace contour::window
{

/// Owns the `chromeStyle` context property and rebuilds it when what it is built from changes.
///
/// A UiStyleProvider's tokens are CONSTANT, so a change of input -- an OS accessibility or appearance
/// change, a reload, a light/dark flip -- is answered by a new provider rather than by notifying every
/// property of the old one. The old one is deleteLater()ed rather than deleted: the change usually
/// arrives from a signal the old provider (or a binding reading it) is still on the stack for.
class ChromeStyleHost
{
  public:
    /// Builds the provider and publishes it as @p context's `chromeStyle`.
    /// @param context Where to publish; borrowed, and only touched from the constructor and update().
    /// @param inputs  What the first provider is built from.
    ChromeStyleHost(QQmlContext& context, ChromeInputs inputs);

    ChromeStyleHost(ChromeStyleHost const&) = delete;
    ChromeStyleHost& operator=(ChromeStyleHost const&) = delete;
    ChromeStyleHost(ChromeStyleHost&&) = delete;
    ChromeStyleHost& operator=(ChromeStyleHost&&) = delete;
    ~ChromeStyleHost();

    /// Rebuilds and republishes the provider from @p inputs; does nothing when they equal the current
    /// ones. The previous provider is deleted on the next DeferredDelete pass.
    /// @param inputs What the provider should now be built from.
    void update(ChromeInputs inputs);

    /// @return The provider currently published.
    [[nodiscard]] UiStyleProvider& provider() noexcept { return *_provider; }

    /// @return What the current provider was built from.
    [[nodiscard]] ChromeInputs const& inputs() const noexcept { return _provider->inputs(); }

  private:
    QQmlContext& _context;
    std::unique_ptr<UiStyleProvider> _provider;
};

} // namespace contour::window
