################################################################################
# VERSIONS
################################################################################
cmake_minimum_required(VERSION 3.5)
# GoogleTest requires at least C++14
set(CMAKE_CXX_STANDARD 17)

################################################################################
# Helpful 'set'
################################################################################
add_compile_options(-Werror)

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
    ${ADM_SOURCE_DIR}/audio_plugin_wrapper.cpp
    ${ADM_SOURCE_DIR}/sound_card_info_wrapper.cpp
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

# ADM unit test enablement, uncomment this to compile AudioDeviceManager
# under unit test environment
#add_definitions(-DUNIT_TEST)

add_definitions(-DCOMPRESMGR_CPU_COMPONENT_NAME="audio")
add_definitions(-DCOMPRESMGR_CPU_THREAD_PCM_MSG="adm_pcmmsg")
add_definitions(-DCOMPRESMGR_CPU_THREAD_CTLQ="adm_ctlq")
add_definitions(-DCOMPRESMGR_CPU_THREAD_EVTQ="adm_eventq")
add_definitions(-DCOMPRESMGR_CPU_THREAD_PCMQ_CP="adm_pcmq_cp")
add_definitions(-DCOMPRESMGR_CPU_THREAD_PCMQ_PB="adm_pcmq_pb")
add_definitions(-DENABLE_FLEXIBLE_THREAD_POLICY_PRIORITY)

################################################################################
# Project meta
################################################################################
##
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
target_link_libraries(${ADM_PROJECT_NAME} PRIVATE pthread glib-2.0 cutils uhab audio_log_util dl)
target_link_libraries(${ADM_PROJECT_NAME} PRIVATE compresmgr_client)
option(ENABLE_COVERAGE "Enable code coverage reporting" OFF)
# Define an option to enable/disable code coverage during compilation
if(ENABLE_COVERAGE)
    target_compile_options(${ADM_PROJECT_NAME} PRIVATE -g -O0 --coverage)
    target_link_options(${ADM_PROJECT_NAME} PRIVATE --coverage)
endif()


# Define an option to enable/disable code coverage during compilation
# by default this flag is set to off
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${ADM_PROJECT_NAME} PRIVATE -g -O0 --coverage)
    target_link_options(${ADM_PROJECT_NAME} PRIVATE --coverage)
endif()

# For binary executable ADM
# install(TARGETS ${ADM_PROJECT_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS ${ADM_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY ${ADM_HEADERS_DIR} ${ADM_SOCKET_INTF_HEADERS_DIR} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
