// SPDX-License-Identifier: Apache-2.0
#include <vthost/ByteStream.hpp>

#include <thread>
#include <utility>
#include <vector>

namespace vthost
{

namespace
{
    /// How much one read may take at most.
    constexpr auto PumpBufferSize = std::size_t { 64 } * 1024;
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

RelayEnd runStdioRelay(std::shared_ptr<IByteStream> client, std::shared_ptr<IByteStream> daemon)
{
    // The thread takes the parameters; this direction keeps references of its own.
    auto const clientSide = client;
    auto const daemonSide = daemon;
    std::thread { [client = std::move(client), daemon = std::move(daemon)]() {
        (void) pumpBytes(*client, *daemon);
    } }.detach();
    return pumpBytes(*daemonSide, *clientSide) == PumpEnd::SourceEnded ? RelayEnd::DaemonEnded
                                                                       : RelayEnd::Failed;
}

} // namespace vthost
