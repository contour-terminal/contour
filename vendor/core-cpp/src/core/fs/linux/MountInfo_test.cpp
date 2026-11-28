// SPDX-License-Identifier: Apache-2.0
//
// The device, root and type columns of Linux's mountinfo, read for a mount a relation walk meets.
#include <core/fs/linux/MountInfo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using core::fs::detail::MountEntry;
using core::fs::detail::mountEntryOf;
using core::fs::detail::MountPlace;
using core::fs::detail::mountPlacesOf;

namespace
{

constexpr auto Table = std::string_view { "22 1 0:21 / / rw,relatime shared:1 - ext4 /dev/sda1 rw\n"
                                          "35 22 0:32 / /tmp rw,nosuid shared:15 - tmpfs tmpfs rw\n"
                                          "61 35 0:32 /a/sub /tmp/m rw,relatime shared:15 - tmpfs tmpfs rw\n"
                                          "62 22 8:1 /home/with\\040space /mnt/s rw - ext4 /dev/sda1 rw\n"
                                          "63 22 8:1 /bad\\09x /mnt/b rw - ext4 /dev/sda1 rw\n"
                                          "x4 22 8:1 /not-a-number /mnt/n rw - ext4 /dev/sda1 rw\n"
                                          "65 22\n"
                                          "66 22 8 /no-minor /mnt/d rw - ext4 /dev/sda1 rw\n"
                                          "67 22 8:1 /a\\000b /mnt/z rw - ext4 /dev/sda1 rw\n"
                                          "68 22 4294967296:1 /wide /mnt/w rw - ext4 /dev/sda1 rw\n"
                                          "69 22 259:3 /home /home rw - btrfs /dev/nvme0n1p3 rw\n"
                                          "70 22 0:50 / /merged rw shared:3 master:1 - overlay overlay rw\n"
                                          "71 22 0:51 / /mnt/r rw - fuse.sshfs host: rw\n"
                                          "72 22 0:52 / /mnt/q rw ext4 /dev/sda1 rw\n"
                                          "73 22 0:53 / /mnt/t rw -\n" };

/// @return A device as mountEntryOf packs it.
[[nodiscard]] constexpr std::uint64_t device(std::uint64_t major, std::uint64_t minor)
{
    return (major << 32U) | minor;
}

} // namespace

TEST_CASE("mountinfo: a mount's device, root and type are its third, fourth and post-separator columns",
          "[fs][relation]")
{
    CHECK(mountEntryOf(Table, 22)
          == std::optional { MountEntry { .device = device(0, 21), .root = "/", .type = "ext4" } });
    CHECK(mountEntryOf(Table, 35)
          == std::optional { MountEntry { .device = device(0, 32), .root = "/", .type = "tmpfs" } });
    CHECK(mountEntryOf(Table, 61)
          == std::optional { MountEntry { .device = device(0, 32), .root = "/a/sub", .type = "tmpfs" } });
    CHECK(mountEntryOf(Table, 62)
          == std::optional {
              MountEntry { .device = device(8, 1), .root = "/home/with space", .type = "ext4" } });
    CHECK(mountEntryOf(Table, 70)
          == std::optional { MountEntry { .device = device(0, 50), .root = "/", .type = "overlay" } });
    CHECK(mountEntryOf(Table, 71)
          == std::optional { MountEntry { .device = device(0, 51), .root = "/", .type = "fuse.sshfs" } });
    CHECK(mountEntryOf(Table, 69)
          == std::optional { MountEntry { .device = device(259, 3), .root = "/home", .type = "btrfs" } });
}

TEST_CASE("mountinfo: no well-formed line is no entry", "[fs][relation]")
{
    CHECK_FALSE(mountEntryOf(Table, 63).has_value()); // a bad escape
    CHECK_FALSE(mountEntryOf(Table, 65).has_value()); // too few columns
    CHECK_FALSE(mountEntryOf(Table, 4).has_value());  // an id that is no number
    CHECK_FALSE(mountEntryOf(Table, 66).has_value()); // a device with no minor
    CHECK_FALSE(mountEntryOf(Table, 67).has_value()); // an escaped NUL in the middle of the root
    CHECK_FALSE(mountEntryOf(Table, 68).has_value()); // a major wider than 32 bits
    CHECK_FALSE(mountEntryOf(Table, 72).has_value()); // no separator before the type
    CHECK_FALSE(mountEntryOf(Table, 73).has_value()); // a separator with no type after it
    CHECK_FALSE(mountEntryOf(Table, 99).has_value()); // no such mount
    CHECK_FALSE(mountEntryOf("", 22).has_value());
}

TEST_CASE("mountinfo: every mount's id and mount point, or nothing where a line does not parse",
          "[fs][relation]")
{
    constexpr auto Good = std::string_view { "22 1 0:21 / / rw - ext4 /dev/sda1 rw\n"
                                             "62 22 8:1 /home /mnt/with\\040space rw - ext4 /dev/sda1 rw\n" };
    CHECK(mountPlacesOf(Good)
          == std::optional { std::vector { MountPlace { .id = 22, .mountPoint = "/" },
                                           MountPlace { .id = 62, .mountPoint = "/mnt/with space" } } });
    CHECK(mountPlacesOf("") == std::optional { std::vector<MountPlace> {} });
    // One line that cannot be read hides a mount the scan must see: the whole table is refused.
    for (auto const* bad: { "x4 22 8:1 / /mnt/n rw - ext4 /dev/sda1 rw\n",
                            "65 22 8:1 /\n",
                            "67 22 8:1 / /mnt/a\\000b rw - ext4 /dev/sda1 rw\n",
                            "68 22 8:1 / /mnt/a\\09x rw - ext4 /dev/sda1 rw\n",
                            "69 22 8:1 / relative rw - ext4 /dev/sda1 rw\n" })
    {
        INFO(bad);
        CHECK_FALSE(mountPlacesOf(std::string { Good } + bad).has_value());
    }
}
