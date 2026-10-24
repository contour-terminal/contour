// SPDX-License-Identifier: Apache-2.0
#include <vthost/CommandRunner.hpp>

#ifdef _WIN32

namespace vthost
{

namespace
{
    /// Windows hosts its service in the Task Scheduler, which needs no external command.
    class UnsupportedCommandRunner final: public CommandRunner
    {
      public:
        [[nodiscard]] std::expected<CommandOutcome, std::error_code> run(
            std::span<std::string const> /*argv*/) override
        {
            return std::unexpected(std::make_error_code(std::errc::not_supported));
        }
    };
} // namespace

std::unique_ptr<CommandRunner> makeNativeCommandRunner()
{
    return std::make_unique<UnsupportedCommandRunner>();
}

} // namespace vthost

#endif // _WIN32
