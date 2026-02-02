cmake_minimum_required(VERSION 3.16)
project(RaidSimulator LANGUAGES CXX)

add_executable(RaidSimulator
    src/main.cpp
)

if(APPLE)
    target_sources(RaidSimulator PRIVATE src/mac_menu.mm)
    target_link_libraries(RaidSimulator PRIVATE "-framework Cocoa")
endif()

target_link_libraries(RaidSimulator
    PRIVATE VoxelEngine SMLParser SMLUI curl
)

target_compile_features(RaidSimulator PUBLIC cxx_std_11)

target_include_directories(RaidSimulator PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_SOURCES
    "${SHADER_DIR}/world.vert"
    "${SHADER_DIR}/world.frag"
    "${SHADER_DIR}/pick.vert"
    "${SHADER_DIR}/pick.frag"
)
set(SHADER_OUTPUTS "")
foreach(shader ${SHADER_SOURCES})
    list(APPEND SHADER_OUTPUTS "${shader}.spv")
endforeach()

add_custom_command(
    OUTPUT ${SHADER_OUTPUTS}
    COMMAND glslc -o "${SHADER_DIR}/world.vert.spv" "${SHADER_DIR}/world.vert"
    COMMAND glslc -o "${SHADER_DIR}/world.frag.spv" "${SHADER_DIR}/world.frag"
    COMMAND glslc -o "${SHADER_DIR}/pick.vert.spv" "${SHADER_DIR}/pick.vert"
    COMMAND glslc -o "${SHADER_DIR}/pick.frag.spv" "${SHADER_DIR}/pick.frag"
    DEPENDS ${SHADER_SOURCES}
    VERBATIM
)

add_custom_target(RaidSimulatorShaders ALL DEPENDS ${SHADER_OUTPUTS})
add_dependencies(RaidSimulator RaidSimulatorShaders)