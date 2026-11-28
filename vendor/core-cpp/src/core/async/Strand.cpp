// SPDX-License-Identifier: Apache-2.0

#include <core/async/Strand.hpp>

#include <coroutine>
#include <utility>

namespace core::async::detail
{

namespace
{
    /// Holds at most one dead pump frame, and frees it when its thread exits.
    class DeadPumpReaper final
    {
      public:
        DeadPumpReaper() noexcept = default;
        DeadPumpReaper(DeadPumpReaper const&) = delete;
        DeadPumpReaper(DeadPumpReaper&&) = delete;
        DeadPumpReaper& operator=(DeadPumpReaper const&) = delete;
        DeadPumpReaper& operator=(DeadPumpReaper&&) = delete;

        ~DeadPumpReaper()
        {
            if (_dead)
                _dead.destroy();
        }

        /// Frees the pump buried before, and keeps @p dead in its place.
        void bury(std::coroutine_handle<> dead) noexcept
        {
            if (auto const previous = std::exchange(_dead, dead))
                previous.destroy();
        }

      private:
        std::coroutine_handle<> _dead;
    };
} // namespace

void buryDeadPump(std::coroutine_handle<> dead) noexcept
{
    thread_local DeadPumpReaper reaper;
    reaper.bury(dead);
}

FreeingAbandoned::Freeing& FreeingAbandoned::slot() noexcept
{
    constinit thread_local Freeing freeing {};
    return freeing;
}

} // namespace core::async::detail
