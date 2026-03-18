#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/version.h>
#include <std_msgs/msg/string.hpp>

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace serialized_topic_monitor
{

/**
 * @brief Runtime topic statistics published to web UI.
 */
struct TopicStats
{
  std::string name;
  std::string type;
  std::size_t publisher_count{0U};
  std::size_t subscriber_count{0U};
  bool alive{false};
  bool stale{true};
  double hz{0.0};
  double bandwidth_bytes_per_sec{0.0};
  double latest_message_size_bytes{0.0};
  std::uint64_t message_count{0U};
  double age_sec{std::numeric_limits<double>::infinity()};
};

/**
 * @brief One node entry in graph payload.
 */
struct GraphNode
{
  std::string id;
  std::string label;
  std::string node_type;      // host | namespace | node | topic
  std::string status;         // ok | stale | down | neutral
  std::string host;
  std::string node_namespace;
  std::string parent_id;      // compound-node parent
};

/**
 * @brief One directed edge entry in graph payload.
 *
 * `qos` remains a short reliability string for in-graph labeling.
 * Full QoS fields are used by side-panel detail rendering.
 */
struct GraphEdge
{
  std::string id;
  std::string source;
  std::string target;
  std::string qos;            // reliability (for edge label/color)
  std::string qos_reliability;
  std::string qos_durability;
  std::string qos_history;
  std::size_t qos_depth{0U};
  std::string qos_liveliness;
  double qos_deadline_sec{0.0};
  double qos_lifespan_sec{0.0};
  double qos_liveliness_lease_duration_sec{0.0};
  bool qos_avoid_ros_namespace_conventions{false};
};

/**
 * @brief Graph payload container.
 */
struct GraphData
{
  std::vector<GraphNode> nodes;
  std::vector<GraphEdge> edges;
};

/**
 * @brief Sliding-window estimator for Hz/bandwidth.
 */
class SlidingWindowEstimator
{
public:
  explicit SlidingWindowEstimator(std::size_t window_size = 20U);
  void tick(const rclcpp::Time & stamp, std::size_t bytes);
  double hz() const;
  double bandwidth_bytes_per_sec() const;
  double latest_message_size_bytes() const;
  void set_window_size(std::size_t window_size);

private:
  void trim_to_window();

  std::deque<rclcpp::Time> timestamps_;
  std::deque<std::size_t> sizes_;
  std::size_t window_size_{20U};
};

/**
 * @brief Per-topic generic subscription and counters.
 */
class TopicMonitor
{
public:
  TopicMonitor(
    rclcpp::Node * node,
    const std::string & topic_name,
    const std::string & topic_type,
    std::size_t window_size);

  TopicMonitor(const TopicMonitor &) = delete;
  TopicMonitor & operator=(const TopicMonitor &) = delete;

  void set_window_size(std::size_t window_size);

  TopicStats snapshot(
    const rclcpp::Time & now,
    double stale_timeout_sec,
    std::size_t publisher_count,
    std::size_t subscriber_count) const;

private:
  void callback(std::shared_ptr<rclcpp::SerializedMessage> msg);

  rclcpp::Node * node_;
  std::string topic_name_;
  std::string topic_type_;
  rclcpp::GenericSubscription::SharedPtr subscription_;
  SlidingWindowEstimator estimator_;
  rclcpp::Time last_message_time_;
  std::uint64_t message_count_{0U};
  bool ever_received_{false};
};

/**
 * @brief Main ROS node that scans topics and publishes stats/graph JSON.
 */
class TopicHzMonitorWebNode : public rclcpp::Node
{
public:
  TopicHzMonitorWebNode();

private:
  void load_parameters();
  void update_topics();
  void publish_payloads();

  bool should_monitor_topic(const std::string & topic_name) const;
  bool should_include_graph_node(
    const std::string & node_namespace,
    const std::string & node_name) const;
  bool is_internal_topic(const std::string & topic_name) const;
  bool is_hidden_topic(const std::string & topic_name) const;

  static std::unordered_set<std::string> to_set(const std::vector<std::string> & input);
  static double bytes_to_mib(double bytes);
  static std::string json_escape(const std::string & value);
  static std::string make_node_fqn(const std::string & node_namespace, const std::string & node_name);
  static std::string make_namespace_cluster_id(const std::string & host, const std::string & node_namespace);
  static std::string reliability_to_string(int policy_value);
  static std::string durability_to_string(int policy_value);
  static std::string history_to_string(int policy_value);
  static std::string liveliness_to_string(int policy_value);
  static double duration_to_seconds(const rclcpp::Duration & d);
  static std::string topic_status_to_string(const TopicStats & stats);
  static std::string get_hostname();

  std::string build_stats_json() const;
  GraphData build_graph_data() const;
  std::string build_graph_json() const;

  rclcpp::TimerBase::SharedPtr scan_timer_;
  rclcpp::TimerBase::SharedPtr report_timer_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr stats_publisher_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr graph_publisher_;

  std::unordered_map<std::string, std::shared_ptr<TopicMonitor>> monitors_;

  std::vector<std::string> allowlist_;
  std::vector<std::string> denylist_;
  std::unordered_set<std::string> allowset_;
  std::unordered_set<std::string> denyset_;

  bool include_hidden_topics_{false};
  bool skip_internal_topics_{true};

  std::int64_t scan_period_ms_{1000};
  std::int64_t report_period_ms_{1000};
  double stale_timeout_sec_{2.0};
  std::size_t window_size_{20U};

  std::string stats_topic_{"/topic_monitor/stats_json"};
  std::string graph_topic_{"/topic_monitor/graph_json"};
};

}  // namespace serialized_topic_monitor
