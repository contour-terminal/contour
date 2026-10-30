// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/LauncherMenu.hpp>

#ifdef _WIN32

    #include <QtCore/QString>

    #include <memory>
    #include <string>
    #include <utility>

    #include <Windows.h>
    // Windows.h first, as the shell headers want it.
    #include <ShObjIdl.h>
    #include <propkey.h>
    #include <propvarutil.h>

namespace contour::platform
{

namespace
{
    /// Releases a COM interface.
    struct ComRelease
    {
        void operator()(IUnknown* object) const noexcept { object->Release(); }
    };

    template <typename T>
    using ComPtr = std::unique_ptr<T, ComRelease>;

    /// Creates the COM object @p clsid and asks it for @p T.
    template <typename T>
    [[nodiscard]] ComPtr<T> createInstance(CLSID const& clsid)
    {
        auto* raw = static_cast<T*>(nullptr);
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw))))
            return nullptr;
        return ComPtr<T> { raw };
    }

    /// @param argument One command-line argument.
    /// @return It quoted the way CommandLineToArgvW reads it back.
    [[nodiscard]] std::wstring quoted(std::string const& argument)
    {
        auto const wide = QString::fromStdString(argument).toStdWString();
        auto out = std::wstring { L"\"" };
        auto backslashes = std::size_t { 0 };
        for (auto const c: wide)
        {
            if (c == L'\\')
            {
                ++backslashes;
                out += c;
                continue;
            }
            if (c == L'"')
                out.append(backslashes + 1, L'\\');
            backslashes = 0;
            out += c;
        }
        out.append(backslashes, L'\\');
        out += L'"';
        return out;
    }

    /// The taskbar button's jump list: each entry a Task, a shell link to this executable with the entry's
    /// arguments, which the shell runs itself, so a click needs no running application to answer it.
    class Win32JumpList final: public LauncherMenu
    {
      public:
        explicit Win32JumpList(std::wstring program): _program(std::move(program)) {}

        [[nodiscard]] command::LauncherSurface surface() const noexcept override
        {
            return command::LauncherSurface::JumpList;
        }

        void set(std::span<command::LauncherEntry const> entries) override
        {
            auto const list = createInstance<ICustomDestinationList>(CLSID_DestinationList);
            auto const tasks = createInstance<IObjectCollection>(CLSID_EnumerableObjectCollection);
            if (!list || !tasks)
                return;

            auto slotCount = UINT { 0 };
            auto* removedRaw = static_cast<IObjectArray*>(nullptr);
            if (FAILED(list->BeginList(&slotCount, IID_PPV_ARGS(&removedRaw))))
                return;
            auto const removed = ComPtr<IObjectArray> { removedRaw };

            for (auto const& entry: entries)
                if (auto const link = taskFor(entry))
                    tasks->AddObject(link.get());

            auto* arrayRaw = static_cast<IObjectArray*>(nullptr);
            if (SUCCEEDED(tasks->QueryInterface(IID_PPV_ARGS(&arrayRaw))))
            {
                auto const array = ComPtr<IObjectArray> { arrayRaw };
                list->AddUserTasks(array.get());
            }
            list->CommitList();
        }

      private:
        [[nodiscard]] ComPtr<IShellLinkW> taskFor(command::LauncherEntry const& entry) const
        {
            auto link = createInstance<IShellLinkW>(CLSID_ShellLink);
            if (!link)
                return nullptr;
            auto arguments = std::wstring {};
            for (auto const& argument: entry.arguments)
            {
                if (!arguments.empty())
                    arguments += L' ';
                arguments += quoted(argument);
            }
            link->SetPath(_program.c_str());
            link->SetArguments(arguments.c_str());
            link->SetIconLocation(_program.c_str(), 0);
            if (!entry.detail.empty())
                link->SetDescription(QString::fromStdString(entry.detail).toStdWString().c_str());

            auto* storeRaw = static_cast<IPropertyStore*>(nullptr);
            if (FAILED(link->QueryInterface(IID_PPV_ARGS(&storeRaw))))
                return nullptr;
            auto const store = ComPtr<IPropertyStore> { storeRaw };
            auto const title = QString::fromStdString(entry.title).toStdWString();
            auto value = PROPVARIANT {};
            if (FAILED(InitPropVariantFromString(title.c_str(), &value)))
                return nullptr;
            auto const stored = store->SetValue(PKEY_Title, value);
            PropVariantClear(&value);
            if (FAILED(stored) || FAILED(store->Commit()))
                return nullptr;
            return link;
        }

        std::wstring _program;
    };
} // namespace

std::unique_ptr<LauncherMenu> makeJumpList(std::string const& programPath)
{
    return std::make_unique<Win32JumpList>(QString::fromStdString(programPath).toStdWString());
}

} // namespace contour::platform

#endif
