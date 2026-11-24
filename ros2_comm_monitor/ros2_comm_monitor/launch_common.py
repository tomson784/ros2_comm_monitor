"""Shared launch argument definitions and node factories.

Used by agent.launch.py, master.launch.py and bringup.launch.py so that every
entry point exposes the same arguments with the same defaults.
"""

import re
import socket
from typing import Dict, List, Union

import yaml
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.launch_context import LaunchContext
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue

STATS_TOPIC_DEFAULT = "/topic_monitor/stats_json"
GRAPH_TOPIC_DEFAULT = "/topic_monitor/graph_json"

# name -> (default, description)
COMMON_ARGS = {
    "stats_topic": (STATS_TOPIC_DEFAULT, "Topic carrying per-host stats JSON."),
    "graph_topic": (GRAPH_TOPIC_DEFAULT, "Topic carrying per-host graph JSON."),
}

AGENT_ARGS = {
    "scan_period_ms": ("1000", "ROS graph re-scan period [ms]."),
    "report_period_ms": ("1000", "JSON publish period [ms]."),
    "stale_timeout_sec": ("2.0", "A topic is stale when no message arrived for this long."),
    "window_size": ("20", "Sliding window size for Hz/bandwidth."),
    "include_hidden_topics": ("false", "Include hidden topics."),
    "skip_internal_topics": ("true", "Skip ROS internal topics."),
    "host_name": ("", "Host name reported by the agent (default: gethostname())."),
    "locality_mode": ("auto", "auto: measure only topics published on this host; all: every topic."),
    "subscribe_policy": (
        "local_only",
        "local_only: skip topics that also have remote publishers; any_local: measure them too.",
    ),
    "local_node_namespaces": (
        "",
        "Namespaces treated as local (non Fast DDS RMW). YAML list or comma separated; "
        "trailing '*' = prefix match.",
    ),
    "local_node_names": (
        "",
        "Node names / FQNs treated as local (non Fast DDS RMW). YAML list or comma separated; "
        "trailing '*' = prefix match.",
    ),
}

MASTER_ARGS = {
    "host": ("0.0.0.0", "HTTP bind address."),
    "port": ("8080", "HTTP port."),
    "agent_timeout_sec": ("5.0", "A host is offline when no JSON arrived for this long."),
    "forget_after_sec": ("300.0", "A host is removed when no JSON arrived for this long."),
}


def declare_args(*arg_tables: Dict[str, tuple]) -> List[DeclareLaunchArgument]:
    """Create DeclareLaunchArgument actions for the given argument tables."""
    actions = []
    for table in arg_tables:
        for name, (default, description) in table.items():
            actions.append(DeclareLaunchArgument(name, default_value=default, description=description))
    return actions


def sanitize_node_name(text: str) -> str:
    """Make a string usable inside a ROS node name (alphanumerics and '_')."""
    sanitized = re.sub(r"[^A-Za-z0-9_]", "_", text)
    return sanitized or "unknown"


def agent_node_name(host_name: str = "") -> str:
    """Return comm_monitor_agent_<hostname> for this (or the given) host."""
    return "comm_monitor_agent_" + sanitize_node_name(host_name or socket.gethostname())


def parse_string_list(text: str) -> List[str]:
    """Parse a launch argument as a list of strings (YAML list or comma separated)."""
    text = (text or "").strip()
    if not text:
        return []
    if text.startswith("["):
        value: Union[list, str] = yaml.safe_load(text) or []
        if not isinstance(value, list):
            raise ValueError(f"Expected a list, got: {text}")
        return [str(v).strip() for v in value if str(v).strip()]
    return [part.strip() for part in text.split(",") if part.strip()]


def _agent_node(context: LaunchContext) -> List[Node]:
    def arg(name: str) -> str:
        return LaunchConfiguration(name).perform(context)

    params = {
        "stats_topic": arg("stats_topic"),
        "graph_topic": arg("graph_topic"),
        "scan_period_ms": int(arg("scan_period_ms")),
        "report_period_ms": int(arg("report_period_ms")),
        "stale_timeout_sec": float(arg("stale_timeout_sec")),
        "window_size": int(arg("window_size")),
        "include_hidden_topics": arg("include_hidden_topics").lower() == "true",
        "skip_internal_topics": arg("skip_internal_topics").lower() == "true",
        "locality_mode": arg("locality_mode"),
        "subscribe_policy": arg("subscribe_policy"),
    }
    if arg("host_name"):
        params["host_name"] = arg("host_name")
    # Empty lists cannot be typed by launch_ros, so only pass non-empty ones.
    for list_arg in ("local_node_namespaces", "local_node_names"):
        values = parse_string_list(arg(list_arg))
        if values:
            params[list_arg] = values

    return [Node(
        package="serialized_topic_monitor",
        executable="serialized_topic_monitor_node",
        name=agent_node_name(arg("host_name")),
        output="screen",
        parameters=[params],
    )]


def agent_actions() -> list:
    """Actions that start the per-host agent (args must be declared by the caller)."""
    return [OpaqueFunction(function=_agent_node)]


def web_server_node() -> Node:
    """Node action for the master web server (args must be declared by the caller)."""
    return Node(
        package="topic_monitor_web_server",
        executable="topic_monitor_web_server",
        name="topic_monitor_web_server",
        output="screen",
        parameters=[{
            "host": LaunchConfiguration("host"),
            "port": ParameterValue(LaunchConfiguration("port"), value_type=int),
            "stats_topic": LaunchConfiguration("stats_topic"),
            "graph_topic": LaunchConfiguration("graph_topic"),
            "agent_timeout_sec": ParameterValue(LaunchConfiguration("agent_timeout_sec"), value_type=float),
            "forget_after_sec": ParameterValue(LaunchConfiguration("forget_after_sec"), value_type=float),
        }],
    )
