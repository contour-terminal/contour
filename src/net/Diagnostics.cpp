// SPDX-License-Identifier: Apache-2.0
#include <net/Diagnostics.hpp>

#include <utility>

namespace net
{

namespace
{
    /// The installed sink. A function-local static in an accessor rather than a
    /// namespace-scope object, so it is initialized on first use and cannot be read
    /// before its constructor runs during static initialization.
    /// @return The sink slot.
    DiagnosticSink& sinkSlot() noexcept
    {
        static auto sink = DiagnosticSink {};
        return sink;
    }
} // namespace

void setDiagnosticSink(DiagnosticSink sink)
{
    sinkSlot() = std::move(sink);
}

bool hasDiagnosticSink() noexcept
{
    return static_cast<bool>(sinkSlot());
}

void reportDiagnostic(std::string_view message)
{
    if (auto const& sink = sinkSlot())
        sink(message);
}

} // namespace net
