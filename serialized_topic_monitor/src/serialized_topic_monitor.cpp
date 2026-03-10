#include "serialized_topic_monitor/serialized_topic_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include <rclcpp/qos.hpp>
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
TopicHzMonitorWebNode::TopicHzMonitorWebNode()
: rclcpp::Node("serialized_topic_monitor_node")
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

  load_parameters();

  stats_publisher_ =
    this->create_publisher<std_msgs::msg::String>(stats_topic_, rclcpp::QoS(1).reliable());
  graph_publisher_ =
    this->create_publisher<std_msgs::msg::String>(graph_topic_, rclcpp::QoS(1).reliable());

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
 * @brief Refresh monitored topic set from current ROS graph state.
 */
void TopicHzMonitorWebNode::update_topics()
{
  load_parameters();

  for (auto & entry : monitors_) {
    entry.second->set_window_size(window_size_);
  }

  const auto topic_map = this->get_topic_names_and_types();

  std::unordered_set<std::string> seen_topics;

  for (const auto & entry : topic_map) {
    const auto & topic_name = entry.first;
    const auto & topic_types = entry.second;

    if (!should_monitor_topic(topic_name) || topic_types.empty()) {
      continue;
    }

    seen_topics.insert(topic_name);

    if (monitors_.find(topic_name) == monitors_.end()) {
      monitors_.emplace(
        topic_name,
        std::make_shared<TopicMonitor>(this, topic_name, topic_types.front(), window_size_));
    }
  }

  std::vector<std::string> to_remove;
  for (const auto & entry : monitors_) {
    if (seen_topics.find(entry.first) == seen_topics.end()) {
      to_remove.push_back(entry.first);
    }
  }

  for (const auto & topic_name : to_remove) {
    monitors_.erase(topic_name);
  }
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
 * @brief Build stats payload consumed by table/chart views.
 */
std::string TopicHzMonitorWebNode::build_stats_json() const
{
  const auto now = this->now();

  std::vector<TopicStats> stats;
  stats.reserve(monitors_.size());

  for (const auto & entry : monitors_) {
    const auto & topic_name = entry.first;
    const auto & monitor = entry.second;
    const std::size_t publisher_count =
      static_cast<std::size_t>(this->count_publishers(topic_name));
    const std::size_t subscriber_count =
      static_cast<std::size_t>(this->count_subscribers(topic_name));
    stats.push_back(
      monitor->snapshot(now, stale_timeout_sec_, publisher_count, subscriber_count));
  }

  std::sort(
    stats.begin(),
    stats.end(),
    [](const TopicStats & a, const TopicStats & b) {
      return a.name < b.name;
    });

  std::ostringstream oss;
  oss << "{";
  oss << "\"generated_at_sec\":" << std::fixed << std::setprecision(3) << now.seconds() << ",";
  oss << "\"topic_count\":" << stats.size() << ",";
  oss << "\"topics\":[";
  for (std::size_t i = 0U; i < stats.size(); ++i) {
    const auto & s = stats[i];
    if (i > 0U) {
      oss << ",";
    }
    oss << "{"
        << "\"name\":\"" << json_escape(s.name) << "\","
        << "\"type\":\"" << json_escape(s.type) << "\","
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
 */
GraphData TopicHzMonitorWebNode::build_graph_data() const
{
  const auto now = this->now();
  const std::string local_host = get_hostname();

  const auto topic_map = this->get_topic_names_and_types();

  GraphData graph;
  std::set<std::string> added_node_ids;
  std::set<std::string> added_edge_ids;
  std::map<std::string, TopicStats> topic_stats_map;

  for (const auto & entry : monitors_) {
    const auto & topic_name = entry.first;
    const auto & monitor = entry.second;
    const std::size_t publisher_count =
      static_cast<std::size_t>(this->count_publishers(topic_name));
    const std::size_t subscriber_count =
      static_cast<std::size_t>(this->count_subscribers(topic_name));
    topic_stats_map.emplace(
      topic_name,
      monitor->snapshot(now, stale_timeout_sec_, publisher_count, subscriber_count));
  }

  auto add_host_cluster = [&](const std::string & host) {
    const std::string host_id = "host://" + host;
    if (added_node_ids.insert(host_id).second) {
      graph.nodes.push_back(GraphNode{
        host_id,
        host,
        "host",
        "neutral",
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

  for (const auto & entry : topic_map) {
    const auto & topic_name = entry.first;
    const auto & topic_types = entry.second;

    if (!should_monitor_topic(topic_name) || topic_types.empty()) {
      continue;
    }

    TopicStats topic_stats;
    const auto stats_it = topic_stats_map.find(topic_name);
    if (stats_it != topic_stats_map.end()) {
      topic_stats = stats_it->second;
    } else {
      topic_stats.name = topic_name;
      topic_stats.type = topic_types.front();
      topic_stats.alive = this->count_publishers(topic_name) > 0;
      topic_stats.stale = true;
    }

    if (added_node_ids.insert(topic_name).second) {
      graph.nodes.push_back(GraphNode{
        topic_name,
        topic_name,
        "topic",
        topic_status_to_string(topic_stats),
        "",
        "",
        ""
      });
    }

    const auto publishers = this->get_publishers_info_by_topic(topic_name);
    for (const auto & pub : publishers) {
      const std::string node_fqn =
        make_node_fqn(pub.node_namespace(), pub.node_name());
      const std::string ns_id =
        add_namespace_cluster(local_host, pub.node_namespace());

      if (added_node_ids.insert(node_fqn).second) {
        graph.nodes.push_back(GraphNode{
          node_fqn,
          node_fqn,
          "node",
          "neutral",
          local_host,
          pub.node_namespace(),
          ns_id
        });
      }

      const std::string edge_id = node_fqn + "->" + topic_name + "#pub";
      if (added_edge_ids.insert(edge_id).second) {
        const auto & qos = pub.qos_profile();
        graph.edges.push_back(GraphEdge{
          edge_id,
          node_fqn,
          topic_name,
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
        });
      }
    }

    const auto subscriptions = this->get_subscriptions_info_by_topic(topic_name);
    for (const auto & sub : subscriptions) {
      const std::string node_fqn =
        make_node_fqn(sub.node_namespace(), sub.node_name());
      const std::string ns_id =
        add_namespace_cluster(local_host, sub.node_namespace());

      if (added_node_ids.insert(node_fqn).second) {
        graph.nodes.push_back(GraphNode{
          node_fqn,
          node_fqn,
          "node",
          "neutral",
          local_host,
          sub.node_namespace(),
          ns_id
        });
      }

      const std::string edge_id = topic_name + "->" + node_fqn + "#sub";
      if (added_edge_ids.insert(edge_id).second) {
        const auto & qos = sub.qos_profile();
        graph.edges.push_back(GraphEdge{
          edge_id,
          topic_name,
          node_fqn,
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
        });
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
