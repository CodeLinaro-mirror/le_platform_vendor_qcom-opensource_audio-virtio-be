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
    SoundCardInfoUtil
)
set(SCIU_SOURCE_DIR
    src
)
set(SCIU_SOURCE
    ${SCIU_SOURCE_DIR}/SoundCardInfoUtil.cpp
)
set(SCIU_HEADERS_DIR
    inc/
)
set(SCIU_HEADERS
    ${SCIU_HEADERS_DIR}/ISoundCardInfoUtil.h
    ${SCIU_HEADERS_DIR}/SoundCardInfoUtil.h
    ${SCIU_HEADERS_DIR}/SoundCardInfoTypes.h
    ${SCIU_HEADERS_DIR}/virtio_snd.h
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
project(SoundCardInfoUtil)
find_package(Threads)
find_package(EXPAT REQUIRED)

################################################################################
# Project dependencies
################################################################################
add_library(${SCIU_PROJECT_NAME}
    SHARED
    ${SCIU_SOURCE} ${DAI_BUILDER_SRC} ${MIXER_CTL_SRC} ${XML_PARSER_SRC}
    ${SCIU_HEADERS} ${DAI_BUILDER_HEADERS} ${MIXER_CTL_HEADERS} ${XML_PARSER_HEADERS}
)

include_directories(
    ${SCIU_HEADERS_DIR}
    ${DAI_BUILDER_HEADERS_DIR}
    ${MIXER_CTL_HEADERS_DIR}
    ${XML_PARSER_HEADERS_DIR}
)

target_link_libraries(${SCIU_PROJECT_NAME} PUBLIC EXPAT::EXPAT)
target_link_libraries(${SCIU_PROJECT_NAME} PUBLIC audio_log_util)

install(TARGETS ${SCIU_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY ${SCIU_HEADERS_DIR} ${DAI_BUILDER_HEADERS_DIR} ${MIXER_CTL_HEADERS_DIR} ${XML_PARSER_HEADERS_DIR}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")