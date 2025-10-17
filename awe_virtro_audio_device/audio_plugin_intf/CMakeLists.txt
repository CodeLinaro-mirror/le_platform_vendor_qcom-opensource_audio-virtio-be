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
    AweAudioPluginIntf
)
set(AWEAP_DIR
    awe_audio_plugin
)
set(AWEAP_SOURCE_DIR
    ${AWEAP_DIR}/src
)
set(AWEAP_SOURCE
    ${AWEAP_SOURCE_DIR}/AweAudioPlugin.cpp
)
set(APIF_HEADERS_DIR
    inc/
)
set(AWEAP_HEADERS_DIR
    ${AWEAP_DIR}/inc/
)
set(AWEAP_HEADERS
    ${AWEAP_HEADERS_DIR}/AweAudioPlugin.h
)
set(APIF_HEADERS
    ${APIF_HEADERS_DIR}/audio_plugin_interface.h
)

################################################################################
# Project meta
################################################################################
project(AweAudioPluginIntf)
find_package(Threads)
find_package(PkgConfig)
pkg_check_modules(GLIB REQUIRED glib-2.0)

################################################################################
# Project dependencies
################################################################################
add_library(${APIF_PROJECT_NAME} SHARED
    ${AWEAP_SOURCE}
    ${AWEAP_HEADERS}
    ${APIF_HEADERS}
)

include_directories(${APIF_HEADERS_DIR} ${AWEAP_HEADERS_DIR})
include_directories(${GLIB_INCLUDE_DIRS})

target_link_libraries(${APIF_PROJECT_NAME} PUBLIC awe_controller awe_utils glib-2.0)

install(TARGETS ${APIF_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY ${APIF_HEADERS_DIR} ${AWEAP_HEADERS_DIR} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
