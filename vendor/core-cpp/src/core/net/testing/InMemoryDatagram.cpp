// SPDX-License-Identifier: Apache-2.0
#include <core/net/testing/InMemoryDatagram.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace core::net::testing
{

namespace
{
    /// Whether an inbox is one a socket bound to @p endpoint holds.
    ///
    /// One predicate rather than the same comparison written at both call sites, which want
    /// different halves of the same answer: @c dropNext starves every match and @c deliver hands a
    /// unicast to the first.
    /// @param endpoint The address being matched; it must outlive the predicate.
    /// @return The predicate.
    [[nodiscard]] auto heldAt(DatagramAddress const& endpoint)
    {
        return [&endpoint](auto const& inbox) {
            return inbox.endpoint == endpoint;
        };
    }
} // namespace

/// One endpoint on a @c DatagramBus.
class InMemoryDatagramSocket final: public IDatagramSocket
{
  public:
    /// Attaches to @p bus at @p endpoint.
    /// @param bus The segment.
    /// @param endpoint This socket's address; other sockets may hold it too.
    InMemoryDatagramSocket(DatagramBus& bus, DatagramAddress endpoint):
        _bus { bus },
        _endpoint { std::move(endpoint) },
        // Held rather than looked up per call. An address no longer names one inbox, so there is
        // nothing to look one up BY — and the reference is sound for as long as this socket lives,
        // because `std::map` keeps its values put and only this socket's destructor erases its
        // entry.
        _attachment { _bus.attach(_endpoint) }
    {
    }

    ~InMemoryDatagramSocket() override { _bus.detach(_attachment.id); }

    InMemoryDatagramSocket(InMemoryDatagramSocket const&) = delete;
    InMemoryDatagramSocket(InMemoryDatagramSocket&&) = delete;
    InMemoryDatagramSocket& operator=(InMemoryDatagramSocket const&) = delete;
    InMemoryDatagramSocket& operator=(InMemoryDatagramSocket&&) = delete;

    std::expected<void, NetError> send(std::span<std::byte const> payload, DatagramAddress const& to) override
    {
        _bus.deliver(payload, to, _endpoint);
        return {};
    }

    std::expected<ReceivedDatagram, DatagramWait> receive(std::chrono::milliseconds timeout) override
    {
        auto lock = std::unique_lock { _bus._mutex };

        auto& inbox = _attachment.inbox;
        auto const ready = [&inbox] {
            return inbox.closed || !inbox.queue.empty();
        };

        if (!_bus._arrived.wait_for(lock, timeout, ready))
            return std::unexpected(DatagramWait::TimedOut);

        // Closed wins over a queued datagram: a socket told to stop should stop, and draining first
        // would make shutdown depend on how much happened to be in flight.
        if (inbox.closed)
            return std::unexpected(DatagramWait::Closed);

        auto datagram = std::move(inbox.queue.front());
        inbox.queue.pop_front();
        return datagram;
    }

    void close() noexcept override
    {
        {
            auto const lock = std::scoped_lock { _bus._mutex };
            _attachment.inbox.closed = true;
        }
        _bus._arrived.notify_all();
    }

    [[nodiscard]] DatagramAddress boundAddress() const override { return _endpoint; }

  private:
    DatagramBus& _bus;

    /// This socket's own copy of what its inbox records, deliberately.
    ///
    /// Reading it back out of the bus would be reading bus state without the bus lock — sound,
    /// since nothing ever writes it after @c attach, but sound for a reason a reader has to
    /// reconstruct. An address is three words.
    DatagramAddress _endpoint;

    DatagramBus::Attachment _attachment;
};

DatagramAddress const& DatagramBus::broadcastAddress()
{
    static auto const address = DatagramAddress { .host = "broadcast", .port = 0 };
    return address;
}

DatagramAddress DatagramBus::broadcastAddressOn(std::uint16_t port)
{
    return DatagramAddress { .host = broadcastAddress().host, .port = port };
}

std::unique_ptr<IDatagramSocket> DatagramBus::open(DatagramAddress endpoint)
{
    return std::make_unique<InMemoryDatagramSocket>(*this, std::move(endpoint));
}

DatagramBus::Attachment DatagramBus::attach(DatagramAddress endpoint)
{
    auto const lock = std::scoped_lock { _mutex };
    auto const id = _nextId++;
    return Attachment { .id = id,
                        .inbox =
                            _inboxes.emplace(id, Inbox { .endpoint = std::move(endpoint) }).first->second };
}

void DatagramBus::detach(std::size_t id)
{
    auto const lock = std::scoped_lock { _mutex };
    _inboxes.erase(id);
}

std::size_t DatagramBus::dropNext(DatagramAddress const& endpoint, std::size_t count)
{
    auto const lock = std::scoped_lock { _mutex };
    std::size_t starved = 0;
    for (auto& inbox: _inboxes | std::views::values | std::views::filter(heldAt(endpoint)))
    {
        inbox.dropsRemaining += count;
        ++starved;
    }
    return starved;
}

std::size_t DatagramBus::sendCount() const
{
    auto const lock = std::scoped_lock { _mutex };
    return _sendCount;
}

void DatagramBus::deliver(std::span<std::byte const> payload,
                          DatagramAddress const& to,
                          DatagramAddress const& from)
{
    {
        auto const lock = std::scoped_lock { _mutex };
        ++_sendCount;

        // A broadcast reaches every inbox INCLUDING the sender's, which is what a real one does and
        // is exactly the case a peer directory has to ignore: a node that recorded its own beacon
        // would believe it had a peer and propose a membership change to admit itself.
        auto const deliverTo = [&](Inbox& inbox) {
            if (inbox.dropsRemaining != 0)
            {
                --inbox.dropsRemaining;
                return;
            }
            inbox.queue.push_back(
                ReceivedDatagram { .payload = { payload.begin(), payload.end() }, .from = from });
        };

        if (to.host == broadcastAddress().host)
        {
            // Port zero is every inbox; any other port is the sockets bound to it, which is what a
            // broadcast to `255.255.255.255:P` reaches.
            auto const hears = [&to](Inbox const& inbox) {
                return to.port == 0 || inbox.endpoint.port == to.port;
            };
            for (auto& inbox: _inboxes | std::views::values | std::views::filter(hears))
                deliverTo(inbox);
        }
        else
        {
            // Exactly one, and the first to have attached — see this function's declaration for why
            // one, and why that one.
            //
            // A datagram to an endpoint nobody holds is silently discarded, which is what UDP does.
            // Reporting it would give the layer above a delivery signal the real network cannot
            // provide.
            auto held = _inboxes | std::views::values;
            if (auto const found = std::ranges::find_if(held, heldAt(to)); found != std::ranges::end(held))
                deliverTo(*found);
        }
    }
    _arrived.notify_all();
}

} // namespace core::net::testing
