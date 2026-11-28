// SPDX-License-Identifier: Apache-2.0
//
// What only the POSIX descriptors owe: an open of a FIFO never waits for its other end, while a
// regular file is opened blocking (a lease is broken and waited for rather than refused); a name
// that is not UTF-8, which only a POSIX file system holds, is refused for its entry alone.
#include <core/fs/Descriptor.hpp>
#include <core/fs/DescriptorTestSupport.hpp>
#include <core/fs/FsError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using core::fs::DescriptorFlags;
using core::fs::DescriptorType;
using core::fs::FsErrorCode;
using core::fs::OpenFlags;
using core::fs::SymlinkPolicy;
using core::fs::test::DescriptorFixture;
using core::fs::test::outcomeOf;

TEST_CASE("posix: a FIFO with no other end opens without waiting", "[fs][native]")
{
    auto fixture = DescriptorFixture::createNative();
    REQUIRE(::mkfifo((fixture.hostDirectory() / "fifo").c_str(), 0600) == 0);

    // Reading opens at once; writing has no reader, which O_NONBLOCK reports as ENXIO.
    auto reader = fixture.run(
        fixture.root().openAt(SymlinkPolicy::NoFollow, "fifo", OpenFlags::None, DescriptorFlags::Read));
    REQUIRE(reader.has_value());
    CHECK(fixture.run((*reader)->type()) == DescriptorType::Fifo);
    reader->reset();
    CHECK(outcomeOf(fixture.run(fixture.root().openAt(
              SymlinkPolicy::NoFollow, "fifo", OpenFlags::None, DescriptorFlags::Write)))
          == FsErrorCode::NoSuchDevice);
}

TEST_CASE("posix: a name that is not UTF-8 is IllegalByteSequence, and the stream goes on",
          "[fs][native][directory]")
{
    auto fixture = DescriptorFixture::createNative();
    fixture.createFile("before", "x");
    // Through the host, which takes any bytes; a file system that refuses such a name (APFS answers
    // EILSEQ) cannot hold the case.
    auto const host = (fixture.hostDirectory() / "bad-").native() + "\xff\xfe";
    auto const made = ::open(host.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (made < 0)
        SKIP("the file system refuses a name that is not UTF-8 (errno " << errno << ")");
    ::close(made);
    fixture.createFile("after", "x");

    auto stream = fixture.run(fixture.root().readDirectory());
    REQUIRE(stream.has_value());
    auto const answers = fixture.readEntries(**stream, 16);
    REQUIRE(answers.size() == 3);
    auto names = std::vector<std::string> {};
    auto refused = std::vector<FsErrorCode> {};
    for (auto const& answer: answers)
    {
        if (answer)
            names.push_back(answer->name);
        else
            refused.push_back(answer.error().code);
    }
    std::ranges::sort(names);
    CHECK(names == std::vector<std::string> { "after", "before" });
    CHECK(refused == std::vector<FsErrorCode> { FsErrorCode::IllegalByteSequence });
}
