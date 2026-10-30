// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/platform/LauncherProgress.hpp>
    #include <contour/platform/TaskbarProgressSink.hpp>

    #include <core/log/LogStore.hpp>

    #include <QtDBus/QDBusConnection>
    #include <QtDBus/QDBusMessage>
    #include <QtGui/QGuiApplication>

namespace contour::platform
{
namespace
{
    auto const taskbarLog = core::log::Category { "taskbar.progress", "Desktop launcher progress" };

    class LauncherProgressSink final: public TaskbarProgressSink
    {
      public:
        /// Acquires the session bus only when desktop progress is needed.
        using BusProvider = QDBusConnection (*)();

        /// @param busProvider Session bus provider used to broadcast launcher updates.
        explicit LauncherProgressSink(BusProvider busProvider): _busProvider(busProvider) {}

        // Clears only what was shown: an adapter that never published must not open the session bus
        // at teardown, which would start Qt's D-Bus thread in every headless `contour <verb>` exit.
        ~LauncherProgressSink() override
        {
            if (_published != LauncherEntry {})
                publish({});
        }

        [[nodiscard]] bool isSupported() const noexcept override
        {
            auto const platform = QGuiApplication::platformName();
            return (platform == QStringLiteral("xcb") || platform == QStringLiteral("wayland"))
                   && _busProvider().isConnected();
        }
        [[nodiscard]] TaskbarProgressScope scope() const noexcept override
        {
            return TaskbarProgressScope::Application;
        }
        void bindWindow(uint64_t, QWindow&) override {}
        void setProgress(uint64_t, TaskbarProgress) override {}
        void removeWindow(uint64_t) override {}
        void setApplicationAttention(TaskbarAttention attention) override
        {
            publish({ .progress = _desired.progress, .attention = attention });
        }
        void setAttention(uint64_t, TaskbarAttention) override {}

        void setApplicationProgress(TaskbarProgress progress) override
        {
            publish({ .progress = progress, .attention = _desired.attention });
        }

      private:
        /// What one LauncherEntry Update carries: the entry has a single property set, so progress and count
        /// travel together and each change re-sends the other as the sink last knew it.
        struct LauncherEntry
        {
            TaskbarProgress progress;
            TaskbarAttention attention;
            bool operator==(LauncherEntry const&) const = default;
        };

        void publish(LauncherEntry entry)
        {
            _desired = entry;
            if (entry == _published)
                return; // The launcher already shows this: no broadcast for an unchanged count or progress.
            auto const uri = launcherProgressUri(QGuiApplication::desktopFileName());
            if (!isSupported() || uri.isEmpty())
                return;
            auto message =
                QDBusMessage::createSignal(QStringLiteral("/org/contourterminal/Contour/LauncherEntry"),
                                           QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                           QStringLiteral("Update"));
            message.setArguments({ uri, launcherEntryProperties(entry.progress, entry.attention) });
            // This protocol is a broadcast, not a request: a desktop without a listener simply ignores it.
            if (!_busProvider().send(message))
                taskbarLog()("Cannot send desktop launcher update");
            else
                _published = entry;
        }

        BusProvider _busProvider;
        LauncherEntry _desired;   ///< The latest progress and count handed to the sink.
        LauncherEntry _published; ///< What the launcher last received.
    };
} // namespace

std::unique_ptr<TaskbarProgressSink> makeTaskbarProgressSink()
{
    return std::make_unique<LauncherProgressSink>(&QDBusConnection::sessionBus);
}
} // namespace contour::platform
#endif
