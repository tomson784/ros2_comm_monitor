#!/bin/bash
# Give every container its own /etc/machine-id: Fast DDS (>= 2.10) derives the
# GUID host id from it, and base images often ship an empty or shared one.
set -e
if [ -n "${MACHINE_ID:-}" ]; then
  echo "${MACHINE_ID}" > /etc/machine-id
elif [ ! -s /etc/machine-id ]; then
  tr -d '-' < /proc/sys/kernel/random/uuid > /etc/machine-id
fi
source "/opt/ros/${ROS_DISTRO}/setup.bash"
source /ws/install/setup.bash
exec "$@"
