#include "serialized_topic_monitor/serialized_topic_monitor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include <rclcpp/qos.hpp>
#include <rmw/rmw.h>
#include <unistd.h>

namespace serialized_topic_monitor
{

/**
 * @brief Construct a fixed-size sliding window estimator.
 *
 * The estimator keeps at least two samples so that rate calculations
 * (Hz / bandwidth) can be computed safely when data is available.
 */
SlidingWindowEstimator::SlidingWindowEstimator(std::size_t window_size)
: window_size_(std::max<std::size_t>(2U, window_size))
{
}

/**
 * @brief Push one message sample into the sliding window.
 *
 * @param stamp Message arrival timestamp.
 * @param bytes Serialized message size.
 */
void SlidingWindowEstimator::tick(const rclcpp::Time & stamp, std::size_t bytes)
{
  timestamps_.push_back(stamp);
  sizes_.push_back(bytes);
  trim_to_window();
}

/**
 * @brief Estimate publish frequency [Hz] from window boundaries.
 */
double SlidingWindowEstimator::hz() const
{
  if (timestamps_.size() < 2U) {
    return 0.0;
  }

  const auto duration = timestamps_.back() - timestamps_.front();
  const double seconds = duration.seconds();
  if (seconds <= 0.0) {
    return 0.0;
  }

  return static_cast<double>(timestamps_.size() - 1U) / seconds;
}

/**
 * @brief Estimate bandwidth [bytes/s] using payload sizes in the window.
 */
double SlidingWindowEstimator::bandwidth_bytes_per_sec() const
{
  if (timestamps_.size() < 2U || sizes_.size() < 2U) {
    return 0.0;
  }

  const auto duration = timestamps_.back() - timestamps_.front();
  const double seconds = duration.seconds();
  if (seconds <= 0.0) {
    return 0.0;
  }

  std::size_t total_bytes = 0U;
  for (std::size_t i = 1U; i < sizes_.size(); ++i) {
    total_bytes += sizes_[i];
  }

  return static_cast<double>(total_bytes) / seconds;
}

/**
 * @brief Return the latest observed serialized message size.
 */
double SlidingWindowEstimator::latest_message_size_bytes() const
{
  return sizes_.empty() ? 0.0 : static_cast<double>(sizes_.back());
}

/**
 * @brief Reconfigure sliding window size at runtime.
 */
void SlidingWindowEstimator::set_window_size(std::size_t window_size)
{
  window_size_ = std::max<std::size_t>(2U, window_size);
  trim_to_window();
}

/**
 * @brief Keep timestamps and payload sizes aligned to the configured window.
 */
void SlidingWindowEstimator::trim_to_window()
{
  while (timestamps_.size() > window_size_) {
    timestamps_.pop_front();
  }
  while (sizes_.size() > window_size_) {
    sizes_.pop_front();
  }
}

/**
 * @brief Create one topic monitor using generic subscription.
 *
 * Generic subscription allows counting and sizing serialized payloads
 * without deserializing user message types.
 */
TopicMonitor::TopicMonitor(
  rclcpp::Node * node,
  const std::string & topic_name,
  const std::string & topic_type,
  std::size_t window_size)
: node_(node),
  topic_name_(topic_name),
  topic_type_(topic_type),
  estimator_(window_size),
  last_message_time_(node->now())
{
  auto qos = rclcpp::QoS(rclcpp::KeepLast(1));
  qos.best_effort();
  qos.durability_volatile();

  subscription_ = node_->create_generic_subscription(
    topic_name_,
    topic_type_,
    qos,
    std::bind(&TopicMonitor::callback, this, std::placeholders::_1));
}

/**
 * @brief Update estimator window size propagated from node parameter.
 */
void TopicMonitor::set_window_size(std::size_t window_size)
{
  estimator_.set_window_size(window_size);
}

/**
 * @brief Build a consistent snapshot for one topic.
 *
 * @param now Current node time.
 * @param stale_timeout_sec Stale threshold in seconds.
 * @param publisher_count Number of current publishers.
 * @param subscriber_count Number of current subscribers.
 */
TopicStats TopicMonitor::snapshot(
  const rclcpp::Time & now,
  double stale_timeout_sec,
  std::size_t publisher_count,
  std::size_t subscriber_count) const
{
  TopicStats stats;
  stats.name = topic_name_;
  stats.type = topic_type_;
  stats.publisher_count = publisher_count;
  stats.subscriber_count = subscriber_count;
  stats.alive = publisher_count > 0U;
  stats.hz = estimator_.hz();
  stats.bandwidth_bytes_per_sec = estimator_.bandwidth_bytes_per_sec();
  stats.latest_message_size_bytes = estimator_.latest_message_size_bytes();
  stats.message_count = message_count_;
  stats.age_sec =
    ever_received_ ?
    (now - last_message_time_).seconds() :
    std::numeric_limits<double>::infinity();
  stats.stale = (!ever_received_) || (stats.age_sec > stale_timeout_sec);
  return stats;
}

/**
 * @brief Subscription callback that records timestamp, size and counters.
 */
void TopicMonitor::callback(std::shared_ptr<rclcpp::SerializedMessage> msg)
{
  if (!msg) {
    return;
  }

  const auto now = node_->now();
  std::size_t bytes = 0U;

#if RCLCPP_VERSION_MAJOR >= 19
  try {
    bytes = msg->size();
  } catch (...) {
    bytes = 0U;
  }
#else
  bytes = msg->get_rcl_serialized_message().buffer_length;
#endif

  estimator_.tick(now, bytes);
  last_message_time_ = now;
  ++message_count_;
  ever_received_ = true;
}

/**
 * @brief Construct monitoring node and initialize timers/publishers.
 */
TopicHzMonitorWebNode::TopicHzMonitorWebNode(const rclcpp::NodeOptions & options)
: rclcpp::Node("serialized_topic_monitor_node", options)
{
  this->declare_parameter<std::vector<std::string>>("allowlist", std::vector<std::string>{});
  this->declare_parameter<std::vector<std::string>>(
    "denylist",
    std::vector<std::string>{"/parameter_events", "/rosout"});
  this->declare_parameter<bool>("include_hidden_topics", false);
  this->declare_parameter<bool>("skip_internal_topics", true);
  this->declare_parameter<std::int64_t>("scan_period_ms", 1000);
  this->declare_parameter<std::int64_t>("report_period_ms", 1000);
  this->declare_parameter<double>("stale_timeout_sec", 2.0);
  this->declare_parameter<std::int64_t>("window_size", 20);
  this->declare_parameter<std::string>("stats_topic", stats_topic_);
  this->declare_parameter<std::string>("graph_topic", graph_topic_);

  // Distributed monitoring parameters.
  this->declare_parameter<std::string>("host_name", "");
  this->declare_parameter<std::string>("locality_mode", locality_mode_);
  this->declare_parameter<std::string>("subscribe_policy", subscribe_policy_);
  this->declare_parameter<std::vector<std::string>>(
    "local_node_namespaces", std::vector<std::string>{});
  this->declare_parameter<std::vector<std::string>>(
    "local_node_names", std::vector<std::string>{});

  load_parameters();

  // Reliable + depth 10 so that a burst of agents publishing at the same time
  // does not drop payloads on the master side.
  const auto payload_qos = rclcpp::QoS(rclcpp::KeepLast(10)).reliable();
  stats_publisher_ =
    this->create_publisher<std_msgs::msg::String>(stats_topic_, payload_qos);
  graph_publisher_ =
    this->create_publisher<std_msgs::msg::String>(graph_topic_, payload_qos);

  init_locality();

  scan_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(scan_period_ms_),
    std::bind(&TopicHzMonitorWebNode::update_topics, this));

  report_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(report_period_ms_),
    std::bind(&TopicHzMonitorWebNode::publish_payloads, this));

  update_topics();
}

/**
 * @brief Load and sanitize dynamic parameters used by monitoring loops.
 */
void TopicHzMonitorWebNode::load_parameters()
{
  allowlist_ = this->get_parameter("allowlist").as_string_array();
  denylist_ = this->get_parameter("denylist").as_string_array();
  include_hidden_topics_ = this->get_parameter("include_hidden_topics").as_bool();
  skip_internal_topics_ = this->get_parameter("skip_internal_topics").as_bool();
  scan_period_ms_ = this->get_parameter("scan_period_ms").as_int();
  report_period_ms_ = this->get_parameter("report_period_ms").as_int();
  stale_timeout_sec_ = this->get_parameter("stale_timeout_sec").as_double();
  window_size_ = static_cast<std::size_t>(
    std::max<std::int64_t>(2, this->get_parameter("window_size").as_int()));
  stats_topic_ = this->get_parameter("stats_topic").as_string();
  graph_topic_ = this->get_parameter("graph_topic").as_string();

  host_name_ = this->get_parameter("host_name").as_string();
  if (host_name_.empty()) {
    host_name_ = get_hostname();
  }

  const auto locality_mode = this->get_parameter("locality_mode").as_string();
  if (locality_mode == "auto" || locality_mode == "all") {
    locality_mode_ = locality_mode;
  } else {
    RCLCPP_WARN_ONCE(
      this->get_logger(),
      "Invalid locality_mode '%s' (expected auto|all); using 'auto'.", locality_mode.c_str());
    locality_mode_ = "auto";
  }

  const auto subscribe_policy = this->get_parameter("subscribe_policy").as_string();
  if (subscribe_policy == "local_only" || subscribe_policy == "any_local") {
    subscribe_policy_ = subscribe_policy;
  } else {
    RCLCPP_WARN_ONCE(
      this->get_logger(),
      "Invalid subscribe_policy '%s' (expected local_only|any_local); using 'local_only'.",
      subscribe_policy.c_str());
    subscribe_policy_ = "local_only";
  }

  local_node_namespaces_ = this->get_parameter("local_node_namespaces").as_string_array();
  local_node_names_ = this->get_parameter("local_node_names").as_string_array();

  if (scan_period_ms_ <= 0) {
    scan_period_ms_ = 1000;
  }
  if (report_period_ms_ <= 0) {
    report_period_ms_ = 1000;
  }
  if (stale_timeout_sec_ <= 0.0) {
    stale_timeout_sec_ = 2.0;
  }

  allowset_ = to_set(allowlist_);
  denyset_ = to_set(denylist_);
}

/**
 * @brief Determine how this agent decides "same host" and warn when it cannot.
 *
 * The self GID comes from our own publisher; with Fast DDS, bytes 2..3 of the
 * GUID prefix identify the host, so remote endpoints can be compared to it.
 */
void TopicHzMonitorWebNode::init_locality()
{
  rmw_identifier_ = rmw_get_implementation_identifier();
  agent_node_fqn_ = this->get_fully_qualified_name();
  env_localhost_only_ = env_forces_localhost();

  self_gid_ = locality::to_gid_bytes(stats_publisher_->get_gid().data);
  // The vendor check alone could (rarely) match a random non-Fast-DDS prefix,
  // so also require a Fast DDS based RMW.
  gid_locality_available_ =
    locality::is_fastdds_gid(self_gid_) &&
    rmw_identifier_.find("fastrtps") != std::string::npos;

  RCLCPP_INFO(
    this->get_logger(),
    "Agent host='%s' rmw='%s' locality_mode='%s' subscribe_policy='%s' self_host_tag='%s'%s",
    host_name_.c_str(),
    rmw_identifier_.c_str(),
    locality_mode_.c_str(),
    subscribe_policy_.c_str(),
    locality::gid_host_tag(self_gid_).c_str(),
    env_localhost_only_ ? " (localhost-only discovery: all endpoints are local)" : "");

  if (locality_mode_ == "auto" && !env_localhost_only_ && !gid_locality_available_ &&
    local_node_namespaces_.empty() && local_node_names_.empty())
  {
    RCLCPP_WARN(
      this->get_logger(),
      "locality_mode=auto cannot determine the host of endpoints with RMW '%s' "
      "(automatic detection supports Fast DDS only) and neither local_node_namespaces nor "
      "local_node_names is set: NO topics will be measured. Set local_node_namespaces / "
      "local_node_names, or use locality_mode:=all for single-host setups.",
      rmw_identifier_.c_str());
  }
}

/**
 * @brief Collect nodes that belong to monitor tooling (agents and web servers).
 *
 * Any node publishing or subscribing the stats/graph topics is treated as part of
 * the monitor itself and excluded from counts and the graph.
 */
void TopicHzMonitorWebNode::refresh_tool_nodes()
{
  tool_node_fqns_.clear();
  tool_participant_prefixes_.clear();
  tool_node_fqns_.insert(agent_node_fqn_);

  auto add = [this](const rclcpp::TopicEndpointInfo & info) {
      if (is_unknown_node_name(info)) {
        // Node names of remote participants can be unknown (e.g. Humble <-> Jazzy,
        // whose ros_discovery_info is incompatible). Never register the placeholder
        // name, or every unknown endpoint would be dropped; remember the DDS
        // participant (process) instead.
        const auto prefix =
          locality::gid_participant_prefix(locality::to_gid_bytes(info.endpoint_gid()));
        if (!prefix.empty()) {
          tool_participant_prefixes_.insert(prefix);
        }
        return;
      }
      tool_node_fqns_.insert(make_node_fqn(info.node_namespace(), info.node_name()));
    };

  for (const auto & topic : {stats_topic_, graph_topic_}) {
    for (const auto & info : this->get_publishers_info_by_topic(topic)) {
      add(info);
    }
    for (const auto & info : this->get_subscriptions_info_by_topic(topic)) {
      add(info);
    }
  }
}

bool TopicHzMonitorWebNode::is_unknown_node_name(const rclcpp::TopicEndpointInfo & info)
{
  return info.node_name().empty() || info.node_name() == "_NODE_NAME_UNKNOWN_";
}

bool TopicHzMonitorWebNode::is_tool_endpoint(const rclcpp::TopicEndpointInfo & info) const
{
  if (is_unknown_node_name(info)) {
    return !tool_participant_prefixes_.empty() &&
           tool_participant_prefixes_.count(
      locality::gid_participant_prefix(locality::to_gid_bytes(info.endpoint_gid()))) > 0U;
  }
  return tool_node_fqns_.count(make_node_fqn(info.node_namespace(), info.node_name())) > 0U;
}

/**
 * @brief Decide whether an endpoint lives on this agent's host.
 */
bool TopicHzMonitorWebNode::is_local_endpoint(const rclcpp::TopicEndpointInfo & info) const
{
  if (locality_mode_ == "all" || env_localhost_only_) {
    return true;
  }

  const std::string ns = info.node_namespace().empty() ? "/" : info.node_namespace();
  if (locality::matches_any_pattern(local_node_namespaces_, ns)) {
    return true;
  }
  if (locality::matches_any_pattern(local_node_names_, info.node_name()) ||
    locality::matches_any_pattern(
      local_node_names_, make_node_fqn(info.node_namespace(), info.node_name())))
  {
    return true;
  }

  if (gid_locality_available_) {
    return locality::compare_host(self_gid_, locality::to_gid_bytes(info.endpoint_gid())) ==
           locality::HostRelation::kSameHost;
  }
  return false;
}

/**
 * @brief Refresh monitored topic set from current ROS graph state.
 *
 * Every scan classifies each topic by where its publishers live and caches the
 * endpoint information for the stats/graph builders.
 */
void TopicHzMonitorWebNode::update_topics()
{
  load_parameters();
  refresh_tool_nodes();

  for (auto & entry : monitors_) {
    entry.second->set_window_size(window_size_);
  }

  const auto topic_map = this->get_topic_names_and_types();
  const bool subscribe_mixed = subscribe_policy_ == "any_local";

  std::map<std::string, TopicEndpointCache> next_cache;

  for (const auto & entry : topic_map) {
    const auto & topic_name = entry.first;
    const auto & topic_types = entry.second;

    if (!should_monitor_topic(topic_name) || topic_types.empty()) {
      continue;
    }

    TopicEndpointCache cache;
    cache.type = topic_types.front();

    for (auto & info : this->get_publishers_info_by_topic(topic_name)) {
      if (is_tool_endpoint(info)) {
        continue;
      }
      ++cache.publisher_count;
      if (is_local_endpoint(info)) {
        ++cache.local_publisher_count;
        cache.local_publishers.push_back(std::move(info));
      } else {
        ++cache.remote_publisher_count;
      }
    }

    for (auto & info : this->get_subscriptions_info_by_topic(topic_name)) {
      if (is_tool_endpoint(info)) {
        continue;
      }
      ++cache.subscriber_count;
      if (is_local_endpoint(info)) {
        cache.local_subscribers.push_back(std::move(info));
      }
    }

    if (cache.publisher_count > 0U) {
      if (cache.remote_publisher_count == 0U) {
        cache.locality = TopicLocality::kLocal;
        cache.monitored = true;
      } else if (cache.local_publisher_count > 0U) {
        cache.locality = TopicLocality::kMixed;
        cache.monitored = subscribe_mixed;
      } else {
        cache.locality = TopicLocality::kRemote;
      }
    } else if (!cache.local_subscribers.empty()) {
      cache.locality = TopicLocality::kOrphan;
    } else {
      cache.locality = TopicLocality::kNone;
    }

    // Keep only topics with an endpoint on this host: every reported topic has one
    // (local/mixed: a local publisher, orphan: a local subscriber), and remote-only
    // topics with a local subscriber are still needed for the graph.
    if (cache.local_publishers.empty() && cache.local_subscribers.empty()) {
      continue;
    }
    next_cache.emplace(topic_name, std::move(cache));
  }

  // Create subscriptions for measured topics; keep an existing one for orphan topics
  // (no publisher -> no traffic, and it preserves age/message_count); drop everything else
  // so that remote payloads stop flowing to this host.
  for (const auto & entry : next_cache) {
    const auto & topic_name = entry.first;
    const auto & cache = entry.second;
    if (cache.monitored && monitors_.find(topic_name) == monitors_.end()) {
      monitors_.emplace(
        topic_name,
        std::make_shared<TopicMonitor>(this, topic_name, cache.type, window_size_));
    }
  }

  for (auto it = monitors_.begin(); it != monitors_.end(); ) {
    const auto cache_it = next_cache.find(it->first);
    const bool keep =
      cache_it != next_cache.end() &&
      (cache_it->second.monitored || cache_it->second.locality == TopicLocality::kOrphan);
    if (keep) {
      ++it;
    } else {
      it = monitors_.erase(it);
    }
  }

  topic_cache_ = std::move(next_cache);
}

void TopicHzMonitorWebNode::publish_payloads()
{
  std_msgs::msg::String stats_msg;
  stats_msg.data = build_stats_json();
  stats_publisher_->publish(stats_msg);

  std_msgs::msg::String graph_msg;
  graph_msg.data = build_graph_json();
  graph_publisher_->publish(graph_msg);
}

/**
 * @brief Decide whether a topic should be monitored.
 *
 * Filters include allow/deny list, hidden/internal topic rules,
 * and self-generated topics used by this tool.
 */
bool TopicHzMonitorWebNode::should_monitor_topic(const std::string & topic_name) const
{
  if (!allowset_.empty() && allowset_.find(topic_name) == allowset_.end()) {
    return false;
  }
  if (denyset_.find(topic_name) != denyset_.end()) {
    return false;
  }
  if (!include_hidden_topics_ && is_hidden_topic(topic_name)) {
    return false;
  }
  if (skip_internal_topics_ && is_internal_topic(topic_name)) {
    return false;
  }
  if (topic_name == stats_topic_ || topic_name == graph_topic_) {
    return false;
  }
  return true;
}

/**
 * @brief Return true if topic is considered ROS internal/system topic.
 */
bool TopicHzMonitorWebNode::is_internal_topic(const std::string & topic_name) const
{
  if (topic_name == "/parameter_events" || topic_name == "/rosout") {
    return true;
  }
  if (topic_name.rfind("/_ros2cli_", 0) == 0) {
    return true;
  }
  if (topic_name.rfind("/statistics", 0) == 0) {
    return true;
  }
  return false;
}

/**
 * @brief Return true if topic is hidden according to ROS naming convention.
 */
bool TopicHzMonitorWebNode::is_hidden_topic(const std::string & topic_name) const
{
  if (topic_name.empty()) {
    return false;
  }
  if (topic_name[0] == '_') {
    return true;
  }

  const auto slash_pos = topic_name.find_last_of('/');
  if (slash_pos != std::string::npos && slash_pos + 1U < topic_name.size()) {
    return topic_name[slash_pos + 1U] == '_';
  }

  return false;
}

/**
 * @brief Convert vector parameter into lookup set.
 */
std::unordered_set<std::string> TopicHzMonitorWebNode::to_set(const std::vector<std::string> & input)
{
  return std::unordered_set<std::string>(input.begin(), input.end());
}

/**
 * @brief Convert bytes to MiB.
 */
double TopicHzMonitorWebNode::bytes_to_mib(double bytes)
{
  return bytes / (1024.0 * 1024.0);
}

/**
 * @brief Get local host name for graph host cluster labeling.
 */
std::string TopicHzMonitorWebNode::get_hostname()
{
  char name[256];
  name[0] = '\0';
  if (gethostname(name, sizeof(name)) != 0) {
    return "unknown_host";
  }
  name[sizeof(name) - 1U] = '\0';
  return std::string(name);
}

/**
 * @brief Build fully-qualified node name from namespace and node name.
 */
std::string TopicHzMonitorWebNode::make_node_fqn(
  const std::string & node_namespace,
  const std::string & node_name)
{
  if (node_namespace.empty() || node_namespace == "/") {
    return "/" + node_name;
  }
  if (node_namespace.back() == '/') {
    return node_namespace + node_name;
  }
  return node_namespace + "/" + node_name;
}

/**
 * @brief Build unique namespace cluster identifier.
 */
std::string TopicHzMonitorWebNode::make_namespace_cluster_id(
  const std::string & host,
  const std::string & node_namespace)
{
  const std::string ns = node_namespace.empty() ? "/" : node_namespace;
  return "ns://" + host + "::" + ns;
}

/**
 * @brief Build a graph node id for a ROS node that is unique across hosts.
 */
std::string TopicHzMonitorWebNode::make_graph_node_id(
  const std::string & host,
  const std::string & node_fqn)
{
  return "node://" + host + "::" + node_fqn;
}

/**
 * @brief Convert reliability policy enum to string.
 */
std::string TopicHzMonitorWebNode::reliability_to_string(int policy_value)
{
  if (policy_value == static_cast<int>(rclcpp::ReliabilityPolicy::Reliable)) {
    return "reliable";
  }
  if (policy_value == static_cast<int>(rclcpp::ReliabilityPolicy::BestEffort)) {
    return "best_effort";
  }
  return "unknown";
}

/**
 * @brief Convert durability policy enum to string.
 */
std::string TopicHzMonitorWebNode::durability_to_string(int policy_value)
{
  if (policy_value == static_cast<int>(rclcpp::DurabilityPolicy::Volatile)) {
    return "volatile";
  }
  if (policy_value == static_cast<int>(rclcpp::DurabilityPolicy::TransientLocal)) {
    return "transient_local";
  }
  return "unknown";
}

/**
 * @brief Convert history policy enum to string.
 */
std::string TopicHzMonitorWebNode::history_to_string(int policy_value)
{
  if (policy_value == static_cast<int>(rclcpp::HistoryPolicy::KeepLast)) {
    return "keep_last";
  }
  if (policy_value == static_cast<int>(rclcpp::HistoryPolicy::KeepAll)) {
    return "keep_all";
  }
  return "unknown";
}

/**
 * @brief Convert liveliness policy enum to string.
 */
std::string TopicHzMonitorWebNode::liveliness_to_string(int policy_value)
{
  if (policy_value == static_cast<int>(rclcpp::LivelinessPolicy::Automatic)) {
    return "automatic";
  }
  if (policy_value == static_cast<int>(rclcpp::LivelinessPolicy::ManualByTopic)) {
    return "manual_by_topic";
  }
  return "unknown";
}

/**
 * @brief Convert rclcpp duration into seconds.
 */
double TopicHzMonitorWebNode::duration_to_seconds(const rclcpp::Duration & d)
{
  return d.seconds();
}

/**
 * @brief Convert topic health state into graph status string.
 */
std::string TopicHzMonitorWebNode::topic_status_to_string(const TopicStats & stats)
{
  if (!stats.alive) {
    return "down";
  }
  if (stats.stale) {
    return "stale";
  }
  return "ok";
}

/**
 * @brief Escape text for manual JSON serialization.
 */
std::string TopicHzMonitorWebNode::json_escape(const std::string & value)
{
  std::ostringstream oss;
  for (const char c : value) {
    switch (c) {
      case '\\': oss << "\\\\"; break;
      case '"': oss << "\\\""; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          oss << "\\u"
              << std::hex
              << std::setw(4)
              << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(c))
              << std::dec;
        } else {
          oss << c;
        }
        break;
    }
  }
  return oss.str();
}

/**
 * @brief Convert topic locality into the string used in stats JSON.
 */
std::string TopicHzMonitorWebNode::locality_to_string(TopicLocality locality)
{
  switch (locality) {
    case TopicLocality::kLocal: return "local";
    case TopicLocality::kMixed: return "mixed";
    case TopicLocality::kOrphan: return "orphan";
    case TopicLocality::kRemote: return "remote";
    case TopicLocality::kNone: break;
  }
  return "none";
}

/**
 * @brief Return true when ROS discovery is restricted to this host by environment.
 *
 * In that case every visible endpoint is necessarily on this host.
 */
bool TopicHzMonitorWebNode::env_forces_localhost()
{
  const char * range = std::getenv("ROS_AUTOMATIC_DISCOVERY_RANGE");
  if (range != nullptr) {
    std::string value(range);
    std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) {return static_cast<char>(std::toupper(c));});
    if (value == "LOCALHOST") {
      return true;
    }
  }
  const char * localhost_only = std::getenv("ROS_LOCALHOST_ONLY");
  return localhost_only != nullptr && std::string(localhost_only) == "1";
}

/**
 * @brief Build one topic's stats, using the live monitor when one exists.
 */
TopicStats TopicHzMonitorWebNode::make_topic_stats(
  const std::string & topic_name,
  const TopicEndpointCache & cache,
  const rclcpp::Time & now) const
{
  const auto monitor_it = monitors_.find(topic_name);
  if (monitor_it != monitors_.end()) {
    return monitor_it->second->snapshot(
      now, stale_timeout_sec_, cache.publisher_count, cache.subscriber_count);
  }

  TopicStats stats;
  stats.name = topic_name;
  stats.type = cache.type;
  stats.publisher_count = cache.publisher_count;
  stats.subscriber_count = cache.subscriber_count;
  stats.alive = cache.publisher_count > 0U;
  stats.stale = true;
  return stats;
}

/**
 * @brief Build stats payload consumed by table/chart views.
 */
std::string TopicHzMonitorWebNode::build_stats_json() const
{
  const auto now = this->now();

  struct Row
  {
    TopicStats stats;
    const TopicEndpointCache * cache;
  };
  std::vector<Row> rows;
  rows.reserve(topic_cache_.size());

  // topic_cache_ is a std::map, so rows are already sorted by topic name.
  for (const auto & entry : topic_cache_) {
    if (!entry.second.reported()) {
      continue;
    }
    rows.push_back(Row{make_topic_stats(entry.first, entry.second, now), &entry.second});
  }

  const std::string host = json_escape(host_name_);

  std::ostringstream oss;
  oss << "{";
  oss << "\"schema_version\":2,";
  oss << "\"host\":\"" << host << "\",";
  oss << "\"agent_node\":\"" << json_escape(agent_node_fqn_) << "\",";
  oss << "\"rmw\":\"" << json_escape(rmw_identifier_) << "\",";
  oss << "\"locality_mode\":\"" << json_escape(locality_mode_) << "\",";
  oss << "\"subscribe_policy\":\"" << json_escape(subscribe_policy_) << "\",";
  oss << "\"generated_at_sec\":" << std::fixed << std::setprecision(3) << now.seconds() << ",";
  oss << "\"topic_count\":" << rows.size() << ",";
  oss << "\"topics\":[";
  for (std::size_t i = 0U; i < rows.size(); ++i) {
    const auto & s = rows[i].stats;
    const auto & c = *rows[i].cache;
    if (i > 0U) {
      oss << ",";
    }
    oss << "{"
        << "\"name\":\"" << json_escape(s.name) << "\","
        << "\"type\":\"" << json_escape(s.type) << "\","
        << "\"host\":\"" << host << "\","
        << "\"monitored\":" << (c.monitored ? "true" : "false") << ","
        << "\"locality\":\"" << locality_to_string(c.locality) << "\","
        << "\"local_publisher_count\":" << c.local_publisher_count << ","
        << "\"remote_publisher_count\":" << c.remote_publisher_count << ","
        << "\"publisher_count\":" << s.publisher_count << ","
        << "\"subscriber_count\":" << s.subscriber_count << ","
        << "\"alive\":" << (s.alive ? "true" : "false") << ","
        << "\"stale\":" << (s.stale ? "true" : "false") << ","
        << "\"hz\":" << std::fixed << std::setprecision(3) << s.hz << ","
        << "\"bandwidth_bytes_per_sec\":" << std::fixed << std::setprecision(3) << s.bandwidth_bytes_per_sec << ","
        << "\"bandwidth_mib_per_sec\":" << std::fixed << std::setprecision(3) << bytes_to_mib(s.bandwidth_bytes_per_sec) << ","
        << "\"latest_message_size_bytes\":" << std::fixed << std::setprecision(3) << s.latest_message_size_bytes << ","
        << "\"latest_message_size_mib\":" << std::fixed << std::setprecision(3) << bytes_to_mib(s.latest_message_size_bytes) << ","
        << "\"message_count\":" << s.message_count << ",";
    if (std::isfinite(s.age_sec)) {
      oss << "\"age_sec\":" << std::fixed << std::setprecision(3) << s.age_sec;
    } else {
      oss << "\"age_sec\":null";
    }
    oss << "}";
  }
  oss << "]}";
  return oss.str();
}

/**
 * @brief Build graph nodes/edges and attach per-link QoS metadata.
 *
 * Only endpoints on this host are emitted. Topic node ids are the topic names,
 * which are identical on every host, so the master can union per-host graphs.
 */
GraphData TopicHzMonitorWebNode::build_graph_data() const
{
  const auto now = this->now();
  const std::string & local_host = host_name_;

  GraphData graph;
  std::set<std::string> added_node_ids;
  std::set<std::string> added_edge_ids;

  auto add_host_cluster = [&](const std::string & host) {
    const std::string host_id = "host://" + host;
    if (added_node_ids.insert(host_id).second) {
      graph.nodes.push_back(GraphNode{
        host_id,
        host,
        "host",
        "ok",
        host,
        "",
        ""
      });
    }
    return host_id;
  };

  auto add_namespace_cluster = [&](const std::string & host, const std::string & node_namespace) {
    const std::string host_id = add_host_cluster(host);
    const std::string ns_id = make_namespace_cluster_id(host, node_namespace);
    const std::string ns_label = node_namespace.empty() ? "/" : node_namespace;

    if (added_node_ids.insert(ns_id).second) {
      graph.nodes.push_back(GraphNode{
        ns_id,
        ns_label,
        "namespace",
        "neutral",
        host,
        node_namespace,
        host_id
      });
    }
    return ns_id;
  };

  auto add_ros_node = [&](const rclcpp::TopicEndpointInfo & info) {
    const std::string node_fqn = make_node_fqn(info.node_namespace(), info.node_name());
    const std::string node_id = make_graph_node_id(local_host, node_fqn);
    const std::string ns_id = add_namespace_cluster(local_host, info.node_namespace());
    if (added_node_ids.insert(node_id).second) {
      graph.nodes.push_back(GraphNode{
        node_id,
        node_fqn,
        "node",
        "neutral",
        local_host,
        info.node_namespace(),
        ns_id
      });
    }
    return node_id;
  };

  auto make_edge = [](
    const std::string & id, const std::string & source, const std::string & target,
    const rclcpp::QoS & qos) {
      return GraphEdge{
        id,
        source,
        target,
        reliability_to_string(static_cast<int>(qos.reliability())),
        reliability_to_string(static_cast<int>(qos.reliability())),
        durability_to_string(static_cast<int>(qos.durability())),
        history_to_string(static_cast<int>(qos.history())),
        static_cast<std::size_t>(qos.depth()),
        liveliness_to_string(static_cast<int>(qos.liveliness())),
        duration_to_seconds(qos.deadline()),
        duration_to_seconds(qos.lifespan()),
        duration_to_seconds(qos.liveliness_lease_duration()),
        qos.avoid_ros_namespace_conventions()
      };
    };

  // Always emit the host so that a host without any endpoint is still visible.
  add_host_cluster(local_host);

  for (const auto & entry : topic_cache_) {
    const auto & topic_name = entry.first;
    const auto & cache = entry.second;

    if (cache.local_publishers.empty() && cache.local_subscribers.empty()) {
      continue;
    }

    std::string status = "neutral";
    if (cache.monitored) {
      status = topic_status_to_string(make_topic_stats(topic_name, cache, now));
    } else if (cache.locality == TopicLocality::kOrphan) {
      status = "down";
    }

    if (added_node_ids.insert(topic_name).second) {
      graph.nodes.push_back(GraphNode{
        topic_name,
        topic_name,
        "topic",
        status,
        "",
        "",
        ""
      });
    }

    for (const auto & pub : cache.local_publishers) {
      const std::string node_id = add_ros_node(pub);
      const std::string edge_id = node_id + "->" + topic_name + "#pub";
      if (added_edge_ids.insert(edge_id).second) {
        graph.edges.push_back(make_edge(edge_id, node_id, topic_name, pub.qos_profile()));
      }
    }

    for (const auto & sub : cache.local_subscribers) {
      const std::string node_id = add_ros_node(sub);
      const std::string edge_id = topic_name + "->" + node_id + "#sub";
      if (added_edge_ids.insert(edge_id).second) {
        graph.edges.push_back(make_edge(edge_id, topic_name, node_id, sub.qos_profile()));
      }
    }
  }

  std::sort(
    graph.nodes.begin(),
    graph.nodes.end(),
    [](const GraphNode & a, const GraphNode & b) {
      if (a.node_type != b.node_type) {
        return a.node_type < b.node_type;
      }
      return a.id < b.id;
    });

  std::sort(
    graph.edges.begin(),
    graph.edges.end(),
    [](const GraphEdge & a, const GraphEdge & b) {
      return a.id < b.id;
    });

  return graph;
}

/**
 * @brief Serialize graph data to JSON for web API response.
 */
std::string TopicHzMonitorWebNode::build_graph_json() const
{
  const auto graph = build_graph_data();

  std::ostringstream oss;
  oss << "{";
  oss << "\"schema_version\":2,";
  oss << "\"host\":\"" << json_escape(host_name_) << "\",";
  oss << "\"node_count\":" << graph.nodes.size() << ",";
  oss << "\"edge_count\":" << graph.edges.size() << ",";
  oss << "\"nodes\":[";
  for (std::size_t i = 0U; i < graph.nodes.size(); ++i) {
    const auto & node = graph.nodes[i];
    if (i > 0U) {
      oss << ",";
    }
    oss << "{"
        << "\"id\":\"" << json_escape(node.id) << "\","
        << "\"label\":\"" << json_escape(node.label) << "\","
        << "\"node_type\":\"" << json_escape(node.node_type) << "\","
        << "\"status\":\"" << json_escape(node.status) << "\","
        << "\"host\":\"" << json_escape(node.host) << "\","
        << "\"node_namespace\":\"" << json_escape(node.node_namespace) << "\","
        << "\"parent_id\":\"" << json_escape(node.parent_id) << "\""
        << "}";
  }

  oss << "],\"edges\":[";
  for (std::size_t i = 0U; i < graph.edges.size(); ++i) {
    const auto & edge = graph.edges[i];
    if (i > 0U) {
      oss << ",";
    }
    oss << "{"
        << "\"id\":\"" << json_escape(edge.id) << "\","
        << "\"source\":\"" << json_escape(edge.source) << "\","
        << "\"target\":\"" << json_escape(edge.target) << "\","
        << "\"qos\":\"" << json_escape(edge.qos) << "\","
        << "\"qos_reliability\":\"" << json_escape(edge.qos_reliability) << "\","
        << "\"qos_durability\":\"" << json_escape(edge.qos_durability) << "\","
        << "\"qos_history\":\"" << json_escape(edge.qos_history) << "\","
        << "\"qos_depth\":" << edge.qos_depth << ","
        << "\"qos_liveliness\":\"" << json_escape(edge.qos_liveliness) << "\","
        << "\"qos_deadline_sec\":" << std::fixed << std::setprecision(6) << edge.qos_deadline_sec << ","
        << "\"qos_lifespan_sec\":" << std::fixed << std::setprecision(6) << edge.qos_lifespan_sec << ","
        << "\"qos_liveliness_lease_duration_sec\":" << std::fixed << std::setprecision(6) << edge.qos_liveliness_lease_duration_sec << ","
        << "\"qos_avoid_ros_namespace_conventions\":" <<
      (edge.qos_avoid_ros_namespace_conventions ? "true" : "false")
        << "}";
  }
  oss << "]}";
  return oss.str();
}

}  // namespace serialized_topic_monitor
