// SPDX-License-Identifier: Apache-2.0
//
// How a name is opened when it is a reparse point, decided over scripted NT answers: a point that
// names another object is a link and is never opened through; one that names nothing else (a cloud
// placeholder, a deduplicated file) is opened again, by the handle that was checked, through its
// filter, so a read sees the data the filter supplies rather than the stub. Then the reparse data a
// link is made and read with.
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/windows/NtFile.hpp>
#include <core/fs/windows/ReparsePoint.hpp>
#include <core/fs/windows/WindowsPathWalk.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

using core::fs::FsErrorCode;
using core::fs::FsResult;
using core::fs::detail::EntryAttributes;
using core::fs::detail::FileIdentity;
using core::fs::detail::INtFiles;
using core::fs::detail::LinkTarget;
using core::fs::detail::NtOpenRequest;
using core::fs::detail::ReparseHandling;
using core::fs::detail::UniqueHandle;
using core::fs::test::outcomeOf;

namespace
{

/// One open the code under test made.
struct OpenCall
{
    HANDLE directory = nullptr;
    std::wstring name;
    NtOpenRequest request;
};

/// NT answers from a script: each open takes the next answer, and each handle it hands out is a
/// real event handle, so the code under test closes what it was given, with the attributes and the
/// identity the script gave it.
class ScriptedNtFiles final: public INtFiles
{
  public:
    /// What one open answers.
    struct Answer
    {
        NTSTATUS status = 0; ///< A failure, or 0 for a handle.
        EntryAttributes attributes;
        FileIdentity identity;
    };

    explicit ScriptedNtFiles(std::deque<Answer> answers): _answers { std::move(answers) } {}

    FsResult<UniqueHandle> open(HANDLE directory,
                                std::wstring const& name,
                                NtOpenRequest const& request) const override
    {
        _calls.push_back(OpenCall { .directory = directory, .name = name, .request = request });
        REQUIRE_FALSE(_answers.empty());
        auto const answer = _answers.front();
        _answers.pop_front();
        if (answer.status < 0)
            return std::unexpected { core::fs::fsErrorFromNtStatus(std::int64_t { answer.status },
                                                                   "scripted") };
        auto handle = UniqueHandle { ::CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        _objects[handle.get()] = answer;
        _handed.push_back(handle.get());
        return handle;
    }

    FsResult<EntryAttributes> attributes(HANDLE handle) const override
    {
        return _objects.at(handle).attributes;
    }

    FsResult<FileIdentity> identity(HANDLE handle) const override { return _objects.at(handle).identity; }

    /// No case here lists a directory.
    FsResult<void> queryDirectory(HANDLE, std::span<std::byte>) const override
    {
        return std::unexpected { core::fs::FsError {
            .code = FsErrorCode::Unsupported, .systemCode = 0, .context = "scripted" } };
    }

    /// @return Every open made, in order.
    [[nodiscard]] std::vector<OpenCall> const& calls() const noexcept { return _calls; }

    /// @return The handles handed out, in order.
    [[nodiscard]] std::vector<HANDLE> const& handed() const noexcept { return _handed; }

  private:
    mutable std::deque<Answer> _answers;
    mutable std::vector<OpenCall> _calls;
    mutable std::map<HANDLE, Answer> _objects;
    mutable std::vector<HANDLE> _handed;
};

constexpr auto Placeholder =
    EntryAttributes { .attributes = FILE_ATTRIBUTE_REPARSE_POINT,
                      .reparseTag = static_cast<std::uint32_t>(IO_REPARSE_TAG_CLOUD_6) };
constexpr auto Symlink = EntryAttributes { .attributes = FILE_ATTRIBUTE_REPARSE_POINT,
                                           .reparseTag = static_cast<std::uint32_t>(IO_REPARSE_TAG_SYMLINK) };
constexpr auto Junction =
    EntryAttributes { .attributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY,
                      .reparseTag = static_cast<std::uint32_t>(IO_REPARSE_TAG_MOUNT_POINT) };
constexpr auto PlainFile = EntryAttributes { .attributes = FILE_ATTRIBUTE_NORMAL, .reparseTag = 0 };
constexpr auto SameFile = FileIdentity { .volume = 7, .idLow = 42, .idHigh = 0 };
constexpr auto OtherFile = FileIdentity { .volume = 7, .idLow = 43, .idHigh = 0 };

constexpr auto ReadRequest = NtOpenRequest { .access = FILE_GENERIC_READ,
                                             .disposition = FILE_OPEN_IF,
                                             .options = FILE_NON_DIRECTORY_FILE,
                                             .reparse = ReparseHandling::OpenThePoint };

/// A directory handle the opens are relative to; never dereferenced by the script.
HANDLE directory() noexcept
{
    return ::GetCurrentProcess();
}

} // namespace

TEST_CASE("a reparse point that names nothing else is opened again through its filter",
          "[fs][native][windows]")
{
    auto const files = ScriptedNtFiles { { { .status = 0, .attributes = Placeholder, .identity = SameFile },
                                           { .status = 0, .attributes = PlainFile, .identity = SameFile } } };
    auto const opened = openEntry(files, directory(), L"placeholder", ReadRequest);
    REQUIRE(opened.has_value());
    REQUIRE(files.calls().size() == 2);

    auto const& first = files.calls()[0];
    CHECK(first.directory == directory());
    CHECK(first.name == L"placeholder");
    CHECK(first.request.reparse == ReparseHandling::OpenThePoint);
    CHECK((first.request.access & FILE_READ_ATTRIBUTES) != 0);

    // Again by the handle checked, with no name: the same object, whatever the name names by now;
    // never created again.
    auto const& again = files.calls()[1];
    REQUIRE_FALSE(files.handed().empty());
    CHECK(again.directory == files.handed().front());
    CHECK(again.name.empty());
    CHECK(again.request.reparse == ReparseHandling::ThroughFilters);
    CHECK(again.request.disposition == ULONG { FILE_OPEN });
    CHECK(again.request.access == first.request.access);
    CHECK(again.request.options == first.request.options);
    CHECK(opened->attributes.reparseTag == Placeholder.reparseTag);
}

TEST_CASE("a name surrogate is never opened through", "[fs][native][windows]")
{
    for (auto const& link: { Symlink, Junction })
    {
        CAPTURE(link.reparseTag);
        auto const files = ScriptedNtFiles { { { .status = 0, .attributes = link, .identity = SameFile } } };
        auto const opened = openEntry(files, directory(), L"link", ReadRequest);
        REQUIRE(opened.has_value());
        CHECK(files.calls().size() == 1);
        CHECK(opened->attributes.isLink());
    }
}

TEST_CASE("a file that is no reparse point is opened once", "[fs][native][windows]")
{
    auto const files = ScriptedNtFiles { { { .status = 0, .attributes = PlainFile, .identity = SameFile } } };
    auto const opened = openEntry(files, directory(), L"f", ReadRequest);
    REQUIRE(opened.has_value());
    CHECK(files.calls().size() == 1);
    CHECK_FALSE(opened->attributes.isLink());
}

TEST_CASE("a reparse point no filter takes is the file itself", "[fs][native][windows]")
{
    auto const files = ScriptedNtFiles {
        { { .status = 0, .attributes = Placeholder, .identity = SameFile },
          { .status = STATUS_IO_REPARSE_TAG_NOT_HANDLED, .attributes = {}, .identity = {} } }
    };
    auto const opened = openEntry(files, directory(), L"private", ReadRequest);
    REQUIRE(opened.has_value());
    CHECK(files.calls().size() == 2);
    CHECK(opened->handle.get() != nullptr);
}

TEST_CASE("a filter that opens another object than the one checked is refused", "[fs][native][windows]")
{
    auto const files =
        ScriptedNtFiles { { { .status = 0, .attributes = Placeholder, .identity = SameFile },
                            { .status = 0, .attributes = PlainFile, .identity = OtherFile } } };
    auto const opened = openEntry(files, directory(), L"placeholder", ReadRequest);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == FsErrorCode::NotPermitted);
}

TEST_CASE("a filter's refusal is the open's", "[fs][native][windows]")
{
    auto const files =
        ScriptedNtFiles { { { .status = 0, .attributes = Placeholder, .identity = SameFile },
                            { .status = STATUS_ACCESS_DENIED, .attributes = {}, .identity = {} } } };
    auto const opened = openEntry(files, directory(), L"placeholder", ReadRequest);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == FsErrorCode::Access);
}

TEST_CASE("a relative symbolic link's reparse data reads back as it was written", "[fs][native][windows]")
{
    auto const bytes = core::fs::detail::relativeSymlinkReparseData(L"..\\x\\y", L"../x\\y");
    auto const data = core::fs::detail::parseReparseData(bytes);
    REQUIRE(data.has_value());
    CHECK(data->tag == static_cast<std::uint32_t>(IO_REPARSE_TAG_SYMLINK));
    CHECK(data->target == LinkTarget::Relative);
    CHECK(data->substituteName == L"..\\x\\y");
    CHECK(data->printName == L"../x\\y");
    CHECK(core::fs::detail::followedTarget(*data).value() == "..\\x\\y");
    CHECK(core::fs::detail::reportedTarget(*data).value() == "../x\\y");

    SECTION("a print name that spells another target is not what is reported")
    {
        auto const lying = core::fs::detail::parseReparseData(
            core::fs::detail::relativeSymlinkReparseData(L"..\\..\\outside", L"inside"));
        REQUIRE(lying.has_value());
        CHECK(core::fs::detail::reportedTarget(*lying).value() == "..\\..\\outside");
    }
}

TEST_CASE("an absolute link, a junction and a mount point are never followed", "[fs][native][windows]")
{
    auto absolute = core::fs::detail::relativeSymlinkReparseData(L"\\??\\C:\\outside", L"C:\\outside");
    absolute[16] = std::byte { 0 }; // the flags: not relative
    auto const junction = core::fs::detail::mountPointReparseData(L"\\??\\C:\\outside\\", L"C:\\outside\\");
    auto const volume =
        core::fs::detail::mountPointReparseData(L"\\??\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\", L"");
    for (auto const& bytes: { absolute, junction, volume })
    {
        auto const data = core::fs::detail::parseReparseData(bytes);
        REQUIRE(data.has_value());
        CHECK(outcomeOf(core::fs::detail::followedTarget(*data)) == FsErrorCode::NotPermitted);
        CHECK(outcomeOf(core::fs::detail::reportedTarget(*data)) == FsErrorCode::NotPermitted);
    }
}

TEST_CASE("reparse data that claims more than it holds is Invalid", "[fs][native][windows]")
{
    auto const bytes = core::fs::detail::relativeSymlinkReparseData(L"target", L"target");
    for (auto const length: { std::size_t { 0 }, std::size_t { 7 }, std::size_t { 19 }, bytes.size() - 1 })
    {
        CAPTURE(length);
        auto const data = core::fs::detail::parseReparseData(std::span { bytes }.first(length));
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().code == FsErrorCode::Invalid);
    }
    auto pastTheEnd = bytes;
    pastTheEnd[8] = std::byte { 0x40 }; // the substitute name's offset, beyond the data
    CHECK(outcomeOf(core::fs::detail::parseReparseData(pastTheEnd)) == FsErrorCode::Invalid);
}

TEST_CASE("a point swapped for a mount point before its filter opens it is refused", "[fs][native][windows]")
{
    // OBJ_DONT_REPARSE makes the reopen meet the mount point rather than follow it.
    auto const files = ScriptedNtFiles {
        { { .status = 0, .attributes = Placeholder, .identity = SameFile },
          { .status = STATUS_REPARSE_POINT_ENCOUNTERED, .attributes = {}, .identity = {} } }
    };
    auto const opened = openEntry(files, directory(), L"placeholder", ReadRequest);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == FsErrorCode::NotPermitted);
    REQUIRE(files.calls().size() == 2);
    CHECK(files.calls()[1].request.reparse == ReparseHandling::ThroughFilters);
}

TEST_CASE("a second handle is the same object, opened again by its handle", "[fs][native][windows]")
{
    constexpr auto AppendRequest = NtOpenRequest { .access = FILE_APPEND_DATA,
                                                   .disposition = FILE_OPEN,
                                                   .options = FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH,
                                                   .reparse = ReparseHandling::OpenThePoint };
    SECTION("a plain file: the point itself, never following one put on it since")
    {
        auto const files =
            ScriptedNtFiles { { { .status = 0, .attributes = PlainFile, .identity = SameFile },
                                { .status = 0, .attributes = PlainFile, .identity = SameFile } } };
        auto const entry = openEntry(files, directory(), L"f", ReadRequest);
        REQUIRE(entry.has_value());
        auto const second = core::fs::detail::openAgain(files, *entry, AppendRequest);
        REQUIRE(second.has_value());
        REQUIRE(files.calls().size() == 2);
        auto const& again = files.calls()[1];
        CHECK(again.directory == files.handed().front());
        CHECK(again.name.empty());
        CHECK(again.request.reparse == ReparseHandling::OpenThePoint);
        CHECK(again.request.disposition == ULONG { FILE_OPEN });
        CHECK((again.request.access & FILE_APPEND_DATA) != 0);
        CHECK((again.request.options & FILE_WRITE_THROUGH) != 0);
    }
    SECTION("the handle opens another object: a symbolic link put on the file was followed")
    {
        auto const files =
            ScriptedNtFiles { { { .status = 0, .attributes = PlainFile, .identity = SameFile },
                                { .status = 0, .attributes = PlainFile, .identity = OtherFile } } };
        auto const entry = openEntry(files, directory(), L"f", ReadRequest);
        REQUIRE(entry.has_value());
        CHECK(outcomeOf(core::fs::detail::openAgain(files, *entry, AppendRequest))
              == FsErrorCode::NotPermitted);
    }
    SECTION("a filtered point: through its filter, and a mount point swapped in is refused")
    {
        auto const files = ScriptedNtFiles {
            { { .status = 0, .attributes = Placeholder, .identity = SameFile },
              { .status = 0, .attributes = PlainFile, .identity = SameFile },
              { .status = STATUS_REPARSE_POINT_ENCOUNTERED, .attributes = {}, .identity = {} } }
        };
        auto const entry = openEntry(files, directory(), L"placeholder", ReadRequest);
        REQUIRE(entry.has_value());
        CHECK(outcomeOf(core::fs::detail::openAgain(files, *entry, AppendRequest))
              == FsErrorCode::NotPermitted);
        REQUIRE(files.calls().size() == 3);
        CHECK(files.calls()[2].request.reparse == ReparseHandling::ThroughFilters);
    }
}

TEST_CASE("an open through filters takes no name", "[fs][native][windows]")
{
    auto const opened =
        core::fs::detail::ntOpenRelative(directory(),
                                         L"name",
                                         NtOpenRequest { .access = FILE_READ_ATTRIBUTES,
                                                         .disposition = FILE_OPEN,
                                                         .options = 0,
                                                         .reparse = ReparseHandling::ThroughFilters });
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == FsErrorCode::Invalid);
}
