################################################################################
# VERSIONS
################################################################################
cmake_minimum_required(VERSION 3.5)
# GoogleTest requires at least C++14
set(CMAKE_CXX_STANDARD 14)

################################################################################
# Helpful 'set'
################################################################################
# Let APIF stand for audio_plugin_intf
set(APIF_PROJECT_NAME
    ArAudioPluginIntf
)
set(AGMAP_DIR
    ${CMAKE_CURRENT_LIST_DIR}
)

set(AGMAP_SOURCE_DIR
    ${AGMAP_DIR}/src
)
set(AGMAP_SOURCE
    ${AGMAP_SOURCE_DIR}/AGMAudioPlugin.cpp
)

set(AGMAP_HEADERS_DIR
    ${AGMAP_DIR}/inc/
)
set(AGMAP_HEADERS
    ${AGMAP_HEADERS_DIR}/AGMAudioPlugin.h
)


################################################################################
# Project meta
################################################################################
project(ArAudioPluginIntf)
find_package(Threads)
find_package(PkgConfig)
pkg_check_modules(GLIB REQUIRED glib-2.0)

################################################################################
# Project dependencies
################################################################################
add_library(${APIF_PROJECT_NAME} SHARED
    ${AGMAP_SOURCE}
    ${AGMAP_HEADERS}
)

include_directories( ${AGMAP_HEADERS_DIR})
include_directories(${GLIB_INCLUDE_DIRS})

target_link_libraries(${APIF_PROJECT_NAME} PRIVATE audio_log_util log agmserverwrapper agm)

install(TARGETS ${APIF_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY  ${AGMAP_HEADERS_DIR} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")