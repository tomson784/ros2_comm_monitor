"""Single-host bringup (kept for compatibility): agent + web server on one PC."""

from launch import LaunchDescription

from ros2_comm_monitor.launch_common import (
    AGENT_ARGS,
    COMMON_ARGS,
    MASTER_ARGS,
    agent_actions,
    declare_args,
    web_server_node,
)


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription(
        declare_args(COMMON_ARGS, MASTER_ARGS, AGENT_ARGS) + [web_server_node()] + agent_actions()
    )
