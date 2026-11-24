#!/bin/bash
# Start the workload for one simulated PC. ROLE: master | host_b | host_c
set -u
pids=()
start() { "$@" & pids+=($!); }
shutdown() { kill -TERM "${pids[@]}" 2>/dev/null; wait; exit 0; }
trap shutdown TERM INT

echo "[$(hostname)] ROLE=${ROLE} ROS_DISTRO=${ROS_DISTRO} RMW=${RMW_IMPLEMENTATION:-default} machine-id=$(cat /etc/machine-id)"

case "${ROLE}" in
  master)
    start ros2 launch ros2_comm_monitor master.launch.py \
      port:=8080 locality_mode:="${LOCALITY_MODE:-auto}" agent_timeout_sec:=5.0
    start ros2 topic pub -r 5 /a_chatter std_msgs/msg/String "{data: from_a}"
    start bash -c 'ros2 topic echo /b_chatter std_msgs/msg/String > /dev/null'
    ;;
  host_b)
    start ros2 launch ros2_comm_monitor agent.launch.py locality_mode:="${LOCALITY_MODE:-auto}"
    start python3 /tools/big_pub.py
    start ros2 topic pub -r 10 /b_chatter std_msgs/msg/String "{data: from_b}"
    ;;
  host_c)
    start ros2 launch ros2_comm_monitor agent.launch.py locality_mode:="${LOCALITY_MODE:-auto}"
    start ros2 topic pub -r 2 /c_chatter std_msgs/msg/String "{data: from_c}"
    start bash -c 'ros2 topic echo /a_chatter std_msgs/msg/String > /dev/null'
    ;;
  *)
    echo "unknown ROLE=${ROLE}" >&2; exit 2 ;;
esac
wait
