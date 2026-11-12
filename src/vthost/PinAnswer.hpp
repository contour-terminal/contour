// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The hand-over of a user's answer to "trust this daemon?" from the thread that asked them to the coroutine
/// that waits for it. The one shape every IPinPrompt that asks off the loop's thread uses (a dialog on the
/// GUI thread, the terminal on a thread of its own), so that none keeps a copy of it.

#include <core/async/AsyncQueue.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/Task.hpp>

#include <atomic>
#include <memory>
#include <utility>

#include <vthost/KnownDaemons.hpp>

namespace vthost
{

/// One answer, given at most once from any thread, awaited by one coroutine.
class PinAnswer final
{
  public:
    /// Gives the answer, from the asking side: when the last copy of it goes without an answer having been
    /// given -- the question was never asked, or its reply was dropped -- the wait ends in Decline instead of
    /// lasting for ever.
    class Sender final
    {
      public:
        /// @param answer What the answer is delivered to.
        explicit Sender(std::shared_ptr<PinAnswer> answer): _answer { std::move(answer) } {}
        ~Sender() { _answer->abandon(); }

        Sender(Sender const&) = delete;
        Sender& operator=(Sender const&) = delete;
        Sender(Sender&&) = delete;
        Sender& operator=(Sender&&) = delete;

        /// Delivers @p decision, unless the answer is settled already. Any thread.
        void give(PinDecision decision) const { _answer->give(decision); }

      private:
        std::shared_ptr<PinAnswer> _answer;
    };

    /// @param executor Where the waiting coroutine is resumed when it parked outside every executor's task;
    ///        must outlive the answer.
    explicit PinAnswer(core::async::IExecutor& executor):
        _queue { executor, core::async::AsyncQueueOptions {} }
    {
    }

    PinAnswer(PinAnswer const&) = delete;
    PinAnswer& operator=(PinAnswer const&) = delete;
    PinAnswer(PinAnswer&&) = delete;
    PinAnswer& operator=(PinAnswer&&) = delete;

    /// @param answer The answer to send to.
    /// @return A sender for it, to hand to whatever asks the user.
    [[nodiscard]] static std::shared_ptr<Sender> senderOf(std::shared_ptr<PinAnswer> const& answer)
    {
        return std::make_shared<Sender>(answer);
    }

    /// @return What the user decided; Decline when nobody did. A wait that is cancelled unwinds, and leaves
    ///         whatever was asking to finish on its own: its answer goes nowhere.
    [[nodiscard]] core::async::Task<PinDecision> await()
    {
        auto const decided = co_await _queue.pop();
        co_return decided.value_or(PinDecision::Decline);
    }

  private:
    void give(PinDecision decision)
    {
        if (!_settled.exchange(true))
            (void) _queue.push(decision);
    }

    void abandon() noexcept
    {
        if (!_settled.exchange(true))
            _queue.close();
    }

    core::async::AsyncQueue<PinDecision> _queue;
    std::atomic<bool> _settled = false;
};

} // namespace vthost
