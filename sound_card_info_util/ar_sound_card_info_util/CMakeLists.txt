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
set(ARSCIU_PROJECT_NAME
    ArSoundCardInfoUtil
)

set(ARSCIU_DIR
    ${CMAKE_CURRENT_LIST_DIR}
)

set(ARSCIU_SOURCE_DIR
    ${ARSCIU_DIR}/src
)
set(ARSCIU_SOURCE
    ${ARSCIU_SOURCE_DIR}/ArSoundCardInfoUtil.cpp
)
set(COMMON_HEADER_DIR
    ${PROJECT_ROOT_DIR}/inc/
)

set(ARSCIU_HEADERS_DIR
    ${ARSCIU_DIR}/inc/
)

set(ARSCIU_HEADERS
    ${ARSCIU_HEADERS_DIR}/ArSoundCardInfoUtil.h
)

set(SCIU_LIBRARIES_DIR
    libs
)

# DAI Builder
set(DAI_BUILDER_SRC_DIR
    ${SCIU_LIBRARIES_DIR}/dai_builder/src
)
set(DAI_BUILDER_SRC
    ${DAI_BUILDER_SRC_DIR}/DigitalAudioIntfBuilder.cpp
)
set(DAI_BUILDER_HEADERS_DIR
    ${SCIU_LIBRARIES_DIR}/dai_builder/inc/
)
set(DAI_BUILDER_HEADERS
    ${DAI_BUILDER_HEADERS_DIR}/DigitalAudioIntfBuilder.h
)

# Mixer Ctl Builder
set(MIXER_CTL_SRC_DIR
    ${SCIU_LIBRARIES_DIR}/mixer_ctl_builder/src
)
set(MIXER_CTL_SRC
    ${MIXER_CTL_SRC_DIR}/MixerCtlBuilder.cpp
)
set(MIXER_CTL_HEADERS_DIR
    ${SCIU_LIBRARIES_DIR}/mixer_ctl_builder/inc/
)
set(MIXER_CTL_HEADERS
    ${MIXER_CTL_HEADERS_DIR}/MixerCtlBuilder.h
)

# XML Parser
set(XML_PARSER_SRC_DIR
    ${SCIU_LIBRARIES_DIR}/xml_parser/src
)
set(XML_PARSER_SRC
    ${XML_PARSER_SRC_DIR}/XMLParser.cpp
)
set(XML_PARSER_HEADERS_DIR
    ${SCIU_LIBRARIES_DIR}/xml_parser/inc/
)
set(XML_PARSER_HEADERS
    ${XML_PARSER_HEADERS_DIR}/XMLParser.h
)

################################################################################
# Project meta
################################################################################
project(ArSoundCardInfoUtil)
find_package(Threads)
find_package(EXPAT REQUIRED)
################################################################################
# Project dependencies
################################################################################
add_library(${ARSCIU_PROJECT_NAME} SHARED
    ${ARSCIU_SOURCE} ${DAI_BUILDER_SRC} ${MIXER_CTL_SRC} ${XML_PARSER_SRC}
    ${COMMON_HEADERS} ${DAI_BUILDER_HEADERS} ${MIXER_CTL_HEADERS} ${XML_PARSER_HEADERS}
    ${ARSCIU_HEADERS}
)

include_directories(
    ${ARSCIU_HEADERS_DIR}
    ${DAI_BUILDER_HEADERS_DIR}
    ${MIXER_CTL_HEADERS_DIR}
    ${XML_PARSER_HEADERS_DIR}
)

target_link_libraries(${ARSCIU_PROJECT_NAME} PUBLIC EXPAT::EXPAT)
target_link_libraries(${ARSCIU_PROJECT_NAME} PUBLIC audio_log_util awe_utils)

install(TARGETS ${ARSCIU_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY ${ARSCIU_HEADERS_DIR}   ${DAI_BUILDER_HEADERS_DIR} ${MIXER_CTL_HEADERS_DIR} ${XML_PARSER_HEADERS_DIR} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
