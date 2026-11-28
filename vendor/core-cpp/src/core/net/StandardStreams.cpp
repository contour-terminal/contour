// SPDX-License-Identifier: Apache-2.0

/// @file
/// `openStandardStreams`: one input stream that waits on the loop where its handle allows and reads
/// on the blocking executor where it does not, and an output stream that always writes there.

#include <core/net/StandardStreams.hpp>

#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/Offload.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/detail/OperationClaim.hpp>
#include <core/net/detail/ParkedCall.hpp>
#include <core/net/detail/StandardStreamPrimitives.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace core::net
{

namespace
{
    using ReadAnswer = std::expected<StreamRead, NetError>;

    /// How a worker read ended.
    enum class WorkerEnding : std::uint8_t
    {
        Answered,    ///< It read, reached the end, failed, or was dropped unrun: @c read says which.
        Interrupted, ///< A stop reached it queued, or cancelled it (Windows): it read nothing, and the next
                     ///< read reads afresh.
    };

    /// What a worker does when its turn on the executor comes.
    enum class WorkerEntry : std::uint8_t
    {
        Read,        ///< Read the handle.
        Interrupted, ///< A stop came while it was queued: answer without reading.
    };

    /// What one worker read hands back.
    struct WorkerAnswer
    {
        ReadAnswer read;
        WorkerEnding ending = WorkerEnding::Answered;
    };

    /// One worker read's rendezvous with the flow that waits for it (`.agent/rules/async-and-net.md`,
    /// Ruling R13): a stop ends the wait, not the read.
    using ReadCall = detail::ParkedCall<WorkerAnswer>;

    /// @return The stream's own refusal of a call its state does not allow: never the OS's.
    [[nodiscard]] NetError refused(std::string what)
    {
        return makeNetError(NetErrorCode::BadHandle, 0, std::move(what));
    }

    /// @return A task that answers @p error without touching any handle.
    template <typename T>
    async::Task<std::expected<T, NetError>> refuse(NetError error)
    {
        co_return std::unexpected(std::move(error));
    }

    /// @return The first @p count bytes of @p buffer as a read: @c InputEnd::Reached when none.
    [[nodiscard]] StreamRead readOf(std::vector<std::byte> buffer, std::size_t count)
    {
        buffer.resize(count);
        auto const end = count == 0 ? InputEnd::Reached : InputEnd::More;
        return StreamRead { .bytes = std::move(buffer), .end = end };
    }

    /// What a read that finds a worker read outstanding does next.
    enum class NextStep : std::uint8_t
    {
        Fresh, ///< The worker is done and left nothing to deliver: start another.
        Late,  ///< The worker is done and its answer was kept: deliver that.
        Wait,  ///< The worker is still reading: wait for its answer.
    };

    /// One worker read, shared by the stream, the worker running it and a stop that may interrupt it.
    ///
    /// **Exactly one worker read is outstanding per stream.** A stopped flow leaves the read running,
    /// and the stream's next read waits for that same worker rather than starting a second beside it:
    /// two blocked reads of one handle would race for its bytes. The worker answers whichever call is
    /// current when it finishes, so the next read swaps in a fresh call (the stopped one's is
    /// cancelled for good) and waits on that; one mutex orders the swap against the worker's answer.
    class BlockingRead
    {
      public:
        /// @param loop Where an answer's waiter is handed back.
        explicit BlockingRead(EventLoop& loop): _loop { &loop }, _call { std::make_shared<ReadCall>(&loop) }
        {
        }

        /// @return The call the first waiter waits on.
        [[nodiscard]] std::shared_ptr<ReadCall> firstCall() const noexcept { return _call; }

        /// The worker, just before its read: records its thread for @c interrupt, unless a stop
        /// already came while it was queued -- then it must not read at all, or it would block a
        /// thread (and on POSIX take the input) for a reader that is gone.
        /// @return Whether to read.
        [[nodiscard]] WorkerEntry enter() noexcept
        {
            auto const guard = std::scoped_lock { _mutex };
            if (_stop == StopRequest::Pending)
                return WorkerEntry::Interrupted;
            _reader.emplace();
            return WorkerEntry::Read;
        }

        /// The worker, after its read: forgets its thread and answers the current call. At most once.
        /// @param answer What the read came to.
        void finish(WorkerAnswer answer) noexcept
        {
            auto const guard = std::scoped_lock { _mutex };
            _reader.reset();
            if (_isFinished == Finished::Yes)
                return;
            _isFinished = Finished::Yes;
            _call->settle(std::move(answer));
        }

        /// The stop callback: cancels the worker's read where the OS allows, or, before the worker
        /// has started it, remembers the stop for @c enter. Any thread.
        void interrupt() noexcept
        {
            auto const guard = std::scoped_lock { _mutex };
            if (_reader.has_value())
                _reader->interrupt();
            else if (_isFinished == Finished::No)
                _stop = StopRequest::Pending;
        }

        /// What a read after a stop does with this worker read.
        struct Next
        {
            NextStep step = NextStep::Fresh;
            std::optional<WorkerAnswer> late {}; ///< For @c NextStep::Late.
            std::shared_ptr<ReadCall> call {};   ///< For @c NextStep::Wait.
        };

        /// Called on the loop's thread by a read that finds this worker read still the stream's.
        /// @return What to do next; a fresh call to wait on where the worker is still reading.
        [[nodiscard]] Next resume()
        {
            auto const guard = std::scoped_lock { _mutex };
            if (_isFinished == Finished::Yes)
            {
                auto late = _call->takeLateAnswer();
                if (!late.has_value() || late->ending == WorkerEnding::Interrupted)
                    return Next { .step = NextStep::Fresh };
                return Next { .step = NextStep::Late, .late = std::move(late) };
            }
            // A new reader wants this read's bytes: the stop it was going to honour was the last one's.
            _stop = StopRequest::None;
            _call = std::make_shared<ReadCall>(_loop);
            return Next { .step = NextStep::Wait, .call = _call };
        }

      private:
        /// Whether a stop reached the read before its worker started it.
        enum class StopRequest : std::uint8_t
        {
            None,
            Pending,
        };

        /// Whether the worker has answered.
        enum class Finished : std::uint8_t
        {
            No,
            Yes,
        };

        std::mutex _mutex;
        EventLoop* _loop;
        std::shared_ptr<ReadCall> _call;
        std::optional<detail::ReadingThread> _reader;
        Finished _isFinished = Finished::No;
        StopRequest _stop = StopRequest::None;
    };

    /// A worker read's hold on its @c BlockingRead, owned by the detached flow that runs it.
    ///
    /// **Its destructor is the guard that nobody is stranded**: a worker that never answers -- its
    /// executor freed it unrun -- answers @c NetErrorCode::Cancelled here.
    class ReadLease
    {
      public:
        explicit ReadLease(std::shared_ptr<BlockingRead> job) noexcept: _job { std::move(job) } {}
        ReadLease(ReadLease&&) noexcept = default;
        ReadLease(ReadLease const&) = delete;
        ReadLease& operator=(ReadLease const&) = delete;
        ReadLease& operator=(ReadLease&&) = delete;
        ~ReadLease() { fail(NetErrorCode::Cancelled); }

        /// @return The read this lease holds.
        [[nodiscard]] BlockingRead& job() const noexcept { return *_job; }

        /// Answers the read, once.
        void finish(WorkerAnswer answer) noexcept
        {
            if (auto const job = std::exchange(_job, nullptr))
                job->finish(std::move(answer));
        }

        /// Answers a failure that allocates nothing, for the paths that cannot take a throw.
        void fail(NetErrorCode code) noexcept
        {
            if (_job)
                finish(WorkerAnswer { .read = std::unexpected(makeNetError(code)) });
        }

      private:
        std::shared_ptr<BlockingRead> _job;
    };

    /// Runs one blocking read of @p handle on @p blocking and answers its call.
    ///
    /// Detached, because nobody may own it: the waiting flow may be stopped and gone, and the stream
    /// with it, long before the read returns. It holds everything it touches by value or by share.
    /// @param blocking Where the read runs.
    /// @param handle What it reads.
    /// @param maxBytes The most it reads.
    /// @param lease The read it answers.
    async::DetachedTask runBlockingRead(async::IExecutor* blocking,
                                        platform::NativeHandle handle,
                                        std::size_t maxBytes,
                                        ReadLease lease)
    {
        try
        {
            co_await async::ResumeOn { *blocking };
            auto buffer = std::vector<std::byte>(maxBytes);
            if (lease.job().enter() == WorkerEntry::Interrupted)
            {
                lease.finish(WorkerAnswer { .read = StreamRead {}, .ending = WorkerEnding::Interrupted });
                co_return;
            }
            auto got = detail::readStream(handle, buffer, detail::ReadWait::Block);
            if (got.has_value())
                lease.finish(WorkerAnswer { .read = readOf(std::move(buffer), *got) });
            else if (got.error().code == NetErrorCode::Cancelled)
                lease.finish(WorkerAnswer { .read = std::unexpected(std::move(got.error())),
                                            .ending = WorkerEnding::Interrupted });
            else
                lease.finish(WorkerAnswer { .read = std::unexpected(std::move(got.error())) });
        }
        catch (...)
        {
            lease.fail(NetErrorCode::SystemError);
        }
    }

    /// The stop callback of a flow waiting for a worker read.
    struct Interrupt
    {
        BlockingRead* job = nullptr;

        void operator()() const noexcept { job->interrupt(); }
    };

    /// An input stream's state, shared with the reads it hands out: a read may outlive its stream.
    /// Touched on the loop's thread only, but for @c BlockingRead, which guards itself.
    struct InputState
    {
        EventLoop* loop;
        async::IExecutor* blocking;
        platform::NativeHandle handle;
        std::size_t maxReadChunk;
        StreamKind kind;                   ///< Turns Blocking for good where the backend refuses it.
        bool isReading = false;            ///< A read is in flight (@c detail::OperationClaim).
        std::shared_ptr<BlockingRead> job; ///< The worker read still outstanding, if any.
        std::vector<std::byte> carry;      ///< A worker's bytes beyond what their read asked for.
    };

    /// @return @p read cut to @p maxBytes, the rest kept in @p state's carry for the next read.
    [[nodiscard]] StreamRead keepBeyond(InputState& state, StreamRead read, std::size_t maxBytes)
    {
        if (read.bytes.size() > maxBytes)
        {
            auto const cut = read.bytes.begin() + static_cast<std::ptrdiff_t>(maxBytes);
            state.carry.assign(cut, read.bytes.end());
            read.bytes.erase(cut, read.bytes.end());
        }
        return read;
    }

    /// @return Up to @p maxBytes of @p state's carry, taken out of it.
    [[nodiscard]] StreamRead takeCarry(InputState& state, std::size_t maxBytes)
    {
        auto read = StreamRead { .bytes = std::exchange(state.carry, {}) };
        return keepBeyond(state, std::move(read), maxBytes);
    }

    /// Reads through the blocking executor, joining the worker read a stopped flow left running.
    async::Task<ReadAnswer> readBlocking(std::shared_ptr<InputState> state, std::size_t maxBytes)
    {
        auto const token = co_await async::thisCoroStopToken();
        while (true)
        {
            if (token.stop_requested())
                throw async::OperationCancelled {};

            auto call = std::shared_ptr<ReadCall> {};
            if (state->job)
            {
                auto next = state->job->resume();
                if (next.step == NextStep::Late)
                {
                    state->job.reset();
                    if (!next.late->read.has_value())
                        co_return std::move(next.late->read);
                    co_return keepBeyond(*state, std::move(*next.late->read), maxBytes);
                }
                if (next.step == NextStep::Wait)
                    call = std::move(next.call);
                else
                    state->job.reset();
            }
            if (!state->job)
            {
                state->job = std::make_shared<BlockingRead>(*state->loop);
                call = state->job->firstCall();
                runBlockingRead(state->blocking, state->handle, maxBytes, ReadLease { state->job });
            }

            auto const job = state->job;
            [[maybe_unused]] auto const interrupt =
                async::StopCallback<Interrupt> { token, Interrupt { .job = job.get() } };
            auto answer = co_await ReadCall::wait(std::move(call));
            state->job.reset();
            if (answer.ending == WorkerEnding::Interrupted)
                continue;
            if (!answer.read.has_value())
                co_return std::move(answer.read);
            co_return keepBeyond(*state, std::move(*answer.read), maxBytes);
        }
    }

    /// Reads @p state's handle: from its carry, through the loop, or through the blocking executor.
    async::Task<ReadAnswer> readInput(std::shared_ptr<InputState> state, std::size_t maxBytes)
    {
        if (!state->carry.empty())
            co_return takeCarry(*state, maxBytes);

        if (state->kind == StreamKind::Pollable)
        {
            auto refusedByBackend = false;
            while (!refusedByBackend)
            {
                try
                {
                    co_await state->loop->waitReadable(state->handle);
                }
                catch (FdRegistrationFailed const&)
                {
                    refusedByBackend = true;
                    continue;
                }
                // Confirmed, because the descriptor may be blocking and the loop's thread must not
                // block: a readiness can be spurious, or another reader's.
                if (detail::readReadinessNow(state->handle) == detail::ReadReadiness::NotReady)
                    continue;
                auto buffer = std::vector<std::byte>(maxBytes);
                auto got = detail::readStream(state->handle, buffer, detail::ReadWait::NoWait);
                if (got.has_value())
                    co_return readOf(std::move(buffer), *got);
                if (got.error().code != NetErrorCode::WouldBlock)
                    co_return std::unexpected(std::move(got.error()));
            }
            // macOS's kqueue refuses some terminals: the executor reads them from now on.
            state->kind = StreamKind::Blocking;
        }
        co_return co_await readBlocking(state, maxBytes);
    }

    /// @return A read of nothing, answered at once.
    async::Task<ReadAnswer> readNothing()
    {
        co_return StreamRead {};
    }

    /// The one input stream: Pollable or Blocking by its handle, one read in flight.
    class StandardInputStream final: public IInputStream
    {
      public:
        explicit StandardInputStream(std::shared_ptr<InputState> state) noexcept: _state { std::move(state) }
        {
        }

        [[nodiscard]] async::Task<ReadAnswer> read(std::size_t maxBytes) override
        {
            if (_state->isReading)
                return refuse<StreamRead>(refused("read while another read is in flight"));
            if (maxBytes == 0)
                return readNothing();
            return detail::holding(detail::OperationClaim<InputState> { _state, &InputState::isReading },
                                   readInput(_state, std::min(maxBytes, _state->maxReadChunk)));
        }

      private:
        std::shared_ptr<InputState> _state;
    };

    /// An output stream's record of its one write in flight.
    struct OutputState
    {
        bool isWriting = false; ///< A write is in flight (@c detail::OperationClaim).
    };

    /// Writes @p bytes to @p handle on @p blocking, coming back to @p loop.
    async::Task<std::expected<std::size_t, NetError>> writeOffloaded(EventLoop* loop,
                                                                     async::IExecutor* blocking,
                                                                     platform::NativeHandle handle,
                                                                     std::vector<std::byte> bytes)
    {
        co_return co_await async::offload(
            loop, blocking, [handle, &bytes] { return detail::writeStream(handle, bytes); });
    }

    /// The one output stream: every write on the blocking executor, one in flight.
    class OffloadedOutputStream final: public IOutputStream
    {
      public:
        OffloadedOutputStream(EventLoop& loop, async::IExecutor& blocking, platform::NativeHandle handle):
            _loop { &loop },
            _blocking { &blocking },
            _handle { handle },
            _state { std::make_shared<OutputState>() }
        {
        }

        [[nodiscard]] async::Task<std::expected<std::size_t, NetError>> write(
            std::vector<std::byte> bytes) override
        {
            if (_state->isWriting)
                return refuse<std::size_t>(refused("write while another write is in flight"));
            return detail::holding(detail::OperationClaim<OutputState> { _state, &OutputState::isWriting },
                                   writeOffloaded(_loop, _blocking, _handle, std::move(bytes)));
        }

      private:
        EventLoop* _loop;
        async::IExecutor* _blocking;
        platform::NativeHandle _handle;
        std::shared_ptr<OutputState> _state;
    };

} // namespace

StandardStreamHandles processStandardHandles() noexcept
{
    return StandardStreamHandles {
        .input = platform::standardInput(),
        .output = platform::standardOutput(),
        .error = platform::standardError(),
    };
}

StandardStreams openStandardStreams(EventLoop& loop,
                                    async::IExecutor& blocking,
                                    StandardStreamHandles handles,
                                    std::size_t maxReadChunk)
{
    auto input = std::make_shared<InputState>(InputState {
        .loop = &loop,
        .blocking = &blocking,
        .handle = handles.input,
        .maxReadChunk = std::max<std::size_t>(maxReadChunk, 1),
        .kind = classifyStreamHandle(handles.input),
        .job = nullptr,
        .carry = {},
    });
    return StandardStreams {
        .input = std::make_unique<StandardInputStream>(std::move(input)),
        .output = std::make_unique<OffloadedOutputStream>(loop, blocking, handles.output),
        .error = std::make_unique<OffloadedOutputStream>(loop, blocking, handles.error),
    };
}

} // namespace core::net
