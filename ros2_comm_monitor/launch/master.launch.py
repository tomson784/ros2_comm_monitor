"""Start the master web server that aggregates every host's agent.

With with_agent:=true (default) the agent for the master PC itself is started too.
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, GroupAction
from launch.conditions import IfCondition
from launch.substitutions import LaunchConfiguration

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
        declare_args(COMMON_ARGS, MASTER_ARGS, AGENT_ARGS)
        + [
            DeclareLaunchArgument(
                "with_agent", default_value="true",
                description="Also start the agent for this (master) host."),
            web_server_node(),
            GroupAction(agent_actions(), condition=IfCondition(LaunchConfiguration("with_agent"))),
        ]
    )
