cmake_minimum_required(VERSION 3.5)

project(sound_card_info_util_header)

set(sound_card_info_util_header_includes
    inc/)

include_directories(${sound_card_info_util_header_includes})

install(DIRECTORY ${sound_card_info_util_header_includes} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")