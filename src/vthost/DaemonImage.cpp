// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonImage.hpp>

#include <core/FNV.hpp>
#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>
#include <system_error>
#include <utility>

namespace vthost
{

namespace fs = std::filesystem;

namespace
{
    /// The file an image holds once, and only once, it is complete. Written last, before the rename.
    constexpr auto CompletionMarker = std::string_view { ".complete" };

    /// What separates an image's name from the staging process's id in a partial copy's name.
    constexpr auto PartialInfix = std::string_view { ".partial-" };

    /// The extension of an image's lease file, a sibling of the image.
    constexpr auto LeaseExtension = std::string_view { ".lease" };

    /// @return The `.app` bundle @p executable lies in, as `<bundle>/Contents/MacOS/<exe>`, if it does.
    [[nodiscard]] std::optional<fs::path> appBundleOf(fs::path const& executable)
    {
        auto const macos = executable.parent_path();
        auto const contents = macos.parent_path();
        auto bundle = contents.parent_path();
        if (macos.filename() != "MacOS" || contents.filename() != "Contents" || bundle.extension() != ".app")
            return std::nullopt;
        return bundle;
    }

    /// @return The plan copying nothing.
    [[nodiscard]] StagingPlan inPlace(StagingInputs const&)
    {
        return StagingPlan { .layout = StagingLayout::InPlace };
    }

    /// One row of stagingPlan()'s table.
    struct PlanRule
    {
        bool (*matches)(StagingInputs const&);
        StagingPlan (*plan)(StagingInputs const&);
    };

    /// stagingPlan()'s table, first match wins (@see stagingPlan for why each row is what it is).
    constexpr auto PlanRules = std::array {
        PlanRule {
            .matches = [](StagingInputs const& in) { return in.sandbox == vtpty::SandboxState::Flatpak; },
            .plan = inPlace },
        PlanRule { .matches = [](StagingInputs const& in) { return in.appImage.has_value(); },
                   .plan =
                       [](StagingInputs const& in) {
                           return StagingPlan { .layout = StagingLayout::AppImage,
                                                .sourceRoot = in.appImage->parent_path(),
                                                .entries = { in.appImage->filename() },
                                                .executable = in.appImage->filename() };
                       } },
        PlanRule { .matches = [](StagingInputs const& in) { return in.platform == HostPlatform::Linux; },
                   .plan = inPlace },
        PlanRule { .matches =
                       [](StagingInputs const& in) {
                           return in.platform == HostPlatform::MacOS
                                  && appBundleOf(in.executable).has_value();
                       },
                   .plan =
                       [](StagingInputs const& in) {
                           auto const bundle = *appBundleOf(in.executable);
                           return StagingPlan { .layout = StagingLayout::AppBundle,
                                                .sourceRoot = bundle.parent_path(),
                                                .entries = { bundle.filename() },
                                                .executable = bundle.filename() / "Contents" / "MacOS"
                                                              / in.executable.filename(),
                                                .symlinks = SymlinkPolicy::Preserve };
                       } },
        PlanRule { .matches = [](StagingInputs const& in) { return in.platform == HostPlatform::Windows; },
                   .plan =
                       [](StagingInputs const& in) {
                           return StagingPlan { .layout = StagingLayout::InstallTree,
                                                .sourceRoot = in.executable.parent_path().parent_path(),
                                                .entries = { "bin", fs::path { "share" } / "contour" },
                                                .executable = fs::path { "bin" } / in.executable.filename() };
                       } },
        PlanRule { .matches = [](StagingInputs const&) { return true; },
                   .plan =
                       [](StagingInputs const& in) {
                           return StagingPlan { .layout = StagingLayout::ExecutableOnly,
                                                .sourceRoot = in.executable.parent_path(),
                                                .entries = { in.executable.filename() },
                                                .executable = in.executable.filename() };
                       } },
    };

    [[nodiscard]] std::string_view describe(StagingErrorCode code) noexcept
    {
        switch (code)
        {
            case StagingErrorCode::SourceUnreadable: return "cannot inspect the executable to stage";
            case StagingErrorCode::CopyFailed: return "cannot copy the daemon image";
            case StagingErrorCode::PublishFailed: return "cannot publish the daemon image";
            case StagingErrorCode::LeaseFailed: return "cannot lease the daemon image";
        }
        return "unknown error";
    }

    /// @return A StagingError of @p code, its context @p reason.
    [[nodiscard]] std::unexpected<StagingError> failure(StagingErrorCode code, std::string reason)
    {
        return std::unexpected(StagingError { .code = code, .context = std::move(reason) });
    }

    /// @return A CopyFailed error, its context @p reason.
    [[nodiscard]] StagingError copyFailed(std::string reason)
    {
        return StagingError { .code = StagingErrorCode::CopyFailed, .context = std::move(reason) };
    }

    /// @return @p name followed by @p suffix, a path.
    [[nodiscard]] fs::path suffixed(fs::path name, std::string_view suffix)
    {
        name += suffix;
        return name;
    }

    /// What prune() finds of one image name: its directory, its partial copies, both, or neither
    /// (a lease file left behind alone).
    struct ImageRemains
    {
        std::optional<fs::path> image;
        std::vector<fs::path> partials;
    };
} // namespace

StagingPlan stagingPlan(StagingInputs const& inputs)
{
    // The last row matches everything, so the search always finds one.
    return std::ranges::find_if(PlanRules, [&](PlanRule const& rule) { return rule.matches(inputs); })
        ->plan(inputs);
}

std::string imageDirectoryName(std::string_view releaseVersion,
                               std::uintmax_t executableSize,
                               fs::file_time_type executableTime)
{
    auto const isSafe = [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.'
               || ch == '_' || ch == '-';
    };
    auto name = std::ranges::to<std::string>(
        releaseVersion | std::views::transform([&](char ch) { return isSafe(ch) ? ch : '_'; }));

    auto const fnv = core::FNV<char, std::uint32_t> {};
    auto const ticks = static_cast<std::int64_t>(executableTime.time_since_epoch().count());
    auto const fingerprint = fnv(fnv(fnv.basis(), static_cast<std::uint64_t>(executableSize)), ticks);
    return std::format("{}-{:08x}", name, fingerprint);
}

std::string StagingError::toString() const
{
    auto result = std::string { describe(code) };
    if (!context.empty())
        result += ": " + context;
    return result;
}

DaemonImageStore::DaemonImageStore(DaemonImageStoreConfig config,
                                   core::platform::FileSystem const& fs,
                                   ImageLeases& leases,
                                   SymbolicLinks const& links):
    _config { std::move(config) },
    _plan { _config.stateOrigin == StateDirectoryOrigin::Configured
                ? stagingPlan(_config.inputs)
                : StagingPlan { .layout = StagingLayout::InPlace } },
    _fs { fs },
    _leases { leases },
    _links { links }
{
}

std::expected<StagedImage, StagingError> DaemonImageStore::stage()
{
    if (_plan.layout == StagingLayout::InPlace)
        return StagedImage { .executable = _config.inputs.executable, .lease = nullptr };

    auto const target = currentTarget();
    if (!target)
        return std::unexpected(target.error());
    if (auto const created = _fs.createDirectories(_config.imagesRoot); !created)
        return failure(StagingErrorCode::CopyFailed, created.error());

    // The lease first: a prune that ran before it has removed the image, and the check below sees
    // it gone; one that runs after it finds the image held.
    auto lease = _leases.hold(leaseFileOf(*target));
    if (!lease)
        return std::unexpected(lease.error());
    if (isComplete(*target))
        return imageAt(*target, std::move(*lease));

    auto const partial = suffixed(*target, std::format("{}{}", PartialInfix, _config.processId));
    auto const discardPartial = [&] {
        std::ignore = _fs.removeAll(partial);
    };
    discardPartial(); // left by an earlier process of this id that died staging
    if (auto const copied = copyInto(partial); !copied)
    {
        discardPartial();
        return std::unexpected(copied.error());
    }

    // Publish first, and only look at what is in the way when that fails: a target another launcher
    // completed is theirs, and ours; one that is incomplete is nobody's, since only complete images
    // are handed out, and is moved out of the way before one more try.
    auto published = _fs.rename(partial, *target);
    if (!published && !isComplete(*target) && _fs.exists(*target))
    {
        clearIncompleteTarget(*target);
        published = _fs.rename(partial, *target);
    }
    if (published || isComplete(*target))
    {
        discardPartial();
        return imageAt(*target, std::move(*lease));
    }
    discardPartial();
    return failure(StagingErrorCode::PublishFailed, published.error());
}

void DaemonImageStore::clearIncompleteTarget(fs::path const& target) const
{
    // Moved aside rather than removed in place: between the check that found it incomplete and now,
    // another launcher may have cleared it and published its complete image there, and a complete
    // image -- leased, and perhaps already handed out -- is never removed here. The rename takes
    // whatever is there as one, and what it took is looked at afterwards.
    auto const aside = suffixed(target, std::format("{}{}-stale", PartialInfix, _config.processId));
    std::ignore = _fs.removeAll(aside);
    if (!_fs.rename(target, aside))
        return;
    if (!isComplete(aside))
    {
        std::ignore = _fs.removeAll(aside);
        return;
    }
    // It was the other launcher's: put it back. Should a third have published in the meantime, an
    // image of the same name is the same image, and this one is left, as a partial of the target, to
    // prune() once nobody holds the target's lease.
    std::ignore = _fs.rename(aside, target);
}

std::size_t DaemonImageStore::prune()
{
    // A directory others could have planted is not this store's to clean, either.
    if (_config.stateOrigin != StateDirectoryOrigin::Configured)
        return 0;
    auto const children = _fs.listDirectory(_config.imagesRoot);
    if (!children)
        return 0;

    // Every name an image, a partial copy or a lease file is left under, with what is left of it.
    auto remains = std::map<std::string, ImageRemains> {};
    for (auto const& child: *children)
    {
        auto const name = child.path.filename().string();
        if (child.isDirectory)
        {
            if (auto const infix = name.find(PartialInfix); infix != std::string::npos)
                remains[name.substr(0, infix)].partials.push_back(child.path);
            else
                remains[name].image = child.path;
        }
        else if (name.ends_with(LeaseExtension))
            std::ignore = remains[name.substr(0, name.size() - LeaseExtension.size())];
    }

    // This executable's own image is kept leased or not: it is about to be, or just was, staged.
    auto const current =
        currentTarget().transform([](fs::path const& target) { return target.filename().string(); });

    auto removed = std::size_t { 0 };
    auto const removeDirectory = [&](fs::path const& directory) {
        if (auto const count = _fs.removeAll(directory); count && *count > 0)
            ++removed;
    };
    for (auto const& [name, what]: remains)
    {
        if (current && name == *current)
            continue;
        auto const leaseFile = _config.imagesRoot / suffixed(name, LeaseExtension);
        // Claimed, not merely probed: a lease taken between a probe and the removal would be handed
        // a directory that is being deleted. The lease file goes last, under the claim.
        auto const claim = _leases.claim(leaseFile);
        if (!claim)
            continue;
        if (what.image)
            removeDirectory(*what.image);
        std::ranges::for_each(what.partials, removeDirectory);
        std::ignore = _fs.remove(leaseFile);
    }
    return removed;
}

bool DaemonImageStore::contains(fs::path const& executable) const
{
    return !imageDirectoryOf(executable).empty();
}

fs::path DaemonImageStore::imageDirectoryOf(fs::path const& executable) const
{
    auto const relative =
        executable.lexically_normal().lexically_relative(_config.imagesRoot.lexically_normal());
    if (relative.empty() || relative == "." || *relative.begin() == "..")
        return {};
    return _config.imagesRoot / *relative.begin();
}

fs::path DaemonImageStore::leaseFileOf(fs::path const& imageDir) const
{
    return suffixed(imageDir, LeaseExtension);
}

std::expected<fs::path, StagingError> DaemonImageStore::currentTarget() const
{
    if (_plan.layout == StagingLayout::InPlace)
        return failure(StagingErrorCode::SourceUnreadable, "nothing is staged for an in-place layout");
    auto const executable = _plan.sourceRoot / _plan.executable;
    auto const describeFailure = [&](std::string const& reason) {
        return StagingError { .code = StagingErrorCode::SourceUnreadable,
                              .context = std::format("{}: {}", core::pathToUtf8(executable), reason) };
    };
    return _fs.fileSize(executable)
        .and_then([&](std::uintmax_t size) {
            return _fs.lastWriteTime(executable).transform([&](fs::file_time_type time) {
                return _config.imagesRoot / imageDirectoryName(_config.releaseVersion, size, time);
            });
        })
        .transform_error(describeFailure);
}

std::expected<void, StagingError> DaemonImageStore::copyInto(fs::path const& partial) const
{
    if (auto const created = _fs.createDirectories(partial); !created)
        return std::unexpected(copyFailed(created.error()));

    for (auto const& entry: _plan.entries)
    {
        auto const from = _plan.sourceRoot / entry;
        auto const to = partial / entry;
        if (!_fs.exists(from))
            continue; // an optional part of the install tree
        auto const copied = _fs.isDirectory(from) ? copyTree(from, to)
                                                  : _fs.createDirectories(to.parent_path())
                                                        .and_then([&] { return _fs.copyFile(from, to); })
                                                        .transform_error(copyFailed);
        if (!copied)
            return copied;
    }

    // A copy need not carry the execute bit (an AppImage downloaded by a browser, a copy across file
    // systems), and the daemon is started from it.
    auto const executable = partial / _plan.executable;
    return _fs.permissions(executable)
        .and_then(
            [&](fs::perms perms) { return _fs.setPermissions(executable, perms | fs::perms::owner_exec); })
        .and_then([&] { return _fs.writeFile(partial / CompletionMarker, _config.releaseVersion); })
        .transform_error(copyFailed);
}

std::expected<void, StagingError> DaemonImageStore::copyTree(fs::path const& from, fs::path const& to) const
{
    if (auto const created = _fs.createDirectories(to); !created)
        return std::unexpected(copyFailed(created.error()));

    auto walkError = std::error_code {};
    for (auto const& entry: _fs.walkDirectoryRecursive(from, &walkError))
    {
        auto const destination = to / entry.path.lexically_relative(from);
        auto const copied = [&]() -> std::expected<void, StagingError> {
            // A bundle's links are part of what its signature seals (a framework's Versions/Current),
            // so they are recreated. Anywhere else a link is copied as what it names; the walk does
            // not descend into a linked directory, so one is copied as a tree of its own.
            if (entry.isSymlink && _plan.symlinks == SymlinkPolicy::Preserve)
                return _links.copy(entry.path, destination).transform_error(copyFailed);
            if (entry.isDirectory && entry.isSymlink)
                return copyTree(entry.path, destination);
            if (entry.isDirectory)
                return _fs.createDirectories(destination).transform_error(copyFailed);
            return _fs.copyFile(entry.path, destination).transform_error(copyFailed);
        }();
        if (!copied)
            return copied;
    }
    if (walkError)
        return std::unexpected(
            copyFailed(std::format("cannot list {}: {}", core::pathToUtf8(from), walkError.message())));
    return {};
}

namespace
{
    class NativeSymbolicLinks final: public SymbolicLinks
    {
      public:
        [[nodiscard]] std::expected<void, std::string> copy(fs::path const& link,
                                                            fs::path const& to) const override
        {
            auto ec = std::error_code {};
            fs::copy_symlink(link, to, ec);
            if (ec)
                return std::unexpected(std::format("cannot copy the link {} to {}: {}",
                                                   core::pathToUtf8(link),
                                                   core::pathToUtf8(to),
                                                   ec.message()));
            return {};
        }
    };
} // namespace

std::unique_ptr<SymbolicLinks> makeNativeSymbolicLinks()
{
    return std::make_unique<NativeSymbolicLinks>();
}

StagedImage DaemonImageStore::imageAt(fs::path const& target, std::unique_ptr<ImageLease> lease) const
{
    return StagedImage { .executable = target / _plan.executable, .lease = std::move(lease) };
}

bool DaemonImageStore::isComplete(fs::path const& imageDir) const
{
    return _fs.isRegularFile(imageDir / CompletionMarker);
}

} // namespace vthost
