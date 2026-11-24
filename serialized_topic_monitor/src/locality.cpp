#include "serialized_topic_monitor/locality.hpp"

#include <cstdio>

namespace serialized_topic_monitor
{
namespace locality
{

namespace
{
constexpr std::size_t kHostIdOffset = 2U;
constexpr std::size_t kMinFastDdsGidSize = 4U;
}  // namespace

bool is_fastdds_gid(const GidBytes & gid)
{
  return gid.size() >= kMinFastDdsGidSize &&
         gid[0] == kFastDdsVendorId0 &&
         gid[1] == kFastDdsVendorId1;
}

HostRelation compare_host(const GidBytes & self_gid, const GidBytes & other_gid)
{
  if (!is_fastdds_gid(self_gid) || !is_fastdds_gid(other_gid)) {
    return HostRelation::kUnknown;
  }
  const bool same =
    self_gid[kHostIdOffset] == other_gid[kHostIdOffset] &&
    self_gid[kHostIdOffset + 1U] == other_gid[kHostIdOffset + 1U];
  return same ? HostRelation::kSameHost : HostRelation::kOtherHost;
}

std::string gid_host_tag(const GidBytes & gid)
{
  if (!is_fastdds_gid(gid)) {
    return "";
  }
  char buf[16];
  std::snprintf(
    buf, sizeof(buf), "fastdds:%02x%02x",
    static_cast<unsigned int>(gid[kHostIdOffset]),
    static_cast<unsigned int>(gid[kHostIdOffset + 1U]));
  return std::string(buf);
}

bool matches_pattern(const std::string & pattern, const std::string & value)
{
  if (pattern.empty()) {
    return false;
  }
  if (pattern.back() == '*') {
    const std::string prefix = pattern.substr(0, pattern.size() - 1U);
    return value.compare(0, prefix.size(), prefix) == 0;
  }
  return pattern == value;
}

bool matches_any_pattern(const std::vector<std::string> & patterns, const std::string & value)
{
  for (const auto & pattern : patterns) {
    if (matches_pattern(pattern, value)) {
      return true;
    }
  }
  return false;
}

}  // namespace locality
}  // namespace serialized_topic_monitor
