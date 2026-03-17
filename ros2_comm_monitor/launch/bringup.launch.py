from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description() -> LaunchDescription:
    stats_topic = LaunchConfiguration("stats_topic")
    graph_topic = LaunchConfiguration("graph_topic")
    host = LaunchConfiguration("host")
    port = LaunchConfiguration("port")
    scan_period_ms = LaunchConfiguration("scan_period_ms")
    report_period_ms = LaunchConfiguration("report_period_ms")
    stale_timeout_sec = LaunchConfiguration("stale_timeout_sec")
    window_size = LaunchConfiguration("window_size")
    include_hidden_topics = LaunchConfiguration("include_hidden_topics")
    skip_internal_topics = LaunchConfiguration("skip_internal_topics")

    return LaunchDescription([
        DeclareLaunchArgument("stats_topic", default_value="/topic_monitor/stats_json"),
        DeclareLaunchArgument("graph_topic", default_value="/topic_monitor/graph_json"),
        DeclareLaunchArgument("host", default_value="0.0.0.0"),
        DeclareLaunchArgument("port", default_value="8080"),
        DeclareLaunchArgument("scan_period_ms", default_value="1000"),
        DeclareLaunchArgument("report_period_ms", default_value="1000"),
        DeclareLaunchArgument("stale_timeout_sec", default_value="2.0"),
        DeclareLaunchArgument("window_size", default_value="20"),
        DeclareLaunchArgument("include_hidden_topics", default_value="false"),
        DeclareLaunchArgument("skip_internal_topics", default_value="true"),
        Node(
            package="serialized_topic_monitor",
            executable="serialized_topic_monitor_node",
            name="serialized_topic_monitor_node",
            output="screen",
            parameters=[{
                "stats_topic": stats_topic,
                "graph_topic": graph_topic,
                "scan_period_ms": scan_period_ms,
                "report_period_ms": report_period_ms,
                "stale_timeout_sec": stale_timeout_sec,
                "window_size": window_size,
                "include_hidden_topics": include_hidden_topics,
                "skip_internal_topics": skip_internal_topics,
            }],
        ),
        Node(
            package="topic_monitor_web_server",
            executable="topic_monitor_web_server",
            name="topic_monitor_web_server",
            output="screen",
            parameters=[{
                "host": host,
                "port": port,
                "stats_topic": stats_topic,
                "graph_topic": graph_topic,
            }],
        ),
    ])
