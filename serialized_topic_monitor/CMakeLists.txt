cmake_minimum_required(VERSION 3.10)
project(serialized_topic_monitor)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_target_dependencies REQUIRED)
find_package(ament_cmake_gtest REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)

include_directories(include)

add_executable(serialized_topic_monitor_node
  src/main.cpp
  src/serialized_topic_monitor.cpp
)

ament_target_dependencies(serialized_topic_monitor_node
  rclcpp
  std_msgs
)

install(TARGETS
  serialized_topic_monitor_node
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include
)

if(BUILD_TESTING)
  ament_add_gtest(test_sliding_window_estimator
    test/test_sliding_window_estimator.cpp
    src/serialized_topic_monitor.cpp
  )
  if(TARGET test_sliding_window_estimator)
    target_include_directories(test_sliding_window_estimator PRIVATE include)
    ament_target_dependencies(test_sliding_window_estimator
      rclcpp
      std_msgs
    )
  endif()
endif()

ament_package()
