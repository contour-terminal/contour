// SPDX-License-Identifier: Apache-2.0
#include <contour/display/GpuInventory.hpp>
#ifdef __linux__
    #include <contour/display/SysfsGpuInventory.hpp>
#endif

#include <fstream>
#include <sstream>

namespace contour::display
{

namespace
{
    class FileSystemTextReader final: public ITextFileReader
    {
      public:
        [[nodiscard]] std::optional<std::string> read(std::filesystem::path const& path) const override
        {
            // Streamed, not sized: sysfs reports a size that is not the content's (as /proc does).
            auto file = std::ifstream(path, std::ios::binary);
            if (!file)
                return std::nullopt;
            auto contents = std::ostringstream {};
            contents << file.rdbuf();
            return std::move(contents).str();
        }

        [[nodiscard]] std::vector<std::string> listDirectory(std::filesystem::path const& path) const override
        {
            auto names = std::vector<std::string> {};
            auto error = std::error_code {};
            for (auto const& entry: std::filesystem::directory_iterator(path, error))
                names.push_back(entry.path().filename().string());
            return names;
        }

        [[nodiscard]] std::optional<std::filesystem::path> readLink(
            std::filesystem::path const& path) const override
        {
            auto error = std::error_code {};
            auto target = std::filesystem::read_symlink(path, error);
            return error ? std::nullopt : std::optional { std::move(target) };
        }
    };

    class EmptyGpuInventory final: public IGpuInventory
    {
      public:
        [[nodiscard]] std::vector<GpuCandidate> list() const override { return {}; }
    };
} // namespace

std::shared_ptr<ITextFileReader const> makeFileSystemTextReader()
{
    return std::make_shared<FileSystemTextReader>();
}

std::shared_ptr<IGpuInventory const> makePlatformGpuInventory()
{
#ifdef __linux__
    return std::make_shared<SysfsGpuInventory>(
        makeFileSystemTextReader(),
        "/sys/class/drm",
        std::vector<std::filesystem::path> { "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids" });
#else
    return std::make_shared<EmptyGpuInventory>();
#endif
}

} // namespace contour::display
