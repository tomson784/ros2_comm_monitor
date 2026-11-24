"""Start the monitor agent on this host (run once per PC).

The agent measures only topics published on this PC and publishes stats/graph
JSON for the master. Node name: comm_monitor_agent_<hostname>.
"""

from launch import LaunchDescription

from ros2_comm_monitor.launch_common import AGENT_ARGS, COMMON_ARGS, agent_actions, declare_args


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription(declare_args(COMMON_ARGS, AGENT_ARGS) + agent_actions())
