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
    AudioPluginIntf
)
set(CSD2AP_DIR
    csd2_audio_plugin
)
set(CSD2AP_SOURCE_DIR
    ${CSD2AP_DIR}/src
)
set(CSD2AP_SOURCE
    ${CSD2AP_SOURCE_DIR}/csd2_audio_plugin.cpp
)
set(APIF_HEADERS_DIR
    inc/
)
set(CSD2AP_HEADERS_DIR
    ${CSD2AP_DIR}/inc/
)
set(CSD2AP_HEADERS
    ${CSD2AP_HEADERS_DIR}/csd2_audio_plugin.h
)
set(APIF_HEADERS
    ${APIF_HEADERS_DIR}/audio_plugin_interface.h
)

################################################################################
# Project meta
################################################################################
project(AudioPluginIntf)
find_package(Threads)

################################################################################
# Project dependencies
################################################################################
add_library(${APIF_PROJECT_NAME} SHARED
    ${CSD2AP_SOURCE}
    ${CSD2AP_HEADERS}
    ${APIF_HEADERS}
)

include_directories(${APIF_HEADERS_DIR} ${CSD2AP_HEADERS_DIR})

target_link_libraries(${APIF_PROJECT_NAME} PRIVATE csd2 audio_log_util log)

install(TARGETS ${APIF_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY ${APIF_HEADERS_DIR} ${CSD2AP_HEADERS_DIR} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
