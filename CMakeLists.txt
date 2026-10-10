cmake_minimum_required(VERSION 3.20)
project(pmcp_cpp VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PMCP_BUILD_TESTS "Build pcp-cpp tests" ON)
option(PMCP_BUILD_EXAMPLES "Build pcp-cpp examples" ON)
option(PMCP_WITH_ROS2 "Build the ROS 2 rclcpp bridge (requires a ROS 2 install)" OFF)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(PMCP_SOURCES
  src/error.cpp
  src/dialect.cpp
  src/safety.cpp
  src/server.cpp
  src/client.cpp
  src/transport.cpp
)

# The bridge is a separate translation unit rather than a second library: with
# PMCP_WITH_ROS2=OFF it compiles to nothing, so downstream code can include
# pmcp/ros2_bridge.hpp unconditionally without a ROS 2 install.
set(PMCP_ROS2_SOURCES src/ros2_bridge.cpp)
if(NOT PMCP_WITH_ROS2)
  list(REMOVE_ITEM PMCP_ROS2_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/ros2_bridge.cpp)
endif()

add_library(pmcp ${PMCP_SOURCES} ${PMCP_ROS2_SOURCES})

target_include_directories(pmcp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(pmcp PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(pmcp PRIVATE /W4)
else()
  target_compile_options(pmcp PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)
target_link_libraries(pmcp PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# ROS 2 bridge
# ---------------------------------------------------------------------------

if(PMCP_WITH_ROS2)
  find_package(rclcpp REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(sensor_msgs REQUIRED)
  find_package(geometry_msgs REQUIRED)
  find_package(std_srvs REQUIRED)
  find_package(builtin_interfaces REQUIRED)
  find_package(rclcpp_components REQUIRED)

  target_link_libraries(pmcp PUBLIC
    rclcpp::rclcpp
    ${std_msgs_TARGETS}
    ${sensor_msgs_TARGETS}
    ${geometry_msgs_TARGETS}
    ${std_srvs_TARGETS}
  )
  target_compile_definitions(pmcp PUBLIC PMCP_WITH_ROS2=1)
  message(STATUS "pcp-cpp: ROS 2 bridge ENABLED")
else()
  message(STATUS "pcp-cpp: ROS 2 bridge disabled (configure -DPMCP_WITH_ROS2=ON to enable)")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PMCP_BUILD_TESTS)
  enable_testing()
  add_executable(pmcp_tests
    tests/main.cpp
    tests/test_dialect.cpp
    tests/test_server.cpp
    tests/test_safety.cpp
  )
  target_link_libraries(pmcp_tests PRIVATE pmcp)
  target_include_directories(pmcp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  if(MSVC)
    target_compile_options(pmcp_tests PRIVATE /W4)
  else()
    # Aggregate brace-init of ActuationParameter/SensorSpec is the idiomatic way
    # to build a spec in a test; naming every member would obscure the point.
    target_compile_options(pmcp_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  endif()
  add_test(NAME pmcp_unit_tests COMMAND pmcp_tests)

  # Cross-implementation interop: drive a real SDK server with the C++ client
  # and serve C++ from a real SDK client. Registered but only meaningful when the
  # sibling repos and their dependencies are present, so it is skipped rather
  # than failed when they are not.
  add_executable(pmcp_interop_server tests/interop/server_main.cpp)
  target_link_libraries(pmcp_interop_server PRIVATE pmcp)

  add_executable(pmcp_interop_client tests/interop/cpp_client_main.cpp)
  target_link_libraries(pmcp_interop_client PRIVATE pmcp)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME pmcp_interop
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/interop/test_interop.py
                     --cpp-server $<TARGET_FILE:pmcp_interop_server>
                     --cpp-client $<TARGET_FILE:pmcp_interop_client>
                     --pmcp-root ${CMAKE_CURRENT_SOURCE_DIR}/..)
    # 77 is CTest's "skipped" convention: the sibling repos or their deps are
    # absent, which is not a failure of pcp-cpp.
    set_tests_properties(pmcp_interop PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
  endif()
endif()

if(PMCP_BUILD_EXAMPLES)
  add_executable(pmcp_example_server examples/simulated_arm_server.cpp)
  target_link_libraries(pmcp_example_server PRIVATE pmcp)

  add_executable(pmcp_example_client examples/client_example.cpp)
  target_link_libraries(pmcp_example_client PRIVATE pmcp)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS pmcp EXPORT pmcpTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/pmcp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---------------------------------------------------------------------------
# CMake package config so downstream `find_package(pmcp)` works
# ---------------------------------------------------------------------------
# The EXPORT set above was declared but never installed, and no Config file was
# generated, so `find_package(pmcp)` failed and the README's "with CMake package
# targets" claim was not true. Install the export and a package config so
# consumers can `find_package(pmcp REQUIRED)` and link `pmcp::pmcp`. This is also
# what a vcpkg/Conan port's config-fixup step expects to find.

include(CMakePackageConfigHelpers)
set(PMCP_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/pmcp)

install(EXPORT pmcpTargets
  FILE pmcpTargets.cmake
  NAMESPACE pmcp::
  DESTINATION ${PMCP_CMAKE_INSTALL_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pmcpConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/pmcpConfig.cmake
  INSTALL_DESTINATION ${PMCP_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/pmcpConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/pmcpConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/pmcpConfigVersion.cmake
  DESTINATION ${PMCP_CMAKE_INSTALL_DIR})

# ament_index_python looks for this at <prefix>/share/ament_index/resource_index.
# Only meaningful with ROS 2 on, but harmless to ship unconditionally so an
# installed prefix is complete either way. The ros2/ packaging files are not in
# this repository, so both installs are OPTIONAL — without that, `cmake --install`
# aborts on the missing plugin.xml and nothing installs at all.
install(FILES ros2/pmcp_ros2_plugin.xml DESTINATION share/ament_index/resource_index/$(PROJECT_NAME) OPTIONAL)
install(FILES ros2/launch/pmcp_bridge.launch.py
        DESTINATION share/${PROJECT_NAME}/launch OPTIONAL)