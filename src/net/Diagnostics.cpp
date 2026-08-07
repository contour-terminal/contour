// SPDX-License-Identifier: Apache-2.0
#include <net/Diagnostics.hpp>

#include <memory>
#include <mutex>
#include <utility>

namespace net
{

namespace
{
    /// The installed sink, held by shared_ptr so a reporting thread can take its own
    /// reference and keep the target alive for the duration of the call. Reading the
    /// raw std::function while another thread assigned it would be a use-after-free
    /// of the callable's captures; swapping a whole pointer is atomic instead.
    ///
    /// A function-local static in an accessor rather than a namespace-scope object,
    /// so it is initialized on first use and cannot be read before its constructor
    /// runs during static initialization.
    /// @return The sink slot.
    std::shared_ptr<DiagnosticSink const>& sinkSlot() noexcept
    {
        static auto sink = std::shared_ptr<DiagnosticSink const> {};
        return sink;
    }

    /// Guards the slot itself. Held only for the pointer read or write, never across
    /// the sink invocation — a sink that reports while reporting would otherwise
    /// deadlock, and a slow sink would serialize every reporting thread.
    /// @return The slot's mutex.
    std::mutex& sinkMutex() noexcept
    {
        static auto mutex = std::mutex {};
        return mutex;
    }

    /// @return The current sink, or nullptr if none is installed.
    [[nodiscard]] std::shared_ptr<DiagnosticSink const> currentSink() noexcept
    {
        auto const lock = std::lock_guard { sinkMutex() };
        return sinkSlot();
    }
} // namespace

void setDiagnosticSink(DiagnosticSink sink)
{
    auto next = sink ? std::make_shared<DiagnosticSink const>(std::move(sink))
                     : std::shared_ptr<DiagnosticSink const> {};
    auto const lock = std::lock_guard { sinkMutex() };
    sinkSlot() = std::move(next);
}

bool hasDiagnosticSink() noexcept
{
    return currentSink() != nullptr;
}

void reportDiagnostic(std::string_view message)
{
    // Hold a reference for the whole call, so a concurrent setDiagnosticSink
    // replacing the sink cannot destroy the callable under us.
    if (auto const sink = currentSink())
        (*sink)(message);
}

} // namespace net
