// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace vtpty
{

class HandoffPty: public Pty
{
  public:
    HandoffPty(HANDLE hInputWrite,
               HANDLE hOutputRead,
               HANDLE hSignal,
               HANDLE hReference,
               HANDLE hServer,
               HANDLE hClient,
               std::wstring const& title);
    ~HandoffPty() override;

    void start() override;
    PtySlave& slave() noexcept override;
    void close() override;
    void waitForClosed() override;
    bool isClosed() const noexcept override;
    std::optional<ReadResult> read(crispy::buffer_object<char>& storage,
                                   std::optional<std::chrono::milliseconds> timeout,
                                   size_t size) override;
    void wakeupReader() override;
    int write(std::string_view buf) override;
    PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

  private:
  private:
    HANDLE _hInputWrite; // We write to this
    HANDLE _hOutputRead; // We read from this
    HANDLE _hSignal;
    HANDLE _hReference;
    HANDLE _hServer;
    HANDLE _hClient;
    std::wstring _title;

    HANDLE _hWakeup; // Event for canceling read
    bool _closed = false;
    PtySlaveDummy _slave;
    PageSize _pageSize;

    OVERLAPPED _readOverlapped {};
    OVERLAPPED _writeOverlapped {};
    std::vector<char> _readBuffer;
};

} // namespace vtpty
