# ATOM-03: one private header, one source, and Public/Sbus.hpp.
if(TARGET atom_03_sbus)
    return()
endif()

add_library(atom_03_sbus STATIC
    Src/sbus.cpp
    Inc/atom03_SBUS/sbus_private.hpp)
add_library(Marshland::atom_03_sbus ALIAS atom_03_sbus)

target_include_directories(atom_03_sbus
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Inc"
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../Public>")

if(TARGET Marshland::PublicHeaders)
    target_link_libraries(atom_03_sbus PUBLIC Marshland::PublicHeaders)
endif()
