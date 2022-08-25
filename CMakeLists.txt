cmake_minimum_required(VERSION 3.24)
project(netlink_station_dump C)

set(CMAKE_C_STANDARD 11)

add_executable(station_dump
        main.c nl80211_attrs_map.h)

include_directories(
        /usr/include
        /usr/include/libnl3
)
target_link_libraries( station_dump
        nl-3
        nl-genl-3
)