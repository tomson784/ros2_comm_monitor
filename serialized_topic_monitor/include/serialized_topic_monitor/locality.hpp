#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace serialized_topic_monitor
{
namespace locality
{

/**
 * @brief Raw endpoint GID bytes (RMW_GID_STORAGE_SIZE differs between distros,
 *        so the helpers work on a size-agnostic byte vector).
 */
using GidBytes = std::vector<std::uint8_t>;

/**
 * @brief Result of comparing the host part of two endpoint GIDs.
 */
enum class HostRelation
{
  kSameHost,
  kOtherHost,
  kUnknown
};

/// Fast DDS vendor id (eProsima) stored in the first two GUID prefix bytes.
constexpr std::uint8_t kFastDdsVendorId0 = 0x01;
constexpr std::uint8_t kFastDdsVendorId1 = 0x0F;

/**
 * @brief Convert any contiguous byte container (std::array, rmw_gid_t::data, ...)
 *        into GidBytes.
 */
template<typename ByteContainer>
GidBytes to_gid_bytes(const ByteContainer & bytes)
{
  return GidBytes(std::begin(bytes), std::end(bytes));
}

/**
 * @brief Return true when the GID was issued by Fast DDS.
 *
 * Fast DDS GUID prefix layout: [0..1] vendor id (0x01 0x0F),
 * [2..3] host id, [4..7] process id, [8..11] participant counter.
 */
bool is_fastdds_gid(const GidBytes & gid);

/**
 * @brief Compare the host-id part of two GIDs.
 *
 * @return kSameHost / kOtherHost when both GIDs are Fast DDS GIDs,
 *         kUnknown otherwise (host cannot be derived from the GID).
 */
HostRelation compare_host(const GidBytes & self_gid, const GidBytes & other_gid);

/**
 * @brief Human readable host tag derived from the GID.
 *
 * Format: "fastdds:hhhh" (lowercase hex of GUID bytes 2..3) for Fast DDS GIDs,
 *         empty string otherwise.
 */
std::string gid_host_tag(const GidBytes & gid);

/**
 * @brief Match a value against a name pattern.
 *
 * A pattern ending in '*' is a prefix match, otherwise an exact match.
 * A lone "*" matches everything. Empty patterns never match.
 */
bool matches_pattern(const std::string & pattern, const std::string & value);

/**
 * @brief Return true if any pattern in the list matches the value.
 */
bool matches_any_pattern(const std::vector<std::string> & patterns, const std::string & value);

}  // namespace locality
}  // namespace serialized_topic_monitor
