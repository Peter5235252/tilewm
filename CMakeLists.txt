cmake_minimum_required(VERSION 3.28)

project(tilewm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED wlroots-0.20)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

add_executable(tilewm
  src/main.cpp
)

target_include_directories(tilewm PRIVATE
  ${WLROOTS_INCLUDE_DIRS}
  ${WAYLAND_SERVER_INCLUDE_DIRS}
  ${XKBCOMMON_INCLUDE_DIRS}
  src
)

target_link_directories(tilewm PRIVATE
  ${WLROOTS_LIBRARY_DIRS}
  ${WAYLAND_SERVER_LIBRARY_DIRS}
  ${XKBCOMMON_LIBRARY_DIRS}
)

target_link_libraries(tilewm PRIVATE
  ${WLROOTS_LIBRARIES}
  ${WAYLAND_SERVER_LIBRARIES}
  ${XKBCOMMON_LIBRARIES}
)

target_compile_options(tilewm PRIVATE
  -Wall -Wextra -Wpedantic
  ${WLROOTS_CFLAGS_OTHER}
  ${WAYLAND_SERVER_CFLAGS_OTHER}
)

# wlroots gates all of its headers behind this macro.
target_compile_definitions(tilewm PRIVATE WLR_USE_UNSTABLE)

# Pure-logic unit test for the tiling layout (no wlroots dependency).
add_executable(test_tiling
  tests/test_tiling.cpp
)
target_include_directories(test_tiling PRIVATE src)
target_compile_options(test_tiling PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME tiling COMMAND test_tiling)
