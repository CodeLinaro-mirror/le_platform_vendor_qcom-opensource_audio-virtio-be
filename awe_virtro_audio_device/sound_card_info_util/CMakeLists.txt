################################################################################
# VERSIONS
################################################################################

cmake_minimum_required(VERSION 3.14)
# GoogleTest requires at least C++14
set(CMAKE_CXX_STANDARD 14)

################################################################################
# Helpful 'set'
################################################################################
add_compile_options(-Werror)

# Let SCIU stand for SoundCardInfoUtil
set(AWESCIU_PROJECT_NAME
    AweSoundCardInfoUtil
)

set(AWESCIU_DIR
    ${CMAKE_CURRENT_LIST_DIR}
)


set(AWESCIU_SOURCE_DIR
    ${AWESCIU_DIR}/src
)
set(AWESCIU_SOURCE
    ${AWESCIU_SOURCE_DIR}/AweSoundCardInfoUtil.cpp
)

set(AWESCIU_HEADERS_DIR
    ${AWESCIU_DIR}/inc/
)


set(AWESCIU_HEADERS
    ${AWESCIU_HEADERS_DIR}/AweSoundCardInfoUtil.h
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
add_library(${AWESCIU_PROJECT_NAME} SHARED
    ${AWESCIU_SOURCE} 
    ${AWESCIU_HEADERS}
)

include_directories(
    ${AWESCIU_HEADERS_DIR}
)

target_link_libraries(${AWESCIU_PROJECT_NAME} PUBLIC awe_controller awe_utils)

install(TARGETS ${AWESCIU_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY ${AWESCIU_HEADERS_DIR}  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/")
