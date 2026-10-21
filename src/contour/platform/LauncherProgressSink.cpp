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
        ~LauncherProgressSink() override { setApplicationProgress({}); }

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

        void setApplicationProgress(TaskbarProgress progress) override
        {
            auto const uri = launcherProgressUri(QGuiApplication::desktopFileName());
            if (!isSupported() || uri.isEmpty())
                return;
            auto message =
                QDBusMessage::createSignal(QStringLiteral("/org/contourterminal/Contour/LauncherEntry"),
                                           QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                           QStringLiteral("Update"));
            message.setArguments({ uri, launcherProgressProperties(progress) });
            // This protocol is a broadcast, not a request: a desktop without a listener simply ignores it.
            if (!_busProvider().send(message))
                taskbarLog()("Cannot send desktop launcher progress update");
        }

      private:
        BusProvider _busProvider;
    };
} // namespace

std::unique_ptr<TaskbarProgressSink> makeTaskbarProgressSink()
{
    return std::make_unique<LauncherProgressSink>(&QDBusConnection::sessionBus);
}
} // namespace contour::platform
#endif
