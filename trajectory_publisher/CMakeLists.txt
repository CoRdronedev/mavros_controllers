cmake_minimum_required(VERSION 2.8.3)
project(trajectory_publisher)

add_definitions(-std=c++11)

find_package(catkin REQUIRED COMPONENTS
  roscpp
  rospy
  tf
  dynamic_reconfigure
  mavros_msgs
  mavlink
  controller_msgs
  message_generation
  libmavconn
)

find_package(
  Eigen3 REQUIRED
)

add_message_files(
  FILES
  TrajectoryInfo.msg
)

generate_messages(
  DEPENDENCIES
  std_msgs
)

generate_dynamic_reconfigure_options(
  cfg/TrajectoryPublisher.cfg
)

catkin_package(
  INCLUDE_DIRS include
  CATKIN_DEPENDS roscpp rospy std_msgs nav_msgs geometry_msgs tf mav_planning_msgs controller_msgs mavros_msgs message_runtime
)

include_directories(
  include
  ${Boost_INCLUDE_DIR}
  ${catkin_INCLUDE_DIRS}
  ${EIGEN3_INCLUDE_DIRS}
)

############
# BINARIES #
############
add_executable(trajectory_publisher
        src/trajectoryPublisher.cpp
        src/trajectoryPublisher_node.cpp
        src/trajectory.cpp
        src/polynomialtrajectory.cpp
        src/shapetrajectory.cpp
)
add_dependencies(${PROJECT_NAME} ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
target_link_libraries(${PROJECT_NAME} ${catkin_LIBRARIES})
##########
# EXPORT #
##########
#cs_install()
#cs_export()