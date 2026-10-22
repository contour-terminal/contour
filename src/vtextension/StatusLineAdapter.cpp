// SPDX-License-Identifier: Apache-2.0
#include <vtextension/StatusLineAdapter.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <utility>
#include <variant>

#include <vtextension/Utf8.hpp>

namespace vtextension
{

namespace
{
    /// One value of an enumeration of the terminal's, and the world's value it stands for.
    template <typename From, typename To>
    struct Mapping
    {
        From from;
        To to;
    };

    /// Every terminal event kind, and the world's. Bit for bit: the terminal's set converts by value.
    constexpr auto KindMappings = std::array {
        Mapping { vtbackend::StatusLineEventKind::Title, EventKind::Title },
        Mapping { vtbackend::StatusLineEventKind::Cwd, EventKind::Cwd },
        Mapping { vtbackend::StatusLineEventKind::ForegroundProcess, EventKind::ForegroundProcess },
        Mapping { vtbackend::StatusLineEventKind::InputMode, EventKind::InputMode },
        Mapping { vtbackend::StatusLineEventKind::Tabs, EventKind::Tabs },
        Mapping { vtbackend::StatusLineEventKind::UserVar, EventKind::UserVar },
        Mapping { vtbackend::StatusLineEventKind::Progress, EventKind::Progress },
        Mapping { vtbackend::StatusLineEventKind::Focus, EventKind::Focus },
        Mapping { vtbackend::StatusLineEventKind::Font, EventKind::Font },
    };

    static_assert(std::ranges::all_of(KindMappings,
                                      [](auto mapping) {
                                          return std::to_underlying(mapping.from)
                                                 == std::to_underlying(mapping.to);
                                      }),
                  "a terminal event kind is its world kind's bit");

    /// @return The bits of every world kind KindMappings names.
    [[nodiscard]] consteval uint16_t mappedKindBits()
    {
        auto bits = uint16_t { 0 };
        for (auto const mapping: KindMappings)
            bits = static_cast<uint16_t>(bits | std::to_underlying(mapping.to));
        return bits;
    }

    static_assert(mappedKindBits() == SubscribableKinds.value(),
                  "every subscribable kind is a terminal event kind");

    constexpr auto InputModeMappings = std::array {
        Mapping { vtbackend::ViMode::Insert, InputMode::Insert },
        Mapping { vtbackend::ViMode::Normal, InputMode::Normal },
        Mapping { vtbackend::ViMode::Visual, InputMode::Visual },
        Mapping { vtbackend::ViMode::VisualLine, InputMode::VisualLine },
        Mapping { vtbackend::ViMode::VisualBlock, InputMode::VisualBlock },
        Mapping { vtbackend::ViMode::Hint, InputMode::Hint },
    };

    /// OSC 9;4's paused state is the one the world calls a warning: both are "needs attention".
    constexpr auto ProgressStateMappings = std::array {
        Mapping { vtbackend::ProgressState::Inactive, ProgressState::Hidden },
        Mapping { vtbackend::ProgressState::Normal, ProgressState::Normal },
        Mapping { vtbackend::ProgressState::Error, ProgressState::Error },
        Mapping { vtbackend::ProgressState::Indeterminate, ProgressState::Indeterminate },
        Mapping { vtbackend::ProgressState::Paused, ProgressState::Warning },
    };

    constexpr auto TabActivityMappings = std::array {
        Mapping { vtbackend::StatusLineTabActivity::Inactive, TabActivity::Inactive },
        Mapping { vtbackend::StatusLineTabActivity::Active, TabActivity::Active },
    };

    constexpr auto FocusStateMappings = std::array {
        Mapping { vtbackend::StatusLineFocus::Unfocused, FocusState::Unfocused },
        Mapping { vtbackend::StatusLineFocus::Focused, FocusState::Focused },
    };

    /// @return What @p from maps to in @p mappings, or @p fallback where it has no row.
    template <typename From, typename To, std::size_t N>
    [[nodiscard]] constexpr To mapped(std::array<Mapping<From, To>, N> const& mappings,
                                      From from,
                                      To fallback)
    {
        auto const row = std::ranges::find(mappings, from, &Mapping<From, To>::from);
        return row != mappings.end() ? row->to : fallback;
    }

    template <typename... F>
    struct Overloaded: F...
    {
        using F::operator()...;
    };
} // namespace

Event toEvent(vtbackend::StatusLineEvent event)
{
    namespace from = vtbackend::StatusLineEvents;
    return std::visit(
        Overloaded {
            [](from::TitleChanged e) -> Event {
                return TitleChanged { .title = toValidUtf8(std::move(e.title)) };
            },
            [](from::CwdChanged e) -> Event { return CwdChanged { .cwd = toValidUtf8(std::move(e.cwd)) }; },
            [](from::ForegroundProcessChanged e) -> Event {
                return ForegroundProcessChanged { .name = toValidUtf8(std::move(e.name)) };
            },
            [](from::InputModeChanged e) -> Event {
                return InputModeChanged { .mode = mapped(InputModeMappings, e.mode, InputMode::Insert) };
            },
            [](from::TabsChanged e) -> Event {
                auto tabs = TabsChanged {};
                tabs.tabs.reserve(e.tabs.size());
                for (auto& tab: e.tabs)
                    tabs.tabs.push_back(
                        Tab { .title = toValidUtf8(std::move(tab.title)),
                              .activity = mapped(TabActivityMappings, tab.activity, TabActivity::Inactive) });
                return tabs;
            },
            [](from::UserVarChanged e) -> Event {
                return UserVarChanged { .name = toValidUtf8(std::move(e.name)),
                                        .value = toValidUtf8(std::move(e.value)) };
            },
            [](from::ProgressChanged e) -> Event {
                return ProgressChanged { .progress = Progress { .state = mapped(ProgressStateMappings,
                                                                                e.progress.state,
                                                                                ProgressState::Hidden),
                                                                .percent = e.progress.percentage } };
            },
            [](from::FocusChanged e) -> Event {
                return FocusChanged { .state = mapped(FocusStateMappings, e.state, FocusState::Unfocused) };
            },
            [](from::FontChanged e) -> Event {
                return FontChanged { .metrics = FontMetrics {
                                         .pointSize = e.metrics.pointSize,
                                         .cellWidth = e.metrics.cellPixelSize.width.as<uint32_t>(),
                                         .cellHeight = e.metrics.cellPixelSize.height.as<uint32_t>() } };
            },
        },
        std::move(event));
}

std::function<void()> publisherFor(vtbackend::StatusLinePublishSink& sink)
{
    return [&sink] {
        sink.publish();
    };
}

vtbackend::StatusLineEventKinds toStatusLineEventKinds(EventKinds kinds) noexcept
{
    // Bit for bit, as KindMappings asserts.
    return vtbackend::StatusLineEventKinds::fromValue(
        static_cast<vtbackend::StatusLineEventKinds::value_type>((kinds & SubscribableKinds).value()));
}

vtbackend::StatusLineEventKinds StatusLineAdapter::subscriptions() const noexcept
{
    return toStatusLineEventKinds(_host.subscriptions());
}

void StatusLineAdapter::post(vtbackend::StatusLineEvent event) noexcept
{
    _host.post(toEvent(std::move(event)));
}

void StatusLineAdapter::resize(vtbackend::PageSize statusLineSize) noexcept
{
    _host.resize(statusLineSize);
}

std::shared_ptr<vtbackend::StatusLineExtensionSnapshot const> StatusLineAdapter::snapshot(
    std::string_view name) const noexcept
{
    return _host.snapshot(name);
}

std::optional<std::string> StatusLineAdapter::takeOverName() const noexcept
{
    return _host.takeOverName();
}

} // namespace vtextension
