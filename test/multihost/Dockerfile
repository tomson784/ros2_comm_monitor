# Image for one simulated "PC" in the multi-host test.
# Build context: repository root (see docker-compose.yml).
ARG BASE_IMAGE=ros:jazzy-ros-base-noble
FROM ${BASE_IMAGE}

SHELL ["/bin/bash", "-c"]

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      curl iproute2 python3-yaml \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /ws
COPY serialized_topic_monitor src/ros2_comm_monitor/serialized_topic_monitor
COPY topic_monitor_web_server src/ros2_comm_monitor/topic_monitor_web_server
COPY ros2_comm_monitor src/ros2_comm_monitor/ros2_comm_monitor
RUN source /opt/ros/${ROS_DISTRO}/setup.bash \
 && colcon build --event-handlers console_cohesion+ --cmake-args -DBUILD_TESTING=OFF \
 && rm -rf build log

COPY test/multihost/tools /tools
RUN chmod +x /tools/*.sh
ENTRYPOINT ["/tools/entrypoint.sh"]
CMD ["/tools/run_role.sh"]
