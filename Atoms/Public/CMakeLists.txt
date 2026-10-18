# Current cross-ATOM contracts. Configure before the ATOM implementation targets.
# Keep this a leaf target: no implementation or ATOM link dependencies.
add_library(marshland_public_headers INTERFACE)
add_library(Marshland::PublicHeaders ALIAS marshland_public_headers)

target_include_directories(marshland_public_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

target_sources(marshland_public_headers INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Sbus.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Auto.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Web.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CanTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Motor.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Servo.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Log.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StateMachineTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/IStateMachine.hpp")
