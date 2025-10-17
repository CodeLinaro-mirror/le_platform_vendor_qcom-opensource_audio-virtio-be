################################################################################
# VERSIONS
################################################################################

cmake_minimum_required(VERSION 3.14)
# GoogleTest requires at least C++14
set(CMAKE_CXX_STANDARD 14)

################################################################################
# Helpful 'set'
################################################################################
# Let SCIU stand for SoundCardInfoUtil
set(SCIU_PROJECT_NAME
    AweSoundCardInfoUtil
)
set(SCIU_SOURCE_DIR
    src
)
set(SCIU_SOURCE
    ${SCIU_SOURCE_DIR}/AweSoundCardInfoUtil.cpp
)
set(SCIU_HEADERS_DIR
    inc/
)
set(SCIU_HEADERS
    ${SCIU_HEADERS_DIR}/ISoundCardInfoUtil.h
    ${SCIU_HEADERS_DIR}/AweSoundCardInfoUtil.h
    ${SCIU_HEADERS_DIR}/SoundCardInfoTypes.h
    ${SCIU_HEADERS_DIR}/virtio_snd.h
)

#add_definitions(-DAWE_MANAGER_ENABLE)
################################################################################
# Project meta
################################################################################
project(AweSoundCardInfoUtil)
find_package(Threads)

################################################################################
# Project dependencies
################################################################################
add_library(${SCIU_PROJECT_NAME} SHARED
    ${SCIU_SOURCE} 
    ${SCIU_HEADERS}
)

include_directories(
    ${SCIU_HEADERS_DIR}
)

target_link_libraries(${SCIU_PROJECT_NAME} PUBLIC awe_controller awe_utils)

install(TARGETS ${SCIU_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY ${SCIU_HEADERS_DIR} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
