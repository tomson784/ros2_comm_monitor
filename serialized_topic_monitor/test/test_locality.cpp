#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "serialized_topic_monitor/locality.hpp"

namespace loc = serialized_topic_monitor::locality;

namespace
{

// Build a 16-byte Fast DDS style GID: vendor(2) + host(2) + pid(4) + counter(4) + entity(4).
loc::GidBytes make_fastdds_gid(std::uint8_t host_hi, std::uint8_t host_lo, std::uint8_t pid)
{
  return loc::GidBytes{
    0x01, 0x0F, host_hi, host_lo,
    pid, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x03};
}

}  // namespace

TEST(LocalityTest, DetectsFastDdsGid)
{
  // Intent: vendor id 0x01 0x0F at bytes 0..1 identifies Fast DDS.
  EXPECT_TRUE(loc::is_fastdds_gid(make_fastdds_gid(0xAB, 0xCD, 1)));

  loc::GidBytes cyclone{0x10, 0x9a, 0x3b, 0x77, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  EXPECT_FALSE(loc::is_fastdds_gid(cyclone));

  // Swapped vendor bytes must not be detected.
  loc::GidBytes swapped = make_fastdds_gid(0xAB, 0xCD, 1);
  swapped[0] = 0x0F;
  swapped[1] = 0x01;
  EXPECT_FALSE(loc::is_fastdds_gid(swapped));

  // Too short to contain a host id.
  EXPECT_FALSE(loc::is_fastdds_gid(loc::GidBytes{0x01, 0x0F, 0xAB}));
  EXPECT_FALSE(loc::is_fastdds_gid(loc::GidBytes{}));
}

TEST(LocalityTest, ComparesSameHost)
{
  // Intent: same host id (bytes 2..3) => same host even across processes.
  const auto self = make_fastdds_gid(0x12, 0x34, 1);
  const auto other_process = make_fastdds_gid(0x12, 0x34, 99);
  EXPECT_EQ(loc::compare_host(self, other_process), loc::HostRelation::kSameHost);
  EXPECT_EQ(loc::compare_host(self, self), loc::HostRelation::kSameHost);
}

TEST(LocalityTest, ComparesOtherHost)
{
  // Intent: a difference in either host-id byte means another host.
  const auto self = make_fastdds_gid(0x12, 0x34, 1);
  EXPECT_EQ(
    loc::compare_host(self, make_fastdds_gid(0x12, 0x35, 1)), loc::HostRelation::kOtherHost);
  EXPECT_EQ(
    loc::compare_host(self, make_fastdds_gid(0x13, 0x34, 1)), loc::HostRelation::kOtherHost);
}

TEST(LocalityTest, NonFastDdsIsUnknown)
{
  // Intent: host cannot be derived unless BOTH gids are Fast DDS gids.
  const auto fast = make_fastdds_gid(0x12, 0x34, 1);
  loc::GidBytes other{0x10, 0x9a, 0x12, 0x34, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  EXPECT_EQ(loc::compare_host(fast, other), loc::HostRelation::kUnknown);
  EXPECT_EQ(loc::compare_host(other, fast), loc::HostRelation::kUnknown);
  EXPECT_EQ(loc::compare_host(other, other), loc::HostRelation::kUnknown);
  EXPECT_EQ(loc::compare_host(loc::GidBytes{}, fast), loc::HostRelation::kUnknown);
}

TEST(LocalityTest, HostTagFormat)
{
  // Intent: tag is "fastdds:" + 4 lowercase hex digits of bytes 2..3.
  EXPECT_EQ(loc::gid_host_tag(make_fastdds_gid(0xAB, 0x0C, 1)), "fastdds:ab0c");
  EXPECT_EQ(loc::gid_host_tag(make_fastdds_gid(0x00, 0x00, 1)), "fastdds:0000");
  EXPECT_EQ(loc::gid_host_tag(loc::GidBytes{0x10, 0x9a, 0x12, 0x34}), "");
}

TEST(LocalityTest, ConvertsFixedSizeArrays)
{
  // Intent: rmw gids are fixed-size arrays (16 or 24 bytes depending on distro).
  std::array<std::uint8_t, 24> raw{};
  raw[0] = 0x01;
  raw[1] = 0x0F;
  raw[2] = 0xFE;
  raw[3] = 0xED;
  const auto gid = loc::to_gid_bytes(raw);
  EXPECT_EQ(gid.size(), 24U);
  EXPECT_EQ(loc::gid_host_tag(gid), "fastdds:feed");
}

TEST(LocalityTest, PatternMatching)
{
  // Intent: trailing '*' is a prefix match, otherwise exact match.
  EXPECT_TRUE(loc::matches_pattern("/robot1", "/robot1"));
  EXPECT_FALSE(loc::matches_pattern("/robot1", "/robot10"));
  EXPECT_TRUE(loc::matches_pattern("/robot1*", "/robot10"));
  EXPECT_TRUE(loc::matches_pattern("/robot1*", "/robot1/sensors"));
  EXPECT_FALSE(loc::matches_pattern("/robot1*", "/robot2"));
  EXPECT_TRUE(loc::matches_pattern("*", "/anything"));
  EXPECT_FALSE(loc::matches_pattern("", "/anything"));

  EXPECT_TRUE(loc::matches_any_pattern({"/a", "/b*"}, "/b/c"));
  EXPECT_FALSE(loc::matches_any_pattern({"/a", "/b*"}, "/c"));
  EXPECT_FALSE(loc::matches_any_pattern({}, "/a"));
}
