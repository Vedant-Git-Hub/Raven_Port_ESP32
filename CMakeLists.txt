cmake_minimum_required(VERSION 3.16)

# The application and its display/touch dependencies are checked into this
# project.  A clone therefore does not depend on any sibling example project.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/port/components")

# The two components above are already present locally.  Do not make the
# component manager resolve their registry metadata during an offline build.
set(ENV{IDF_COMPONENT_MANAGER} "0")

# Only pull the application and its declared dependencies; Raven does not
# need Wi-Fi, BLE, HTTP, or the other default example components.
set(COMPONENTS src)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(raven_hello)
