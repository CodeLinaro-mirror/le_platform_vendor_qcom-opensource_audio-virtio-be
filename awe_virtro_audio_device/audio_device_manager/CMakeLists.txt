################################################################################
# VERSIONS
################################################################################
cmake_minimum_required(VERSION 3.5)
# GoogleTest requires at least C++14
set(CMAKE_CXX_STANDARD 14)

################################################################################
# Helpful 'set'
################################################################################
add_compile_options(-Werror)

# Let APIF stand for audio_plugin_intf
set(ADM_PROJECT_NAME
    AweDeviceManager
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

# ADM unit test enablement, uncomment this to compile AudioDeviceManager
# under unit test environment
#add_definitions(-DUNIT_TEST)

if ("${ENABLE_TARGET}" STREQUAL "sa8797")
    add_definitions(-DENABLE_RT_THREAD_PCMMSG)
    add_definitions(-DCOMPRESMGR_CPU_COMPONENT_NAME="audio")
    add_definitions(-DCOMPRESMGR_CPU_THREAD_GRP_NAME="awe_adm_pcmmsg")
endif()

################################################################################
# Project meta
################################################################################
project(AweDeviceManager)
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
target_link_libraries(${ADM_PROJECT_NAME} PRIVATE AweAudioPluginIntf AweSoundCardInfoUtil)
target_link_libraries(${ADM_PROJECT_NAME} PRIVATE awe_utils)
target_link_libraries(${ADM_PROJECT_NAME} PRIVATE pthread glib-2.0 cutils)
target_link_libraries(${ADM_PROJECT_NAME} PRIVATE uhab)
if ("${ENABLE_TARGET}" STREQUAL "sa8797")
    target_link_libraries(${ADM_PROJECT_NAME} PRIVATE compresmgr_client)
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
