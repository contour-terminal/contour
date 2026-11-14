// SPDX-License-Identifier: Apache-2.0
#include <vthost/ByteStream.hpp>

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace vthost
{

namespace
{
    /// How much one read may take at most.
    constexpr auto PumpBufferSize = std::size_t { 64 } * 1024;

    /// Whether the daemon has answered the relay's client yet.
    enum class DaemonAnswer : std::uint8_t
    {
        NotYet = 0, ///< Nothing has been read from the daemon.
        Given,      ///< A read of the daemon returned: bytes, its end, or a failure.
    };

    /// What the relay's two directions report to the caller waiting on them.
    struct RelayProgress
    {
        std::mutex mutex;
        std::condition_variable changed;
        /// When the client first spoke or ended its side; the daemon's deadline runs from it.
        std::optional<std::chrono::steady_clock::time_point> owedSince;
        DaemonAnswer answer = DaemonAnswer::NotYet;
        std::optional<PumpEnd> daemonEnd; ///< How the daemon→client direction ended, once it has.

        /// Updates the progress under the lock, and wakes the waiting caller.
        void update(std::invocable<RelayProgress&> auto&& change)
        {
            {
                auto const lock = std::scoped_lock { mutex };
                change(*this);
            }
            changed.notify_all();
        }
    };

    /// A stream that reports the first return of its readSome(), whatever it returned.
    class FirstReadObserver final: public IByteStream
    {
      public:
        FirstReadObserver(IByteStream& inner, std::function<void()> onFirstRead):
            _inner { inner }, _onFirstRead { std::move(onFirstRead) }
        {
        }

        [[nodiscard]] std::expected<std::size_t, std::string> readSome(std::span<std::byte> buffer) override
        {
            auto result = _inner.readSome(buffer);
            if (auto const onFirstRead = std::exchange(_onFirstRead, {}))
                onFirstRead();
            return result;
        }

        [[nodiscard]] std::expected<void, std::string> writeAll(std::span<std::byte const> bytes) override
        {
            return _inner.writeAll(bytes);
        }

        void closeWrite() noexcept override { _inner.closeWrite(); }

      private:
        IByteStream& _inner;
        std::function<void()> _onFirstRead;
    };
} // namespace

PumpEnd pumpBytes(IByteStream& source, IByteStream& sink)
{
    auto buffer = std::vector<std::byte>(PumpBufferSize);
    auto const end = [&]() {
        while (true)
        {
            auto const count = source.readSome(buffer);
            if (!count)
                return PumpEnd::SourceFailed;
            if (*count == 0)
                return PumpEnd::SourceEnded;
            if (!sink.writeAll(std::span { buffer }.first(*count)))
                return PumpEnd::SinkFailed;
        }
    }();
    sink.closeWrite();
    return end;
}

RelayEnd runStdioRelay(std::shared_ptr<IByteStream> client,
                       std::shared_ptr<IByteStream> daemon,
                       RelayConfig config)
{
    auto const progress = std::make_shared<RelayProgress>();

    std::thread { [client, daemon, progress]() {
        auto source = FirstReadObserver { *client, [&progress]() {
                                             progress->update([](RelayProgress& p) {
                                                 if (p.answer == DaemonAnswer::NotYet)
                                                     p.owedSince = std::chrono::steady_clock::now();
                                             });
                                         } };
        (void) pumpBytes(source, *daemon);
    } }.detach();

    // The second thread takes the parameters; the caller keeps a reference of its own to end the client.
    auto const clientSide = client;
    std::thread { [client = std::move(client), daemon = std::move(daemon), progress]() {
        auto source =
            FirstReadObserver { *daemon, [&progress]() {
                                   progress->update([](RelayProgress& p) { p.answer = DaemonAnswer::Given; });
                               } };
        auto const end = pumpBytes(source, *client);
        progress->update([end](RelayProgress& p) { p.daemonEnd = end; });
    } }.detach();

    auto lock = std::unique_lock { progress->mutex };
    while (!progress->daemonEnd)
    {
        if (progress->answer == DaemonAnswer::Given || !progress->owedSince)
        {
            progress->changed.wait(lock);
            continue;
        }
        auto const deadline = *progress->owedSince + config.daemonReplyDeadline;
        if (progress->changed.wait_until(lock, deadline, [&] {
                return progress->daemonEnd || progress->answer == DaemonAnswer::Given;
            }))
            continue;
        lock.unlock();
        // The daemon's read stays parked on its thread, which the process's exit ends; the client learns now.
        clientSide->closeWrite();
        return RelayEnd::DaemonSilent;
    }
    return *progress->daemonEnd == PumpEnd::SourceEnded ? RelayEnd::DaemonEnded : RelayEnd::Failed;
}

} // namespace vthost
