#include "serialized_topic_monitor/serialized_topic_monitor.hpp"

#include <memory>

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<serialized_topic_monitor::TopicHzMonitorWebNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
