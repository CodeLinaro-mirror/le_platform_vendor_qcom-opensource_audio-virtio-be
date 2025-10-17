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
set(ADM_PROJECT_NAME
    AudioDeviceManager
)

# ADM src and inc files
set(ADM_SOURCE_DIR
    src
)
set(ADM_SOURCE
    ${ADM_SOURCE_DIR}/main.cpp
    ${ADM_SOURCE_DIR}/AudioDeviceManager.cpp
)
set(ADM_HEADERS_DIR
    inc/
)
set(ADM_HEADERS
    ${ADM_HEADERS_DIR}/AudioDeviceManager.h
)

# ADM socket interface src and inc files
set(ADM_SOCKET_INTF_SOURCE_DIR
    socket_intf/src
)
set(ADM_SOCKET_INTF_SOURCE
    ${ADM_SOCKET_INTF_SOURCE_DIR}/HabImpl.cpp
)
set(ADM_SOCKET_INTF_HEADERS_DIR
    socket_intf/inc/
)
set(ADM_SOCKET_INTF_HEADERS
    ${ADM_SOCKET_INTF_HEADERS_DIR}/hab_interface.h
)

# Option to enable code coverage in test scenario
# OFF by default in production
option(ENABLE_COVERAGE "Enable code coverage reporting" OFF)

################################################################################
# Project meta
################################################################################
project(AudioDeviceManager)
find_package(Threads)

################################################################################
# Project dependencies
################################################################################
add_library(${ADM_PROJECT_NAME} SHARED
${ADM_SOURCE}
${ADM_SOCKET_INTF_SOURCE}
${ADM_HEADERS}
${ADM_SOCKET_INTF_HEADERS}
)

# For binary executable ADM
# add_executable(${ADM_PROJECT_NAME} ${ADM_SOURCE} ${ADM_SOCKET_INTF_SOURCE})

include_directories(${ADM_HEADERS_DIR} ${ADM_SOCKET_INTF_HEADERS_DIR})

target_link_libraries(${ADM_PROJECT_NAME} PRIVATE AudioPluginIntf SoundCardInfoUtil)
target_link_libraries(${ADM_PROJECT_NAME} PRIVATE pthread uhab audio_log_util)
target_link_libraries(${ADM_PROJECT_NAME} PRIVATE glib-2.0 cutils)

# Define an option to enable/disable code coverage during compilation
if(ENABLE_COVERAGE)
    target_compile_options(${ADM_PROJECT_NAME} PRIVATE -g -O0 --coverage)
    target_link_options(${ADM_PROJECT_NAME} PRIVATE --coverage)
endif()

# For binary executable ADM
# install(TARGETS ${ADM_PROJECT_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS ${ADM_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY ${ADM_HEADERS_DIR} ${ADM_SOCKET_INTF_HEADERS_DIR} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
