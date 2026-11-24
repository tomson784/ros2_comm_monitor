cmake_minimum_required(VERSION 3.10)
project(ros2_comm_monitor)

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_python REQUIRED)

# Shared launch helpers (ros2_comm_monitor.launch_common).
ament_python_install_package(${PROJECT_NAME})

install(DIRECTORY launch
  DESTINATION share/${PROJECT_NAME}
)

ament_package()
